# daemonPorts (Gatekeeper)

多端口启动引导门卫:接管端口、按需拉起后端、提供带鉴权的控制面接口。本上下文覆盖 gatekeeper 的监控与应急控制领域。

## Language

**System monitor (系统资源监控)**:
主机级 CPU 与内存(物理 + 虚拟/swap)使用率采样;由顶层 `system_monitor` 配置段启用,输出周期日志与控制接口。
_Avoid_: monitor(单独使用)、resource monitor

**Connection monitor (连接监控)**:
每端口独立的 TCP 连接活跃度采样,按该端口配置的间隔统计非监听连接数并记录日志。
_Avoid_: monitor(单独使用)、TCP monitor

**Fast interval (快周期)**:
物理内存使用率超过高阈值时启用的缩短采样周期(默认 60 秒);常规周期默认 300 秒。

**Emergency state (应急态)**:
swap 使用率超过阈值时进入的状态;期间应急命令名单内的 ad-hoc 控制命令跳过 PIN 校验。
_Avoid_: override、bypass mode

**Eviction (驱逐)**:
物理与虚拟内存双双持续超过临界阈值(默认 15 分钟)后,gatekeeper 自动关闭"无数据流量最久"的运行中子配置项;被关闭条目本次运行内粘性禁用(重启或 /reload 才重新武装)。
_Avoid_: OOM kill、杀最重者