# DisplaySSD1306

SSD1306 Grove OLED demo for CardputerZero.

## Functions

- Switch GROVE mux to I2C via the same Grove control pins as GroveENV
- Drive a 128x64 SSD1306 OLED over I2C
- Run a simple content carousel on the external display
- Show current OLED status and slide info on the built-in LVGL screen

## Tuning

If the OLED image looks shifted, it is usually an offset issue rather than the
128x64 resolution itself.

- `DISPLAY_SSD1306_ADDR=0x3C`
- `DISPLAY_SSD1306_WIDTH=128`
- `DISPLAY_SSD1306_HEIGHT=64`
- `DISPLAY_SSD1306_COL_OFFSET=0`
- `DISPLAY_SSD1306_ROW_OFFSET=0`

Common values to try are `COL_OFFSET=2` or `COL_OFFSET=4`. If the picture is
vertically shifted, try small `ROW_OFFSET` values such as `1` or `2`.
If the content looks bigger than the screen, try `DISPLAY_SSD1306_HEIGHT=32`.
If the content still feels too wide, try `DISPLAY_SSD1306_WIDTH=64`.

## Build

cd projects/DisplaySSD1306
CardputerZero=y scons -j$(nproc)

## Package As APPLaunch App (.deb)

Build app binary first:

cd projects/DisplaySSD1306
CardputerZero=y scons -j$(nproc)

Generate Debian package:

chmod +x package_deb.sh
./package_deb.sh

Output package path example:

dist/displayssd1306_0.1.0_m5stack1_arm64.deb

## Install On Device

Copy package to device and install:

scp dist/displayssd1306_0.1.0_m5stack1_arm64.deb pi@<device-ip>:/tmp/
ssh pi@<device-ip> "sudo dpkg -i /tmp/displayssd1306_0.1.0_m5stack1_arm64.deb && sudo systemctl restart APPLaunch.service"

After service restart, APPLaunch will show:

Display SSD1306
