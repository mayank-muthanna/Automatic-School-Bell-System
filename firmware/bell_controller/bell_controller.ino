#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <RTClib.h>
#include "bell_secrets.h"

const char* WIFI_NAME = "KALS-BELL";
static_assert(sizeof(WIFI_PASSWORD) >= 9 && sizeof(WIFI_PASSWORD) <= 64,
              "Choose an 8 to 63 character Wi-Fi password in bell_secrets.h");

RTC_DS3231 rtc;
WebServer server(80);

void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "");
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


  Serial.println("RTC time updated.");
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
  Wire.begin(21, 22);

  if (!rtc.begin()) {
    Serial.println("RTC NOT FOUND!");
    while (true) delay(1000);
  }

  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  WiFi.mode(WIFI_AP);

  if (!WiFi.softAP(WIFI_NAME, WIFI_PASSWORD)) {
    Serial.println("Wi-Fi failed!");
    while (true) delay(1000);
  }

  server.on("/", HTTP_GET, handleHome);
  server.on("/time", HTTP_GET, handleTime);

  server.on("/settime", HTTP_POST, handleSetTime);
  server.begin();

  Serial.println("KALS Bell Controller Ready!");
  Serial.print("Wi-Fi IP: ");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  server.handleClient();
  delay(2);
}
