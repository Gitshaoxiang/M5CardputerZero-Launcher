# CapGPS

参考 `ExtBusLED` 工程结构创建的 `CapGPS` 项目骨架。

当前提供：

- 与 `ExtBusLED` 一致的目录布局
- 独立的 `SConstruct`、`package_deb.sh`、desktop 文件
- 一个基于 `TinyGPS++` 的 GNSS 搜星与坐标定位 Demo
- 串口固定为 `/dev/ttyS0`，默认波特率 `115200`

## 功能

- 持续从 `/dev/ttyS0` 读取 NMEA 数据
- 使用仓库内置的 `TinyGPS++` 解析器计算：
  - 经纬度
  - 已用于定位的卫星数
  - 可见卫星数（GSV 统计）
  - 海拔、速度、航向、HDOP
  - UTC 日期与时间
- 界面实时显示：
  - 搜星/锁定状态
  - 搜星加载动画
  - 简洁的卫星计数
  - 经纬度定位结果
  - 底部滚动诊断日志

## 目录

```text
CapGPS/
├── SConstruct
├── config_defaults.mk
├── package_deb.sh
├── assets/applications/capgps.desktop
└── main
    ├── Kconfig
    ├── SConstruct
    ├── src/main.cpp
    └── ui/ui.cpp
```

## 构建

### PC 本机调试（默认，SDL/x86_64）

```bash
cd projects/CapGPS
scons -j$(nproc)
```

### 交叉编译到 CardputerZero（AArch64）

```bash
cd projects/CapGPS
export cardputer=y
scons distclean
scons -j$(nproc)
```

## 打包

```bash
chmod +x package_deb.sh
./package_deb.sh
```

默认输出：

```text
dist/CapGPS_0.1.0_m5stack1_arm64.deb
dist/capgps_0.1.0_m5stack1_arm64.deb
```

## 可配置环境变量

- `CAPGPS_BAUD`：串口波特率，默认 `115200`
