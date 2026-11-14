# -*- coding: utf-8 -*-
# M-E8 机械验收：python 假编辑器 server（真跨进程）。
#   ① --preview 层级树：scene.getTree 结构断言（mode/count/kind/idx/parent/name）
#   ② sceneChanged 推送：scene.setMaterial 改 tint → 引擎指纹变化 → notify
#      editor.sceneChanged 到达（真跨进程推送链实锤）+ 不改参数无推送（负向）
#   ③ model 模式：scene.getTree mode=model 空树（占位口径）
# 协议：4 字节大端长度前缀 + msgpack 信封 Map{jsonrpc:1,id,type,method,params}。
import socket, struct, subprocess, sys, time, os

PORT = 29110
fails = []
def check(name, cond, detail=""):
    print(("PASS  " if cond else "FAIL  ") + name + ("  | " + detail if detail else ""), flush=True)
    if not cond: fails.append(name)

# ---------- 极简 msgpack（只覆盖本测试所需子集，M-E7 骨架同款） ----------
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

def wait_notify(conn, method, timeout=5.0, keepalive=True):
    """等待指定 notify 到达（跳过 resp），超时返回 None。
    keepalive 用一次性 notify（engine.ping，引擎无此 handler 静默忽略）——
    不能用 req：req 的响应匹配循环会把路过的 sceneChanged notify 吞掉。"""
    deadline = time.time() + timeout
    next_ping = time.time() + 1.0
    old = conn.gettimeout()
    conn.settimeout(0.4)
    try:
        while time.time() < deadline:
            if keepalive and time.time() > next_ping:
                send_msg(conn, {"jsonrpc": 1, "id": 0, "type": "notify",
                                "method": "engine.ping", "params": None})
                next_ping = time.time() + 1.0
            try:
                r = recv_msg(conn)
            except (socket.timeout, TimeoutError):
                continue   # 短超时 = 无数据，继续到 deadline
            if isinstance(r, dict) and r.get("type") == "notify" and r.get("method") == method:
                return r
        return None
    finally:
        conn.settimeout(old)

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

# ================= 段 ①：--preview 层级树 =================
print("---- segment 1: scene.getTree on --preview ----")
srv = start_server()
proc = subprocess.Popen([EXE, "--preview", "--editor-bridge", "--no-hud"])
try:
    srv.settimeout(20)
    conn, _ = srv.accept()
    conn.settimeout(5)
    ann = wait_announce(conn)
    check("preview: announce received", ann is not None)

    r = req(conn, "scene.getTree")
    check("getTree ok", ok_of(r), str(err_of(r))[:100])
    tr = result_of(r) or {}
    check("getTree mode=scene", tr.get("mode") == "scene", str(tr.get("mode")))
    check("getTree count=2（preview 场景 2 实体）", tr.get("count") == 2, str(tr.get("count")))
    nodes = tr.get("nodes") or []
    byName = {n.get("name"): n for n in nodes if isinstance(n, dict)}
    pm = byName.get("preview-model")
    pg = byName.get("preview-ground")
    check("node preview-model present (kind=model idx=0 parent=0 mesh=Suzanne)",
          pm is not None and pm.get("kind") == "model" and pm.get("idx") == 0 and
          pm.get("parent") == 0 and pm.get("mesh") == "Suzanne.glb", str(pm)[:160])
    check("node preview-ground present (kind=model idx=1 parent=0)",
          pg is not None and pg.get("kind") == "model" and pg.get("idx") == 1 and
          pg.get("parent") == 0, str(pg)[:160])
    check("model node carries tint[3]/rough/metal",
          pm is not None and isinstance(pm.get("tint"), list) and len(pm["tint"]) == 3 and
          "rough" in pm and "metal" in pm, str(pm)[:160])
    check("preview 场景无灯实体（无 kind=light）",
          all(n.get("kind") != "light" for n in nodes), str([n.get("kind") for n in nodes]))

    # --- 段 ②：sceneChanged 推送 ---
    print("---- segment 2: editor.sceneChanged push ----")
    # 排空积压（前序请求的 resp 已在 req() 消费；等 0.6s 让初始化帧指纹 notify 流过——
    # 首帧指纹 init 也会触发一次 notify，先收掉它）。
    time.sleep(0.6)
    init_push = wait_notify(conn, "editor.sceneChanged", timeout=1.5)
    check("初始化指纹变化触发一次推送（首帧 init 或 --preview 生效帧）",
          init_push is not None)
    # 等待指纹稳定（连变帧可能连发），排空至 1s 静默
    while True:
        n = wait_notify(conn, "editor.sceneChanged", timeout=1.0)
        if n is None: break
    # 负向：不改参数，1.2s 内无推送
    n = wait_notify(conn, "editor.sceneChanged", timeout=1.2)
    check("参数不变 → 无推送（负向）", n is None)
    # 正向：setMaterial 改 tint → 指纹变 → 下一帧推送
    r = req(conn, "scene.setMaterial", {"i": 0, "tint": [0.2, 0.8, 0.4]})
    check("setMaterial tint ok", ok_of(r), str(err_of(r))[:100])
    push = wait_notify(conn, "editor.sceneChanged", timeout=3.0)
    check("改 tint → sceneChanged 推送到达（跨进程实锤）", push is not None)
    # 连续第二次改（防抖不丢第二变）
    r = req(conn, "scene.setMaterial", {"i": 0, "rough": 0.9})
    check("setMaterial rough ok", ok_of(r), str(err_of(r))[:100])
    push2 = wait_notify(conn, "editor.sceneChanged", timeout=3.0)
    check("改 rough → 第二次推送到达", push2 is not None)

    # --- getTree 读回写回值（树数据实时性侧面证据） ---
    r = req(conn, "scene.getTree")
    tr = result_of(r) or {}
    nodes = tr.get("nodes") or []
    pm2 = next((x for x in nodes if x.get("name") == "preview-model"), {})
    check("getTree 读回 rough=0.9（写回后树数据同步）",
          abs((pm2.get("rough") or 0) - 0.9) < 1e-3, str(pm2)[:160])
finally:
    proc.terminate()
    try: proc.wait(timeout=5)
    except Exception: proc.kill()
    srv.close()

# ================= 段 ③：model 模式空树 =================
print("---- segment 3: model mode empty tree ----")
srv = start_server()
proc = subprocess.Popen([EXE, "--mode", "cluster", "--model", "Suzanne-materials.nmesh",
                         "--editor-bridge", "--no-hud"])
try:
    srv.settimeout(20)
    conn, _ = srv.accept()
    conn.settimeout(5)
    ann = wait_announce(conn)
    check("model mode: announce received", ann is not None)
    r = req(conn, "scene.getTree")
    check("model getTree ok", ok_of(r), str(err_of(r))[:100])
    tr = result_of(r) or {}
    check("model getTree mode=model count=0",
          tr.get("mode") == "model" and tr.get("count") == 0, str(tr)[:140])
    # setMaterial 在 model 模式仍 error 100（M-E5 既有契约保形）
    r = req(conn, "scene.setMaterial", {"i": 0, "tint": [1, 1, 1]})
    check("model setMaterial -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:100])
finally:
    proc.terminate()
    try: proc.wait(timeout=5)
    except Exception: proc.kill()
    srv.close()

print("\n==== M-E8 cross-process: %d fail ====" % len(fails))
sys.exit(1 if fails else 0)
