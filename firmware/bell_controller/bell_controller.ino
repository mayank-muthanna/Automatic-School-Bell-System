#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <RTClib.h>
#include <Preferences.h>
#include "bell_secrets.h"

const char* WIFI_NAME = "KALS-BELL";
static_assert(sizeof(WIFI_PASSWORD) >= 9 && sizeof(WIFI_PASSWORD) <= 64,
              "Choose an 8 to 63 character Wi-Fi password in bell_secrets.h");

const int LED_PIN = 2;
const int MAX_EVENTS = 60;

RTC_DS3231 rtc;
WebServer server(80);

struct BellEvent {
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint16_t durationMs;
};

BellEvent events[MAX_EVENTS] = {};
int eventCount = 0;
bool scheduleEnabled = false;

const char* DAY_NAMES[] = {
  "Sunday", "Monday", "Tuesday",
  "Wednesday", "Thursday", "Friday", "Saturday"
};

bool bellActive = false;
uint32_t bellEnd = 0;
uint32_t lastMinuteSeen = 0;

String pad2(int n) {
  return n < 10 ? "0" + String(n) : String(n);
}

uint32_t durationFromRequest() {
  if (!server.hasArg("duration")) return 0;

  float seconds = server.arg("duration").toFloat();

  if (seconds < 0.1f || seconds > 30.0f) {
    return 0;
  }

  return (uint32_t)(seconds * 1000 + 0.5f);
}

void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
}

void saveSettings() {
  Preferences p;
  if (!p.begin("kalsbell", false)) return;

  p.putUChar("count", eventCount);
  p.putBytes("events", events, sizeof(events));
  p.putBool("enabled", scheduleEnabled);

  p.end();
}

void loadSettings() {
  Preferences p;
  if (!p.begin("kalsbell", true)) return;

  int savedCount = p.getUChar("count", 0);

  if (savedCount >= 0 &&
      savedCount <= MAX_EVENTS &&
      p.getBytesLength("events") == sizeof(events)) {

    p.getBytes("events", events, sizeof(events));
    eventCount = savedCount;
  }

  scheduleEnabled = p.getBool("enabled", false);
  p.end();

  for (int i = 0; i < eventCount; i++) {
    if (events[i].day > 6 ||
        events[i].hour > 23 ||
        events[i].minute > 59 ||
        events[i].durationMs < 100 ||
        events[i].durationMs > 30000) {
      eventCount = 0;
      scheduleEnabled = false;
      break;
    }
  }
}

void startBell(uint32_t durationMs) {
  bellActive = true;
  digitalWrite(LED_PIN, HIGH);
  bellEnd = millis() + durationMs;
}

void updateBell() {
  if (bellActive && (int32_t)(millis() - bellEnd) >= 0) {
    digitalWrite(LED_PIN, LOW);
    bellActive = false;
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

  if (!scheduleEnabled) return;

  for (int i = 0; i < eventCount; i++) {
    BellEvent &e = events[i];

    if (e.day == now.dayOfTheWeek() &&
        e.hour == now.hour() &&
        e.minute == now.minute()) {

      startBell(e.durationMs);
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

  if (parts < 5 || year < 2020 || year > 2099) {
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
  redirectHome();
}

void handleAdd() {
  if (eventCount >= MAX_EVENTS) {
    server.send(400, "text/plain", "Schedule is full");
    return;
  }

  if (!server.hasArg("day") ||
      !server.hasArg("at")) {
    server.send(400, "text/plain", "Missing event fields");
    return;
  }

  int day = server.arg("day").toInt();

  int hour = -1;
  int minute = -1;

  String timeValue = server.arg("at");

  if (sscanf(
        timeValue.c_str(),
        "%d:%d",
        &hour, &minute
      ) != 2) {
    server.send(400, "text/plain", "Invalid time");
    return;
  }

  uint32_t duration = durationFromRequest();

  if (day < 0 || day > 6 ||
      hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 ||
      duration == 0) {
    server.send(400, "text/plain", "Invalid event");
    return;
  }

  for (int i = 0; i < eventCount; i++) {
    if (events[i].day == day &&
        events[i].hour == hour &&
        events[i].minute == minute) {
      server.send(
        400,
        "text/plain",
        "An event already exists at this time"
      );
      return;
    }
  }

  events[eventCount] = {
    (uint8_t)day,
    (uint8_t)hour,
    (uint8_t)minute,
    (uint16_t)duration
  };

  eventCount++;
  saveSettings();
  redirectHome();
}

void handleDelete() {
  if (!server.hasArg("id")) {
    server.send(400, "text/plain", "Missing ID");
    return;
  }

  int id = server.arg("id").toInt();

  if (id < 0 || id >= eventCount) {
    server.send(400, "text/plain", "Invalid ID");
    return;
  }

  for (int i = id; i < eventCount - 1; i++) {
    events[i] = events[i + 1];
  }

  eventCount--;
  saveSettings();
  redirectHome();
}

void handleToggle() {
  scheduleEnabled = !scheduleEnabled;
  saveSettings();
  redirectHome();
}

void handleTest() {
  uint32_t duration = durationFromRequest();

  if (duration == 0) {
    server.send(400, "text/plain", "Invalid duration");
    return;
  }

  startBell(duration);
  redirectHome();
}

void handleHome() {
  String page = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport"
      content="width=device-width, initial-scale=1">

<title>KALS Bell Controller</title>

<style>
* { box-sizing: border-box; }

body {
  font-family: Arial, sans-serif;
  background: #f3f5f9;
  color: #172033;
  margin: 0;
  padding: 20px;
}

.container {
  max-width: 800px;
  margin: auto;
}

.card {
  background: white;
  border-radius: 14px;
  padding: 22px;
  margin-bottom: 18px;
  box-shadow: 0 2px 12px #00000010;
}

h1 { font-size: 26px; }
h2 { font-size: 19px; margin-top: 0; }

.clock {
  font-size: clamp(23px, 5vw, 36px);
  font-weight: bold;
  text-align: center;
  padding: 20px 0;
}

input, select, button {
  padding: 11px;
  margin: 5px 0;
  font-size: 15px;
  border-radius: 7px;
}

input, select {
  border: 1px solid #ccd2dc;
  max-width: 100%;
}

button {
  background: #2563eb;
  color: white;
  border: none;
  cursor: pointer;
}

button.danger { background: #dc2626; }
button.secondary { background: #475569; }

.row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 8px;
  margin-bottom: 8px;
}

.event {
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-wrap: wrap;
  gap: 10px;
  border-top: 1px solid #e5e7eb;
  padding: 8px 0;
}

.event form { margin: 0; }

.small {
  color: #64748b;
  font-size: 13px;
}

.status {
  color: #16a34a;
  font-weight: bold;
}
</style>
</head>

<body>
<div class="container">

<div class="card">
  <h1>KALS Bell Controller</h1>
  <p class="status">ESP32 Online - Local Wi-Fi</p>
  <div class="clock" id="clock">Loading...</div>
  <p class="small">Time is read from the DS3231 RTC.</p>
</div>

<div class="card">
  <h2>Set Clock</h2>

  <button type="button" onclick="syncPC()">
    Sync with Laptop Time
  </button>

  <p>Or manually select the date and time:</p>

  <form action="/settime" method="POST">
    <div class="row">
      <input type="datetime-local"
             name="dt"
             step="1"
             required>
      <button type="submit">Set RTC</button>
    </div>
  </form>
</div>

<div class="card">
  <h2>Test Bell</h2>
  <p class="small">
    Light the onboard LED for the selected duration.
  </p>

  <form action="/test" method="POST">
    <div class="row">
      <label>
        Duration (seconds)
        <input type="number"
               name="duration"
               value="6"
               min="0.1"
               max="30"
               step="0.1"
               required>
      </label>
      <button type="submit">Test Bell</button>
    </div>
  </form>
</div>

<div class="card">
  <h2>Weekly Schedule</h2>

  <form action="/toggle" method="POST">
)HTML";

  page += "<p>Automatic scheduling: <b>";
  page += scheduleEnabled ? "ENABLED" : "PAUSED";
  page += "</b></p>";

  page += "<button type='submit' class='secondary'>";
  page += scheduleEnabled ? "Pause Schedule" : "Enable Schedule";
  page += "</button></form>";

  page += R"HTML(
  <h3>Add Bell Event</h3>

  <form action="/add" method="POST">
    <div class="row">

      <select name="day" required>
        <option value="1">Monday</option>
        <option value="2">Tuesday</option>
        <option value="3">Wednesday</option>
        <option value="4">Thursday</option>
        <option value="5">Friday</option>
        <option value="6">Saturday</option>
        <option value="0">Sunday</option>
      </select>

      <input type="time"
             name="at"
             required>

      <label>
        Duration (seconds)
        <input type="number"
               name="duration"
               min="0.1"
               max="30"
               step="0.1"
               value="6"
               required>
      </label>

      <button type="submit">Add Event</button>
    </div>
  </form>

  <h3>Saved Events</h3>
)HTML";

  if (eventCount == 0) {
    page += "<p class='small'>No events yet.</p>";
  }

  for (int i = 0; i < eventCount; i++) {
    BellEvent &e = events[i];

    page += "<div class='event'><span><b>";
    page += DAY_NAMES[e.day];
    page += "</b> &nbsp;";
    page += pad2(e.hour);
    page += ":";
    page += pad2(e.minute);
    page += " &nbsp;(";
    page += String(e.durationMs / 1000.0f, 1);
    page += " seconds)</span>";

    page += "<form method='POST' action='/delete'>";
    page += "<input type='hidden' name='id' value='";
    page += String(i);
    page += "'>";
    page += "<button class='danger' type='submit'>";
    page += "Delete</button></form></div>";
  }

  page += R"HTML(
</div>

<p class="small" style="text-align:center">
  KALS Automated Bell System - LED Prototype
</p>

</div>

<script>
async function refreshClock() {
  try {
    const r = await fetch("/time", {
      cache: "no-store"
    });

    if (!r.ok) throw new Error("RTC unavailable");

    document.getElementById("clock").textContent =
      await r.text();

  } catch (e) {
    document.getElementById("clock").textContent =
      "Connection lost";
  }
}

function syncPC() {
  const d = new Date();
  const p = n => String(n).padStart(2, "0");

  const dt =
    d.getFullYear() + "-" +
    p(d.getMonth() + 1) + "-" +
    p(d.getDate()) + "T" +
    p(d.getHours()) + ":" +
    p(d.getMinutes()) + ":" +
    p(d.getSeconds());

  fetch("/settime", {
    method: "POST",
    headers: {
      "Content-Type":
        "application/x-www-form-urlencoded"
    },
    body: new URLSearchParams({dt})
  })
  .then(r => {
    if (!r.ok) throw new Error("Clock update failed");
    location.reload();
  })
  .catch(e => alert(e.message));
}

refreshClock();
setInterval(refreshClock, 1000);
</script>

</body>
</html>
)HTML";

  server.send(
    200,
    "text/html; charset=utf-8",
    page
  );
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

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
