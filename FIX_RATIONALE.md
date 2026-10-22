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

## 修复：停机卡死 — 监听线程 accept() 永不返回 + delayMs 长眠 (v1.1.13, 2026-10-22)

### 现象

每次 `systemctl stop gatekeeper`（及关机）都精确撞满 `TimeoutStopSec`：
实测 7/7 次 boot，150s/45s 两种配置全部超时 → Fedora 默认 `TimeoutStopFailureMode=abort`
→ SIGABRT + coredump。机器每次关机多等 2.5 分钟。

### 根因链（coredump 实证）

1. `PortRelay::listenLoop()` 阻塞在 `accept()` 上，而 `stop()`/`signalStop()`
   只 `close(fd)` —— **Linux 上从另一线程 `close()` 不会唤醒阻塞中的 `accept()`**。
   线程永不返回 → `pthread_join` 永久卡死。coredump 主线程栈：
   `pthread_clockjoin_ex ← joinThread ← main 停止序列`，其余线程 1 个卡在
   `accept()`、多个卡在 `nanosleep`。
2. 三处 `sleep_for(delayMs_)`（后端退出后等待重启的 35s 窗口）**无 `stop_` 检查**，
   SIGTERM 时正睡在其中 → join 再卡大半 delayMs。实测修复 1 后仍 57s（omniroute
   0.3s 就退，gatekeeper 干等 52s）。

### 修复（relay.cpp）

1. `stop()` 与 `signalStop()` 在 `close_fd()` 前先 `shutdown_fd(fd, SHUT_RDWR)`，
   唤醒阻塞的 `accept()`（accept 循环本就检查 `EINVAL`，只是没人触发）。
2. `monitorBackend()` 的 `findPidUsingPort` 重试循环加 `stop_`/端口释放早退。
3. 三处 `sleep_for(delayMs_)` 改为步进等待：**关闭程序时该端口 delay 重置为 1 秒**，
   其余情况保留原 `delayMs_`；`resetForIdle()` 复位 `stop_`，驱逐行为不受影响。

### 验证

| 阶段 | systemctl stop 耗时 |
|---|---|
| 修复前 | 150s 整 + SIGABRT + coredump |
| 修复 1（accept 唤醒） | 56.9s（不再超时，但卡 delayMs） |
| 修复 2（delay 重置 1s） | **2.57s**，`Deactivated successfully`，0 timeout 0 abort |

隔离测试同款：v2 卡 32s → v3 0.18s。部署前务必确认装的是 `gatekeeper-systemd`
变体（`Type=notify` 要求 HAVE_SYSTEMD 编译，见上文"部署检查清单"）。

## 历史记录

Added missing `<unistd.h>` include to `src/port_group.cpp` to provide declaration for `close()` used when releasing listening sockets. This resolves compilation error during build.
