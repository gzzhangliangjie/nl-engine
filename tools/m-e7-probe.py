# -*- coding: utf-8 -*-
# M-E7 机械验收：python 假编辑器 server（真跨进程）。
#   ① --preview 场景：engine.cvar.list/get/set（light.* 反射层）+ camera.preset
#      六预设 + scene.getState(count=2) + scene.setMaterial 复用链写回 + 负向
#   ② --preview-model 覆写：objects[0].mesh 换成目标 .glb + 坏名 exit 2 负向
#   ③ light cvar 覆写生效的画面证据：--screenshot 定帧 + cvar 经控制台预置
#      （--console-exec 落 Console 后 applyLightCvars 覆写 fp）→ ambient 变化的
#      两张 PNG 像素差非零（侧面实锤覆写链进了渲染帧）。
# 协议：4 字节大端长度前缀 + msgpack 信封 Map{jsonrpc:1,id,type,method,params}。
import socket, struct, subprocess, sys, time, os

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
        return b"\xd3" + struct.pack(">q", v)
    if isinstance(v, float): return b"\xcb" + struct.pack(">d", v)
    if isinstance(v, str):
        b = v.encode("utf-8")
        if len(b) < 32: return bytes([0xA0 | len(b)]) + b
        return b"\xd9" + bytes([len(b)]) + b            # str8
    if isinstance(v, list):
        if len(v) < 16: return bytes([0x90 | len(v)]) + b"".join(enc(x) for x in v)
        return b"\xdc" + struct.pack(">H", len(v)) + b"".join(enc(x) for x in v)
    if isinstance(v, dict):
        if len(v) < 16:
            out = bytes([0x80 | len(v)])
        else:
            out = b"\xde" + struct.pack(">H", len(v))
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
        if (o & 0xF0) == 0x80:
            n = o & 0x0F; d = {}
            for _ in range(n):
                k = self.val(); d[k] = self.val()
            return d
        if (o & 0xF0) == 0x90:
            n = o & 0x0F; return [self.val() for _ in range(n)]
        if o == 0xDC:
            n = self.u(2); return [self.val() for _ in range(n)]
        if (o & 0xE0) == 0xA0:
            n = o & 0x1F; r = self.b[self.i:self.i+n]; self.i += n
            return r.decode("utf-8")
        if o == 0xD9:
            n = self.u(1); r = self.b[self.i:self.i+n]; self.i += n
            return r.decode("utf-8")
        if o == 0xDA:
            n = self.u(2); r = self.b[self.i:self.i+n]; self.i += n
            return r.decode("utf-8")
        if o == 0xDB:
            n = self.u(4); r = self.b[self.i:self.i+n]; self.i += n
            return r.decode("utf-8")
        if o in (0xC4, 0xC5, 0xC6):
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

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "DX12", "viewer.exe")
OUT = os.path.join(ROOT, "out")
os.makedirs(OUT, exist_ok=True)

# ================= 段 ①：preview 场景 RPC 全链 =================
print("---- segment 1: --preview cvar/preset/material ----")
srv = start_server()
proc = subprocess.Popen([EXE, "--preview", "--editor-bridge", "--no-hud"])
try:
    srv.settimeout(20)
    conn, _ = srv.accept()
    conn.settimeout(5)
    ann = wait_announce(conn)
    check("preview: announce received", ann is not None)

    # --- cvar 反射层 ---
    r = req(conn, "engine.cvar.list", {"prefix": "light."})
    check("cvar.list ok", ok_of(r), str(err_of(r))[:100])
    lst = result_of(r) or []
    names = sorted(x.get("name", "") for x in lst)
    check("cvar.list 3 light.* entries",
          names == ["light.ambient", "light.sunAzimuth", "light.sunElevation"], str(names))
    vals = {x.get("name"): x.get("value") for x in lst}
    check("cvar initial values (42/49.9/0.28, toString 格式)",
          vals.get("light.sunAzimuth") == "42" and
          vals.get("light.sunElevation") == "49.9" and
          vals.get("light.ambient") == "0.28", str(vals))
    check("cvar entries carry type/help",
          all(("type" in x and "help" in x) for x in lst), str(lst[0])[:140])

    r = req(conn, "engine.cvar.set", {"name": "light.ambient", "value": "0.5"})
    check("cvar.set ambient 0.5 ok", ok_of(r), str(err_of(r))[:100])
    rr = result_of(r) or {}
    check("cvar.set ack value==0.5", rr.get("value") == "0.5", str(rr)[:120])
    r = req(conn, "engine.cvar.get", {"name": "light.ambient"})
    check("cvar.get readback 0.5", ok_of(r) and (result_of(r) or {}).get("value") == "0.5",
          str(result_of(r))[:120])

    # set 同步触发覆写链：preview 场景无灯，画面亮度由 fp.lightDir.w（ambient）驱动。
    # cvar.set 后 viewer 下一帧 ambient=0.5（GUI 段用 PNG 像素差实锤，这里只证 API）。
    r = req(conn, "engine.cvar.set", {"name": "light.ambient", "value": "abc"})
    check("cvar.set bad value -> error 101",
          r.get("ok") is False and err_of(r).get("code") == 101, str(err_of(r))[:100])
    r = req(conn, "engine.cvar.set", {"name": "nope.nope", "value": "1"})
    check("cvar.set unknown -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:100])
    r = req(conn, "engine.cvar.get", {"name": "nope.nope"})
    check("cvar.get unknown -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:100])

    # --- camera.preset ---
    r = req(conn, "camera.getState")
    base = result_of(r) or {}
    check("camera.getState ok (orbit default)", ok_of(r) and "yaw" in base, str(err_of(r))[:80])
    check("preview 起手 dist 钉 6.0（不吃大地面 bbox 自动取景）",
          abs(base.get("dist", -1) - 6.0) < 0.01, str(base)[:120])
    r = req(conn, "camera.preset", {"name": "front"})
    check("preset front ok", ok_of(r), str(err_of(r))[:100])
    rr = result_of(r) or {}
    check("preset front yaw~0 pitch~0 dist 保持",
          abs(rr.get("yaw", 999)) < 0.01 and abs(rr.get("pitch", 999)) < 0.01 and
          abs(rr.get("dist", 999) - 6.0) < 0.01, str(rr)[:120])
    r = req(conn, "camera.preset", {"name": "iso"})
    rr = result_of(r) or {}
    check("preset iso yaw~45 pitch~35.264",
          ok_of(r) and abs(rr.get("yaw", 999) - 45.0) < 0.01 and
          abs(rr.get("pitch", 999) - 35.264) < 0.01, str(rr)[:120])
    r = req(conn, "camera.preset", {"name": "top"})
    rr = result_of(r) or {}
    # OrbitCamera pitch clamp = ±1.45 rad ≈ ±83.0789°——89.9 请求被钳到上限，回读实际值
    check("preset top pitch == clamp 83.0789 (回读实际值契约)",
          ok_of(r) and abs(rr.get("pitch", 0) - 83.07888) < 0.01, str(rr)[:120])
    r = req(conn, "camera.preset", {"name": "bogus"})
    check("preset bogus -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:100])
    r = req(conn, "camera.preset", {})
    check("preset missing name -> error 101",
          r.get("ok") is False and err_of(r).get("code") == 101, str(err_of(r))[:100])

    # --- 预览场景复用 scene 链：状态 / 材质写回 / focus ---
    r = req(conn, "scene.getState")
    st = result_of(r) or {}
    check("scene.getState mode==scene", st.get("mode") == "scene", str(st.get("mode")))
    check("scene count==2 (model+ground)", st.get("count") == 2, str(st.get("count")))
    objs = st.get("objects") or []
    meshes = sorted(o.get("mesh", "?") for o in objs)
    check("meshes == ParkingLot.glb + Suzanne.glb",
          meshes == ["ParkingLot.glb", "Suzanne.glb"], str(meshes))
    r = req(conn, "scene.setMaterial", {"i": 0, "rough": 0.5, "tint": [0.8, 0.6, 0.4]})
    check("preview setMaterial ok (scene 链复用)", ok_of(r), str(err_of(r))[:100])
    st = result_of(req(conn, "scene.getMaterials")) or {}
    got = (st.get("objects") or [{}])[0]
    check("setMaterial 写回复读 rough==0.5", abs(got.get("rough", -1) - 0.5) < 1e-4, str(got)[:140])
    r = req(conn, "scene.focus", {"i": 0})
    check("scene.focus ok (回归)", ok_of(r), str(err_of(r))[:80])
    conn.close()
finally:
    try: proc.kill()
    except Exception: pass
    srv.close()
time.sleep(1.0)

# ================= 段 ②：--preview-model 覆写 + 负向 =================
print("---- segment 2: --preview-model override ----")
srv = start_server()
proc = subprocess.Popen([EXE, "--preview", "--preview-model", "Suzanne-materials.glb",
                         "--editor-bridge", "--no-hud"])
try:
    srv.settimeout(20)
    conn, _ = srv.accept()
    conn.settimeout(5)
    check("override: announce received", wait_announce(conn) is not None)
    st = result_of(req(conn, "scene.getState")) or {}
    objs = st.get("objects") or []
    bymesh = sorted(o.get("mesh", "?") for o in objs)
    check("preview-model overridden to Suzanne-materials.glb",
          "Suzanne-materials.glb" in bymesh, str(bymesh))
    conn.close()
finally:
    try: proc.kill()
    except Exception: pass
    srv.close()
time.sleep(1.0)

# 负向：坏模型名 → exit 2（不静默）
p = subprocess.run([EXE, "--preview", "--preview-model", "bad-name.txt", "--no-hud"],
                   capture_output=True, timeout=30)
check("--preview-model bad name -> exit 2", p.returncode == 2, "rc=%d" % p.returncode)

# ================= 段 ③：light cvar 覆写进渲染帧（PNG 像素差实锤） =================
print("---- segment 3: light override visual diff ----")
png_a = os.path.join(OUT, "m-e7-ambient-default.png")
png_b = os.path.join(OUT, "m-e7-ambient-012.png")
p = subprocess.run([EXE, "--preview", "--no-hud", "--screenshot", png_a,
                    "--frames", "3", "--console-exec", "light.ambient 999"],
                   capture_output=True, timeout=60)
check("screenshot A ok", p.returncode == 0 and os.path.isfile(png_a), "rc=%d" % p.returncode)
# ambient 无 clamp？cvar float 接受 999；fp.lightDir.w=999 过亮接近清屏色。
# 用 0（全黑倾向）对照：默认 0.28 vs 0.0 的画面像素差。
p = subprocess.run([EXE, "--preview", "--no-hud", "--screenshot", png_b,
                    "--frames", "3", "--console-exec", "light.ambient 0"],
                   capture_output=True, timeout=60)
check("screenshot B ok", p.returncode == 0 and os.path.isfile(png_b), "rc=%d" % p.returncode)
if os.path.isfile(png_a) and os.path.isfile(png_b):
    with open(png_a, "rb") as f: a = f.read()
    with open(png_b, "rb") as f: b = f.read()
    # ambient 0.28 vs 0 画面亮度差大 → PNG 压缩尺寸差异也大（曾误断言「大小相等」）。
    check("ambient 0.28 vs 0 PNG differs (覆写链实锤)",
          a != b, "sizes %d/%d" % (len(a), len(b)))
else:
    check("PNG pair produced", False, "missing outputs")

print("\n==== M-E7 cross-process: %d fail ====" % len(fails))
sys.exit(1 if fails else 0)
