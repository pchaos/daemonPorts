# DaemonPorts Gatekeeper Skill

> Project: /home/user/myDocs/YUNIO/tmp/gupiao/daemonPorts
> 场景：多端口启动引导门卫（simple/mixed/proxy 模式）

## 项目结构

```
daemonPorts/
├── src/
│   ├── main.cpp        # 程序入口，统一监控线程
│   ├── config.h/cpp     # 配置解析（JSON）
│   ├── relay.h/cpp      # PortRelay 类，端口接力核心
│   ├── port_group.h/cpp # PortGroup 类，多端口分组协调
│   ├── tcp_monitor.h/cpp # TCP 连接监控（NETLINK_INET_DIAG）
│   └── json.h/cpp       # 轻量 JSON 解析器
├── test/                # 单元测试（doctest）
├── xmake.lua            # 构建脚本
├── gatekeeper.service   # systemd 服务模板
├── gatekeeper-config.json # 示例配置
```

## 构建

```bash
xmake f -m release && xmake          # release
xmake f -m debug && xmake            # debug（含测试）
xmake build test-gatekeeper          # 测试
```

输出目录：`build/linux/x86_64/release/gatekeeper` 和 `gatekeeper-systemd`。

**两个二进制版本的区别**：
- `gatekeeper` — 标准版，无 systemd sd_notify
- `gatekeeper-systemd` — 编译时带 `-DHAVE_SYSTEMD`，启用 `sd_notify`（`Type=notify` 服务必需）

> 注意：xmake 默认构建不启用 systemd。如需 systemd 版，在 `xmake.lua` 中设置 `defines("HAVE_SYSTEMD")`。

## systemd 部署

### 服务文件

`/etc/systemd/system/gatekeeper.service`：

```ini
[Unit]
Description=DaemonPorts Gatekeeper
After=network.target

[Service]
Type=notify                          # 必须用 notify（需 systemd 版二进制）
User=user
WorkingDirectory=/home/user/myDocs/YUNIO/tmp/gupiao/daemonPorts
ExecStart=/usr/local/sbin/gatekeeper-wrapper.sh gatekeeper-config.json
Restart=on-failure
RestartSec=5s
TimeoutStopSec=10                     # 防止 stop 超时卡死

[Install]
WantedBy=multi-user.target
```

### Wrapper 脚本

`/usr/local/sbin/gatekeeper-wrapper.sh`：

```bash
#!/bin/bash
exec /usr/local/bin/gatekeeper-systemd "$@"
```

### 部署步骤

1. 复制二进制：`sudo cp build/linux/x86_64/release/gatekeeper-systemd /usr/local/bin/gatekeeper-systemd`
2. 复制配置：`sudo cp gatekeeper-config.json /usr/local/etc/gatekeeper/config.json`
3. 重启服务：`sudo systemctl restart gatekeeper`

**⚠ 替换二进制时必须先停服务**（`sudo systemctl stop gatekeeper`），否则 `Text file busy`。如果 stop 超时卡死，用 `sudo pkill -9 -f gatekeeper` 强杀。

## 配置文件

### 配置路径

运行时配置文件：`/usr/local/etc/gatekeeper/config.json`

### 格式

```json
{
  "ports": [
    {
      "name": "9router",
      "enabled": true,
      "listen": ":20128",
      "command": "node /path/to/cli.js start --host 0.0.0.0 -p 20128 --skip-update --no-browser",
      "stop_command": "node /path/to/cli.js stop",
      "idle_minutes": 20,
      "monitor": {
        "enabled": true,
        "interval_seconds": 60,
        "log_dedup": "off"
      },
      "delay": 30000,
      "refresh_seconds": 15,
      "auto_restart": true
    }
  ]
}
```

### 关键字段

| 字段 | 说明 |
|------|------|
| `name` | 日志标识，默认用 listen 地址 |
| `listen` | 门卫监听的端口 |
| `command` | 后端启动命令 |
| `stop_command` | 优雅关闭命令 |
| `idle_minutes` | 空闲超时（分钟） |
| `monitor.enabled` | 是否启用 TCP 连接监控 |
| `monitor.interval_seconds` | 监控轮询间隔（秒） |
| `monitor.log_dedup` | 日志去重：`"skip"`（默认，不变跳过）/ `"throttle"`（每5轮一次）/ `"off"`（始终打印） |

## 常见问题

### 1. 服务启动超时 / 卡住

原因：`ExecStart` 用的是非 systemd 版二进制（`gatekeeper`），缺少 `sd_notify()`，`Type=notify` 等不到 READY=1 信号。

修复：确认 `/usr/local/bin/gatekeeper-systemd` 是 systemd 编译版。验证方法：启动日志应包含 `systemd: READY=1`。

### 2. stop 超时

原因：子进程（如 node/next-server）不在 systemd cgroup 内，`SIGTERM` 只杀主进程，子进程还在跑，systemd 等子进程退出超时。

修复：服务文件中设置 `TimeoutStopSec=10`，或用 `pkill -9 -f gatekeeper` 强杀。

### 3. 日志去重导致流量监控消失

原因：默认 `log_dedup: "skip"`，连接数不变时不打印日志。

修复：`monitor.log_dedup` 设为 `"off"`（始终打印）或 `"throttle"`（降频）。

### 4. 配置修改不生效

原因：运行时配置文件是 `/usr/local/etc/gatekeeper/config.json`，不是仓库里的 `gatekeeper-config.json`。

修复：改 `/usr/local/etc/gatekeeper/config.json` 后 `systemctl restart gatekeeper`。

## 调试

```bash
# 实时日志
sudo journalctl -u gatekeeper -f

# 最近 20 条
sudo journalctl -u gatekeeper -n 20 --no-pager

# 状态
sudo systemctl status gatekeeper
```
