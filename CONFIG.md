# Gatekeeper 配置说明

## 配置文件结构

配置文件是一个 JSON 文件，顶层包含一个 `ports` 数组，每个元素定义一个端口的行为。

```json
{
  "ports": [
    {
      "name": "my-service",
      "listen": ":3000",
      "command": "./app --port 3000",
      "mode": "simple",
      ...
    }
  ]
}
```

## 全局配置项

每个端口条目支持以下字段：

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `name` | string | `listen` 值 | 端口名称，用于日志标识和"xxx 启动中"页面 |
| `enabled` | bool | `true` | 是否启用此端口，设为 `false` 可临时关闭而不删除配置 |
| `listen` | string | **必填** | 监听地址，格式 `:端口` 或 `IP:端口`，如 `:3000` |
| `mode` | string | `"simple"` | 工作模式：`"simple"` / `"mixed"` / `"proxy"` |
| `command` | string | - | 启动后端程序的 shell 命令（`hold_port: true` 模式下可选，由协议各自提供） |
| `stop_command` | string | `""` | 优雅关闭后端的命令，gatekeeper 收到 SIGTERM/SIGINT 时执行 |
| `group` | string | `""` | 端口组名，同组端口共享后端启动状态，避免重复启动 |
| `delay` | number | `5000` | 等待后端就绪的超时时间(毫秒)，超时后视为启动失败 |
| `refresh_seconds` | number | `5` | 启动页的 `<meta refresh>` 刷新间隔(秒) |
| `retry_seconds` | number | `10` | 绑定失败后的初始重试间隔(秒)（需 `auto_restart: true`） |
| `max_retry_seconds` | number | `300` | 重试惩罚机制的上限(秒)，超过此值不再增加 |
| `auto_restart` | bool | `false` | 后端退出后，下次访问时是否自动重新启动 |
| `launch_on_start` | bool | `false` | gatekeeper 启动时立即启动后端（首启一次性，热加载不触发） |
| `stack_size` | number | `512` | 该端口的监听线程栈大小(KB)，默认 512KB |
| `idle_minutes` | number | `20` | 空闲超时分钟数，启用 TCP 监控后，超过此时间无活跃连接则关闭后端 |
| `hold_port` | bool | `false` | `mixed` 模式下是否持住端口：`false`=引导后释放，`true`=gatekeeper 常驻做代理转发 |
| `protocols` | array | `[]` | `mixed` 模式的协议列表，详见 [mixed 模式协议配置](#mixed-模式-protocols-配置) |
| `auth` | object | `{}` | `proxy` 模式的认证配置，详见 [认证配置](#认证配置) |
| `http_target` | string | `""` | `proxy` 模式下 SOCKS5 转发目标地址，如 `"127.0.0.1:8080"` |
| `monitor` | object | - | TCP 连接监控配置，详见 [TCP 监控配置](#tcp-监控配置) |

---

## 工作模式详解

### simple 模式（默认）

最简单的模式。gatekeeper 监听端口，收到连接后启动后端，释放端口让后端接管。

**适用场景：** 单端口单后端，后端启动后独立运行。

```json
{
  "ports": [
    {
      "name": "web-service",
      "listen": ":3000",
      "command": "./web-app --port 3000",
      "delay": 5000,
      "auto_restart": true,
      "monitor": {
        "enabled": true,
        "interval_seconds": 60
      }
    }
  ]
}
```

**执行流程：**
1. gatekeeper 监听 `:3000`，内存占用极小
2. 客户端连接 → 返回含 `<meta http-equiv="refresh">` 的启动页
3. 同时 fork+exec 执行 `command`（监听 socket 带 `SOCK_CLOEXEC`，不会被继承）
4. 关闭自己的监听 socket，释放端口
5. 不断尝试连接 `:3000`，直到后端就绪或超时（`delay`）
6. 浏览器自动刷新 → 直连后端

**注意事项：**
- `command` 中的后端程序必须监听同一个端口（`listen` 指定的端口）
- 后端启动后 gatekeeper 不再介入，流量直通后端
- `auto_restart: true` 时后端退出后 gatekeeper 会重新监听，下次访问再次引导

---

### mixed 模式

mixed 模式让一个端口同时支持多种协议（HTTP / SOCKS5 / SOCKS4），根据 `hold_port` 设置有两种行为。

#### hold_port: false — 协议感知引导（释放模式）

gatekeeper 检测连接协议类型，发送对应的引导响应，然后启动后端并释放端口（与 simple 模式一致）。

**适用场景：** 后端自身支持多协议（如 sing-box mixed、v2ray 等）。

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

**协议检测与引导响应：**

| 连接类型 | 首字节特征 | 引导响应 |
|---------|-----------|---------|
| HTTP | `GET` / `POST` / `PUT` / `DELETE` 等 | 启动页 HTML（`<meta refresh>` 自动刷新） |
| HTTPS | `0x16`（TLS ClientHello） | **不支持** — 静默关闭连接 |
| SOCKS5 | `0x05` | `0x05 0xFF`（无可用认证方法） |
| SOCKS4 | `0x04` | 连接被拒 |
| 未知 | 不匹配以上 | 静默关闭 |

> **HTTPS 限制：** TLS 流量经过加密，gatekeeper 无法在 TCP 层解密或识别 HTTP 请求内容。如需 HTTPS 支持，建议在后端使用 TLS 终止代理（如 nginx/caddy），或使用 simple 模式让 HTTPS 后端直接接管端口。

#### hold_port: true — 协议路由代理（持住模式）

gatekeeper **常驻端口不释放**，检测协议后按需启动对应的后端程序，然后建立 TCP 隧道将流量转发给后端。

**适用场景：** 聚合多个独立后端服务到同一个端口。

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

**执行流程：**
```
gatekeeper 常驻 :3128（不释放）
  │
  ├─ HTTP 请求 → web-app 没启动？启动 → 就绪后 TCP 隧道 :3128 ↔ :8080
  ├─ SOCKS5 请求 → socks-app 没启动？启动 → 就绪后 TCP 隧道 :3128 ↔ :1080
  └─ 后端启动中 → 发送协议对应临时响应（HTTP 启动页 / SOCKS 拒绝）
```

**注意事项：**
- `hold_port: true` 时顶级 `command` 字段可选，每个协议需要各自提供 `command`
- 每个协议后端需要监听不同的端口（`proxy_to`），避免冲突
- 后端启动后通过 TCP 隧道转发，gatekeeper 有少量转发开销

---

### proxy 模式（SOCKS5 代理）

gatekeeper 作为 SOCKS5 代理服务器运行，**常驻端口不释放**。客户端通过 SOCKS5 协议连接，gatekeeper 完成握手后建立 TCP 隧道将流量转发到指定的 HTTP 后端。

**适用场景：**
- 将 SOCKS5 代理请求转发到本地 HTTP 服务
- 在受限网络环境下通过 SOCKS5 代理访问内部服务
- 配合认证机制控制代理访问权限

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

**执行流程：**
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
  └─ 客户端发送 SOCKS5 请求 (CONNECT)
      └─ 转发到 http_target
```

**注意事项：**
- `http_target` 必须设置为有效的后端地址（如 `127.0.0.1:8080`）
- 支持 IPv4、IPv6 地址解析
- 支持域名解析（通过 `getaddrinfo`）
- 不支持 UDP ASSOCIATE（SOCKS5 UDP 功能）

---

## 认证配置

proxy 模式下的 SOCKS5 认证配置：

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `auth.type` | string | `"none"` | 认证类型：`"none"`（无认证）或 `"userpass"`（用户名密码） |
| `auth.username` | string | `""` | USERPASS 认证的用户名（`type` 为 `"userpass"` 时必填） |
| `auth.password` | string | `""` | USERPASS 认证的密码（`type` 为 `"userpass"` 时必填） |

**无认证示例：**
```json
{
  "listen": ":1080",
  "mode": "proxy",
  "http_target": "127.0.0.1:8080"
}
```

**USER/PASS 认证示例：**
```json
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
```

---

## mixed 模式 protocols 配置

`protocols` 数组有两种写法：

### 简写形式（仅声明类型，hold_port: false 时使用）

```json
"protocols": ["http", "socks5", "socks4"]
```

### 完整对象形式（hold_port: true 时使用）

```json
"protocols": [
  {
    "type": "http",
    "command": "./web-app --port 8080",
    "proxy_to": "127.0.0.1:8080",
    "delay": 5000,
    "enabled": true
  }
]
```

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `type` | string | **必填** | 协议类型：`"http"` / `"socks5"` / `"socks4"` |
| `command` | string | - | 启动该协议后端的 shell 命令（`hold_port=true` 时必填） |
| `proxy_to` | string | - | 后端监听地址，如 `"127.0.0.1:8080"`（`hold_port=true` 时必填） |
| `delay` | number | 顶级 `delay` | 等待该后端就绪的超时时间(毫秒)，覆盖全局 `delay` |
| `enabled` | bool | `true` | 是否启用此协议 |

---

## TCP 监控配置

gatekeeper 支持通过 NETLINK_INET_DIAG 实时采样端口的 TCP 连接状态（仅 Linux 平台）。

```json
{
  "ports": [
    {
      "listen": ":3000",
      "command": "./app",
      "monitor": {
        "enabled": true,
        "interval_seconds": 60,
        "log_dedup": "skip"
      }
    }
  ]
}
```

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `monitor.enabled` | bool | `false` | 是否启用 TCP 连接监控 |
| `monitor.interval_seconds` | number | `60` | 采样间隔（秒） |
| `monitor.log_dedup` | string | `"skip"` | 日志去重模式 |

**日志去重模式说明：**

| 值 | 行为 |
|----|------|
| `"skip"` | 连接数不变时不打印日志，避免重复刷屏 |
| `"throttle"` | 每 5 轮采样打印一次，适合调试时观察趋势 |
| `"off"` | 每次采样都打印，最详细的日志输出 |

**监控日志示例：**
```
TCP 连接监控已启动，轮询间隔 60 秒
  [web-service] ACTIVE=1  connections=3  non-listen=2
  [api-service] ACTIVE=0  connections=1  non-listen=0 (idle)
```

**监控的实际用途：**
- 更新 `lastActiveTime` 时间戳，用于空闲超时判定
- 当 `idle_minutes` 内无活跃连接时，自动关闭后端以节省资源
- 日志输出便于排查端口连接问题

---

## 端口分组（group）

`group` 字段将多个端口归入同一组，用于协调启动逻辑：

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

**行为：**
- 同组端口共享后端启动状态，不会重复启动同一组
- 当组内任一端口被访问时，该组标记为"启动中"，其他端口立即返回启动页
- 适合需要同时启动多个关联后端的场景

---

## 优雅关闭（stop_command）

`stop_command` 在 gatekeeper 收到 SIGTERM/SIGINT 时执行，用于优雅关闭后端：

```json
{
  "ports": [
    {
      "name": "node-app",
      "listen": ":3000",
      "command": "node server.js --port 3000",
      "stop_command": "node server.js --stop"
    }
  ]
}
```

**行为：**
- gatekeeper 退出时先执行 `stop_command`，等待后端进程退出
- 超时后（默认 5 秒）强制终止后端
- 如果未设置 `stop_command`，gatekeeper 直接向后端进程发送 SIGTERM

---

## 重试与惩罚机制

当 `auto_restart: true` 且端口绑定失败时，gatekeeper 使用指数退避重试：

```
第 1 次失败 → 等待 retry_seconds (10s)
第 2 次失败 → 等待 retry_seconds × 2 (20s)
第 3 次失败 → 等待 retry_seconds × 4 (40s)
...
上限 → max_retry_seconds (300s)
```

- 每次失败后重试间隔翻倍，直到达到 `max_retry_seconds` 上限
- 后端成功绑定后重置为 `retry_seconds`
- 收到 SIGTERM 或端口释放成功时停止重试

---

## 控制端口配置

gatekeeper 支持通过 HTTP 控制端口进行运行时配置热加载，无需重启进程。

### 配置方式

在 config.json 顶层添加 `control` 对象：

```json
{
  "control": {
    "listen": ":19999",
    "auth": {
      "type": "token",
      "token": "my-secret-token"
    }
  },
  "ports": []
}
```

### 配置字段

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `control.listen` | string | `""` | 控制端口监听地址，如 `":19999"`。为空时不启动控制端口 |
| `control.auth.type` | string | `"none"` | 认证方式：`"none"`（无认证）或 `"token"`（Token 认证） |
| `ports[].listen` | string | **必填** | 监听地址，如 `":3000"` |
| `ports[].command` | string | **必填** | 启动命令。**注意**：gatekeeper 使用 `execvp` 直接执行，**不支持** `VAR=val` 环境变量前缀。需改用 `/usr/bin/env VAR=val ...` 写法，如 `/usr/bin/env DATA_DIR=/data node app.js` |
| `ports[].stop_command` | string | `""` | 停止命令，同 `command`，需用 `/usr/bin/env` 包装环境变量 |
| `ports[].delay` | integer | `3000` | 端口释放后等待后端就绪的毫秒数，建议设为后端冷启动耗时的 1.5-2 倍 |
| `ports[].idle_minutes` | integer | `20` | 空闲超时分钟数，超时后自动停止后端 |
| `ports[].auto_restart` | boolean | `true` | 后端退出后是否自动重启 |
| `ports[].enabled` | boolean | `true` | 是否启用该端口 |

### CLI 快捷方式

也可以通过命令行参数 `--control-port` 指定控制端口地址，优先级高于 config.json：

```bash
./gatekeeper --control-port :19999 gatekeeper-config.json
```

### HTTP 端点

| 端点 | 方法 | 说明 |
|------|------|------|
| `GET /health` | GET | 健康检查，返回 `{"status":"ok"}` |
| `GET /version` | GET | 返回 gatekeeper 版本号 JSON，如 `{"version":"1.1.5"}` |
| `GET /status` | GET | 返回当前端口运行状态 JSON |
| `POST /reload` | POST | 重新读取配置文件并应用变更 |
| `POST /config` | POST | 接收请求体中的新配置 JSON 并应用 |

### 认证

当 `auth.type` 为 `"token"` 时，所有请求需要在 HTTP 头中携带 Token：

```bash
curl -X POST http://127.0.0.1:19999/reload \
  -H "X-Auth-Token: my-secret-token"
```

无认证或 Token 错误时返回 `401 Unauthorized`。

### 热加载行为

`POST /reload` 触发配置重新加载，gatekeeper 会：

1. 按 `listenAddr` 对比新旧配置
2. **新增端口**：自动创建并启动新的监听线程
3. **删除端口**：旧端口标记为 `pendingRemoval`，停止接受新连接，等待后端空闲后自动停止
4. **修改端口**：旧端口标记为 `pendingRemoval`，同时创建并启动新端口
5. **未变化端口**：保持不变

> **注意**：删除/修改端口时，旧后端不会立即停止，而是等待空闲超时（`idle_minutes`）后自动 `gracefulStop()`，确保活跃连接不受影响。

### 示例

```bash
# 以下示例假设 control.auth.type = "none"（无认证）
# 若 auth.type = "token"，每条请求需加: -H "X-Auth-Token: <token>"

# 健康检查
curl http://127.0.0.1:19999/health

# 查询版本
curl http://127.0.0.1:19999/version

# 触发配置重新加载
curl -X POST http://127.0.0.1:19999/reload

# 直接发送新配置
curl -X POST http://127.0.0.1:19999/config \
  -H "Content-Type: application/json" \
  -d '{"ports":[{"listen":":3000","command":"./app"}]}'
```

## 配置加载方式

### 从文件加载

```bash
./gatekeeper /path/to/config.json
```

### 从 stdin 加载

适用于动态生成配置的场景：

```bash
echo '{"ports":[{"listen":":3000","command":"./app"}]}' | ./gatekeeper -
```

### 加载顺序

1. 命令行参数指定的配置文件路径
2. 参数为 `-` 时从标准输入读取
3. 配置解析失败时输出错误信息并退出

---

## 配置文件示例

### 最小配置

```json
{
  "ports": [
    {
      "listen": ":3000",
      "command": "python3 -m http.server 3000"
    }
  ]
}
```

### 单端口完整配置

```json
{
  "ports": [
    {
      "name": "web-service",
      "enabled": true,
      "listen": ":3000",
      "command": "./web-app --port 3000",
      "stop_command": "./web-app --stop",
      "group": "web-group",
      "mode": "simple",
      "delay": 5000,
      "refresh_seconds": 3,
      "retry_seconds": 10,
      "max_retry_seconds": 300,
      "auto_restart": true,
      "stack_size": 512,
      "idle_minutes": 20,
      "monitor": {
        "enabled": true,
        "interval_seconds": 60,
        "log_dedup": "skip"
      }
    }
  ]
}
```

### 多端口配置

```json
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
```

### mixed 模式（hold_port: false）

```json
{
  "ports": [
    {
      "name": "sing-box-mixed",
      "listen": ":3128",
      "command": "./sing-box run",
      "mode": "mixed",
      "protocols": ["http", "socks5", "socks4"]
    }
  ]
}
```

### mixed 模式（hold_port: true）

```json
{
  "ports": [
    {
      "name": "multi-protocol-gateway",
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
      ],
      "monitor": {
        "enabled": true,
        "interval_seconds": 30
      }
    }
  ]
}
```

### proxy 模式（无认证）

```json
{
  "ports": [
    {
      "name": "socks5-proxy",
      "listen": ":1080",
      "mode": "proxy",
      "http_target": "127.0.0.1:8080"
    }
  ]
}
```

### proxy 模式（USER/PASS 认证）

```json
{
  "ports": [
    {
      "name": "socks5-proxy-auth",
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

## 常见问题

### 端口被占用怎么办？

gatekeeper 启动时如果端口已被占用，会输出错误日志并跳过该端口。如果 `auto_restart: true`，gatekeeper 会按指数退避重试（参见 [重试机制](#重试与惩罚机制)）。

### 后端启动超时怎么办？

如果后端在 `delay` 毫秒内没有就绪，gatekeeper 会放弃等待并关闭监听 socket。此时：
- 端口处于无人监听状态，客户端连接会失败
- 如果 `auto_restart: true`，下次连接会重新触发引导流程
- 建议检查后端启动日志，适当增加 `delay` 值

### HTTPS 端口可以用吗？

HTTPS 流量经过 TLS 加密，gatekeeper 无法解密。在 `mixed` 模式下 HTTPS 连接会被静默关闭。建议：
- 使用 `simple` 模式让 HTTPS 后端直接接管端口
- 在后端前置 nginx/caddy 做 TLS 终止

### 如何禁用某个端口而不删除配置？

将 `enabled` 设为 `false`：

```json
{
  "name": "temp-disabled",
  "listen": ":3000",
  "command": "./app",
  "enabled": false
}
```

gatekeeper 启动时会跳过该端口，输出日志："xxx 已禁用，跳过"。

### 多个端口共享同一个后端如何避免重复启动？

使用 `group` 字段将端口分组，同组端口共享启动状态：

```json
{
  "ports": [
    { "name": "port-1", "listen": ":3000", "command": "./app", "group": "my-app" },
    { "name": "port-2", "listen": ":3001", "command": "./app", "group": "my-app" }
  ]
}
```

### 线程栈大小应该设多少？

默认 512KB 适用于绝大多数场景。如果后端是内存密集型应用，可适当降低（最低 64KB）。如果线程栈溢出（罕见），可增大到 1024KB。注意这个值影响的是 gatekeeper 的监听线程，不是后端的栈大小。