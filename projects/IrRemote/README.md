# IrRemote

CardputerZero 上的三板块红外遥控应用。

## 功能

- 空调控制：第一页，集成真实空调协议，当前支持 `Midea`、`Gree`、`Haier YR-W02`、`Coolix Intl`，可切换品牌并发送电源、温度、模式、风量、扫风与当前状态
- 复制遥控：第二页，录制原始红外波形、自动本地保存、列表选择后完整回放
- NEC 工具：第三页，查看已录制信号是否能被解析为 `NEC/NECX/NEC32`，并可按 NEC 方式单独发射
- 本地存储：已录制信号保存在 `signals.db`，界面状态保存在 `state.ini`
- 核心红外逻辑来自 `reference/` 中新增的接收、发送与 NEC 编解码能力

## 按键

- `4`：切换到上一功能板块
- `6`：切换到下一功能板块
- `5`：选择上一项
- `7`：选择下一项
- `8`：修改当前项参数
- `Enter`：发射当前命令，或执行录制/删除/回放动作

## 构建

```bash
cd projects/IrRemote
scons -j$(nproc)
```

## 运行

```bash
cd projects/IrRemote
./dist/M5CardputerZero-IrRemote
```

## 红外后端

当前实现直接基于 LIRC 设备节点进行原始红外收发，并附带 NEC 解析能力。

- 发送/接收设备会从 `/sys/class/rc` 和 `/dev/lirc*` 自动探测
- 可通过 `IRREMOTE_LIRC_SEND_DEVICE=/dev/lircX` 或 `IRREMOTE_LIRC_RECV_DEVICE=/dev/lircY` 强制指定设备
- 运行日志默认写到 `~/.config/m5cardputerzero/irremote/runtime.log`
- 已录制信号保存到 `~/.config/m5cardputerzero/irremote/signals.db`
- 页面状态保存到 `~/.config/m5cardputerzero/irremote/state.ini`

注意：

- 当前“复制遥控”板块会优先保存原始脉冲序列，不要求先解码成 NEC
- 如果某条已录制信号恰好能解析为 `NEC/NECX/NEC32`，就会在 `NEC` 板块里显示解析结果
- 空调页不再使用占位 NEC 码表，而是通过 `IRremoteESP8266` 协议子集生成真实空调原始波形，再交给本工程的 LIRC 发送后端发射
- vendored 的 `IRremoteESP8266` 子集代码位于 `main/third_party/irremoteesp8266/`，保留其 `LGPL-2.1` 许可证文本
