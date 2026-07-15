# daemonPorts - 多端口启动引导门卫

## 场景

有一个或多个占用内存较多的程序，每个程序监听一个端口提供 TCP 服务。门卫程序先接管这些端口（占用极少内存，二进制仅 39KB），当有人访问某个端口时，页面显示 **"xxx 启动中"**，自动启动对应的后端程序，然后释放端口让后端程序接管。

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
| `command` | - | 启动后端程序的 shell 命令（后端直接监听 `listen` 端口） |
| `delay` | `5000` | 等待后端就绪的超时时间(ms) |
| `refresh_seconds` | `3` | 启动页自动刷新的间隔(秒) |
| `auto_restart` | `false` | 后端退出后，下次访问时是否自动重启 |

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

1. **门卫监听** `listen` 端口，占用极少内存（39KB 二进制，～0 运行时内存）
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
