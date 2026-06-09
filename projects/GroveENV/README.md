# GroveENV

Grove ENV temperature and humidity monitor for CardputerZero.

## Functions

- Switch GROVE mux to I2C via gpiochip0
- Read SHT30 at I2C address 0x44
- Show temperature, humidity, and live LVGL trend curves

## Build

cd projects/GroveENV
CardputerZero=y scons -j$(nproc)

## Package As APPLaunch App (.deb)

Build app binary first:

cd projects/GroveENV
CardputerZero=y scons -j$(nproc)

Generate Debian package:

chmod +x package_deb.sh
./package_deb.sh

Output package path example:

dist/groveenv_0.1.0_m5stack1_arm64.deb

## Install On Device

Copy package to device and install:

scp dist/groveenv_0.1.0_m5stack1_arm64.deb pi@<device-ip>:/tmp/
ssh pi@<device-ip> "sudo dpkg -i /tmp/groveenv_0.1.0_m5stack1_arm64.deb && sudo systemctl restart APPLaunch.service"

After service restart, APPLaunch will show:

Grove ENV Monitor
