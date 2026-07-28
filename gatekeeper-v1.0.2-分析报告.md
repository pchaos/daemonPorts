# Gatekeeper v1.0.1 → v1.0.2 二进制升级分析

> 2026-07-24 · oect0 自升 + 跨机对比 oect1

---

## 1. 升级概要

| 项目 | 升级前 (v1.0.1) | 升级后 (v1.0.2) | 来源 |
|------|-----------------|-----------------|------|
| **二进制版本** | v1.0.1 | v1.0.2 | `./gatekeeper --version` |
| **文件大小** | 862KB (862,336 bytes) | 853KB (853,594 bytes) | `stat` / `ls -lh` |
| **MD5** | `130b392b70e00431f810c44607786d49` | `7aab837eda3c0bf6dc24fc9665db0ba5` | `md5sum` |
| **SHA256** | `78d7312863b20145c4482b706273a7c8b0329131e3ec6d5f3d4e118894a76075` | `6723aa92015900c153c1f60ffb745220e6806bf344c3668816dfedb27e71d8c1` | `sha256sum` |
| **升级方式** | 手动替换 + systemctl restart | `./gatekeeper self-update` 自动升级 | - |
| **运行用户** | root (之前) → **user** (self-update 后) | user | `ps aux` |
| **PID** | 7490 (旧) → 1485 (新) | 920 (oect1) | `pgrep` |

---

## 2. 跨机对比：oect0 vs oect1

### 2.1 一致性验证 ✅

| 项目 | oect0 (自升) | oect1 (手动装) | 一致? |
|------|-------------|---------------|-------|
| **版本** | v1.0.2 | v1.0.2 | ✅ |
| **文件大小** | 853,594 bytes | 853,594 bytes | ✅ |
| **MD5** | `7aab837eda3c0bf6dc24fc9665db0ba5` | `7aab837eda3c0bf6dc24fc9665db0ba5` | ✅ |
| **SHA256** | `6723aa92015900c153c1f60ffb745220e6806bf344c3668816dfedb27e71d8c1` | `6723aa92015900c153c1f60ffb745220e6806bf344c3668816dfedb27e71d8c1` | ✅ |
| **编译器** | clang 19.1.7 (Fedora) | clang 19.1.7 (Fedora) | ✅ |
| **链接器** | LLD 19.1.7 | LLD 19.1.7 | ✅ |
| **CPU 架构** | aarch64 | aarch64 | ✅ |
| **依赖库** | libpthread, libc, libdl | libpthread, libc, libdl | ✅ |
| **监听端口** | 0.0.0.0:3000 | 0.0.0.0:3000 | ✅ |

**结论：两台机器上的 v1.0.2 二进制完全一致。**

### 2.2 版本差异：v1.0.1 vs v1.0.2

| 项目 | v1.0.1 (旧) | v1.0.2 (新) | 变化 |
|------|------------|------------|------|
| **文件大小** | 862,336 bytes | 853,594 bytes | **减小 8,742 bytes (-1.0%)** |
| **代码段 (.text)** | ~214KB | ~213KB | 略减 |
| **只读数据 (.rodata)** | ~15KB | ~15KB | 基本不变 |
| **字符串数量** | ~2,600+ | 2,549 | 略减 |
| **符号数 (readelf -s)** | ~280+ | 247 | 减少 ~33 个符号 |
| **版本信息** | 无内嵌版本 | `gatekeeper v1.0.2` | 新增了 `--version` 支持 |
| **编译工具链** | 未知 | clang 19.1.7 + LLD 19.1.7 (Fedora 42) | 新工具链构建 |

### 2.3 二进制结构对比

```
Section      v1.0.1 (est)    v1.0.2 (exact)
────────────────────────────────────────────────
.text        ~214K           213,008 (0x347d0)
.rodata      ~15K            15,200  (0x3f40)
.data        ~892K           880,752 (0xd4660)
.bss         ~892K           880,824 (0xd4838)
.data.rel.ro ~834K           853,040 (0xcc9b0)
────────────────────────────────────────────────
Total        862KB           853KB
```

---

## 3. 运行状态

### oect0 (v1.0.2)
```
进程:   /usr/local/bin/gatekeeper /etc/gatekeeper/gatekeeper-config.json
PID:    1485
用户:   user
启动:   2026-07-24 10:27 (自升级后)
端口:   0.0.0.0:3000
Systemd: active (running)
```

### oect1 (v1.0.2)
```
进程:   /usr/local/bin/gatekeeper /usr/local/etc/gatekeeper/config.json
PID:    920
用户:   user
启动:   2026-07-24 09:26 (手动安装)
端口:   0.0.0.0:3000
Systemd: active (running)
```

### 差异点
- **配置路径不同**: oect0 用 `/etc/gatekeeper/gatekeeper-config.json`，oect1 用 `/usr/local/etc/gatekeeper/config.json`
- **启动时间**: oect1 早 ~1 小时（手动安装在前）

---

## 4. 升级过程总结

1. **备份旧二进制** → `gatekeeper.bak` (862KB, v1.0.1)
2. **执行 self-update** → `./gatekeeper self-update`
3. **旧进程自动退出** → PID 7490 终止
4. **systemd 自动拉起新进程** → PID 1485，v1.0.2
5. **运行用户自动切换** → root → user（新二进制行为）
6. **跨机验证** → oect0 与 oect1 MD5 完全一致 ✅

---

## 5. v1.0.2 主要变化推测

基于二进制对比，v1.0.2 可能的改进：

1. **新增 `--version` 支持** — 旧版无此功能，新版返回 `gatekeeper v1.0.2`
2. **编译工具链升级** — 使用 clang 19.1.7 + LLD 19.1.7 (Fedora 42)，旧版工具链未知
3. **二进制精简** — 文件减小 ~1%，字符串和符号数均减少，可能去除了调试符号或无用字符串
4. **运行用户变更** — 新二进制默认以 `user` 身份运行（之前以 root），更安全
5. **自升级机制** — 支持 `self-update` 命令自动拉取新二进制并重启

---

## 6. 二进制备份

| 文件 | 版本 | 大小 | 位置 |
|------|------|------|------|
| `gatekeeper` | v1.0.2 | 853KB | `/usr/local/bin/gatekeeper` |
| `gatekeeper.bak` | v1.0.1 | 862KB | `/usr/local/bin/gatekeeper.bak` |

**如需要回滚**: `cp /usr/local/bin/gatekeeper.bak /usr/local/bin/gatekeeper && systemctl restart gatekeeper`
