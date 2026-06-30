# RTC

Adjustable digital clock app for CardputerZero.

## Current Scope

- LVGL digital clock UI tuned for `320x170`
- Built for CardputerZero and driven by the Cardputer keyboard input path
- Keys `4 5 6 7 8` support field select, decrement, edit toggle, increment, next
- Starts from system local time and maintains a live editable offset clock

## Build

```bash
cd projects/RTC
CardputerZero=y scons -j$(nproc)
```

## Run

```bash
cd projects/RTC
./dist/M5CardputerZero-RTC
```

## Package As APPLaunch App

```bash
cd projects/RTC
chmod +x package_deb.sh
./package_deb.sh
```
