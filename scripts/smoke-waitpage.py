#!/usr/bin/env python3
"""等待页「快速调整等待延时」按钮冒烟测试:
- 按钮分组标签(增减/设为)与 ±10/+10 按钮存在且顺序正确
- 增减/设为 基于已确认总延时 target(不随倒计时递减), 出队时才计算 → 连点不丢操作
- 串行队列: 每次点击入队, 上一次请求确认后才发下一个 → 无并发覆盖、无双倒计时链
- 停用: set 0 → 清 meta, 不 reload
用法: scripts/smoke-waitpage.py [gatekeeper 二进制路径]  (默认 ./build/linux/x86_64/gatekeeper)
"""
import json, os, re, subprocess, sys, tempfile, time, urllib.request

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/linux/x86_64/gatekeeper"
if not os.path.exists(BIN):
    print(f"找不到二进制: {BIN}"); sys.exit(2)

PORT = 13782
CTL = 13999
CFG = tempfile.mktemp(suffix=".json")
open(CFG, "w").write(json.dumps({
    "control": {"listen": f"127.0.0.1:{CTL}", "auth": {"type": "token", "token": "t"}},
    "ports": [{
        "name": "web", "enabled": True, "listen": f"127.0.0.1:{PORT}",
        "command": "/bin/sleep 30", "delay": 2000,
        "refresh_seconds": 60, "auto_restart": True
    }]
}))

proc = subprocess.Popen([BIN, CFG], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(50):
        try:
            r = urllib.request.urlopen(f"http://127.0.0.1:{PORT}/", timeout=1)
            html = r.read().decode("utf-8", "replace"); break
        except OSError:
            time.sleep(0.1)
    else:
        print("FAIL: gatekeeper 未就绪"); sys.exit(1)
finally:
    proc.kill(); proc.wait(timeout=5)

fails = []
def check(name, cond):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond: fails.append(name)

# 1) 页面结构
check("页面含『快速调整等待延时』", "快速调整等待延时" in html)
check("按钮分组 增减/设为", ">增减:</span>" in html and ">设为:</span>" in html)
for x in ['adjustRefresh(-10)">-10秒', 'adjustRefresh(-5)">-5秒', 'adjustRefresh(-1)">-1秒',
          'adjustRefresh(1)">+1秒', 'adjustRefresh(5)">+5秒', 'adjustRefresh(10)">+10秒',
          'onclick="setRefresh(1)">1秒']:
    assert x in html, x
order = [html.index(x) for x in
         ['adjustRefresh(-10)">-10秒', 'adjustRefresh(-5)">-5秒', 'adjustRefresh(-1)">-1秒',
          'adjustRefresh(1)">+1秒', 'adjustRefresh(5)">+5秒', 'adjustRefresh(10)">+10秒',
          'onclick="setRefresh(1)">1秒']]
check("增减按钮顺序 -10..+10 且先于设为组", order == sorted(order))
check("居中放大 + 分组行", "text-align:center;font-size:larger" in html)
check("无 base/剩余倒计时残留", "var base" not in html and "var n = secs + delta" not in html)
check("基于 target 的队列", "var target = 60" in html and "target + op.val" in html)

# 2) 抽出页面真实脚本, node 驱动(已确认 target=60, 倒计时剩 25):
#    连点 +5、+1(不等响应)→ 串行: 65→finish→66→finish; 再设为 10 → 10; 再 -100 → 停用
script = re.search(r"<script>(.*?)</script>", html, re.S).group(1)
script = script.replace("timer = setTimeout(tick, 1000);", "/* tick loop disabled in harness */")
harness = r"""
var __metaEl = {
  content: null,
  setAttribute: function(k,v){ if(k==='content') this.content = String(v); },
  parentNode: { removeChild: function(){ __metaEl.content = null; } }
};
var __st = {};
var document = {
  getElementById: function(id){ return __st[id] || (__st[id] = { style: {} }); },
  querySelector: function(){ return __metaEl.content === null ? null : __metaEl; },
  createElement: function(){ return __metaEl; },
  head: { appendChild: function(){} }
};
var location = { hostname: 'x', reload: function(){ __st.reloaded = true; } };
var __fetchLog = [];
var fetch = function(url){
  var m = /secs=(-?\d+)/.exec(String(url));
  var n = m ? +m[1] : NaN;
  __fetchLog.push(n);
  /* 模拟服务器确认值 */
  return Promise.resolve({ json: function(){ return Promise.resolve({ ok: true, refresh_seconds: n }); } });
};
"""
drive = r"""
""" + script + r"""
/* 已确认 target=60; 页面倒计时已跑掉一些(模拟剩 25, 但不改 target) */
secs = 25;
/* 连点 +5、+1, 不等响应: 都应基于最新确认的 target(60→65→66), 不是递减的 secs */
adjustRefresh(5);
adjustRefresh(1);
/* 等两个串行请求全部完成 */
setTimeout(function(){
  console.log('A1|' + target + '|' + secs + '|' + __metaEl.content + '|' + (!!__st.reloaded));
  setRefresh(10);                 /* 设为 10 */
  setTimeout(function(){
    console.log('A2|' + target + '|' + __metaEl.content);
    adjustRefresh(-100);          /* 10-100 → 0 停用 */
    setTimeout(function(){
      console.log('A3|' + target + '|' + __metaEl.content + '|' + enabled + '|' + (!!__st.reloaded));
      console.log('A4|' + __fetchLog.join(','));
    }, 50);
  }, 50);
}, 50);
"""
tmpdir = tempfile.mkdtemp()
jsfile = os.path.join(tmpdir, "t.js")
open(jsfile, "w").write(harness + drive)
node = subprocess.run(["node", jsfile], capture_output=True, text=True)
lines = node.stdout.splitlines()
def val(prefix):
    l = next((x for x in lines if x.startswith(prefix + "|")), "")
    return l[len(prefix) + 1:].split("|") if l else []
a1, a2 = val("A1"), val("A2")
a3 = val("A3")
a4 = next((x for x in lines if x.startswith("A4|")), "")
if node.returncode != 0 or not a1:
    check("node 驱动页面脚本", False)
    print("  node stdout:", node.stdout[:600])
    print("  node stderr:", node.stderr[:600]); sys.exit(1)
check("node 驱动页面脚本", True)
check("60+5,+1 连点串行 → target=66, secs=65(tick 已减 1)", a1[:2] == ["66", "65"])
check("meta 同步 66, 未 reload", a1[2] == "66" and a1[3] == "false")
check("设为 10 → target=10", a2[:2] == ["10", "10"])
check("10-100 → 夹到 0 停用, 清 meta, 无 reload", a3[0] == "0" and a3[1] == "null" and a3[2] == "false" and a3[3] == "false")
seq = [int(x) for x in a4.split("A4|")[1].split(",")] if a4 else []
check("请求序列 65→66→10→0(串行, 每次基于最新 target)", seq == [65, 66, 10, 0])

os.remove(CFG)
print("\n%s" % ("全部通过" if not fails else f"失败 {len(fails)} 项: {fails}"))
sys.exit(0 if not fails else 1)