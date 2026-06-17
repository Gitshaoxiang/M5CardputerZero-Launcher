# SSHClient

Modern SSH profile manager for CardputerZero.

## Features

- CRUD for saved SSH profiles
- Local profile persistence
- `320x170` UI tuned for CardputerZero
- Hardware keyboard input
- Connection loading animation
- Lightweight in-app SSH session view

## Build

```bash
cd projects/SSHClient
scons -j$(nproc)
```

## Run

```bash
cd projects/SSHClient
./dist/M5CardputerZero-SSHClient
```

## Package

```bash
cd projects/SSHClient
chmod +x package_deb.sh
./package_deb.sh
```
