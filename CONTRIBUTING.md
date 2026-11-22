# Contributing

## Submit a change

1. Create a branch from `develop` for one firmware, hardware, or documentation change.
2. Keep the change in one focused commit and open a pull request against `develop`.
3. Describe what changed and list the checks you ran. For hardware checks, name the board and module, note the wiring, and record the observed result.
4. Squash-merge feature pull requests into `develop`. Promote tested snapshots to `main` through a separate pull request.

The [compile workflow](.github/workflows/compile.yml) builds the LED example, RTC example, and controller for **ESP32 Dev Module** with Arduino ESP32 core **3.0.7** and RTClib **2.1.4**. Follow the [README](README.md) for library setup, wiring, and manual checks. Keep your local `bell_secrets.h` out of commits and issue reports.

## Check firmware changes on the bench

- **Board and clock** — Upload the LED and RTC examples. Check the blink pattern and verify that the RTC retains time after disconnecting USB.
- **Dashboard and schedule** — Connect to the ESP32 access point, set local time, and add a bell a few minutes ahead. Disconnect the laptop and verify that the bell fires at the selected minute. Restart the ESP32 and check that the event and schedule setting persist.
- **Bell output** — Run Test Bell and verify that the buzzer and LED start and stop together.

## Report an issue

Include the commit or release, ESP32 model, affected sketch, and steps to reproduce. For hardware problems, add relevant GPIO connections and serial output.
