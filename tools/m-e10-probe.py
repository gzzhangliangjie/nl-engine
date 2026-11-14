# -*- coding: utf-8 -*-
# M-E10 机械验收：对象级参数泛化面（python 假编辑器 server，真跨进程）。
#   ① getProps 描述表：Transform/Model/Light 全字段（component/name/type/range/value）
#   ② setProp 全类型：Transform position/scale/rotation（渲染表直读证据
#      scene.getState pos + 指纹推送）、Model roughness/metallic/tint/meshFile、
#      Light intensity/type/range/direction（灯缓冲 updateEcsLights 无错即通）
#   ③ 负向：未知 prop/组件 error 100、类型不符 error 101、点光 shadow error 101、
#      meshFile 白名单 error 101、Transform on 灯实体 error 100
#   ④ model 模式：getProps/setProp -> error 100
# 协议：4 字节大端长度前缀 + msgpack 信封 Map{jsonrpc:1,id,type,method,params}。
import socket, struct, subprocess, sys, time, os

PORT = 29110
fails = []
def check(name, cond, detail=""):
    print(("PASS  " if cond else "FAIL  ") + name + ("  | " + detail if detail else ""), flush=True)
    if not cond: fails.append(name)

# ---------- 极简 msgpack（只覆盖本测试所需子集，M-E9 骨架同款） ----------
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
        if o >= 0xE0: return o - 256          # negative fixint（idx=-1）
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

def wait_notify(conn, method, timeout=5.0):
    deadline = time.time() + timeout
    next_ping = time.time() + 1.0
    old = conn.gettimeout()
    conn.settimeout(0.4)
    try:
        while time.time() < deadline:
            if time.time() > next_ping:
                send_msg(conn, {"jsonrpc": 1, "id": 0, "type": "notify",
                                "method": "engine.ping", "params": None})
                next_ping = time.time() + 1.0
            try:
                r = recv_msg(conn)
            except (socket.timeout, TimeoutError):
                continue
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

# ================= --preview：泛化面全链 =================
print("---- segment 1: getProps/setProp on --preview ----")
srv = start_server()
proc = subprocess.Popen([EXE, "--preview", "--editor-bridge", "--no-hud"])
try:
    srv.settimeout(20)
    conn, _ = srv.accept()
    conn.settimeout(5)
    ann = wait_announce(conn)
    check("preview: announce received", ann is not None)

    r = req(conn, "scene.getTree")
    nodes = (result_of(r) or {}).get("nodes") or []
    byName = {n.get("name"): n for n in nodes if isinstance(n, dict)}
    pmId = byName.get("preview-model", {}).get("id")

    # --- ① 描述表结构 ---
    r = req(conn, "scene.getProps", {"id": pmId})
    check("getProps ok", ok_of(r), str(err_of(r))[:100])
    pr = result_of(r) or {}
    props = pr.get("props") or []
    idx = {(p.get("component"), p.get("name")): p for p in props if isinstance(p, dict)}
    tp = idx.get(("Transform", "position"), {})
    check("Transform.position vec3 描述", tp.get("type") == "vec3" and
          isinstance(tp.get("value"), list) and len(tp["value"]) == 3, str(tp)[:140])
    check("Transform.rotation quat", idx.get(("Transform", "rotation"), {}).get("type") == "quat")
    check("Model.roughness float range 0..1",
          idx.get(("Model", "roughness"), {}).get("type") == "float" and
          idx.get(("Model", "roughness"), {}).get("min") == 0 and
          idx.get(("Model", "roughness"), {}).get("max") == 1,
          str(idx.get(("Model", "roughness")))[:120])
    check("Model.meshFile string", idx.get(("Model", "meshFile"), {}).get("type") == "string")
    check("Model.tint color", idx.get(("Model", "tint"), {}).get("type") == "color")

    # 加一盏点光验 Light 描述表
    r = req(conn, "scene.addEntity", {"name": "lampX", "kind": "light", "lightType": 1})
    lampId = (result_of(r) or {}).get("id")
    r = req(conn, "scene.getProps", {"id": lampId})
    lp = result_of(r) or {}
    li = {(p.get("component"), p.get("name")): p for p in (lp.get("props") or [])}
    check("Light.type int range 0..1",
          li.get(("Light", "type"), {}).get("type") == "int" and
          li.get(("Light", "type"), {}).get("value") == 1, str(li.get(("Light", "type")))[:120])
    check("Light.intensity/range/position/shadow 描述齐",
          all(("Light", k) in li for k in ("intensity", "range", "position", "shadow")),
          str(list(li))[:160])
    check("灯实体无 Transform 描述（kind=light 只挂 Light）",
          all(p.get("component") != "Transform" for p in (lp.get("props") or [])))

    # --- ② setProp 正向 ---
    time.sleep(0.6)
    while wait_notify(conn, "editor.sceneChanged", timeout=1.0) is not None:
        pass   # 排空积压推送（加灯等既有变更）
    r = req(conn, "scene.setProp", {"id": pmId, "component": "Transform",
                                    "name": "position", "value": [0.5, 1.25, 0.0]})
    check("setProp Transform.position ok", ok_of(r), str(err_of(r))[:100])
    # 指纹推送（Transform 进指纹 → sceneChanged）。必须紧跟 setProp——后续 req
    # 的响应匹配循环会把路过的 notify 吞掉（M-E8 踩坑同款）。
    push = wait_notify(conn, "editor.sceneChanged", timeout=3.0)
    check("Transform 编辑 → sceneChanged 推送", push is not None)
    r = req(conn, "scene.getProps", {"id": pmId})
    props = (result_of(r) or {}).get("props") or []
    pos = next((p for p in props if p.get("component") == "Transform" and p.get("name") == "position"), {})
    check("getProps 读回 position=[0.5,1.25,0]",
          abs(pos.get("value", [0,0,0])[0] - 0.5) < 1e-3 and
          abs(pos.get("value", [0,0,0])[1] - 1.25) < 1e-3, str(pos)[:120])
    # 渲染表直刷证据：scene.getState 实例世界矩阵平移分量
    r = req(conn, "scene.getState")
    objs = (result_of(r) or {}).get("objects") or []
    check("getState objects[0].pos 读回 [0.5,1.25,0]（渲染表直刷）",
          len(objs) > 0 and abs(objs[0].get("pos", [9,9,9])[0] - 0.5) < 1e-3 and
          abs(objs[0].get("pos", [9,9,9])[1] - 1.25) < 1e-3, str(objs[0])[:140] if objs else "empty")

    r = req(conn, "scene.setProp", {"id": pmId, "component": "Model", "name": "roughness", "value": 0.35})
    check("setProp Model.roughness ok", ok_of(r), str(err_of(r))[:100])
    r = req(conn, "scene.getProps", {"id": pmId})
    props = (result_of(r) or {}).get("props") or []
    rough = next((p for p in props if p.get("name") == "roughness"), {})
    check("getProps 读回 roughness=0.35", abs((rough.get("value") or 0) - 0.35) < 1e-3,
          str(rough)[:100])

    r = req(conn, "scene.setProp", {"id": lampId, "component": "Light", "name": "intensity", "value": 3.5})
    check("setProp Light.intensity ok", ok_of(r), str(err_of(r))[:100])
    r = req(conn, "scene.getProps", {"id": lampId})
    props = (result_of(r) or {}).get("props") or []
    inten = next((p for p in props if p.get("name") == "intensity"), {})
    check("getProps 读回 intensity=3.5", abs((inten.get("value") or 0) - 3.5) < 1e-3,
          str(inten)[:100])
    r = req(conn, "scene.setProp", {"id": lampId, "component": "Light",
                                    "name": "position", "value": [1.0, 2.0, 3.0]})
    check("setProp Light.position ok", ok_of(r), str(err_of(r))[:100])

    # --- ③ 负向 ---
    r = req(conn, "scene.setProp", {"id": pmId, "component": "Transform", "name": "nope", "value": [0,0,0]})
    check("未知 prop -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:100])
    r = req(conn, "scene.setProp", {"id": pmId, "component": "Nothing", "name": "x", "value": 1})
    check("未知组件 -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:100])
    r = req(conn, "scene.setProp", {"id": pmId, "component": "Transform", "name": "position", "value": 5})
    check("类型不符（vec3 收到标量）-> error 101",
          r.get("ok") is False and err_of(r).get("code") == 101, str(err_of(r))[:100])
    r = req(conn, "scene.setProp", {"id": pmId, "component": "Transform", "name": "rotation", "value": [0,0,0]})
    check("quat 长度不符 -> error 101",
          r.get("ok") is False and err_of(r).get("code") == 101, str(err_of(r))[:100])
    r = req(conn, "scene.setProp", {"id": lampId, "component": "Transform", "name": "position", "value": [0,0,0]})
    check("灯实体 setProp Transform -> error 100（无组件）",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:100])
    r = req(conn, "scene.setProp", {"id": lampId, "component": "Light", "name": "shadow", "value": True})
    check("点光 shadow=true -> error 101（写前拒绝）",
          r.get("ok") is False and err_of(r).get("code") == 101, str(err_of(r))[:100])
    r = req(conn, "scene.setProp", {"id": lampId, "component": "Light", "name": "range", "value": 0})
    check("点光 range=0 -> error 101",
          r.get("ok") is False and err_of(r).get("code") == 101, str(err_of(r))[:100])
    r = req(conn, "scene.setProp", {"id": pmId, "component": "Model", "name": "meshFile", "value": "evil.glb"})
    check("meshFile 白名单 -> error 101",
          r.get("ok") is False and err_of(r).get("code") == 101, str(err_of(r))[:100])
    r = req(conn, "scene.getProps", {"id": 999999})
    check("getProps 未知实体 -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:100])
    # meshFile 合法变更（集合签名重建 → getMaterials mesh 变）
    r = req(conn, "scene.setProp", {"id": pmId, "component": "Model", "name": "meshFile",
                                    "value": "ParkingLot.glb"})
    check("setProp meshFile 合法 ok", ok_of(r), str(err_of(r))[:100])
    time.sleep(0.5)
    r = req(conn, "scene.getMaterials")
    objs = (result_of(r) or {}).get("objects") or []
    check("meshFile 变更 → 重建后 getMaterials mesh=ParkingLot.glb",
          len(objs) > 0 and objs[0].get("mesh") == "ParkingLot.glb", str(objs[0])[:120] if objs else "empty")
finally:
    proc.terminate()
    try: proc.wait(timeout=5)
    except Exception: proc.kill()
    srv.close()

# ================= model 模式负向 =================
print("---- segment 2: getProps/setProp in model mode -> error 100 ----")
srv = start_server()
proc = subprocess.Popen([EXE, "--mode", "cluster", "--model", "Suzanne-materials.nmesh",
                         "--editor-bridge", "--no-hud"])
try:
    srv.settimeout(20)
    conn, _ = srv.accept()
    conn.settimeout(5)
    ann = wait_announce(conn)
    check("model mode: announce received", ann is not None)
    r = req(conn, "scene.getProps", {"id": 1})
    check("model getProps -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:90])
    r = req(conn, "scene.setProp", {"id": 1, "component": "Transform", "name": "position", "value": [0,0,0]})
    check("model setProp -> error 100",
          r.get("ok") is False and err_of(r).get("code") == 100, str(err_of(r))[:90])
finally:
    proc.terminate()
    try: proc.wait(timeout=5)
    except Exception: proc.kill()
    srv.close()

print("\n==== M-E10 cross-process: %d fail ====" % len(fails))
sys.exit(1 if fails else 0)
