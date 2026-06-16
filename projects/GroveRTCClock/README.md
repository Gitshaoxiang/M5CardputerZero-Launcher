# GroveRTCClock

Digital RTC-style clock demo for CardputerZero.

## Current Scope

- LVGL digital clock UI tuned for `320x170`
- Uses system local time as the display source
- Refreshes once per second with a lightweight animated header
- No RTC or I2C communication yet

## Build

```bash
cd projects/GroveRTCClock
scons -j$(nproc)
```

## Run

```bash
cd projects/GroveRTCClock
./dist/M5CardputerZero-GroveRTCClock
```

## Package As APPLaunch App

```bash
cd projects/GroveRTCClock
chmod +x package_deb.sh
./package_deb.sh
```
