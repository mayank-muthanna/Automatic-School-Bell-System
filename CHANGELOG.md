# Changelog

## Unreleased

- Added a GPIO 18 output for a MOSFET-driven solenoid. Scheduled rings and Test Bell can each request one pulse of up to 150 ms, with a three-second cooldown between pulse starts.

## [0.1.0] — 2026-11-22

First bench prototype for the ESP32 DevKit V1 and DS3231 RTC.

- Added standalone ESP32 LED and DS3231 clock checks.
- Added a local Wi-Fi dashboard for setting the clock, testing the bell, and managing a persistent weekly timetable.
- Added synchronized MAYANK Morse output for the active-low buzzer and onboard LED.
- Documented wiring, first-run checks, and troubleshooting. Added automated compilation for all three sketches.

The prototype signals through the onboard LED and a bench buzzer. A solenoid that presses the school's existing manual switch is a future hardware milestone.

[0.1.0]: https://github.com/mayank-muthanna/Automatic-School-Bell-System/releases/tag/v0.1.0
