# GroveRoller485

Grove Roller485 control demo for CardputerZero.

## Functions

- Switch GROVE mux to UART via gpiochip0
- Configure /dev/ttyS0 to 115200 8N1
- Send Unit Roller 485 control frames only
- `Enter` toggles motor run/stop
- `Z` decreases target speed
- `C` increases target speed

## Build

cd projects/GroveRoller485
scons -j$(nproc)
