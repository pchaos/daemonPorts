# daemonPorts - 多端口 TCP 接力门卫

## 场景

有一个或多个占用内存较多的程序，每个程序监听一个端口提供 TCP 服务。门卫程序先接管这些端口（占用极少内存，二进制仅 39KB），当有人访问某个端口时，自动启动对应的后端程序，透明代理所有流量。

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
      "backend": ":3001",
      "command": "./web-app --port 3001",
      "delay": 500,
      "auto_restart": false
    },
    {
      "name": "api-service",
      "listen": ":4000",
      "backend": ":4001",
      "command": "./api-server --port 4001",
      "delay": 1000,
      "auto_restart": true
    }
  ]
}
```

### 配置项说明

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `name` | `listen` 值 | 端口名称，用于日志标识 |
| `enabled` | `true` | 是否启用此端口，设为 `false` 可临时关闭而不删除配置 |
| `listen` | - | 门卫监听地址（客户端连接到这里） |
| `backend` | - | 后端程序监听地址（门卫代理到此处） |
| `command` | - | 启动后端程序的 shell 命令 |
| `delay` | `500` | 等待后端就绪的超时时间(ms) |
| `auto_restart` | `false` | 后端退出后，下次连接时是否自动重启 |

## 使用

```bash
./gatekeeper config.json
```

## 工作原理

启动后，每个端口都进入**接力模式**：

```
客户端1 ──> :3000 ──> gatekeeper ──> :3001 ──> web-app
客户端2 ──> :4000 ──>   (proxy)   ──> :4001 ──> api-server
```

1. **门卫监听** `listen` 端口，占用极少内存（39KB 二进制，～0 运行时内存）
2. **首次连接**时，自动 fork+exec 执行 `command`
3. **等待就绪**：不断尝试连接 `backend` 端口，直到成功或超时
4. **透明代理**：双向搬运 TCP 数据（32KB 缓冲区）
5. **自动重启**（可选）：如果 `auto_restart: true`，后端退出后将在下次连接时重启

## 示例

```bash
# Python HTTP 服务器
./gatekeeper -config <<<EOF
{
  "ports": [
    {
      "listen": ":8080",
      "backend": ":8081",
      "command": "python3 -m http.server 8081"
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
      "backend": ":8001",
      "command": "mkdocs serve -a 127.0.0.1:8001",
      "auto_restart": true
    },
    {
      "name": "api",
      "listen": ":9000",
      "backend": ":9001",
      "command": "node server.js --port 9001"
    }
  ]
}
CFG
./gatekeeper my-config.json
```
