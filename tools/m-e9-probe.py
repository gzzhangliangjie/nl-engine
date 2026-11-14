# -*- coding: utf-8 -*-
# M-E9 机械验收：场景树编辑操作（python 假编辑器 server，真跨进程）。
#   ① rename：改名 → getTree 读回；未知实体 error 100
#   ② addEntity：light/model/empty 三 kind → getTree + getMaterials 数量联动
#      （rebuild 后 modelIds 扩容 = 集合签名重建传导实锤）；白名单/缺 mesh 负向
#   ③ reparent/setParent：挂父读回 + 环防护 error 101 + 解除挂接
#   ④ removeEntity：摘除 → getTree/getMaterials 复原；未知实体 error 100
# 协议：4 字节大端长度前缀 + msgpack 信封 Map{jsonrpc:1,id,type,method,params}。
import socket, struct, subprocess, sys, time, os

PORT = 29110
fails = []
def check(name, cond, detail=""):
    print(("PASS  " if cond else "FAIL  ") + name + ("  | " + detail if detail else ""), flush=True)
    if not cond: fails.append(name)

# ---------- 极简 msgpack（只覆盖本测试所需子集，M-E8 骨架同款） ----------
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
        if o >= 0xE0: return o - 256          # negative fixint（灯实体 idx=-1）
        if (o & 0xF0) == 0x80:
            n = o & 0x0F; d = {}
            for _ in range(n):
                k = self.val(); d[k] = self.val()
            return d
        if (o & 0xF0) == 0x90:
            n = o & 0x0F; return [self.val() for _ in range(n)]
        if o == 0xDC:
            n = self.u(2); return [self.val() for _ in range(n)]
        if o == 0xDD:
            n = self.u(4); return [self.val() for _ in range(n)]
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

def settle(t=0.5):
    time.sleep(t)   # 等帧边界集合签名重建 + 渲染收集更新

def start_server():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT)); srv.listen(1)
    return srv

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "DX12", "viewer.exe")

# ================= --preview：编辑操作全链 =================
print("---- segment 1: rename/add/reparent/remove on --preview ----")
srv = start_server()
proc = subprocess.Popen([EXE, "--preview", "--editor-bridge", "--no-hud"])
try:
    srv.settimeout(20)
    conn, _ = srv.accept()
    conn.settimeout(5)
    ann = wait_announce(conn)
    check("preview: announce received", ann is not None)

    r = req(conn, "scene.getTree")
    tr = result_of(r) or {}
    nodes = tr.get("nodes") or []
    check("baseline: getTree count=2", tr.get("count") == 2, str(tr.get("count")))
    byName = {n.get("name"): n for n in nodes if isinstance(n, dict)}
    pm = byName.get("preview-model")
    pg = byName.get("preview-ground")
    check("baseline: preview-model/ground present",
          pm is not None and pg is not None, str(list(byName))[:120])
    pmId, pgId = pm.get("id"), pg.get("id")

    # --- ① rename ---
    r = req(conn, "scene.rename", {"id": pmId, "name": "my-suzanne"})
    check("rename ok", ok_of(r), str(err_of(r))[:100])
    r = req(conn, "scene.getTree")
    nodes = (result_of(r) or {}).get("nodes") or []
    check("getTree 读回改名 my-suzanne",
          any(n.get("name") == "my-suzanne" for n in nodes),
          str([n.get("name") for n in nodes])[:120])
    r = req(conn, "scene.rename", {"id": 999999, "name": "x"})
    check("rename 未知实体 -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:100])

    # --- ② addEntity ---
    r = req(conn, "scene.addEntity", {"name": "lamp1", "kind": "light", "lightType": 1})
    check("addEntity light ok", ok_of(r), str(err_of(r))[:100])
    lampId = (result_of(r) or {}).get("id")
    r = req(conn, "scene.getTree")
    tr = result_of(r) or {}
    nodes = tr.get("nodes") or []
    lamp = next((n for n in nodes if n.get("id") == lampId), {})
    check("getTree 读回新灯实体 kind=light lightType=1",
          lamp.get("kind") == "light" and lamp.get("lightType") == 1, str(lamp)[:140])
    check("getTree count=3（加灯后）", tr.get("count") == 3, str(tr.get("count")))

    r = req(conn, "scene.addEntity", {"name": "child", "kind": "model",
                                      "mesh": "ParkingLot.glb", "parent": pmId})
    check("addEntity model ok", ok_of(r), str(err_of(r))[:100])
    childId = (result_of(r) or {}).get("id")
    r = req(conn, "scene.addEntity", {"name": "bad", "kind": "model", "mesh": "evil.glb"})
    check("addEntity model 白名单拒绝 -> error 101",
          r.get("ok") is False and err_of(r).get("code") == 101, str(err_of(r))[:100])
    r = req(conn, "scene.addEntity", {"name": "bad2", "kind": "model"})
    check("addEntity model 缺 mesh -> error 101",
          r.get("ok") is False and err_of(r).get("code") == 101, str(err_of(r))[:100])
    r = req(conn, "scene.addEntity", {"name": "bad3", "kind": "alien"})
    check("addEntity 未知 kind -> error 101",
          r.get("ok") is False and err_of(r).get("code") == 101, str(err_of(r))[:100])

    settle()
    # 集合重建传导：新 model 实体进渲染收集（idx 分配 + getMaterials 计数 +3）
    r = req(conn, "scene.getTree")
    nodes = (result_of(r) or {}).get("nodes") or []
    child = next((n for n in nodes if n.get("id") == childId), {})
    check("重建后 child 拿到实例行 idx>=0（签名重建传导）",
          child.get("kind") == "model" and child.get("idx", -1) >= 0, str(child)[:140])
    r = req(conn, "scene.getMaterials")
    mats = result_of(r) or {}
    check("getMaterials count=3（2 基线 + child，集合重建）", mats.get("count") == 3, str(mats.get("count")))
    r = req(conn, "scene.getState")
    st = result_of(r) or {}
    check("getState count=3（渲染实例集合扩容）", st.get("count") == 3, str(st.get("count")))

    # --- ③ reparent/setParent ---
    r = req(conn, "scene.reparent", {"id": childId, "parent": pmId})
    check("reparent ok", ok_of(r), str(err_of(r))[:100])
    r = req(conn, "scene.getTree")
    nodes = (result_of(r) or {}).get("nodes") or []
    child = next((n for n in nodes if n.get("id") == childId), {})
    check("getTree 读回 child.parent == my-suzanne", child.get("parent") == pmId,
          str(child.get("parent")))
    r = req(conn, "scene.reparent", {"id": pmId, "parent": childId})
    check("reparent 环防护 -> error 101",
          r.get("ok") is False and err_of(r).get("code") == 101, str(err_of(r))[:100])
    r = req(conn, "scene.reparent", {"id": pmId, "parent": pmId})
    check("reparent parent==self -> error 101",
          r.get("ok") is False and err_of(r).get("code") == 101, str(err_of(r))[:100])
    r = req(conn, "scene.setParent", {"id": pgId, "parent": 0})
    check("setParent 解除挂接（parent=0）ok", ok_of(r), str(err_of(r))[:100])
    r = req(conn, "scene.setParent", {"id": pgId, "parent": 999999})
    check("setParent 未知父 -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:100])

    # --- ④ removeEntity ---
    r = req(conn, "scene.removeEntity", {"id": childId})
    check("removeEntity ok", ok_of(r), str(err_of(r))[:100])
    r = req(conn, "scene.removeEntity", {"id": 999999})
    check("removeEntity 未知实体 -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:100])
    settle()
    r = req(conn, "scene.getMaterials")
    mats = result_of(r) or {}
    check("移除后 getMaterials count=2（集合收编回 2）", mats.get("count") == 2,
          str(mats.get("count")))
    r = req(conn, "scene.getTree")
    nodes = (result_of(r) or {}).get("nodes") or []
    check("移除后 getTree 无 child", all(n.get("id") != childId for n in nodes),
          str([n.get("name") for n in nodes])[:120])
finally:
    proc.terminate()
    try: proc.wait(timeout=5)
    except Exception: proc.kill()
    srv.close()

# ================= model 模式：编辑通道负向 =================
print("---- segment 2: edit RPCs in model mode -> error 100 ----")
srv = start_server()
proc = subprocess.Popen([EXE, "--mode", "cluster", "--model", "Suzanne-materials.nmesh",
                         "--editor-bridge", "--no-hud"])
try:
    srv.settimeout(20)
    conn, _ = srv.accept()
    conn.settimeout(5)
    ann = wait_announce(conn)
    check("model mode: announce received", ann is not None)
    for m in ("scene.rename", "scene.setParent", "scene.reparent",
              "scene.addEntity", "scene.removeEntity"):
        r = req(conn, m, {"id": 1, "name": "x", "kind": "empty", "parent": 0})
        check("model %s -> error 100" % m,
              r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:90])
finally:
    proc.terminate()
    try: proc.wait(timeout=5)
    except Exception: proc.kill()
    srv.close()

print("\n==== M-E9 cross-process: %d fail ====" % len(fails))
sys.exit(1 if fails else 0)
