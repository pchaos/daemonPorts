#!/usr/bin/env python3
"""N3 修复冒烟测试: 驱逐期间控制接口不得被全局锁冻结

窗口 = 日志出现「执行关闭命令」→「驱逐完成」(stop_command sleep 5 + 轮询 + SIGTERM 宽限,
最长 ~45s)。窗口内持续探测 GET /status; 修复前 evict() 持 g_relaysMutex, /status 会被
阻塞 ~5s (stop_command), 修复后每次响应 < 500ms。
用法: scripts/smoke-n3.py [gatekeeper 二进制路径]
"""
import socket, subprocess, sys, tempfile, json, time, os, threading, urllib.request

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/linux/x86_64/gatekeeper"
if not os.path.exists(BIN):
    print(f"找不到二进制: {BIN}"); sys.exit(2)

# 驱逐需要 swap 已使用 (swapPercent > 0.1% 才进双满载判定);
# 无 swap 或 swap 全空闲的环境无法触发驱逐 → 跳过而非 FAIL
def swap_usable():
    try:
        with open("/proc/meminfo") as f:
            d = dict(line.split()[:2] for line in f if ":" in line)
        total = int(d.get("SwapTotal:", "0"))
        free = int(d.get("SwapFree:", "0"))
        return total > 0 and free < total
    except (OSError, ValueError):
        return False

if not swap_usable():
    print("SKIP: 本机无 swap 或 swap 全空闲, 驱逐条件无法构造")
    sys.exit(0)

PORT, CTRL = 18081, 12999
CFG = tempfile.mktemp(suffix=".json")
LOG = tempfile.mktemp(suffix=".log")
open(CFG, "w").write(json.dumps({
    "control": {
        "listen": f"127.0.0.1:{CTRL}",
        "auth": {"type": "token", "token": "n3test"},
        "max_connections": 1000,
        "rate_limit_seconds": 60
    },
    "ports": [{
        "name": "sleeper",
        "listen": f"127.0.0.1:{PORT}",
        "command": "sleep 300",
        "stop_command": "sleep 5",   # 慢 stop_command: 修复前锁内 ~5s
        "delay": 2000
    }],
    "system_monitor": {
        "enabled": True,
        "interval_seconds": 1,
        # 高负载(内存>99%)时也保持 1s 采样, 否则驱逐计时会被快周期(默认 60s)拖慢
        "fast_interval_seconds": 1,
        "memory_high_threshold": 0.99,
        "eviction": {
            "enabled": True,
            "memory_critical": 0.001,
            "swap_critical": 0.001,
            "sustain_seconds": 1
        }
    }
}))

proc = subprocess.Popen([BIN, CFG], stdout=open(LOG, "w"), stderr=subprocess.STDOUT)
try:
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", CTRL), 0.5).close(); break
        except OSError:
            time.sleep(0.1)
    else:
        print("FAIL: gatekeeper 未就绪"); sys.exit(1)

    # 触发后端启动
    s = socket.create_connection(("127.0.0.1", PORT), 2); s.close()

    # 持续探测 /status, 记录 (ts, 延迟)
    lat = []
    stop = threading.Event()
    def probe():
        while not stop.is_set():
            t0 = time.time()
            try:
                req = urllib.request.Request(f"http://127.0.0.1:{CTRL}/status",
                                             headers={"X-Auth-Token": "n3test"})
                urllib.request.urlopen(req, timeout=2).read()
                lat.append((time.time(), time.time() - t0))
            except Exception:
                lat.append((time.time(), time.time() - t0))
            time.sleep(0.03)
    th = threading.Thread(target=probe); th.start()

    def log_has(txt):
        try:
            return txt in open(LOG).read()
        except OSError:
            return False

    # 等驱逐开始 (执行 stop_command)
    t0 = time.time()
    while time.time() - t0 < 30 and not log_has("执行关闭命令"):
        time.sleep(0.2)
    if not log_has("执行关闭命令"):
        stop.set(); th.join()
        print("FAIL: 驱逐未触发"); sys.exit(1)

    # 等驱逐完成 (SIGKILL 兜底后日志出现「驱逐完成」, 最长 ~50s)
    t0 = time.time()
    while time.time() - t0 < 60 and not log_has("驱逐完成"):
        time.sleep(0.2)
    done = log_has("驱逐完成")
    stop.set(); th.join()

    if not done:
        print("FAIL: 驱逐未完成 (60s 超时)"); sys.exit(1)

    # 只看 stop_command(sleep 5)执行窗口 ≈ 从「执行关闭命令」起 5s 内的采样
    start = next(t for t, _ in lat)  # 保守: 全窗口最差
    worst = max(l for _, l in lat)
    print(f"/status 采样 {len(lat)} 次, 最差延迟 {worst*1000:.0f}ms")
    if worst < 0.5:
        print("PASS: 驱逐全程控制接口未被冻结 (修复前 stop_command 段会阻塞 ~5s)")
        sys.exit(0)
    print("FAIL: /status 在驱逐期间被阻塞 (锁仍被持住)"); sys.exit(1)
finally:
    proc.terminate()
    try: proc.wait(5)
    except subprocess.TimeoutExpired: proc.kill()
    for f in (CFG, LOG):
        try: os.unlink(f)
        except OSError: pass