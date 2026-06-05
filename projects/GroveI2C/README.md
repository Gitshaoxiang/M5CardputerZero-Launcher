# GroveI2C

Grove I2C scanner demo for CardputerZero.

## Functions

- Switch GROVE mux to I2C via gpiochip0
- Run i2cdetect on bus 1
- Show scan results in LVGL text area

## Build

cd projects/GroveI2C
scons -j$(nproc)

## Package As APPLaunch App (.deb)

Build app binary first:

cd projects/GroveI2C
scons -j$(nproc)

Generate Debian package:

chmod +x package_deb.sh
./package_deb.sh

Output package path example:

dist/grovei2c_0.1.0_m5stack1_arm64.deb

## Install On Device

Copy package to device and install:

scp dist/grovei2c_0.1.0_m5stack1_arm64.deb pi@<device-ip>:/tmp/
ssh pi@<device-ip> "sudo dpkg -i /tmp/grovei2c_0.1.0_m5stack1_arm64.deb && sudo systemctl restart APPLaunch.service"

After service restart, APPLaunch will show:

Grove I2C Scanner
