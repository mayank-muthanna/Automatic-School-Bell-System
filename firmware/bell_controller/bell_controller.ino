#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <RTClib.h>
#include <Preferences.h>
#include <atomic>
#include <cmath>
#include <esp_timer.h>
#include "bell_secrets.h"

const char* WIFI_NAME = "KALS-BELL";
static_assert(sizeof(WIFI_PASSWORD) >= 9 && sizeof(WIFI_PASSWORD) <= 64,
              "Choose an 8 to 63 character Wi-Fi password in bell_secrets.h");

const int LED_PIN = 2;
const int BUZZER_PIN = 23;  // 3-pin buzzer module I/O
const int SOLENOID_PIN = 18;  // Active-high MOSFET input
#ifndef BELL_SOLENOID_MAX_MS
#define BELL_SOLENOID_MAX_MS 150
#endif
constexpr uint16_t SOLENOID_MAX_MS = BELL_SOLENOID_MAX_MS;
constexpr uint16_t BUZZER_MAX_MS = 30000;
constexpr uint16_t SOLENOID_HOLD_CEILING_MS = 12000;
static_assert(BELL_SOLENOID_MAX_MS >= 1 && BELL_SOLENOID_MAX_MS <= SOLENOID_HOLD_CEILING_MS,
              "Solenoid limit must be between 1 and 12000 ms");
const uint32_t SOLENOID_COOLDOWN_MS = 3000;

// The buzzer module on this prototype has an active-low input.
const int BUZZER_ACTIVE_LEVEL = LOW;
const int BUZZER_INACTIVE_LEVEL = HIGH;

const char MORSE_MAYANK[] = "-- .- -.-- .- -. -.-";
const uint32_t MORSE_DOT_MS = 100;
const uint32_t MORSE_DASH_MS = 3 * MORSE_DOT_MS;
const uint32_t MORSE_SYMBOL_GAP_MS = MORSE_DOT_MS;
const uint32_t MORSE_LETTER_GAP_MS = 3 * MORSE_DOT_MS;
const uint32_t MORSE_REPEAT_GAP_MS = 7 * MORSE_DOT_MS;

const int MAX_EVENTS = 60;

RTC_DS3231 rtc;
WebServer server(80);

struct LegacyEvent {
  uint8_t day, hour, minute;
  uint16_t durationMs;
};

struct BellEvent {
  uint8_t day, hour, minute;
  uint16_t buzzerMs, solenoidMs;
};

struct Schedule {
  uint8_t count;
  bool enabled;
  BellEvent events[MAX_EVENTS];
};

Schedule schedule{};

const char* DAY_NAMES[] = {
  "Sunday", "Monday", "Tuesday",
  "Wednesday", "Thursday", "Friday", "Saturday"
};

esp_timer_handle_t solenoidTimer = nullptr;
std::atomic<bool> solenoidActive{false};
bool solenoidHasRun = false;
std::atomic<uint32_t> lastSolenoidOffAt{0};

bool bellActive = false;
bool morseToneOn = false;
uint8_t morseIndex = 0;

uint32_t bellEnd = 0;
uint32_t nextMorseTransition = 0;
uint32_t lastMinuteSeen = 0;

int readDuration(const char* field, uint16_t maximumMs) {
  const String value = server.arg(field);
  char* end = nullptr;
  const double seconds = strtod(value.c_str(), &end);
  if (value.isEmpty() || *end || !std::isfinite(seconds) ||
      seconds < 0 || seconds > maximumMs / 1000.0) return -1;

  const long milliseconds = lround(seconds * 1000);
  if (fabs(seconds * 1000 - milliseconds) > 0.00001) return -1;
  return milliseconds;
}

bool saveSettings() {
  Preferences p;
  if (!p.begin("kalsbell", false)) return false;
  const bool saved = p.putBytes("scheduleV2", &schedule, sizeof(schedule)) == sizeof(schedule);
  p.end();
  return saved;
}

void loadSettings() {
  Preferences p;
  if (!p.begin("kalsbell", true)) return;

  if (p.isKey("scheduleV2")) {
    if (p.getBytesLength("scheduleV2") == sizeof(schedule)) {
      p.getBytes("scheduleV2", &schedule, sizeof(schedule));
    }
  } else {
    LegacyEvent previous[MAX_EVENTS]{};
    const uint8_t count = p.getUChar("count", 0);
    if (count <= MAX_EVENTS && p.getBytesLength("events") == sizeof(previous) &&
        p.getBytes("events", previous, sizeof(previous)) == sizeof(previous)) {
      schedule.count = count;
      schedule.enabled = p.getBool("enabled", false);
      for (int i = 0; i < count; i++) {
        const LegacyEvent& event = previous[i];
        if (event.durationMs < 100 || event.durationMs > BUZZER_MAX_MS) {
          schedule = {};
          break;
        }
        schedule.events[i] = {
          event.day, event.hour, event.minute, event.durationMs,
          min(event.durationMs, static_cast<uint16_t>(150))
        };
      }
    }
  }
  p.end();

  if (schedule.count > MAX_EVENTS) {
    schedule = {};
    return;
  }

  for (int i = 0; i < schedule.count; i++) {
    const BellEvent& event = schedule.events[i];
    if (event.day > 6 || event.hour > 23 || event.minute > 59 ||
        event.buzzerMs > BUZZER_MAX_MS || event.solenoidMs > SOLENOID_HOLD_CEILING_MS ||
        (event.buzzerMs == 0 && event.solenoidMs == 0)) {
      schedule = {};
      return;
    }
  }
}

void stopSolenoid(void*) {
  digitalWrite(SOLENOID_PIN, LOW);
  lastSolenoidOffAt.store(millis());
  solenoidActive.store(false);
}

void startSolenoidHold(uint32_t durationMs) {
  if (solenoidTimer == nullptr || durationMs == 0) return;

  if (solenoidActive.load() ||
      (solenoidHasRun && millis() - lastSolenoidOffAt.load() < SOLENOID_COOLDOWN_MS)) {
    Serial.println("Solenoid skipped: cooling down.");
    return;
  }

  const uint32_t holdMs = min(durationMs, static_cast<uint32_t>(SOLENOID_MAX_MS));
  solenoidActive.store(true);
  digitalWrite(SOLENOID_PIN, HIGH);

  // Shut off from the ESP timer task even if the web server is busy.
  if (esp_timer_start_once(solenoidTimer, holdMs * 1000ULL) != ESP_OK) {
    stopSolenoid(nullptr);
    Serial.println("Solenoid hold timer failed to start.");
    return;
  }

  solenoidHasRun = true;
}

void setMorseOutputs(bool on) {
  morseToneOn = on;

  digitalWrite(LED_PIN, on ? HIGH : LOW);
  digitalWrite(
    BUZZER_PIN,
    on ? BUZZER_ACTIVE_LEVEL : BUZZER_INACTIVE_LEVEL
  );
}

void stopBell() {
  bellActive = false;
  setMorseOutputs(false);
}

void startBell(uint16_t buzzerMs, uint16_t solenoidMs) {
  startSolenoidHold(solenoidMs);
  if (buzzerMs == 0) return;

  const uint32_t now = millis();
  bellActive = true;
  bellEnd = now + buzzerMs;
  morseIndex = 0;
  setMorseOutputs(true);
  nextMorseTransition = now + MORSE_DASH_MS;
}

void updateBell() {
  if (!bellActive) return;

  uint32_t now = millis();

  if ((int32_t)(now - bellEnd) >= 0) {
    stopBell();
    Serial.println("Bell finished.");
    return;
  }

  if ((int32_t)(now - nextMorseTransition) < 0) return;

  if (morseToneOn) {
    setMorseOutputs(false);
    morseIndex++;

    if (MORSE_MAYANK[morseIndex] == '\0') {
      morseIndex = 0;
      nextMorseTransition = now + MORSE_REPEAT_GAP_MS;
    } else if (MORSE_MAYANK[morseIndex] == ' ') {
      morseIndex++;
      nextMorseTransition = now + MORSE_LETTER_GAP_MS;
    } else {
      nextMorseTransition = now + MORSE_SYMBOL_GAP_MS;
    }
  } else {
    const char symbol = MORSE_MAYANK[morseIndex];
    setMorseOutputs(true);
    nextMorseTransition = now +
      (symbol == '.' ? MORSE_DOT_MS : MORSE_DASH_MS);
  }
}

void checkSchedule() {
  static uint32_t lastCheck = 0;

  if (millis() - lastCheck < 150) return;
  lastCheck = millis();

  DateTime now = rtc.now();
  uint32_t currentMinute = now.unixtime() / 60;

  if (currentMinute == lastMinuteSeen) return;

  lastMinuteSeen = currentMinute;

  if (!schedule.enabled) return;

  for (int i = 0; i < schedule.count; i++) {
    const BellEvent& e = schedule.events[i];

    if (e.day == now.dayOfTheWeek() &&
        e.hour == now.hour() &&
        e.minute == now.minute()) {

      startBell(e.buzzerMs, e.solenoidMs);
      break;
    }
  }
}

void handleTime() {
  DateTime now = rtc.now();

  char buffer[32];

  snprintf(
    buffer,
    sizeof(buffer),
    "%02d/%02d/%04d %02d:%02d:%02d",
    now.day(), now.month(), now.year(),
    now.hour(), now.minute(), now.second()
  );

  server.send(200, "text/plain", buffer);
}

void handleSetTime() {
  if (!server.hasArg("dt")) {
    server.send(400, "text/plain", "Missing date/time");
    return;
  }

  int year = 0, month = 0, day = 0;
  int hour = 0, minute = 0, second = 0;

  String input = server.arg("dt");

  int parts = sscanf(
    input.c_str(),
    "%d-%d-%dT%d:%d:%d",
    &year, &month, &day,
    &hour, &minute, &second
  );

  if (parts < 5 || year < 2020 || year > 2099 ||
      month < 1 || month > 12 || day < 1 || day > 31 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      second < 0 || second > 59) {
    server.send(400, "text/plain", "Invalid date/time");
    return;
  }

  DateTime newTime(
    year, month, day,
    hour, minute, second
  );

  if (!newTime.isValid()) {
    server.send(400, "text/plain", "Invalid date/time");
    return;
  }

  rtc.adjust(newTime);

  // Skip the current minute after a reset or manual clock adjustment.
  lastMinuteSeen = rtc.now().unixtime() / 60;

  Serial.println("RTC time updated.");
  server.send(204);
}

int readWeekday(const char* field) {
  const String value = server.arg(field);
  if (value.length() != 1 || value[0] < '0' || value[0] > '6') return -1;
  return value[0] - '0';
}

int findEvent(int day, int hour, int minute) {
  for (int i = 0; i < schedule.count; i++) {
    const BellEvent& event = schedule.events[i];
    if (event.day == day && event.hour == hour && event.minute == minute) return i;
  }
  return -1;
}

bool readClockTime(const char* field, int& hour, int& minute) {
  const String value = server.arg(field);
  char extra;
  return value.length() == 5 &&
         sscanf(value.c_str(), "%2d:%2d%c", &hour, &minute, &extra) == 2 &&
         hour >= 0 && hour < 24 && minute >= 0 && minute < 60;
}

bool readOutputs(int& buzzer, int& solenoid) {
  buzzer = readDuration("buzzer", BUZZER_MAX_MS);
  solenoid = readDuration("solenoid", SOLENOID_MAX_MS);
  if (buzzer < 0 || solenoid < 0 || (buzzer == 0 && solenoid == 0)) {
    server.send(400, "text/plain", "Enter valid durations for at least one output.");
    return false;
  }
  return true;
}

void commitSchedule(const Schedule& previous) {
  if (!saveSettings()) {
    schedule = previous;
    server.send(500, "text/plain", "Could not save schedule.");
    return;
  }
  server.send(204);
}

void handleAdd() {
  const int day = readWeekday("day");
  int hour, minute, buzzer, solenoid;
  if (day < 0 || !readClockTime("at", hour, minute)) {
    server.send(400, "text/plain", "Choose a valid day and time.");
    return;
  }
  if (!readOutputs(buzzer, solenoid)) return;

  int target = schedule.count;
  if (server.hasArg("oldDay")) {
    const int oldDay = readWeekday("oldDay");
    int oldHour, oldMinute;
    if (oldDay < 0 || !readClockTime("oldAt", oldHour, oldMinute)) {
      server.send(400, "text/plain", "Invalid event reference.");
      return;
    }
    target = findEvent(oldDay, oldHour, oldMinute);
    if (target < 0) {
      server.send(404, "text/plain", "That event no longer exists.");
      return;
    }
  } else if (target == MAX_EVENTS) {
    server.send(400, "text/plain", "Schedule is full.");
    return;
  }

  const int duplicate = findEvent(day, hour, minute);
  if (duplicate >= 0 && duplicate != target) {
    server.send(409, "text/plain", "An event already exists at this time.");
    return;
  }
  const Schedule previous = schedule;
  schedule.events[target] = {
    static_cast<uint8_t>(day), static_cast<uint8_t>(hour),
    static_cast<uint8_t>(minute), static_cast<uint16_t>(buzzer),
    static_cast<uint16_t>(solenoid)
  };
  if (target == schedule.count) schedule.count++;
  commitSchedule(previous);
}

void handleDelete() {
  const int day = readWeekday("day");
  int hour, minute;
  if (day < 0 || !readClockTime("at", hour, minute)) {
    server.send(400, "text/plain", "Invalid event reference.");
    return;
  }
  const int index = findEvent(day, hour, minute);
  if (index < 0) {
    server.send(404, "text/plain", "That event no longer exists.");
    return;
  }
  const Schedule previous = schedule;
  for (int i = index + 1; i < schedule.count; i++) {
    schedule.events[i - 1] = schedule.events[i];
  }
  schedule.events[--schedule.count] = {};
  commitSchedule(previous);
}

void handleToggle() {
  const Schedule previous = schedule;
  schedule.enabled = !schedule.enabled;
  commitSchedule(previous);
}

void handleTest() {
  int buzzer, solenoid;
  if (!readOutputs(buzzer, solenoid)) return;
  startBell(buzzer, solenoid);
  server.send(204);
}

void handleEvents() {
  String json;
  json.reserve(1700);
  json = schedule.enabled ? "{\"enabled\":true" : "{\"enabled\":false";
  json += ",\"limit\":";
  json += SOLENOID_MAX_MS;
  json += ",\"events\":[";
  for (int i = 0; i < schedule.count; i++) {
    const BellEvent& e = schedule.events[i];
    char row[32];
    snprintf(row, sizeof(row), "%s[%u,%u,%u,%u,%u]", i ? "," : "",
             e.day, e.hour, e.minute, e.buzzerMs, e.solenoidMs);
    json += row;
  }
  json += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

const char DASHBOARD[] PROGMEM = R"BELL(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Automatic Bell System</title>
<style>
:root{color-scheme:light;--ink:#1c241f;--paper:#f4f2eb;--line:#a2a59b;--muted:#4e584f}
*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);font:15px/1.45 system-ui,sans-serif}
header{border-bottom:1px solid var(--line)}.shell{max-width:1080px;margin:auto;padding:24px}
.top,.bar,.actions{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px}
h1,h2{font-family:Georgia,serif;font-weight:400;line-height:1.06;margin:0}h1{font-size:clamp(2rem,5vw,3.6rem)}h2{font-size:1.7rem}
#clock,td{font-variant-numeric:tabular-nums}summary{cursor:pointer;color:var(--muted);margin-top:4px}
section{border:1px solid var(--line);padding:20px;margin-bottom:18px}
.grid{display:flex;flex-wrap:wrap;align-items:end;gap:12px;margin-top:14px}
label{display:grid;gap:4px;font-size:.84rem;color:var(--muted)}input,select,button{font:inherit;color:var(--ink);background:#fcfbf7;border:1px solid var(--line);border-radius:0;padding:8px 10px;min-height:40px}
input[type=number]{width:8rem}button{cursor:pointer}button.primary{background:var(--ink);border-color:var(--ink);color:white}button:hover{border-color:var(--ink)}button:disabled{opacity:.55;cursor:wait}
:focus-visible{outline:2px solid #346349;outline-offset:2px}
.table{overflow:auto;margin-top:14px}table{border-collapse:collapse;width:100%;min-width:540px;text-align:left}
th,td{padding:9px 12px;border-bottom:1px solid var(--line)}th{font-size:.8rem;font-weight:600;background:#eae9e1}tr:last-child td{border-bottom:0}td button{min-height:0;padding:4px 7px;margin-right:4px}
#notice{margin:0 0 12px;color:#8c3326}#notice:empty{display:none}#scheduleState{color:var(--muted);font-size:.85rem}
@media(max-width:640px){.shell{padding:16px}section{padding:14px}}
</style>
</head>
<body>
<header><div class="shell top">
<h1>Automatic Bell System</h1>
<div><time id="clock">Connecting</time>
<details><summary>Set clock</summary>
<form id="clockForm" class="grid">
<label>Date and time<input name="dt" type="datetime-local" step="1" required></label>
<button>Set clock</button><button type="button" id="sync">Use laptop time</button>
</form>
</details></div>
</div></header>
<main class="shell">
<p id="notice" role="status"></p>
<section>
<h2>Test Bell</h2>
<form id="testForm" class="grid">
<label>Buzzer (s)<input name="buzzer" type="number" min="0" max="30" step=".001" value="6" required></label>
<label>Solenoid hold (s)<input name="solenoid" type="number" min="0" step=".001" required></label>
<button class="primary" disabled>Ring</button>
</form>
</section>
<section>
<h2 id="formTitle">Add event</h2>
<form id="eventForm" class="grid">
<label>Day<select name="day"></select></label>
<label>Time<input name="at" type="time" required></label>
<label>Buzzer (s)<input name="buzzer" type="number" min="0" max="30" step=".001" value="6" required></label>
<label>Solenoid hold (s)<input name="solenoid" type="number" min="0" step=".001" required></label>
<button class="primary" id="eventSubmit" disabled>Add event</button>
<button id="cancel" type="button" hidden>Cancel</button>
</form>
</section>
<section>
<div class="bar"><h2>Events</h2><div class="actions">
<span id="scheduleState"></span><button id="toggle" disabled>Resume</button>
<label>Sort by<select id="sort">
<option value="week">Day then time</option>
<option value="time">Time of day</option>
<option value="buzzer">Buzzer duration</option>
<option value="solenoid">Solenoid hold</option>
</select></label>
<button id="direction" type="button" aria-label="Sort descending">Ascending</button>
</div></div>
<div class="table"><table>
<thead><tr><th scope="col">Day</th><th scope="col">Time</th><th scope="col">Buzzer (s)</th><th scope="col">Solenoid hold (s)</th><th scope="col">Actions</th></tr></thead>
<tbody id="rows"><tr><td colspan="5">Loading events</td></tr></tbody>
</table></div>
</section>
</main>
<script>
const byId = id => document.getElementById(id);
const days = ['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'];
const dayOrder = day => (day + 6) % 7;
const pad = value => String(value).padStart(2,'0');
const eventTime = event => pad(event[1]) + ':' + pad(event[2]);
const el = (tag, text) => {
  const node = document.createElement(tag);
  node.textContent = text;
  return node;
};
const testForm = byId('testForm');
const eventForm = byId('eventForm');
const clockForm = byId('clockForm');
const sort = byId('sort');
let events = [], solenoidLimit = 0, editing = null, descending = false;

[1,2,3,4,5,6,0].forEach(day => {
  eventForm.elements.day.add(new Option(days[day], day));
});

async function post(path, values) {
  const result = await fetch(path, {
    method:'POST',
    body:new URLSearchParams(values)
  });
  if (!result.ok) throw new Error(await result.text());
}

async function run(action) {
  byId('notice').textContent = '';
  try { await action(); }
  catch (error) { byId('notice').textContent = error.message; }
}

async function loadEvents() {
  const response = await fetch('/events', {cache:'no-store'});
  if (!response.ok) throw new Error('Could not load events.');
  const data = await response.json();
  events = data.events;
  if (!solenoidLimit) {
    solenoidLimit = data.limit / 1000;
    for (const form of [testForm, eventForm]) {
      form.elements.solenoid.max = solenoidLimit;
      form.elements.solenoid.defaultValue = Math.min(.15, solenoidLimit);
      form.querySelector('button').disabled = false;
    }
  }
  byId('scheduleState').textContent = data.enabled ? 'Schedule on' : 'Schedule paused';
  byId('toggle').textContent = data.enabled ? 'Pause' : 'Resume';
  byId('toggle').disabled = false;
  renderEvents();
}

function renderEvents() {
  const time = event => event[1] * 60 + event[2];
  const week = event => dayOrder(event[0]) * 1440 + time(event);
  const key = {week,time,buzzer:event => event[3],solenoid:event => event[4]}[sort.value];
  const ordered = [...events].sort((a,b) =>
    (key(a) - key(b)) * (descending ? -1 : 1) || week(a) - week(b)
  );
  const body = byId('rows');
  body.replaceChildren();
  if (!ordered.length) {
    const cell = el('td','No events');
    cell.colSpan = 5;
    body.insertRow().append(cell);
  }
  for (const event of ordered) {
    const row = body.insertRow();
    [days[event[0]],eventTime(event),event[3]/1000,event[4]/1000].forEach(value => {
      row.append(el('td', value));
    });
    const actions = row.insertCell();
    const edit = el('button','Edit');
    const remove = el('button','Delete');
    edit.onclick = () => editEvent(event);
    remove.onclick = () => run(async () => {
      await post('/delete', {day:event[0], at:eventTime(event)});
      if (editing && editing[0] === event[0] && eventTime(editing) === eventTime(event)) resetForm();
      await loadEvents();
    });
    actions.append(edit, remove);
  }
}

function editEvent(event) {
  editing = event;
  eventForm.elements.day.value = event[0];
  eventForm.elements.at.value = eventTime(event);
  eventForm.elements.buzzer.value = event[3] / 1000;
  eventForm.elements.solenoid.value = event[4] / 1000;
  byId('formTitle').textContent = 'Edit event';
  byId('eventSubmit').textContent = 'Save event';
  byId('cancel').hidden = false;
  eventForm.elements.at.focus();
}

function resetForm() {
  editing = null;
  eventForm.reset();
  byId('formTitle').textContent = 'Add event';
  byId('eventSubmit').textContent = 'Add event';
  byId('cancel').hidden = true;
}

function bindForm(form, action) {
  form.onsubmit = event => {
    event.preventDefault();
    const values = new FormData(form);
    const submit = event.submitter || form.querySelector('button');
    submit.disabled = true;
    run(async () => {
      try { await action(values); }
      finally { submit.disabled = false; }
    });
  };
}

bindForm(testForm, values => post('/test', values));
bindForm(eventForm, async values => {
  if (editing) {
    values.set('oldDay', editing[0]);
    values.set('oldAt', eventTime(editing));
  }
  await post('/add', values);
  resetForm();
  await loadEvents();
});
byId('cancel').onclick = resetForm;
byId('toggle').onclick = () => run(async () => {
  byId('toggle').disabled = true;
  try {
    await post('/toggle', {});
    await loadEvents();
  } finally {
    byId('toggle').disabled = false;
  }
});
sort.onchange = renderEvents;
byId('direction').onclick = () => {
  descending = !descending;
  byId('direction').textContent = descending ? 'Descending' : 'Ascending';
  byId('direction').setAttribute('aria-label', descending ? 'Sort ascending' : 'Sort descending');
  renderEvents();
};

async function refreshClock() {
  try {
    const response = await fetch('/time', {cache:'no-store'});
    if (!response.ok) throw new Error();
    byId('clock').textContent = await response.text();
  } catch (error) {
    byId('clock').textContent = 'Clock offline';
  } finally {
    setTimeout(refreshClock, 2000);
  }
}

bindForm(clockForm, values => post('/settime', values));
byId('sync').onclick = () => run(async () => {
  const now = new Date();
  const date = [now.getFullYear(),pad(now.getMonth()+1),pad(now.getDate())].join('-');
  const time = [pad(now.getHours()),pad(now.getMinutes()),pad(now.getSeconds())].join(':');
  await post('/settime', {dt:date+'T'+time});
});
refreshClock();
run(loadEvents);
</script>
</body>
</html>
)BELL";

void handleHome() {
  server.send_P(200, PSTR("text/html; charset=utf-8"), DASHBOARD);
}

void setup() {
  digitalWrite(SOLENOID_PIN, LOW);
  pinMode(SOLENOID_PIN, OUTPUT);

  Serial.begin(115200);

  esp_timer_create_args_t timerArgs{};
  timerArgs.callback = stopSolenoid;
  timerArgs.dispatch_method = ESP_TIMER_TASK;
  timerArgs.name = "solenoid-off";
  if (esp_timer_create(&timerArgs, &solenoidTimer) != ESP_OK) {
    solenoidTimer = nullptr;
    Serial.println("Solenoid disabled: could not create hold timer.");
  }

  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  digitalWrite(BUZZER_PIN, BUZZER_INACTIVE_LEVEL);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, BUZZER_INACTIVE_LEVEL);

  Wire.begin(21, 22);

  if (!rtc.begin()) {
    Serial.println("RTC NOT FOUND!");
    while (true) delay(1000);
  }

  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  loadSettings();

  // Skip the current minute after a reset or manual clock adjustment.
  lastMinuteSeen = rtc.now().unixtime() / 60;

  WiFi.mode(WIFI_AP);

  if (!WiFi.softAP(WIFI_NAME, WIFI_PASSWORD)) {
    Serial.println("Wi-Fi failed!");
    while (true) delay(1000);
  }

  server.on("/", HTTP_GET, handleHome);
  server.on("/time", HTTP_GET, handleTime);
  server.on("/events", HTTP_GET, handleEvents);

  server.on("/settime", HTTP_POST, handleSetTime);
  server.on("/add", HTTP_POST, handleAdd);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/toggle", HTTP_POST, handleToggle);
  server.on("/test", HTTP_POST, handleTest);

  server.begin();

  Serial.println("KALS Bell Controller Ready!");
  Serial.print("Wi-Fi IP: ");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  server.handleClient();

  updateBell();
  checkSchedule();

  delay(2);
}
