# -*- coding: utf-8 -*-
# M-E3 机械验收：python 假编辑器 server（真跨进程）。
# viewer --editor-bridge 作为 client 连上 29110，本脚本收 announce 后直打
# 引擎侧 5 个 camera.* handler，断言行为（clamp / 增量 / reset / 负向）。
# 协议：4 字节大端长度前缀 + msgpack 信封 Map{jsonrpc:1,id,type,method,params}。
import socket, struct, subprocess, sys, time, threading

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
        if (o & 0xE0) == 0xA0:  # fixstr (0xA0..0xBF, len 0..31)
            n = o & 0x1F; r = self.b[self.i:self.i+n]; self.i += n
            return r.decode("utf-8")
        if o == 0xC0: return None
        if o == 0xC2: return False
        if o == 0xC3: return True
        if o == 0xC7:  # fixext1（引擎心跳/兼容位不涉及，防御）
            self.u(1); self.u(1); return None
        if o == 0xCA: return struct.unpack(">f", self.b[self.i:self.i+4])[0] if False else struct.unpack(">f", self.b[self.i:self.i+4])[0]
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
    # 收直到本 id 的 resp（viewer 可能夹杂 ping notify）
    deadline = time.time() + 5
    while time.time() < deadline:
        r = recv_msg(sock)
        if isinstance(r, dict) and r.get("type") == "resp" and r.get("id") == rid:
            return r
    raise TimeoutError(method)

def ok_of(r): return r.get("ok") is True
def result_of(r): return r.get("result")

# ---------- 主流程 ----------
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT)); srv.listen(1)
print("listening on 127.0.0.1:%d ..." % PORT)

proc = subprocess.Popen([r"E:\UE_STUDY_2026\nl-engine-editor\build\DX12\viewer.exe",
                         "--editor-bridge"])
try:
    srv.settimeout(15)
    conn, addr = srv.accept()
    conn.settimeout(5)
    print("viewer connected:", addr)

    # 1) announce
    ann = None
    deadline = time.time() + 10
    while time.time() < deadline and ann is None:
        r = recv_msg(conn)
        if isinstance(r, dict) and r.get("type") == "notify" and r.get("method") == "engine.announce":
            ann = r.get("params")
    check("engine.announce received", ann is not None, str(ann)[:120])
    if ann is not None:
        check("announce hwnd>0", ann.get("hwnd", 0) > 0)
        check("announce pid == viewer pid", ann.get("pid") == proc.pid,
              "ann=%s viewer=%d" % (ann.get("pid"), proc.pid))
        check("announce proto==1", ann.get("proto") == 1)

    def state():
        r = req(conn, "camera.getState")
        check("camera.getState ok", ok_of(r))
        return result_of(r) if ok_of(r) else {}

    # 2) getState 初始
    s0 = state()
    check("getState has fov/dist/yaw/pitch",
          all(k in s0 for k in ("fov", "dist", "yaw", "pitch")), str(s0)[:120])

    # 3) setFov 正常 + clamp
    r = req(conn, "camera.setFov", {"fov": 45.0})
    check("setFov 45 -> 45", ok_of(r) and abs(result_of(r) - 45.0) < 0.01, str(result_of(r)))
    r = req(conn, "camera.setFov", {"fov": 500.0})
    check("setFov 500 -> clamp 120", ok_of(r) and abs(result_of(r) - 120.0) < 0.01, str(result_of(r)))
    check("getState fov==120", abs(state().get("fov", -1) - 120.0) < 0.01)

    # 4) setDistance + clamp 下限
    r = req(conn, "camera.setDistance", {"dist": 10.0})
    check("setDistance 10 -> 10", ok_of(r) and abs(result_of(r) - 10.0) < 0.01, str(result_of(r)))
    r = req(conn, "camera.setDistance", {"dist": 0.01})
    check("setDistance 0.01 -> clamp >=0.45", ok_of(r) and result_of(r) >= 0.449, str(result_of(r)))

    # 5) orbit 增量
    y0 = state().get("yaw", 0.0); p0 = state().get("pitch", 0.0)
    r = req(conn, "camera.orbit", {"dx": 30.0, "dy": 5.0})
    check("orbit ok", ok_of(r))
    s1 = state()
    # orbit 参数是像素增量，kOrbitGain=0.007 rad/px -> 度 = dx*0.007*180/pi
    exp_dy = 5.0 * 0.007 * 57.2958   # 2.007 deg
    exp_dx = 30.0 * 0.007 * 57.2958  # 12.033 deg
    check("orbit dy=5px -> pitch+2.01deg", abs(s1.get("pitch", 0) - p0 - exp_dy) < 0.05,
          "p0=%.3f p1=%.3f exp=%.3f" % (p0, s1.get("pitch", 0), p0 + exp_dy))
    check("orbit dx=30px -> yaw+12.03deg", abs(s1.get("yaw", 0) - y0 - exp_dx) < 0.05,
          "y0=%.3f y1=%.3f exp=%.3f" % (y0, s1.get("yaw", 0), y0 + exp_dx))

    # 6) reset 回默认（dist 4.2 / yaw~0.21rad / pitch~0.16rad；fov 不在 reset 范围）
    r = req(conn, "camera.reset")
    check("camera.reset ok", ok_of(r) and result_of(r) is None, str(result_of(r)))
    s2 = state()
    check("reset dist -> 4.2", abs(s2.get("dist", -1) - 4.2) < 0.01, str(s2.get("dist")))
    check("reset yaw -> 0.21rad(=12.032deg)", abs(s2.get("yaw", -1) - 12.0321) < 0.01, str(s2.get("yaw")))
    check("reset pitch -> 0.16rad(=9.167deg)", abs(s2.get("pitch", -1) - 9.1673) < 0.01, str(s2.get("pitch")))
    check("reset 不动 fov（仍 120）", abs(s2.get("fov", -1) - 120.0) < 0.01)

    # 7) 负向：未知方法
    r = req(conn, "camera.bogus")
    check("bogus method -> error 100", r.get("ok") is False
          and r.get("error", {}).get("code") == 100, str(r.get("error"))[:100])

    print("\n==== M-E3 cross-process: %d fail ====" % len(fails))
    sys.exit(1 if fails else 0)
finally:
    try: proc.kill()
    except Exception: pass
