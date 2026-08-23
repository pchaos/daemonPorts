# FIX RATIONALE / 经验记录

## 部署事故：装错二进制变体导致 Type=notify 服务无限重启（2026-08-23）

### 现象

`systemctl status gatekeeper` 卡在 `activating (start)`，PID 每 ~5 秒更换一次；
浏览器访问 :20128 一直看到 "omniroute 启动中... 13秒后自动重试" 页面循环。

### 根因链

1. 部署时 xmake 处于 **debug 模式**（此前跑测试用 `xmake f --mode=debug` 切过），
   构建产物是 debug 版（2.1M，未 strip），CPU/内存异常偏高。
2. 复制的是普通 target `gatekeeper`，而 `/etc/systemd/system/gatekeeper.service`
   配置的是 **`Type=notify`** —— 要求二进制以 `HAVE_SYSTEMD` 编译并调用
   `sd_notify(READY=1)`。
3. 普通版不发 READY=1 → systemd 等 `TimeoutStartSec` 超时 → 杀进程 →
   `RestartSec=5s` 后重启 → 无限循环。
4. 每次重启 gatekeeper 重新绑定 20128 并发启动页，形成"一直启动中"的表象。

### 正确组合

| service Type | 必须安装的二进制 |
|--------------|------------------|
| `Type=notify` | `gatekeeper-systemd`（始终 HAVE_SYSTEMD，可复制为任意文件名） |
| `Type=simple` | 普通 `gatekeeper` 即可 |

### 部署检查清单

1. **确认构建模式**：部署前 `xmake f --mode=release -y && xmake`；
   用文件大小交叉验证（release ≈ 216K，debug ≈ 2.1M）。
2. **确认二进制变体与 service Type 匹配**：先读
   `/etc/systemd/system/gatekeeper.service` 的 `Type=`，再决定复制哪个产物。
3. **替换被 systemd 管理的二进制前先停服务**：
   `sudo systemctl stop gatekeeper`，否则 `Text file busy`；
   直接 pkill 主进程会触发 systemd 自动拉起（Restart=on-failure）。
4. **部署后验证三件套**：
   - `<binary> --version` 版本正确
   - 文件大小符合 release 特征
   - `systemctl status` 为 `active (running)` 且观察 > RestartSec 无 PID 变化，
     journalctl 中出现 `systemd: READY=1`。

### 关联提交

- `5e0f043` feat: add launch_on_start per-port config for immediate backend launch (v1.1.3)

## 历史记录

Added missing `<unistd.h>` include to `src/port_group.cpp` to provide declaration for `close()` used when releasing listening sockets. This resolves compilation error during build.
