#!/usr/bin/env python3
"""N1 修复冒烟测试: proxy+userpass 模式下 NO_AUTH 必须被拒, USER/PASS 正常通过
用法: scripts/smoke-n1.py [gatekeeper 二进制路径]  (默认 ./build/linux/x86_64/gatekeeper)
"""
import socket, subprocess, sys, tempfile, json, time, os

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/linux/x86_64/gatekeeper"
if not os.path.exists(BIN):
    print(f"找不到二进制: {BIN}"); sys.exit(2)
PORT = 18080

CFG = tempfile.mktemp(suffix=".json")
open(CFG, "w").write(json.dumps({
    "ports": [{
        "name": "s5",
        "listen": f"127.0.0.1:{PORT}",
        "mode": "proxy",
        "auth": {"type": "userpass", "username": "admin", "password": "secret123"}
    }]
}))

proc = subprocess.Popen(
    [BIN, CFG],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    # 等待端口就绪
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), 0.5); s.close(); break
        except OSError:
            time.sleep(0.1)
    else:
        print("FAIL: gatekeeper 未就绪"); sys.exit(1)

    # 用例1: 仅 NO_AUTH → 期望 0x05 0xFF (拒绝)
    s = socket.create_connection(("127.0.0.1", PORT), 2)
    s.sendall(bytes([0x05, 0x01, 0x00]))
    resp = s.recv(2)
    s.close()
    ok1 = resp == bytes([0x05, 0xFF])
    print(f"用例1 仅NO_AUTH: 收到 {resp.hex()} → {'PASS' if ok1 else 'FAIL'}")

    # 用例2: USER/PASS 正确凭证 → 期望 0x05 0x02, 认证 0x01 0x00
    s = socket.create_connection(("127.0.0.1", PORT), 2)
    s.sendall(bytes([0x05, 0x01, 0x02]))
    resp = s.recv(2)
    ok2a = resp == bytes([0x05, 0x02])
    if ok2a:
        u, p = b"admin", b"secret123"
        s.sendall(bytes([0x01, len(u)]) + u + bytes([len(p)]) + p)
        resp = s.recv(2)
        ok2 = resp == bytes([0x01, 0x00])
    else:
        ok2 = False
    s.close()
    print(f"用例2 USER/PASS: 方法 {resp.hex() if not ok2a else '0x0502'} 认证 {resp.hex()} → {'PASS' if ok2 else 'FAIL'}")

    # 用例3: USER/PASS 错误密码 → 期望 0x01 0x01 (拒绝)
    s = socket.create_connection(("127.0.0.1", PORT), 2)
    s.sendall(bytes([0x05, 0x01, 0x02]))
    s.recv(2)
    u, p = b"admin", b"wrong"
    s.sendall(bytes([0x01, len(u)]) + u + bytes([len(p)]) + p)
    resp = s.recv(2)
    s.close()
    ok3 = resp == bytes([0x01, 0x01])
    print(f"用例3 错密码: 收到 {resp.hex()} → {'PASS' if ok3 else 'FAIL'}")

    sys.exit(0 if (ok1 and ok2 and ok3) else 1)
finally:
    proc.terminate()
    try: proc.wait(5)
    except subprocess.TimeoutExpired: proc.kill()
    os.unlink(CFG)
