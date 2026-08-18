# LilyGo T5 4.7" ePaper (ESP32-S3, non-touch) — component test

A PlatformIO firmware that exercises most of the onboard hardware as a
mock "alarm clock":

- **PSRAM** — required for the display framebuffer, checked at boot.
- **4.7" e-paper panel** (960x540) — full-screen draws, text rendering, a
  clock face, and an alarm banner.
- **RTC (PCF8563)** over I2C — keeps time, seeds itself from the compiler's
  build timestamp the first time it powers on with a dead backup battery,
  and drives a real hardware alarm.
- **microSD slot** — probed at boot; reports capacity if a card is present,
  otherwise reports "not detected" and continues (a card is optional).
- **Battery ADC** — reads and prints an approximate battery voltage.
- **WiFi radio** — prints its MAC address as a basic smoke test. No
  network credentials required (it never joins an AP).
- **BUTTON_1 / GPIO21** — wired up but **optional**. You mentioned your
  board has no button fitted, so the demo never depends on it.

Touch is intentionally not used — this is written for the **non-touch**
panel variant (no GT911 controller).

## How the "alarm clock" behaves with no button

1. On boot it shows a diagnostics screen (pass/fail per component) for a
   few seconds.
2. It switches to a clock face and arms a real PCF8563 hardware alarm for
   ~2 minutes later.
3. When the alarm fires, the screen flips to a black "ALARM !" banner.
4. After ~20 seconds it auto-dismisses, clears the RTC alarm flag, and
   re-arms the next test alarm — so it loops indefinitely without any
   input.

If you ever solder a button between GPIO21 and GND, pressing it will
dismiss an active alarm immediately, or trigger a manual test alarm on
demand — but it's not required for anything above.

E-paper panels aren't meant to be refreshed every second, so the clock
face only redraws once a minute (plus on alarm transitions). Watch the
serial monitor at 115200 baud if you want per-second proof the RTC is
ticking.

## Build & flash

Requires [PlatformIO](https://platformio.org/) (VS Code extension or CLI).

```sh
pio run -t upload
pio device monitor
```

The `boards/T5-ePaper-S3.json` file and `platformio.ini` are already set
up for 16MB flash / OPI PSRAM / USB-CDC, matching LilyGo's own board
definition. The e-paper driver, fonts, and pin map come straight from
[Xinyuan-LilyGO/LilyGo-EPD47](https://github.com/Xinyuan-LilyGO/LilyGo-EPD47)
(`esp32s3` branch); RTC support comes from
[lewisxhe/SensorLib](https://github.com/lewisxhe/SensorLib); the optional
button uses [LennartHennigs/Button2](https://github.com/LennartHennigs/Button2).

## Things you may want to tweak

- `ALARM_TEST_MINUTES` / `ALARM_DISPLAY_MS` / `DIAG_HOLD_MS` in
  `src/main.cpp` — timing of the demo.
- Battery voltage assumes a 2:1 resistor divider ahead of `BATT_PIN`
  (LilyGo's usual arrangement). Check against a multimeter once if you
  need accurate readings — the code prints the raw ADC-derived value
  before doubling it, in a comment next to `readBatteryVoltage()`.
- No WiFi/NTP time sync is included on purpose (so it builds and runs
  with zero configuration). If you want real-world time instead of the
  compiler's build timestamp, add a `WiFi.begin(ssid, pass)` +
  `configTzTime()` + `rtc.setDateTime()` block in `setup()` — the
  upstream `examples/demo` and `examples/wifi_sync` sketches in the
  LilyGo-EPD47 repo show the pattern.
