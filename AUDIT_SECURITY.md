# Gatekeeper 安全审计报告

**项目**: daemonPorts (gatekeeper) - C++ 端口引导守护进程
**版本**: v1.1.3 (commit 5e0f043)
**审计日期**: 2026-08-23
**审计方法**: 手动代码审查 + 3 个并行探索代理（输入验证、内存安全、权限/并发）

---

## 严重程度分级

| 等级 | 数量 | 定义 |
|------|------|------|
| CRITICAL | 3 | 可被远程利用导致 RCE 或完全绕过 |
| HIGH | 5 | 可利用的安全漏洞，需特定条件 |
| MEDIUM | 6 | 防御性缺陷，可被用于 DoS 或辅助攻击 |
| LOW | 3 | 代码质量/健壮性问题 |

---

## 🔴 CRITICAL

### C1. 控制服务器默认无认证，可远程 RCE

- **文件**: `src/control_server.cpp:159-164`
- **输入点**: 控制服务器监听端口（默认 `:19999`）
- **漏洞**: `checkAuth()` 当 `config_.auth.type != "token"` 时直接返回 `true`（无认证）
- **默认配置**: `config.h:69` 中 `ControlAuth.type = "none"`
- **利用链**: 攻击者访问 `POST /config` 发送任意 JSON → `reloadFromJson()` 解析 → 新配置中 `command` 字段传递给 `execl("/bin/sh", "-c", ...)` → **任意命令执行**
- **进一步**: 控制服务器默认绑定地址取决于 `parseSockaddr` 对空 host 的处理 — 若绑定到 `0.0.0.0:19999` 则局域网内皆可访问

### C2. `/config` 端点直接注入任意配置

- **文件**: `src/control_server.cpp:236-244`
- **输入点**: `POST /config` 请求体
- **漏洞**: `reloadFromJson(req.body)` 直接将 HTTP 请求体解析为完整配置，无任何授权检查（除 token 外）或输入限制
- **影响**: 等同于远程配置覆盖，可添加任意端口+命令

### C3. RateLimiter 无锁，线程不安全

- **文件**: `src/control_server.h`（RateLimiter 实现）
- **漏洞**: 多线程同时调用 `allow()` 访问 `std::map<std::string, Entry>` 无 `mutex` 保护
- **影响**: 数据竞争 → map 迭代器失效 → 崩溃或绕过限流

---

## 🟠 HIGH

### H1. 命令注入：`execl("/bin/sh -c", command_)`

- **文件**: `src/relay_posix.cpp:100-101`
- **输入点**: 配置文件中的 `command` 字段
- **漏洞**: `execl("/bin/sh", "sh", "-c", command_.c_str(), nullptr)` — 通过 shell 执行命令字符串，`command` 中含 `;`、`|`、`$()` 等皆可注入
- **缓解**: 假设配置文件受信任（本地文件），但若控制服务器未认证则远程攻击者可注入任意命令

### H2. 无权限降级（始终以 root 运行）

- **文件**: `src/main.cpp` 全文件
- **漏洞**: 无任何 `setuid()`/`setgid()`/`prctl(PR_SET_NO_NEW_PRIVS)` 调用
- **影响**: 启动的后端进程继承 root 权限。若任何一个后端被攻破，攻击者获得完全 root 权限
- **systemd 服务**: `User=root`（默认）

### H3. `system()` 调用在 gracefulStop

- **文件**: `src/relay.cpp`（gracefulStop 方法）
- **漏洞**: 使用 `system(stopCommand_.c_str())` 执行关闭命令，同样通过 shell 执行
- **影响**: 若 stopCommand 来自配置且可被篡改，同命令注入

### H4. `popen()` 在 Linux 平台实现

- **文件**: `src/relay_linux.cpp:15, 92`
- **漏洞**: `snprintf(cmd, ..., "ss -tlnp 2>/dev/null | grep ':%u '", port)` 和 `"ss -tnlp 2>/dev/null | grep ':%u '"` — `port` 来自配置的 `listenAddr` 解析
- **影响**: 若端口号可被控制（通过注入配置），可注入 shell 命令

### H5. 信号处理器持有 `std::mutex`

- **文件**: `src/main.cpp:54-68`
- **漏洞**: `handleSignal()` 调用 `std::lock_guard<std::mutex> lock(g_groupsMutex)` — 信号处理器运行在信号上下文中，不可重入，若在持有锁时收到信号则死锁
- **影响**: 进程无法正常退出，需要 `SIGKILL`

---

## 🟡 MEDIUM

### M1. `std::stoi`/`std::stod` 无异常处理

- **文件**: `src/relay.cpp:61, 305`, `src/control_server.cpp:39, 148`, `src/json.cpp:38`
- **漏洞**: `std::stoi`/`std::stod` 在输入非数字时抛出 `std::invalid_argument` 或 `std::out_of_range`，整个程序无 `try/catch` 顶层包装
- **影响**: 恶意 HTTP 请求（如 `Content-Length: abc`）或畸形配置导致进程崩溃 → DoS

### M2. 控制服务器监听地址默认可能绑定到所有接口

- **文件**: `src/control_server.cpp:44-46`
- **漏洞**: `parseSockaddr` 中空 host 或 `0.0.0.0` 映射到 `INADDR_ANY`，`localhost`/`127.0.0.1` 才映射到回环地址
- **默认配置**: 若 `ControlConfig.listen` 为空或 `:19999`，则绑定到所有接口

### M3. RateLimiter map 永不清理

- **文件**: `src/control_server.h`（RateLimiter 实现）
- **漏洞**: 每个新 IP 在 map 中创建条目，但无淘汰机制。长期运行后内存持续增长
- **影响**: 内存泄漏（低速率，但长期）

### M4. 配置文件无权限检查

- **文件**: `src/config.cpp` 全文件
- **漏洞**: `parseConfig` 不检查配置文件的所有者/权限。若配置被非 root 用户篡改，可注入任意命令

### M5. 无 SIGCHLD 处理器

- **文件**: `src/main.cpp` 全文件
- **漏洞**: 未设置 `SIGCHLD` 信号处理器，子进程退出时可能产生僵尸进程（若 `waitChild` 未及时调用）

### M6. HTTP 解析器单线程阻塞

- **文件**: `src/control_server.cpp:113-123`
- **漏洞**: `parseHttpRequest` 在 `while` 循环中不断 `read_fd`，虽然有 15 秒 `SO_RCVTIMEO`，但超时期间该线程完全阻塞，无法处理其他请求
- **影响**: 慢连接可占用控制服务器线程，阻碍合法请求

---

## 🟢 LOW

### L1. JSON 解析器递归深度无限制

- **文件**: `src/json.cpp:43-70`
- **漏洞**: `parse_obj`/`parse_arr`/`parse_val` 互相递归调用，深度嵌套 JSON 可导致栈溢出
- **影响**: DoS（崩溃），但需要控制配置输入

### L2. `snprintf` 缓冲区大小固定

- **文件**: `src/relay_linux.cpp:15, 60, 66, 92`
- **漏洞**: 256 字节固定缓冲区，超长路径/端口号被截断
- **影响**: 功能异常（路径截断）而非安全漏洞

### L3. PortGroup 使用裸指针

- **文件**: `src/port_group.cpp:13-18, 27-42`
- **漏洞**: `PortRelay*` 裸指针存储 vector，无所有权语义
- **影响**: 若 PortRelay 在 PortGroup 之前析构，出现悬空指针

---

## 主动接受的安全取舍（非漏洞，勿"修复"）

以下为**有意设计**的安全削弱，是有意的可恢复性/可用性取舍。修复它们会破坏设计意图。详见 `docs/adr/0001`、`docs/adr/0002`。

### E1. 应急态下 ad-hoc 命令可免 PIN（swap 超阈值）

- **行为**：当 swap 使用率超过 `system_monitor.swap_high_threshold`（默认 50%）进入应急态时，`POST /run` 的 ad-hoc 裸命令若首词命中 `emergency_commands` 名单（默认 `reboot`/`shutdown`），跳过 PIN 校验。
- **意图**：swap 打满、记不起 PIN 时，从远端紧急 `reboot`/`shutdown` 恢复机器。
- **已知代价**：该豁免全局生效（不限回环）。任何能触达控制端口的客户端可**自行打满 swap** 后免密 `reboot`/`shutdown` → 反复重启的 DoS 面。已评估并接受。
- **缓解边界**：`sudo`/`doas` 前缀会被剥离后匹配（`sudo reboot` 也命中），但 `echo reboot` 等"首词不在名单"的命令不豁免；无 swap 或采样未启用时永不进入应急态。

### E2. 双满载自动驱逐运行中的后端（物理 ∧ swap 均近满载）

- **行为**：物理内存 ∧ swap 双双超过 `eviction` 阈值并持续 `sustain_seconds`（默认 15 分钟）后，gatekeeper 自动关闭"端口无数据流量最久"的运行中子配置项。
- **意图**：双满载持续 15 分钟不做动作，下一步就是内核 OOM 任意杀进程（可能杀掉 gatekeeper 或活跃后端且无粘性禁用）。受控驱逐最闲置者优于失控 OOM。
- **已知代价**：被驱逐条目本次运行内粘性禁用，该端口服务暂时不可用，需重启或 `/reload` 恢复。这是保活优先策略，杀最闲置可能释放内存较少。

---

## 修复建议优先级

### P0（立即修复）

| ID | 修复方案 | 估计工作量 |
|----|---------|-----------|
| C1+C2 | 控制服务器默认绑定 `127.0.0.1`；默认启用 `auth.type = "token"` 并生成随机 token 输出到日志 | 1 天 |
| C3 | RateLimiter 的 `allow()` 加 `std::mutex` | 2 小时 |

### P1（本周内修复）

| ID | 修复方案 | 估计工作量 |
|----|---------|-----------|
| H1 | `execvp(argv[0], argv)` 数组传参，避免 shell 解释；将 `command` 字符串按空格分割为参数数组 | 1 天 |
| H2 | 启动后端前 `fork()` → `setgid()` + `setuid()` 降权到非特权用户；或通过 systemd `User=` 配置，启动前降权 | 2 天 |
| H5 | 信号处理器只设 `atomic<bool>` 标志，不在信号上下文中加锁；主循环中检查标志后执行清理 | 半天 |

### P2（本月内修复）

| ID | 修复方案 | 估计工作量 |
|----|---------|-----------|
| M1 | 所有 `stoi`/`stod`/`stoul` 调用加 `try/catch` 或改用 `std::from_chars` | 半天 |
| M3 | 控制服务器默认强制绑定 `127.0.0.1` | 修改 1 行 |
| M4 | RateLimiter 加 LRU 淘汰或定期清理过期条目 | 1 天 |
| M5 | 添加 `SIGCHLD` handler（`SA_NOCLDSTOP` + `SIG_IGN` 自动回收，或 `waitpid` 循环） | 半天 |
| M6 | 添加 `poll()`/`select()` 超时机制，或使用线程池处理连接 | 2 天 |