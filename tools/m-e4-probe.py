# -*- coding: utf-8 -*-
# M-E4a/b/d 机械验收：python 假编辑器 server（真跨进程）。
# viewer --scene ... --editor-bridge 作为 client 连上 29110，本脚本收 announce 后：
#   ① M-E4b caps：announce 带窗体像素尺寸 + 后端名
#   ② M-E4d scene.getState：5 实例表（mesh/pos/tint/rough/metal）
#   ③ M-E4d scene.focus：orbit 相机对准对象 i（yaw/pitch/dist 断言）+ 负向索引
# 协议：4 字节大端长度前缀 + msgpack 信封 Map{jsonrpc:1,id,type,method,params}。
import socket, struct, subprocess, sys, time, os

PORT = 29110
fails = []
def check(name, cond, detail=""):
    print(("PASS  " if cond else "FAIL  ") + name + ("  | " + detail if detail else ""))
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
        if o == 0xC0: return None
        if o == 0xC2: return False
        if o == 0xC3: return True
        if o == 0xC7:
            self.u(1); self.u(1); return None
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

# ---------- 帧与信封 ----------
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

# ---------- 主流程 ----------
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT)); srv.listen(1)
print("listening on 127.0.0.1:%d ..." % PORT)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
scene = os.path.join(ROOT, "example", "ecs-e1b", "ecs-e1b.scene")
proc = subprocess.Popen([os.path.join(ROOT, "build", "DX12", "viewer.exe"),
                         "--scene", scene, "--editor-bridge", "--no-hud"])
try:
    srv.settimeout(15)
    conn, addr = srv.accept()
    conn.settimeout(5)
    print("viewer connected:", addr)

    # 1) announce（M-E4b caps 断言）
    ann = None
    deadline = time.time() + 10
    while time.time() < deadline and ann is None:
        r = recv_msg(conn)
        if isinstance(r, dict) and r.get("type") == "notify" and r.get("method") == "engine.announce":
            ann = r.get("params")
    check("engine.announce received", ann is not None, str(ann)[:120])
    caps = (ann or {}).get("caps") or {}
    check("caps.w/h == 1280x720 (窗口像素尺寸)",
          caps.get("w") == 1280 and caps.get("h") == 720, str(caps))
    check("caps.backend == DX12", caps.get("backend") == "DX12", str(caps))

    # 2) scene.getState（E1b 场景 = 5 物体）
    r = req(conn, "scene.getState")
    check("scene.getState ok", ok_of(r), str(err_of(r))[:100])
    st = result_of(r) or {}
    check("mode == scene", st.get("mode") == "scene", str(st.get("mode")))
    check("count == 5", st.get("count") == 5, str(st.get("count")))
    objs = st.get("objects") or []
    check("objects array len 5", len(objs) == 5, str(len(objs)))
    if objs:
        o0 = objs[0]
        check("obj fields mesh/pos/tint/rough/metal",
              all(k in o0 for k in ("mesh", "pos", "tint", "rough", "metal")), str(o0)[:140])
        meshes = [o.get("mesh", "?") for o in objs]
        check("all mesh == Suzanne.glb (E1b 全 Suzanne)", all(m.endswith("Suzanne.glb") for m in meshes), str(meshes))
        # 位置分布（E1b：不同 TRS）——至少出现 2 个不同位置
        poss = [tuple(round(p, 3) for p in o.get("pos", [])) for o in objs]
        check("positions non-degenerate", len(set(poss)) >= 2, str(poss))

    # 3) scene.focus 正向：取 pos 的理论 yaw/pitch，断言引擎回读一致
    p0 = objs[0]["pos"]
    import math
    px, py, pz = p0
    r_xz = math.sqrt(px * px + pz * pz)
    ln = math.sqrt(px * px + py * py + pz * pz)
    exp_yaw = math.degrees(math.atan2(px, pz))
    exp_pitch = math.degrees(math.atan2(py, r_xz))
    r = req(conn, "scene.focus", {"i": 0})
    check("scene.focus[0] ok", ok_of(r), str(err_of(r))[:100])
    fr = result_of(r) or {}
    check("focus yaw -> %.2f" % exp_yaw, abs(fr.get("yaw", -999) - exp_yaw) < 0.5,
          "got %.3f" % fr.get("yaw", -999))
    check("focus pitch -> %.2f" % exp_pitch, abs(fr.get("pitch", -999) - exp_pitch) < 0.5,
          "got %.3f" % fr.get("pitch", -999))
    check("focus dist -> |P|*2.5=%.2f" % (ln * 2.5), abs(fr.get("dist", -1) - ln * 2.5) < 0.5,
          "got %.3f" % fr.get("dist", -1))
    # getState 回读与 focus 返回一致（相机真被移动了）
    st2 = req(conn, "scene.getState")
    st2r = result_of(st2) or {}
    check("getState yaw==focus yaw", abs(st2r.get("yaw", -999) - fr.get("yaw", -998)) < 0.01,
          "state=%.3f focus=%.3f" % (st2r.get("yaw", -999), fr.get("yaw", -998)))

    # 4) 负向：索引越界
    r = req(conn, "scene.focus", {"i": 99})
    check("focus[99] -> error 100", r.get("ok") is False and err_of(r).get("code") == 100,
          str(err_of(r))[:100])
    r = req(conn, "scene.focus", {"i": -1})
    check("focus[-1] -> error 100", r.get("ok") is False and err_of(r).get("code") == 100,
          str(err_of(r))[:100])

    # 5) 未知方法仍 error 100
    r = req(conn, "scene.bogus")
    check("scene.bogus -> error 100", r.get("ok") is False and err_of(r).get("code") == 100,
          str(err_of(r))[:100])

    print("\n==== M-E4 cross-process: %d fail ====" % len(fails))
    sys.exit(1 if fails else 0)
finally:
    try: proc.kill()
    except Exception: pass
