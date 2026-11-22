# Automatic School Bell System

A school bell controller that runs from an ESP32 and a DS3231 clock. A laptop connects directly to the controller to set the time, test the bell, and edit the weekly timetable. Once configured, the controller runs the schedule with the laptop disconnected.

The bench prototype uses the onboard blue LED and an active-low buzzer. Both spell **MAYANK** in Morse code for the selected ring duration. Driving the school's existing momentary switch with a solenoid is a future hardware milestone.

## Hardware and connections

| Part | Connection on the prototype | Purpose |
| --- | --- | --- |
| ESP32 DevKit V1, 30-pin | USB data cable to laptop | Upload firmware, supply bench power, and view serial output |
| Onboard LED | GPIO 2 | Visual bell output and initial board check |
| DS3231 RTC module | SDA → GPIO 21, SCL → GPIO 22, VCC → 3V3, GND → GND | Keep local time while the ESP32 runs |
| RTC backup cell | Battery holder on the RTC module | Keep time while USB power is disconnected |
| Active-low buzzer module | I/O → GPIO 23, GND → common GND | Sound the same dots and dashes as the LED |

Use a buzzer module compatible with a 3.3 V logic signal and power its supply pin according to its rating. GPIO 23 is the control signal. The RTC backup cell powers the clock only. Check the cell type against the particular RTC board's charging circuit.

## Bring up the board

1. In Arduino IDE, add the [Espressif board index](https://espressif.github.io/arduino-esp32/package_esp32_index.json) under **Preferences → Additional boards manager URLs**. Install **esp32 by Espressif Systems** and select **ESP32 Dev Module** and the USB serial port. The repository build uses core **3.0.7**.
2. Upload [esp32_led.ino](examples/esp32_led/esp32_led.ino). The onboard LED should turn on for half a second and off for half a second. Open Serial Monitor at **115200 baud** and press EN to see the startup message.
3. Disconnect USB, wire the RTC, then reconnect. In Library Manager install **RTClib by Adafruit 2.1.4**, including its Adafruit BusIO dependency. Upload [rtc_clock.ino](examples/rtc_clock/rtc_clock.ino). Serial Monitor should print a timestamp every second. To check the backup cell, disconnect USB for a minute, reconnect, and compare the displayed time with your watch.
4. In [firmware/bell_controller](firmware/bell_controller), copy `bell_secrets.example.h` to `bell_secrets.h`. Set `WIFI_PASSWORD` to a private passphrase of 8–63 characters. Git ignores this local file.
5. Open [bell_controller.ino](firmware/bell_controller/bell_controller.ino) and upload. Connect the laptop to **KALS-BELL** with your chosen passphrase and open **http://192.168.4.1/**. Serial Monitor also prints the controller address. Anyone connected to this network can change the timetable.

## Run a first scheduled bell

1. Select **Sync with Laptop Time** or enter the local date and time manually. Check the displayed date and time against the laptop. If the RTC reports power loss on boot, the sketch initially uses its build timestamp.
2. Select **Test Bell** for six seconds. The blue LED and buzzer should pulse together. A shorter duration can end partway through a letter.
3. Add an event for today's weekday at least two minutes ahead, then select **Enable Schedule**. The clock processes each minute once and skips the minute in which it starts up or the clock is set.
4. Disconnect the laptop and leave the controller powered. At the chosen minute, check the LED and buzzer. Reconnect and restart the ESP32 to check that the event and schedule setting persist.

The timetable stores up to **60 weekly events** in ESP32 flash. Each event has a weekday, a local time to the minute, and a duration from **0.1 to 30 seconds**. A weekday and time can appear only once. A fresh configuration starts with scheduling paused.

## Troubleshooting

| Observation | What to check |
| --- | --- |
| Serial Monitor prints `RTC NOT FOUND!` and Wi-Fi never appears | Check the RTC power, common ground, SDA and SCL. Run the standalone RTC sketch first. |
| Test Bell works but an event is silent | Check the displayed date and time, event weekday, and **ENABLED** status. Try a fresh event two minutes ahead. |
| Clock is wrong after losing USB power | Resync it from the dashboard, then repeat the RTC backup-cell test. |
| Buzzer sounds at idle or only the LED works | Check the module's I/O pin, active-low polarity, supply rating and common ground. |

## Development

The milestones reconstruct the bench build in order. The first two sketches remain available as independent hardware checks. The controller then adds the local clock dashboard, the persistent timetable with LED output, and the Morse buzzer.

Use one focused commit per feature branch and open its pull request against `develop`. Squash-merge the feature PR, then promote a reviewed snapshot from `develop` into `main`. Include the board used and the observed result for hardware changes. The compile workflow builds the two examples and the controller for the ESP32 Dev Module.
