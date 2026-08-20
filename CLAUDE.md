# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Single-sketch Arduino project (`Gauge-clock/Gauge-clock.ino`) for an ESP32-based clock that displays
values (e.g. temperature/pressure/time) on analog voltmeter gauges. It drives a 4-channel DAC
(MCP4728) to position voltmeter needles and reads ambient conditions from an AHT20 (temperature/
humidity) and a BMP280 (temperature/pressure) sensor, all over I2C. A DS3231 RTC provides the
time-of-day: `get_time_from_rtc()` reads the RTC at startup and seeds the ESP32's system clock (via
`settimeofday()`) that `loop()` reads from thereafter.

The sketch also drives 10 WS2812 addressable LEDs on pin `IO20` (`LED_PIN`) via `Adafruit_NeoPixel`,
with `leds_off()`/`set_backlight_led()` helpers to switch them off or on at `led_brightness` (0-255)
in `led_color` (a `GPcolor`, default orange), both persisted to EEPROM and settable via the web UI.
`flicker_backlight_led()`, called every `loop()` iteration, emulates a vintage lamp with a loose
contact by briefly dimming one random backlight LED (index 4-9) to `led_color/6` before restoring it,
waiting a random interval before the next flicker. Both the flicker duration and the wait between
flickers are configurable ranges (`led_flicker_min_duration`/`led_flicker_max_duration`, default
50-250ms, and `led_flicker_min_interval`/`led_flicker_max_interval`, default 2000-8000ms), settable
via the web UI's LEDs tab and persisted to EEPROM alongside the other LED settings. It never touches
the gauge_3-mode indicator LEDs (0-3, see below), since those carry state rather than being purely
decorative.

The sketch also connects to WiFi (via `WiFiManager`, falling back to an on-device "GaugeClock"
config AP if no saved credentials work) and, when connected, periodically syncs time from an NTP
server (`getNtpTime()`, adapted from the Arduino `TimeNTP` example) and writes it back to the
DS3231 so the RTC stays correct across power loss. A `GyverPortal` web UI (`build()`/`action()`)
lets you set the timezone offset, toggle NTP/RTC use, change the NTP server, and set the time by
hand; settings are persisted to EEPROM (`read_eeprom_data()`/`write_eeprom_data()`).

The lowest gauge (`gauge_3`) can show seconds, temperature, humidity, barometric pressure, or cycle
through those at random; which one is chosen via a `gauge_3_mode` select in the web UI (`Gauge3Mode`
enum), persisted to EEPROM, and applied with `set_gauge3_range()` (sets the gauge's value range:
15-35°C, 0-100% RH, or 735-780 mmHg) alongside `loop()`'s per-mode `disp.set_value(gauge_3, ...)`
call. Temperature/humidity readings come from the AHT20 and pressure from the BMP280 (converted from
Pa to mmHg), cached in `sensor_temperature`/`sensor_humidity`/`sensor_pressure` by `read_sensors()`.
In `GAUGE3_RANDOM` mode, `gauge3_random_pick_and_apply()` (called from `loop()` once the scheduled
`gauge_3_random_switch_at` `millis()` deadline passes) picks a different one of the other four modes
into `gauge_3_random_submode` and reschedules itself 10-30s out; `gauge3_effective_mode()` resolves
what gauge_3 should actually show at any moment (the random submode, or `gauge_3_mode` itself when
not random), and both `set_gauge3_range()` and `leds_indicate_gauge3_mode()` key off it.
`leds_indicate_gauge3_mode()` lights one of the first 4 WS2812 LEDs white (at `led_brightness`) to
show which value gauge_3 is on: LED 0 = seconds, LED 1 = humidity, LED 2 = pressure, LED 3 =
temperature, clearing the other three each call; it runs after `set_backlight_led()` (in `setup()` and
after a web UI settings update, via `gauge3_apply_mode()`) since that call clears the whole strip.

The target is the ESP32 Arduino core specifically (not vanilla AVR Arduino): note the two-argument
`Wire.begin(SDA_PIN, SCL_PIN)` call and use of `log_printf`, both ESP32-core-specific APIs. I2C pins
are fixed in code: `SDA_PIN = 8`, `SCL_PIN = 9`.

`Gauge-clock/ci.yml` declares a Kconfig-style build requirement (`CONFIG_SOC_I2C_SUPPORTED=y`) used by
Espressif's arduino-esp32 example CI to restrict compilation to chip targets that support I2C. Keep this
in sync if hardware peripheral requirements change.

## Dependencies

Arduino libraries (install via Arduino IDE Library Manager or `arduino-cli lib install`):
- `Adafruit_MCP4728`
- `Adafruit_BMP280`
- `Adafruit_AHTX0` (pulls in `Adafruit_Sensor` / `Adafruit_BusIO`)
- `DS3231` (NorthernWidget/Andrew Wickert library, header `DS3231.h`)
- `Adafruit_NeoPixel` — drives the WS2812 status LEDs
- `WiFiManager` (tzapu) — captive-portal WiFi provisioning
- `GyverPortal` — web-based configuration UI
- `TimeLib` — only used for the `SECS_PER_HOUR` constant in NTP timezone math
- `EEPROM` and `WiFiUdp` ship with the ESP32 Arduino core, no separate install needed

## Build

There is no build script or test suite in this repo; it's compiled as a standard Arduino sketch
against an ESP32 board target, e.g.:

```
arduino-cli compile --fqbn esp32:esp32:esp32 Gauge-clock
arduino-cli upload -p <PORT> --fqbn esp32:esp32:esp32 Gauge-clock
```

## Code structure

Everything lives in `Gauge-clock.ino`:
- `GaugeDisplay` wraps the MCP4728 DAC. `set_range()` defines the input value's low/high bounds per
  channel; `set_value()` maps an arbitrary float onto a DAC count (0-4095) for that channel and writes it.
- `set_value()` has a needle-braking behavior: when a channel's value drops sharply from >3000 to <10, it
  first snaps to the low value, delays, then briefly overdrives the channel to ~4000 before setting the
  real value — this counteracts mechanical momentum/overshoot in the analog gauge's needle. Preserve this
  behavior when touching `set_value()`.
- `setup()` brings up I2C, the DAC, and both sensors, halting in an infinite `delay` loop if any device
  fails `begin()`; it also loads settings from EEPROM, syncs time from the DS3231, connects to WiFi via
  `initialize_network()`, and starts the `GyverPortal` web UI.
- `loop()` reads the system clock (seeded from the DS3231/NTP) each iteration to drive the hour/minute/
  second gauges, periodically resyncs from NTP (`NTP_UPDATE_INTERVAL` seconds), ticks the web portal
  (`ui.tick()`), and every ~100 iterations reads and logs sensor values (`delay(100)` per iteration, so
  roughly every ~10s).
- `getNtpTime()`/`sendNTPpacket()` implement the NTP client (adapted from the Arduino `TimeNTP` sample);
  a successful sync updates both the DS3231 and the system clock.
- `build()`/`action()` define and handle the `GyverPortal` config forms: timezone shift, NTP/RTC use
  toggles, NTP server name, and manual time entry, all served from `setup()`'s `ui.start()`.
