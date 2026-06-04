# GroveUART

Grove UART debug tool demo for CardputerZero.

## Functions

- Switch GROVE mux to UART via gpiochip0
- Configure /dev/ttyS0 to 115200
- Keyboard input and send
- Receive data polling and display in log area

## Build

cd projects/GroveUART
scons -j$(nproc)
