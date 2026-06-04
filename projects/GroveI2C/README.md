# GroveI2C

Grove I2C scanner demo for CardputerZero.

## Functions

- Switch GROVE mux to I2C via gpiochip0
- Run i2cdetect on bus 1
- Show scan results in LVGL text area

## Build

cd projects/GroveI2C
scons -j$(nproc)
