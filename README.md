# daemonPorts - 多端口启动引导门卫

## 场景

有一个或多个占用内存较多的程序，每个程序监听一个端口提供 TCP 服务。门卫程序先接管这些端口（二进制 ~84KB，运行 5 个端口时 RSS 约 4MB），当有人访问某个端口时，页面显示 **"xxx 启动中"**，自动启动对应的后端程序，然后释放端口让后端程序接管。

如果一个端口需要同时支持多种协议（如 HTTP、SOCKS5），或者想把多个独立后端服务聚合到同一个端口对外暴露，可以使用 **mixed 模式**——gatekeeper 检测连接协议类型，按需启动对应的后端，并通过 TCP 隧道转发流量。

支持两种工作模式：

- **simple 模式（默认）**：引导 → 释放端口，后端接管。适合单端口单后端场景
- **mixed 模式**：协议感知引导，或常驻端口做协议路由。适合 sing-box 等多种协议聚合场景

## 编译

### 方式一：xmake（推荐，支持跨平台/跨架构）

```bash
# 安装 xmake（如未安装）
# curl -fsSL https://xmake.io/install.sh | bash

# 默认编译（当前平台）
xmake

# 输出文件位于 build/<平台>/<架构>/gatekeeper
# 也可直接复制到项目根目录
cp build/$(xmake show-config 2>/dev/null | awk '{print $2" "$4}' | tr ' ' '/')/gatekeeper .

# --- 交叉编译示例 ---

# Linux ARM64 (如树莓派)
xmake f -p linux -a arm64
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
g++ -std=c++11 -O2 -o gatekeeper gatekeeper.cpp -lpthread
strip gatekeeper  # 可选，减小体积
```

## 配置

编辑 `config.json`：

```json
{
  "ports": [
    {
      "name": "web-service",
      "listen": ":3000",
      "command": "./web-app --port 3000",
      "delay": 5000,
      "refresh_seconds": 3,
      "auto_restart": false
    },
    {
      "name": "api-service",
      "listen": ":4000",
      "command": "./api-server --port 4000",
      "delay": 5000,
      "refresh_seconds": 3,
      "auto_restart": true
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
| `delay` | `5000` | 等待后端就绪的超时时间(ms) |
| `stack_size` | `512` | 该端口的线程栈大小(KB)，默认 512KB |
| `refresh_seconds` | `5` | 启动页自动刷新的间隔(秒) |
| `auto_restart` | `false` | 后端退出后，下次访问时是否自动重启 |
| `mode` | `"simple"` | 工作模式：`"simple"`（引导释放）或 `"mixed"`（混合模式） |
| `hold_port` | `false` | `mixed` 模式下是否持住端口：`false`=引导后释放，`true`=常驻代理 |
| `protocols` | `[]` | `mixed` 模式的协议列表，详见下方说明 |

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
| SOCKS5 | 首字节 `0x05` | `0x05 0xFF`（无可用认证方法） |
| SOCKS4 | 首字节 `0x04` | 请求被拒 |
| 未知 | 不匹配以上 | 静默关闭 |

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

## 使用

```bash
./gatekeeper config.json
```

## 工作原理

启动后，每个端口进入**启动引导模式**：

```
阶段1: 客户端 ──→ :3000 ──→ gatekeeper (返回 "web 启动中..." 页面)
阶段2: gatekeeper 启动后端程序，关闭 :3000
阶段3: 后端程序绑定 :3000
阶段4: 浏览器自动刷新 ──→ :3000 ──→ 后端程序
```

1. **门卫监听** `listen` 端口，占用极少内存（二进制 ~84KB，RSS ~4MB/5端口，每增加 1 端口 RSS 增加约 16KB；线程栈从默认 8MB 缩减为 256KB，100 端口 VmSize 仅 58MB）
2. **首次连接**时，返回含 `<meta http-equiv="refresh">` 的 HTML 页面
3. **同时** fork+exec 执行 `command`
4. **释放端口**：关闭自己的监听 socket，让后端程序绑定
5. **等待就绪**：不断尝试连接 `listen` 端口，直到成功或超时
6. **浏览器自动刷新**后，直连后端程序
7. **自动重启**（可选）：如果 `auto_restart: true`，后端退出后重新监听，下次访问再次引导

## 示例

```bash
# Python HTTP 服务器
./gatekeeper -config <<<EOF
{
  "ports": [
    {
      "listen": ":8080",
      "command": "python3 -m http.server 8080"
    }
  ]
}
EOF

# 多端口示例
cat > my-config.json << 'CFG'
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
CFG
./gatekeeper my-config.json
```
