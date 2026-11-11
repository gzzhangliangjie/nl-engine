# -*- coding: utf-8 -*-
# M-E5 机械验收：python 假编辑器 server（真跨进程）+ editor.exe 插件 DLL 加载检查。
#   ① M-E5 scene 模式：scene.getMaterials 实例材质字段 + scene.setMaterial 写回（rough/tint）
#      + 负向（坏索引 error 100）
#   ② M-E5 model 模式（--mode cluster --model Suzanne-materials.nmesh）：
#      getMaterials 返回 .nmesh NSUB 表（meshlets=711, submeshCount=3, 材质 id=子网格序号）
#      + setMaterial 拒绝（model 模式无实例表 → error 100）
#   ③ editor.exe plugins/ 动态扫描：hello_plugin.dll 载入日志 + 坏 DLL 跳过日志
# 协议：4 字节大端长度前缀 + msgpack 信封 Map{jsonrpc:1,id,type,method,params}。
import socket, struct, subprocess, sys, time, os, shutil

PORT = 29110
fails = []
def check(name, cond, detail=""):
    print(("PASS  " if cond else "FAIL  ") + name + ("  | " + detail if detail else ""), flush=True)
    if not cond: fails.append(name)

# ---------- 极简 msgpack（只覆盖本测试所需子集） ----------
def enc(v):
    if v is None: return b"\xc0"
    if v is True: return b"\xc3"
    if v is False: return b"\xc2"
    if isinstance(v, int):
        if 0 <= v < 128: return bytes([v])
        return b"\xd3" + struct.pack(">q", v)          # int64
    if isinstance(v, float): return b"\xcb" + struct.pack(">d", v)
    if isinstance(v, str):
        b = v.encode("utf-8"); assert len(b) < 32
        return bytes([0xA0 | len(b)]) + b
    if isinstance(v, list):
        assert len(v) < 16
        return bytes([0x90 | len(v)]) + b"".join(enc(x) for x in v)
    if isinstance(v, dict):
        assert len(v) < 16
        out = bytes([0x80 | len(v)])
        for k, x in v.items(): out += enc(k) + enc(x)
        return out
    raise TypeError(type(v))

class Dec:
    def __init__(self, b): self.b, self.i = b, 0
    def u(self, n):
        r = int.from_bytes(self.b[self.i:self.i+n], "big"); self.i += n; return r
    def val(self):
        o = self.u(1)
        if o < 0x80: return o
        if (o & 0xF0) == 0x80:  # fixmap
            n = o & 0x0F; d = {}
            for _ in range(n):
                k = self.val(); d[k] = self.val()
            return d
        if (o & 0xF0) == 0x90:  # fixarray
            n = o & 0x0F; return [self.val() for _ in range(n)]
        if (o & 0xE0) == 0xA0:  # fixstr (0xA0..0xBF, len 0..31)
            n = o & 0x1F; r = self.b[self.i:self.i+n]; self.i += n
            return r.decode("utf-8")
        if o == 0xD9:  # str8
            n = self.u(1); r = self.b[self.i:self.i+n]; self.i += n
            return r.decode("utf-8")
        if o == 0xDA:  # str16
            n = self.u(2); r = self.b[self.i:self.i+n]; self.i += n
            return r.decode("utf-8")
        if o == 0xDB:  # str32
            n = self.u(4); r = self.b[self.i:self.i+n]; self.i += n
            return r.decode("utf-8")
        if o in (0xC4, 0xC5, 0xC6):  # bin8/16/32
            n = self.u({0xC4: 1, 0xC5: 2, 0xC6: 4}[o]); r = self.b[self.i:self.i+n]; self.i += n
            return r
        if o == 0xC0: return None
        if o == 0xC2: return False
        if o == 0xC3: return True
        if o == 0xCA:
            r = struct.unpack(">f", self.b[self.i:self.i+4])[0]; self.i += 4; return r
        if o == 0xCB:
            r = struct.unpack(">d", self.b[self.i:self.i+8])[0]; self.i += 8; return r
        if o == 0xCC: return self.u(1)
        if o == 0xCD: return self.u(2)
        if o == 0xCE: return self.u(4)
        if o == 0xCF: return self.u(8)
        if o == 0xD0: r = self.u(1); return r - 256 if r > 127 else r
        if o == 0xD1: r = self.u(2); return r - 65536 if r > 32767 else r
        if o == 0xD2: r = self.u(4); return r - (1 << 32) if r > (1 << 31) - 1 else r
        if o == 0xD3:
            r = self.u(8); return r - (1 << 64) if r > (1 << 63) - 1 else r
        raise ValueError("msgpack op 0x%02x" % o)

def dec(b): return Dec(b).val()

def send_msg(sock, obj):
    body = enc(obj)
    sock.sendall(struct.pack(">I", len(body)) + body)

def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        c = sock.recv(n - len(buf))
        if not c: raise ConnectionError("peer closed")
        buf += c
    return buf

def recv_msg(sock):
    ln = struct.unpack(">I", recv_exact(sock, 4))[0]
    return dec(recv_exact(sock, ln))

_next_id = [100]
def req(sock, method, params=None):
    _next_id[0] += 1
    rid = _next_id[0]
    m = {"jsonrpc": 1, "id": rid, "type": "req", "method": method}
    m["params"] = params if params is not None else None
    send_msg(sock, m)
    deadline = time.time() + 5
    while time.time() < deadline:
        r = recv_msg(sock)
        if isinstance(r, dict) and r.get("type") == "resp" and r.get("id") == rid:
            return r
    raise TimeoutError(method)

def ok_of(r): return r.get("ok") is True
def result_of(r): return r.get("result")
def err_of(r): return r.get("error") or {}

def wait_announce(conn, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        r = recv_msg(conn)
        if isinstance(r, dict) and r.get("type") == "notify" and r.get("method") == "engine.announce":
            return r.get("params")
    return None

def start_server():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT)); srv.listen(1)
    return srv

ROOT = r"E:\UE_STUDY_2026\nl-engine-editor"
EXE = os.path.join(ROOT, "build", "DX12", "viewer.exe")
SCENE = os.path.join(ROOT, "example", "ecs-e1b", "ecs-e1b.scene")

# ================= 段 ①：scene 模式 材质读写 =================
print("---- segment 1: scene mode ----")
srv = start_server()
proc = subprocess.Popen([EXE, "--scene", SCENE, "--editor-bridge", "--no-hud"])
try:
    srv.settimeout(15)
    conn, _ = srv.accept()
    conn.settimeout(5)
    ann = wait_announce(conn)
    check("scene: announce received", ann is not None)

    r = req(conn, "scene.getMaterials")
    check("scene.getMaterials ok", ok_of(r), str(err_of(r))[:100])
    st = result_of(r) or {}
    check("mode == scene", st.get("mode") == "scene", str(st.get("mode")))
    check("count == 5", st.get("count") == 5, str(st.get("count")))
    objs = st.get("objects") or []
    check("objects len 5", len(objs) == 5, str(len(objs)))
    o0 = objs[0] if objs else {}
    check("fields i/mesh/tint/rough/metal",
          all(k in o0 for k in ("i", "mesh", "tint", "rough", "metal")), str(o0)[:140])
    base_rough = o0.get("rough", -1)
    base_tint = o0.get("tint", [])

    # 写回 rough
    r = req(conn, "scene.setMaterial", {"i": 0, "rough": 0.5})
    check("setMaterial rough ok", ok_of(r), str(err_of(r))[:100])
    rr = result_of(r) or {}
    check("setMaterial ack rough==0.5", abs(rr.get("rough", -1) - 0.5) < 1e-4, str(rr)[:120])
    st = result_of(req(conn, "scene.getMaterials")) or {}
    check("getMaterials rough==0.5 (写回生效)",
          abs((st.get("objects") or [{}])[0].get("rough", -1) - 0.5) < 1e-4)

    # 写回 tint
    r = req(conn, "scene.setMaterial", {"i": 0, "tint": [0.25, 0.5, 0.75]})
    check("setMaterial tint ok", ok_of(r), str(err_of(r))[:100])
    st = result_of(req(conn, "scene.getMaterials")) or {}
    got = (st.get("objects") or [{}])[0].get("tint", [])
    check("getMaterials tint==[0.25,0.5,0.75]",
          len(got) == 3 and all(abs(a - b) < 1e-4 for a, b in zip(got, [0.25, 0.5, 0.75])), str(got))

    # clamp 负向：rough 5 → clamp 1
    r = req(conn, "scene.setMaterial", {"i": 0, "rough": 5.0})
    rr = result_of(r) or {}
    check("rough 5 clamped to 1", ok_of(r) and abs(rr.get("rough", -1) - 1.0) < 1e-4, str(rr)[:120])

    # 负向索引
    r = req(conn, "scene.setMaterial", {"i": 99, "rough": 0.5})
    check("setMaterial[99] -> error 100", r.get("ok") is False and err_of(r).get("code") == 100,
          str(err_of(r))[:100])
    r = req(conn, "scene.setMaterial", {"i": -1})
    check("setMaterial[-1] -> error 100", r.get("ok") is False and err_of(r).get("code") == 100,
          str(err_of(r))[:100])

    # 既有接口不回归：scene.getState / scene.focus
    r = req(conn, "scene.getState")
    check("scene.getState still ok (回归)", ok_of(r) and (result_of(r) or {}).get("count") == 5,
          str(err_of(r))[:80])
    conn.close()
finally:
    try: proc.kill()
    except Exception: pass
    srv.close()
time.sleep(1.0)   # 等 viewer 进程退出释放端口

# ================= 段 ②：model 模式 .nmesh NSUB 表 =================
print("---- segment 2: model mode (cluster + Suzanne-materials.nmesh) ----")
srv = start_server()
proc = subprocess.Popen([EXE, "--mode", "cluster", "--model", "Suzanne-materials.nmesh",
                         "--editor-bridge", "--no-hud"])
try:
    srv.settimeout(20)
    conn, _ = srv.accept()
    conn.settimeout(5)
    ann = wait_announce(conn)
    check("model: announce received", ann is not None)

    r = req(conn, "scene.getMaterials")
    check("model.getMaterials ok", ok_of(r), str(err_of(r))[:100])
    st = result_of(r) or {}
    check("mode == model", st.get("mode") == "model", str(st.get("mode")))
    check("file == Suzanne-materials.nmesh", st.get("file") == "Suzanne-materials.nmesh", str(st.get("file")))
    nm = st.get("nmesh") or {}
    check("nmesh.meshlets == 711", nm.get("meshlets") == 711, str(nm.get("meshlets")))
    check("nmesh.submeshCount == 3 (NSUB 段)", nm.get("submeshCount") == 3, str(nm.get("submeshCount")))
    subs = nm.get("submeshes") or []
    check("submeshes len 3", len(subs) == 3, str(len(subs)))
    if subs:
        check("materialId == 子网格序号 0/1/2",
              [s.get("materialId") for s in subs] == [0, 1, 2], str([s.get("materialId") for s in subs]))
        check("submesh[0].clusterFirst == 0", subs[0].get("clusterFirst") == 0, str(subs[0]))
        check("submesh[2].clusterCount > 0", subs[2].get("clusterCount", 0) > 0, str(subs[2]))

    # 负向：model 模式无实例表，setMaterial 拒绝
    r = req(conn, "scene.setMaterial", {"i": 0, "rough": 0.5})
    check("model setMaterial -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:100])
    conn.close()
finally:
    try: proc.kill()
    except Exception: pass
    srv.close()
time.sleep(1.0)

# ================= 段 ③：editor.exe plugins/ DLL 动态扫描 =================
print("---- segment 3: editor plugin dll scan ----")
EXE_EDITOR = os.path.join(ROOT, "build", "DX12", "editor.exe")
PLUGINS_DIR = os.path.join(ROOT, "build", "DX12", "plugins")
check("hello_plugin.dll built into plugins/", os.path.isfile(os.path.join(PLUGINS_DIR, "editor_plugin_hello.dll")))
# 坏 DLL 注入：非插件 DLL（缺 C ABI 导出）→ 应跳过不拖死宿主
bad = os.path.join(PLUGINS_DIR, "_bad_test.dll")
bad_src = os.path.join(ROOT, "build", "DX12", "nl_imgui_dll.dll")
injected = False
if os.path.isfile(bad_src) and os.path.isdir(PLUGINS_DIR):
    shutil.copyfile(bad_src, bad)
    injected = True
try:
    proc = subprocess.Popen([EXE_EDITOR], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    out = b""
    try:
        out = proc.communicate(timeout=10)[0] or b""
    except subprocess.TimeoutExpired:
        proc.kill()
        out = proc.communicate()[0] or b""
    text = out.decode("utf-8", "replace")
    check("editor.exe ran (stdout alive)", "plugin" in text.lower() or len(out) > 0, text[:120].replace("\n", " "))
    lines = [l for l in text.splitlines() if "plugin" in l.lower()]
    check("hello dll loaded line", "plugin dll loaded: editor_plugin_hello.dll (Hello (DLL))" in text,
          " | ".join(lines[:3]))
    if injected:
        check("bad dll skipped line",
              "plugin dll _bad_test.dll skipped: missing C ABI exports" in text,
              " | ".join(lines[:3]))
finally:
    if injected and os.path.isfile(bad):
        try: os.remove(bad)
        except Exception: pass

print("\n==== M-E5 cross-process: %d fail ====" % len(fails))
sys.exit(1 if fails else 0)
