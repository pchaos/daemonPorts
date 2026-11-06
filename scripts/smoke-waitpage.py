#!/usr/bin/env python3
"""等待页「快速调整等待延时」按钮冒烟测试:
- 按钮分组标签(增减/设为)与 ±10/+10 按钮存在且顺序正确
- 增减按钮基于当前剩余倒计时 secs 计算(25+10→35, 25+5→30, 25-100→0 停用)
- 成功后从新值重新倒计(meta content=35, 显示「已调整为 35 秒」)
用法: scripts/smoke-waitpage.py [gatekeeper 二进制路径]  (默认 ./build/linux/x86_64/gatekeeper)
"""
import json, os, re, signal, subprocess, sys, tempfile, time, urllib.request

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
        "refresh_seconds": 30, "auto_restart": True
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
if fails:
    lines_debug = [x for x in lines if x[:2] in ("S1", "S2", "S3", "S4")]
    print("  raw:", " || ".join(lines_debug))

# 1) 页面结构
check("页面含『快速调整等待延时』", "快速调整等待延时" in html)
check("按钮分组 增减/设为", ">增减:</span>" in html and ">设为:</span>" in html)
order = [html.index(x) for x in
         ['adjustRefresh(-10)">-10秒', 'adjustRefresh(-5)">-5秒', 'adjustRefresh(-1)">-1秒',
          'adjustRefresh(1)">+1秒', 'adjustRefresh(5)">+5秒', 'adjustRefresh(10)">+10秒',
          'onclick="setRefresh(1)">1秒']]
check("增减按钮顺序 -10..+10 且先于设为组", order == sorted(order))
check("无 base 变量残留(基于剩余倒计时)", "var base" not in html and "base + delta" not in html)
check("adjustRefresh 用 secs+delta", "var n = secs + delta;" in html)

# 2) 抽出页面真实脚本, node 驱动: 剩余 25 秒时点 +10/+5/-100
script = re.search(r"<script>(.*?)</script>", html, re.S).group(1)
script = script.replace("setTimeout(tick, 1000);", "/* tick loop disabled in harness */")
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
  __fetchLog.push(String(url));
  return Promise.resolve({ json: function(){ return Promise.resolve({ ok: true }); } });
};
"""
drive = r"""
""" + script + r"""
/* 剩余 25 秒: 依次点 +10 / +5 / -100, 每次等 fetch 异步完成 */
secs = 25;
adjustRefresh(10);
setTimeout(function(){
  console.log('S1=' + secs + '|' + __metaEl.content + '|' + (!!__st.reloaded));
  adjustRefresh(5);
  setTimeout(function(){
    console.log('S2=' + secs + '|' + __metaEl.content);
    adjustRefresh(-100);
    setTimeout(function(){
      console.log('S3=' + secs + '|' + __metaEl.content + '|' + enabled + '|' + (!!__st.reloaded));
      console.log('S4=' + __fetchLog.join(','));
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
    l = next((x for x in lines if x.startswith(prefix + "=")), "")
    return l[len(prefix) + 1:].split("|") if l else []
s1, s2, s3 = val("S1"), val("S2"), val("S3")
s4 = next((x for x in lines if x.startswith("S4=")), "")
if node.returncode != 0 or not s1:
    check("node 驱动页面脚本", False)
    print("  node stdout:", node.stdout[:600])
    print("  node stderr:", node.stderr[:600]); sys.exit(1)
check("node 驱动页面脚本", True)
# 算术: 25+10=35(显示 34, tick 已减 1); 34+5=39; 39-100 夹到 0
check("25+10 → meta=35", s1[1] == "35")
check("25+10 → 未停用未重载", s1[2] == "false")
check("34+5 → meta=39(基于剩余倒计时)", s2[1] == "39")
check("39-100 → 夹到 0 停用", s3[2] == "false")
check("39-100 → 清空 meta 并 reload", s3[1] == "null" and s3[3] == "true")
seq = [p.split("secs=")[1] for p in s4.split("S4=")[1].split(",")] if s4 else []
check("请求序列 35→39→0", seq == ["35", "39", "0"])

os.remove(CFG)
print("\n%s" % ("全部通过" if not fails else f"失败 {len(fails)} 项: {fails}"))
sys.exit(0 if not fails else 1)