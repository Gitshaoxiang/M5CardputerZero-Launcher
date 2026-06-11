# ExtBusLED

基于 `GroveI2C` 项目结构的新建 LED/按键测试示例，适配扩展接口 ExtBus。

## 功能

- 按你的要求执行 EXT-007 切换动作：
  - `gpioset -c gpiochip0 17=1`
  - `gpioset -c gpiochip1 0=1`
- 默认 GPIO 映射：
  - `LED1 = G22`
  - `LED2 = G23`
  - `BUTTON = G26`
- 屏幕分两个区域显示：
  - `OUTPUT`：每路输出的 GPIO 与当前 `HIGH/LOW`
  - `INPUT`：输入 GPIO、`raw`、`HIGH/LOW`、按下状态
- `HIGH` 用绿色显示；每路状态加了圆形指示器。
- `LED` 不再需要手动按钮控制：启动后每 1 秒自动切换一次（`LED1`、`LED2` 都会按当前状态翻转）。
- 输入端口仅做状态读取展示，不再参与 LED 翻转控制。
- 输入状态保留消抖处理（3 次采样），用于 `raw` 与 `Press` 标识更稳。

## 切换与显示说明

- 自动切换流程：
  1. 每 1000ms 触发定时回调
  2. 读取当前 `g_led1_state/g_led2_state`
  3. 各自写入 `!current`
  4. 刷新 `HIGH/LOW` 文本与圆形状态灯（绿色/灰色）

## 构建

### PC 本机调试（默认，SDL/x86_64）

```bash
cd projects/ExtBusLED
scons -j$(nproc)
```

默认会生成可在当前 PC 上运行的 SDL 版本。

### 交叉编译到 CardputerZero（AArch64）

```bash
cd projects/ExtBusLED
export cardputer=y
scons distclean
scons -j$(nproc)
```

也兼容 `export CardputerZero=y`。

如果之前已经在 PC 上编过一次，务必先执行一次 `scons distclean`，否则旧的 SDL/x86 配置可能会被沿用。

## 打包为 APPLaunch 应用

```bash
chmod +x package_deb.sh
./package_deb.sh
```

## 可配置环境变量

- `EXT_POWER_GPIOCHIP`（默认 `gpiochip0`）
- `EXT_POWER_GPIO`（默认 `17`）
- `EXT_ROUTE_GPIOCHIP`（默认 `gpiochip1`）
- `EXT_ROUTE_GPIO`（默认 `0`）
- `EXT_LED1_GPIOCHIP`（默认 `gpiochip0`）
- `EXT_LED1_GPIO`（默认 `22`）
- `EXT_LED2_GPIOCHIP`（默认 `gpiochip0`）
- `EXT_LED2_GPIO`（默认 `23`）
- `EXT_BUTTON_GPIOCHIP`（默认 `gpiochip0`）
- `EXT_BUTTON_GPIO`（默认 `26`）
- `EXT_BUTTON_ACTIVE_LOW`（默认 `1`，`1` 为低电平按下）
