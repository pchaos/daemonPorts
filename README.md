# daemonPorts - 多端口启动引导门卫

[![Build Status](https://github.com/pchaos/daemonPorts/actions/workflows/build.yml/badge.svg)](https://github.com/pchaos/daemonPorts/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platforms](https://img.shields.io/badge/platforms-linux%20%7C%20macOS%20%7C%20windows-lightgrey.svg)](https://github.com/pchaos/daemonPorts/actions/workflows/build.yml)

## 场景

有一个或多个占用内存较多的程序，每个程序监听一个端口提供 TCP 服务。门卫程序先接管这些端口（二进制 ~84KB，运行 5 个端口时 RSS 约 4MB），当有人访问某个端口时，页面显示 **"xxx 启动中"**，自动启动对应的后端程序，然后释放端口让后端程序接管。

如果一个端口需要同时支持多种协议（如 HTTP、SOCKS5），或者想把多个独立后端服务聚合到同一个端口对外暴露，可以使用 **mixed 模式**——gatekeeper 检测连接协议类型，按需启动对应的后端，并通过 TCP 隧道转发流量。

如果需要将 gatekeeper 作为 **SOCKS5 代理服务器**使用，可以使用 **proxy 模式**——gatekeeper 直接监听端口，处理完整的 SOCKS5 握手（支持无认证和 USER/PASS 认证），并将连接转发到指定的 HTTP 后端。

支持两种工作模式：

- **simple 模式（默认）**：引导 → 释放端口，后端接管。适合单端口单后端场景
- **mixed 模式**：协议感知引导，或常驻端口做协议路由。适合 sing-box 等多种协议聚合场景

## 特性

- **极简内存占用**：二进制 ~84KB，每端口线程栈从 8MB 缩减至 512KB（可配置），100 端口 VmSize 仅 ~58MB
- **多平台编译**：Linux（x86_64 / ARM64 / i386 / RISC-V）、macOS（Intel / Apple Silicon）、Windows（MinGW / MSVC）
- **simple 模式**：引导后释放端口，后端直接接管
- **mixed 模式**：协议感知引导 或 常驻代理转发，支持 HTTP / SOCKS5 / SOCKS4
- **proxy 模式**：SOCKS5 代理服务器，支持无认证 / USER+PASS 认证，转发到 HTTP 后端
- **自动重启**：`auto_restart: true` 时后端退出后自动重新启动
- **绑定失败指数退避重试**：端口被占用时指数退避重试，最长不超过 `max_retry_seconds`
- **TCP 连接监控**：通过 NETLINK_INET_DIAG 实时采样端口连接状态，记录活跃时间
- **协议无关**：监控和端口接管在 TCP 层运作，HTTP/HTTPS 端口对 gatekeeper 无区别——只认端口号，不看协议。HTTPS 的 TLS 流量不会被解密或检测
- **端口活跃跟踪**：`hasRecentActivity()` 接口，查询端口是否在最近 N 分钟内有过活跃连接
- **systemd 集成**：`sd_notify` 精确通知启动完成，支持 `Type=notify`
- **stdin 配置加载**：`./gatekeeper -` 从标准输入读取配置，便于动态生成
- **CI/CD 自动构建**：GitHub Actions 多平台交叉编译，发布自动打包

## 项目结构

```
daemonPorts/
├── src/
│   ├── main.cpp        # 程序入口，统一监控线程
│   ├── config.h/cpp     # 配置解析（JSON）
│   ├── relay.h/cpp      # PortRelay 类，端口接力核心
│   ├── tcp_monitor.h/cpp # TCP 连接监控（NETLINK_INET_DIAG）
│   └── json.h/cpp       # 轻量 JSON 解析器
├── test/
│   ├── doctest.h        # 单文件测试框架
│   ├── test_main.cpp
│   ├── test_json.cpp
│   ├── test_config.cpp
│   ├── test_relay.cpp
│   └── test_tcp_monitor.cpp
├── xmake.lua            # xmake 构建脚本（多平台/多架构）
├── gatekeeper.service   # systemd 服务模板
├── gatekeeper-config.json # 示例配置
├── start-deeptutor.sh   # deeptutor 容器启动脚本
└── .github/workflows/build.yml  # GitHub Actions CI/CD
```

## 编译

### 方式一：xmake（推荐，支持跨平台/跨架构）

```bash
# 安装 xmake（如未安装）
# curl -fsSL https://xmake.io/install.sh | bash

# 默认编译（当前平台）
xmake

# 发布模式编译
xmake f -m release
xmake

# systemd 集成编译（启用 sd_notify）
xmake f -D HAVE_SYSTEMD=y
xmake

# 输出文件位于 build/<平台>/<架构>/gatekeeper
# 也可直接复制到项目根目录
cp build/$(xmake show-config 2>/dev/null | awk '{print $2" "$4}' | tr ' ' '/')/gatekeeper .

# --- 交叉编译示例 ---

# Linux ARM64 (如树莓派) — 方案一：安装交叉编译器
dnf install -y gcc-c++-aarch64-linux-gnu
xmake f -p cross -a arm64 --sdk=/usr --cross=aarch64-linux-gnu- -c
xmake

# Linux ARM64 (如树莓派) — 方案二：使用 Zig 跨编译（无需安装交叉编译器）
# 需要先创建 zig 交叉编译器包装器
mkdir -p /tmp/zig-toolchain/bin
cat > /tmp/zig-toolchain/bin/aarch64-linux-gnu-gcc << 'EOF'
#!/bin/bash
exec zig c++ -target aarch64-linux-gnu "$@"
EOF
cat > /tmp/zig-toolchain/bin/aarch64-linux-gnu-g++ << 'EOF'
#!/bin/bash
exec zig c++ -target aarch64-linux-gnu "$@"
EOF
cat > /tmp/zig-toolchain/bin/aarch64-linux-gnu-ar << 'EOF'
#!/bin/bash
exec zig ar "$@"
EOF
cat > /tmp/zig-toolchain/bin/aarch64-linux-gnu-ranlib << 'EOF'
#!/bin/bash
exec zig ranlib "$@"
EOF
chmod +x /tmp/zig-toolchain/bin/*
# 配置 xmake 并编译
xmake f -c -p cross -a arm64 --sdk=/tmp/zig-toolchain --cross=aarch64-linux-gnu-
xmake

# Linux x86 32位
xmake f -p linux -a i386
xmake

# Linux RISC-V 64
xmake f -p linux -a riscv64
xmake

# macOS (Intel)
xmake f -p macosx -a x86_64
xmake

# macOS (Apple Silicon)
xmake f -p macosx -a arm64
xmake

# 清理
xmake clean
xmake f -c   # 清除配置缓存
```

> 更换平台/架构后，先执行 `xmake f -c` 清理缓存再重新配置编译。

### 方式二：g++ 直接编译

```bash
# 标准编译
g++ -std=c++11 -O2 -o gatekeeper src/*.cpp -lpthread
strip gatekeeper  # 可选，减小体积

# systemd 集成编译
g++ -std=c++11 -DHAVE_SYSTEMD -O2 -o gatekeeper src/*.cpp -lpthread -lsystemd
```

## 测试

```bash
# 编译并自动运行测试（debug 模式）
xmake f -m debug
xmake

# 手动运行测试
./build/linux/x86_64/debug/test-gatekeeper
```

## 配置

编辑配置文件（如 `gatekeeper-config.json`）：

```json
{
  "ports": [
    {
      "name": "web-service",
      "listen": ":3000",
      "command": "./web-app --port 3000",
      "delay": 5000,
      "refresh_seconds": 3,
      "retry_seconds": 10,
      "max_retry_seconds": 300,
      "auto_restart": true,
      "stack_size": 512,
      "monitor": {
        "enabled": true,
        "interval_seconds": 60
      }
    },
    {
      "name": "api-service",
      "listen": ":4000",
      "command": "./api-server --port 4000",
      "delay": 5000,
      "auto_restart": false
    }
  ]
}
```
```json
{
  "ports": [
    {
      "name": "redis-node-1",
      "listen": ":6379",
      "command": "./redis-server --port 6379",
      "group": "redis-cluster"
    },
    {
      "name": "redis-node-2",
      "listen": ":6380",
      "command": "./redis-server --port 6380",
      "group": "redis-cluster"
    }
  ]
}
```

### 配置项说明

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `name` | `listen` 值 | 端口名称，用于日志标识和"xxx 启动中"页面 |
| `enabled` | `true` | 是否启用此端口，设为 `false` 可临时关闭而不删除配置 |
| `listen` | - | 门卫/后端监听地址，如 `:3000`（门卫先占，后端就绪后移交） |
| `command` | - | 启动后端程序的 shell 命令（`hold_port: true` 模式下可选，由协议各自提供） |
| `group` | `` (empty) | 可选字符串，默认空，定义该端口所属的组名，用于 PortGroup 关联 |
| `delay` | `5000` | 等待后端就绪的超时时间(ms) |
| `stack_size` | `512` | 该端口的线程栈大小(KB)，默认 512KB |
| `refresh_seconds` | `5` | 启动页自动刷新的间隔(秒) |
| `retry_seconds` | `10` | `auto_restart: true` 时，绑定失败后的初始重试间隔(秒) |
| `max_retry_seconds` | `300` | `auto_restart: true` 时，惩罚机制的最大重试间隔上限(秒)，超过此值保持不变 |
| `auto_restart` | `false` | 后端退出后，下次访问时是否自动重启 |
| `mode` | `"simple"` | 工作模式：`"simple"`（引导释放）、`"mixed"`（混合模式）或 `"proxy"`（SOCKS5 代理） |
| `auth` | `{}` | `proxy` 模式的认证配置，详见下方说明 |
| `http_target` | `` | `proxy` 模式下 SOCKS5 连接转发的 HTTP 后端地址，如 `127.0.0.1:8080` |
| `hold_port` | `false` | `mixed` 模式下是否持住端口：`false`=引导后释放，`true`=常驻代理 |
| `protocols` | `[]` | `mixed` 模式的协议列表，详见下方说明 |
| `monitor` | - | TCP 连接监控配置，详见下方说明 |

### TCP 连接监控

gatekeeper 支持通过 NETLINK_INET_DIAG 实时采样端口的 TCP 连接状态（仅 Linux 平台）。

```json
{
  "ports": [
    {
      "listen": ":3000",
      "command": "./app",
      "monitor": {
        "enabled": true,
        "interval_seconds": 60
      }
    }
  ]
}
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `monitor.enabled` | `false` | 是否启用 TCP 连接监控 |
| `monitor.interval_seconds` | `60` | 采样间隔（秒） |

启用后，gatekeeper 会在一个统一监控线程中轮询所有启用了监控的端口，查询其 TCP 连接状态并更新活跃时间戳。日志示例：

```
TCP 连接监控已启动，轮询间隔 60 秒
  [web-service] ACTIVE=1  connections=3  non-listen=2
  [api-service] ACTIVE=0  connections=1  non-listen=0 (idle)
```

### proxy 模式（SOCKS5 代理）

proxy 模式将 gatekeeper 作为 SOCKS5 代理服务器运行。客户端通过 SOCKS5 协议连接 gatekeeper，gatekeeper 完成握手后建立 TCP 隧道将流量转发到指定的 HTTP 后端。

**配置示例：**

```json
{
  "ports": [
    {
      "name": "socks5-proxy",
      "listen": ":1080",
      "mode": "proxy",
      "auth": {
        "type": "userpass",
        "username": "admin",
        "password": "secret123"
      },
      "http_target": "127.0.0.1:8080"
    }
  ]
}
```

**认证配置：**

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `auth.type` | `"none"` | 认证类型：`"none"`（无认证）或 `"userpass"`（用户名密码） |
| `auth.username` | `` | USERPASS 认证的用户名（`type` 为 `"userpass"` 时必填） |
| `auth.password` | `` | USERPASS 认证的密码（`type` 为 `"userpass"` 时必填） |
| `http_target` | `` | SOCKS5 连接转发的 HTTP 后端地址，格式为 `host:port` |

**工作模式：**

1. **无认证**（默认）：客户端直接连接，gatekeeper 接受任何 SOCKS5 请求并转发到 `http_target`
2. **USER/PASS 认证**：客户端必须先提供正确的用户名密码，验证通过后才允许连接

**使用场景：**
- 将 SOCKS5 代理请求转发到本地 HTTP 服务
- 在受限网络环境下通过 SOCKS5 代理访问内部服务
- 配合认证机制控制代理访问权限

## 使用

```bash
# 从文件读取配置
./gatekeeper gatekeeper-config.json

# 从 stdin 读取配置（适用于动态生成配置）
echo '{"ports":[{"listen":":3000","command":"./app"}]}' | ./gatekeeper -

# SOCKS5 代理模式（快速启动）
echo '{"ports":[{"listen":":1080","mode":"proxy","http_target":"127.0.0.1:8080"}]}' | ./gatekeeper -
```

## systemd 集成

编译时添加 `-DHAVE_SYSTEMD` 即可启用 systemd sd_notify 支持。gatekeeper 会在使用完毕后发送 `READY=1` 通知，让 systemd 准确知道启动完成时机。

### 安装服务文件

项目附带 `gatekeeper.service` 模板，安装步骤：

```bash
# 1. 复制二进制和服务文件
cp build/linux/x86_64/release/gatekeeper /usr/local/bin/
cp gatekeeper.service /etc/systemd/system/
cp gatekeeper-config.json /usr/local/etc/gatekeeper/config.json

# 2. 修改 service 文件中的 ExecStart 路径（如需要）
#    - 使用 Type=notify（需 HAVE_SYSTEMD 编译）
#    - 使用 Type=simple（标准编译）

# 3. 启用并启动
sudo systemctl daemon-reload
sudo systemctl enable --now gatekeeper
```

> `Type=notify` 配合 `sd_notify()` 可以精确感知 gatekeeper 是否成功启动（端口已接管）。非 systemd 编译版本使用 `Type=simple` 即可。

### 常用操作

```bash
systemctl enable --now gatekeeper   # 启用并启动
systemctl status gatekeeper         # 查看状态
journalctl -u gatekeeper -f         # 查看实时日志
systemctl restart gatekeeper        # 重启服务
```

### 安全加固

`gatekeeper.service` 模板内置了 systemd 安全加固：

- `NoNewPrivileges=yes` — 禁止提权
- `ProtectSystem=strict` — 只读文件系统
- `ProtectHome=yes` — 隐藏用户主目录
- `PrivateTmp=yes` — 隔离临时目录
- `LimitNOFILE=65536` — 文件描述符限制
- `LimitNPROC=1024` — 进程数限制

如果后端程序需要读写特定目录，可将 `ProtectSystem` 改为 `full`，或通过 `ReadWritePaths=` 放行。

## 工作原理

### simple 模式流程

```
阶段1: 客户端 ──→ :3000 ──→ gatekeeper (返回 "web 启动中..." 页面)
阶段2: gatekeeper fork+exec 启动后端程序，关闭 :3000 监听
阶段3: 后端程序绑定 :3000
阶段4: 浏览器自动刷新 ──→ :3000 ──→ 后端程序
```

1. **门卫监听** `listen` 端口，占用极少内存（二进制 ~84KB，RSS ~4MB/5端口，每增加 1 端口 RSS 增加约 16KB；线程栈从默认 8MB 缩减为 256KB，100 端口 VmSize 仅 58MB）
2. **首次连接**时，返回含 `<meta http-equiv="refresh">` 的 HTML 页面
3. **同时** fork+exec 执行 `command`（监听 socket 带有 `SOCK_CLOEXEC` 标志，确保子进程不会继承该端口）
4. **释放端口**：关闭自己的监听 socket，让后端程序绑定
5. **等待就绪**：不断尝试连接 `listen` 端口，直到成功或超时
6. **浏览器自动刷新**后，直连后端程序
7. **自动重启**（可选）：如果 `auto_restart: true`，后端退出后重新监听，下次访问再次引导
8. **绑定失败重试**（可选）：如果 `auto_restart: true` 且端口被占用，gatekeeper 会每隔 `retry_seconds` 秒重试一次。每次失败后重试间隔翻倍（惩罚机制），但最长不超过 `max_retry_seconds`。端口释放成功或收到 SIGTERM 时停止重试。

### mixed 模式

mixed 模式让一个端口同时支持多种协议（HTTP / SOCKS5 / SOCKS4），**根据 `hold_port` 设置有两种行为**。

#### hold_port: false — 协议感知引导（释放模式）

gatekeeper 检测连接协议类型，发送对应的引导响应，然后启动后端并释放端口（与 simple 模式一致）。

适合**后端自身支持多协议**的场景（如 sing-box mixed、v2ray 等）。

```json
{
  "ports": [
    {
      "name": "mixed-release",
      "listen": ":3128",
      "command": "./sing-box run",
      "mode": "mixed",
      "protocols": ["http", "socks5", "socks4"]
    }
  ]
}
```

协议检测与引导响应：

| 连接类型 | 特征 | 引导响应 |
|---------|------|---------|
| HTTP | `GET` / `POST` 等 | 启动页 HTML（浏览器自动刷新） |
| HTTPS | TLS 握手（首字节 `0x16`） | **不支持** — 不匹配，静默关闭 |
| SOCKS5 | 首字节 `0x05` | `0x05 0xFF`（无可用认证方法） |
| SOCKS4 | 首字节 `0x04` | 请求被拒 |
| 未知 | 不匹配以上 | 静默关闭 |

> HTTPS 流量经过 TLS 加密，gatekeeper 无法在 TCP 层解密或识别 HTTP 请求内容。如需 HTTPS 支持，建议在后端使用 TLS 终止代理（如 nginx/caddy），或使用 simple 模式让 HTTPS 后端直接接管端口。

#### hold_port: true — 协议路由代理（持住模式）

gatekeeper **常驻端口不释放**，检测协议后按需启动对应的后端程序，然后建立 TCP 隧道将流量转发给后端。

适合**聚合多个独立后端服务到同一个端口**的场景。

```json
{
  "ports": [
    {
      "name": "mixed-gateway",
      "listen": ":3128",
      "mode": "mixed",
      "hold_port": true,
      "protocols": [
        {
          "type": "http",
          "command": "./web-app --port 8080",
          "proxy_to": "127.0.0.1:8080"
        },
        {
          "type": "socks5",
          "command": "./socks-app --port 1080",
          "proxy_to": "127.0.0.1:1080",
          "delay": 3000
    }
]
    }
  ]
}
```

> Note: group mode only works with `simple` mode, and all ports in a group should share the same command.

`protocols` 数组中的每个条目：

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `type` | - | 协议类型：`"http"` / `"socks5"` / `"socks4"` |
| `command` | - | `hold_port=true` 时必须，启动该协议后端的 shell 命令 |
| `proxy_to` | - | `hold_port=true` 时必须，后端监听地址，如 `127.0.0.1:8080` |
| `delay` | 顶级 `delay` | 等待该后端就绪的超时时间(ms) |
| `enabled` | `true` | 是否启用此协议 |

> `protocols` 也可用简写形式：`["http", "socks5", "socks4"]`，适用于 `hold_port: false` 仅需声明协议类型时。

执行流程：

```
gatekeeper 常驻 :3128（不释放）
  │
  ├─ HTTP 请求 → web-app 没启动？启动 → 就绪后 TCP 隧道 :3128 ↔ :8080
  ├─ SOCKS5 请求 → socks-app 没启动？启动 → 就绪后 TCP 隧道 :3128 ↔ :1080
  └─ 后端启动中 → 发送协议对应临时响应（HTTP 启动页 / SOCKS 拒绝）
```

### proxy 模式

proxy 模式将 gatekeeper 作为 SOCKS5 代理服务器运行，**常驻端口不释放**。

执行流程：

```
gatekeeper 常驻 :1080（SOCKS5 代理）
  │
  ├─ 客户端连接
  │   ├─ 无认证 (NO_AUTH)
  │   │   └─ 直接建立 TCP 隧道 → http_target
  │   └─ 有认证 (USER/PASS)
  │       ├─ 验证用户名密码
  │       ├─ 失败 → 返回 AUTH_FAILED
  │       └─ 成功 → 建立 TCP 隧道 → http_target
  └─ 客户端发送 SOCKS5 请求 (CONNECT/UDP)
      └─ 转发到 http_target
```

1. **监听端口**：gatekeeper 监听 SOCKS5 端口
2. **SOCKS5 握手**：客户端发起 SOCKS5 连接，gatekeeper 处理认证（NO_AUTH 或 USER/PASS）
3. **目标连接**：认证通过后，gatekeeper 连接 `http_target` 后端
4. **双向隧道**：建立客户端 ↔ gatekeeper ↔ http_target 的 TCP 双向隧道
5. **错误处理**：认证失败返回 `0x05 0x01 0x00 0x00 0x01 0x00 0x00 0x00 0x00 0x00`，其他错误返回 `0x05 0x7E`（不支持）

**注意事项：**
- `http_target` 必须设置为有效的后端地址（如 `127.0.0.1:8080`）
- 支持 IPv4 和 IPv6 地址解析
- 支持域名解析（通过 `getaddrinfo`）

## 示例

```bash
# Python HTTP 服务器
cat > my-config.json << 'EOF'
{
  "ports": [
    {
      "listen": ":8080",
      "command": "python3 -m http.server 8080"
    }
  ]
}
EOF
./gatekeeper my-config.json

# 多端口示例
cat > my-config.json << 'EOF'
{
  "ports": [
    {
      "name": "docs",
      "listen": ":8000",
      "command": "mkdocs serve -a 127.0.0.1:8000",
      "auto_restart": true
    },
    {
      "name": "api",
      "listen": ":9000",
      "command": "node server.js --port 9000"
    }
  ]
}
EOF
./gatekeeper my-config.json

# stdin 管道（适用于动态生成配置）
echo '{"ports":[{"listen":":3000","command":"./app"}]}' | ./gatekeeper -

# SOCKS5 代理示例（无认证）
cat > my-config.json << 'EOF'
{
  "ports": [
    {
      "listen": ":1080",
      "mode": "proxy",
      "http_target": "127.0.0.1:8080"
    }
  ]
}
EOF
./gatekeeper my-config.json

# SOCKS5 代理示例（USER/PASS 认证）
cat > my-config.json << 'EOF'
{
  "ports": [
    {
      "listen": ":1080",
      "mode": "proxy",
      "auth": {
        "type": "userpass",
        "username": "admin",
        "password": "secret123"
      },
      "http_target": "127.0.0.1:8080"
    }
  ]
}
EOF
./gatekeeper my-config.json
```

## CI/CD

项目使用 GitHub Actions 进行多平台自动构建。支持的平台/架构：

| 平台 | 架构 | 构建方式 |
|------|------|---------|
| Linux | x86_64 | 原生编译 |
| Linux | arm64 | 交叉编译（aarch64-linux-gnu） |
| Linux | i386 | 交叉编译（i686-linux-gnu） |
| Windows | x86_64 | MinGW 交叉编译 / MSVC 原生 |
| Windows | i686 | MinGW 交叉编译 / MSVC 原生 |
| macOS | x86_64 | 原生编译（Intel） |
| macOS | arm64 | 原生编译（Apple Silicon） |

构建触发条件：
- `push` 到 main/master/develop 分支
- Pull Request 到 main/master 分支
- Release 发布
- 手动触发（workflow_dispatch）

Release 发布时会自动收集所有平台构建产物并上传到 GitHub Releases。

## License

MIT License — 详见 [LICENSE](LICENSE) 文件。
