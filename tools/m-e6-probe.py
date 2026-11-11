# -*- coding: utf-8 -*-
# M-E6 机械验收：命名管道传输 + 多视口（viewId 路由），全部真跨进程。
#   ① 真 viewer.exe 走 pipe:// 连 python 假编辑器（命名管道 server）
#   ② 真 viewer.exe 走默认 tcp:// 回归 M-E3/E4/E5 线格式不变
#   ③ 真 editor.exe（tcp）+ 2 个 python 假引擎 client：多视图注册 / 定向投递 / 定向转发
#   ④ 真 editor.exe（pipe）+ 1 个 python 管道 client：管道 server 侧跨进程验收
#   ⑤ 负向：坏 endpoint / 未知视图 / 无服务端时的崩溃隔离
# 协议：4 字节**大端**长度前缀 + msgpack 信封 Map{jsonrpc,id,type,method,params}。
import ctypes, os, select, socket, struct, subprocess, sys, time
from ctypes import wintypes

ROOT = r"E:\UE_STUDY_2026\nl-engine-editor"
BUILD = os.path.join(ROOT, "build", "DX12")
EXE_VIEWER = os.path.join(BUILD, "viewer.exe")
EXE_EDITOR = os.path.join(BUILD, "editor.exe")
SCENE = os.path.join(ROOT, "example", "ecs-e1b", "ecs-e1b.scene")
PORT = 29110

fails = []
def check(name, cond, detail=""):
    print(("PASS  " if cond else "FAIL  ") + name + ("  | " + str(detail) if detail else ""), flush=True)
    if not cond: fails.append(name)

# ---------------- 极简 msgpack（覆盖本测试所需子集） ----------------
def enc(v):
    if v is None: return b"\xc0"
    if v is True: return b"\xc3"
    if v is False: return b"\xc2"
    if isinstance(v, int):
        if 0 <= v < 128: return bytes([v])
        if -32 <= v < 0: return bytes([0xE0 | (v + 32)])
        return b"\xd3" + struct.pack(">q", v)
    if isinstance(v, float): return b"\xcb" + struct.pack(">d", v)
    if isinstance(v, str):
        b = v.encode("utf-8")
        if len(b) < 32: return bytes([0xA0 | len(b)]) + b
        if len(b) < 256: return b"\xd9" + bytes([len(b)]) + b
        return b"\xda" + struct.pack(">H", len(b)) + b
    if isinstance(v, (list, tuple)):
        n = len(v)
        head = bytes([0x90 | n]) if n < 16 else (b"\xdc" + struct.pack(">H", n))
        return head + b"".join(enc(x) for x in v)
    if isinstance(v, dict):
        n = len(v)
        head = bytes([0x80 | n]) if n < 16 else (b"\xde" + struct.pack(">H", n))
        return head + b"".join(enc(k) + enc(x) for k, x in v.items())
    raise TypeError(type(v))

class Dec:
    def __init__(self, b): self.b, self.i = b, 0
    def u(self, n):
        r = int.from_bytes(self.b[self.i:self.i + n], "big"); self.i += n; return r
    def val(self):
        o = self.u(1)
        if o < 0x80: return o
        if o >= 0xE0: return o - 256
        if (o & 0xF0) == 0x80: return {self.val(): self.val() for _ in range(o & 0x0F)}
        if (o & 0xF0) == 0x90: return [self.val() for _ in range(o & 0x0F)]
        if (o & 0xE0) == 0xA0:
            n = o & 0x1F; r = self.b[self.i:self.i+n]; self.i += n; return r.decode("utf-8")
        if o in (0xD9, 0xDA, 0xDB):
            n = self.u({0xD9: 1, 0xDA: 2, 0xDB: 4}[o])
            r = self.b[self.i:self.i+n]; self.i += n; return r.decode("utf-8")
        if o == 0xDC: return [self.val() for _ in range(self.u(2))]
        if o == 0xDD: return [self.val() for _ in range(self.u(4))]
        if o == 0xDE: return {self.val(): self.val() for _ in range(self.u(2))}
        if o == 0xDF: return {self.val(): self.val() for _ in range(self.u(4))}
        if o in (0xC4, 0xC5, 0xC6):
            n = self.u({0xC4: 1, 0xC5: 2, 0xC6: 4}[o]); r = self.b[self.i:self.i+n]; self.i += n; return r
        if o == 0xC0: return None
        if o == 0xC2: return False
        if o == 0xC3: return True
        if o == 0xCA: r = struct.unpack(">f", self.b[self.i:self.i+4])[0]; self.i += 4; return r
        if o == 0xCB: r = struct.unpack(">d", self.b[self.i:self.i+8])[0]; self.i += 8; return r
        if o == 0xCC: return self.u(1)
        if o == 0xCD: return self.u(2)
        if o == 0xCE: return self.u(4)
        if o == 0xCF: return self.u(8)
        if o == 0xD0: r = self.u(1); return r - 256 if r > 127 else r
        if o == 0xD1: r = self.u(2); return r - 65536 if r > 32767 else r
        if o == 0xD2: r = self.u(4); return r - (1 << 32) if r > (1 << 31) - 1 else r
        if o == 0xD3: r = self.u(8); return r - (1 << 64) if r > (1 << 63) - 1 else r
        raise ValueError("msgpack op 0x%02x" % o)

def dec(b): return Dec(b).val()
def frame(obj):
    body = enc(obj)
    return struct.pack(">I", len(body)) + body

def notify(method, params=None):
    return {"jsonrpc": 1, "id": 0, "type": "notify", "method": method, "params": params}

def request(method, params=None):
    global _rid
    _rid += 1
    return _rid, {"jsonrpc": 1, "id": _rid, "type": "req", "method": method, "params": params}
_rid = 500

def resp(rid, ok=True, result=None, code=0, message=""):
    m = {"jsonrpc": 1, "id": rid, "type": "resp", "ok": ok}
    if ok: m["result"] = result
    else: m["error"] = {"code": code, "message": message}
    return m

# ---------------- Win32 命名管道 ----------------
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
INVALID = (1 << (ctypes.sizeof(ctypes.c_void_p) * 8)) - 1
PIPE_ACCESS_DUPLEX = 0x3
PIPE_TYPE_BYTE, PIPE_READMODE_BYTE, PIPE_WAIT = 0, 0, 0
FILE_FLAG_OVERLAPPED = 0x40000000
FILE_ATTRIBUTE_NORMAL = 0x80
ERROR_PIPE_CONNECTED, ERROR_PIPE_BUSY = 535, 231
GENERIC_RW = 0x80000000 | 0x40000000
OPEN_EXISTING = 3

k32.CreateNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                 wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                 wintypes.DWORD, wintypes.LPVOID]
k32.CreateNamedPipeW.restype = ctypes.c_void_p
k32.ConnectNamedPipe.argtypes = [ctypes.c_void_p, wintypes.LPVOID]
k32.ConnectNamedPipe.restype = wintypes.BOOL
k32.PeekNamedPipe.argtypes = [ctypes.c_void_p, wintypes.LPVOID, wintypes.DWORD,
                              ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
                              ctypes.POINTER(wintypes.DWORD)]
k32.PeekNamedPipe.restype = wintypes.BOOL
k32.ReadFile.argtypes = [ctypes.c_void_p, wintypes.LPVOID, wintypes.DWORD,
                         ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
k32.ReadFile.restype = wintypes.BOOL
k32.WriteFile.argtypes = [ctypes.c_void_p, wintypes.LPVOID, wintypes.DWORD,
                          ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
k32.WriteFile.restype = wintypes.BOOL
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                            wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
k32.CreateFileW.restype = ctypes.c_void_p
k32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
k32.WaitNamedPipeW.restype = wintypes.BOOL
k32.CloseHandle.argtypes = [ctypes.c_void_p]
k32.DisconnectNamedPipe.argtypes = [ctypes.c_void_p]

def pipe_peek(h):
    avail = wintypes.DWORD(0)
    if not k32.PeekNamedPipe(h, None, 0, None, ctypes.byref(avail), None): return -1
    return avail.value

def pipe_read(h, n):
    buf = ctypes.create_string_buffer(n)
    got = wintypes.DWORD(0)
    if not k32.ReadFile(h, buf, n, ctypes.byref(got), None): return b""
    return buf.raw[:got.value]

def pipe_write(h, data):
    got = wintypes.DWORD(0)
    buf = ctypes.create_string_buffer(data)
    if not k32.WriteFile(h, buf, len(data), ctypes.byref(got), None): return False
    return got.value == len(data)

class PipeServer:
    """假编辑器：命名管道 server（PIPE_WAIT + 线程内阻塞 accept，主循环照常收包）。"""
    def __init__(self, name):
        import threading
        self.full = r"\\.\pipe\%s" % name
        self.h = k32.CreateNamedPipeW(self.full, PIPE_ACCESS_DUPLEX,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                      4, 65536, 65536, 0, None)
        if self.h in (None, INVALID): raise OSError("CreateNamedPipeW failed %d" % ctypes.get_last_error())
        self.ev = threading.Event()
        self.ok = False
        self.th = threading.Thread(target=self._accept, daemon=True)
        self.th.start()
    def _accept(self):
        ok = k32.ConnectNamedPipe(self.h, None)
        self.ok = bool(ok) or ctypes.get_last_error() == ERROR_PIPE_CONNECTED
        self.ev.set()
    def wait(self, timeout=25): return self.ev.wait(timeout) and self.ok
    def close(self):
        try: k32.DisconnectNamedPipe(self.h)
        except Exception: pass
        try: k32.CloseHandle(self.h)
        except Exception: pass

def pipe_client(name, timeout=15):
    full = r"\\.\pipe\%s" % name
    deadline = time.time() + timeout
    while time.time() < deadline:
        h = k32.CreateFileW(full, GENERIC_RW, 0, None, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None)
        if h not in (None, INVALID): return h
        if ctypes.get_last_error() == ERROR_PIPE_BUSY:
            k32.WaitNamedPipeW(full, 200)
        else:
            time.sleep(0.1)
    raise TimeoutError("pipe client: no server at " + full)

# ---------------- 统一连接封装 ----------------
class Conn:
    def __init__(self, kind, handle, name=""):
        self.kind, self.h, self.name, self.buf = kind, handle, name, b""
    def poll(self, timeout=0.05):
        if self.kind == "tcp":
            self.h.settimeout(timeout)
            try: c = self.h.recv(65536)
            except socket.timeout: return False
            if not c: raise ConnectionError("peer closed")
            self.buf += c; return True
        deadline = time.time() + timeout
        while True:
            a = pipe_peek(self.h)
            if a < 0: raise ConnectionError("pipe closed")
            if a > 0:
                self.buf += pipe_read(self.h, min(a, 65536)); return True
            if time.time() >= deadline: return False
            time.sleep(0.005)
    def pop(self):
        if len(self.buf) < 4: return None
        ln = struct.unpack(">I", self.buf[:4])[0]
        if len(self.buf) < 4 + ln: return None
        body = self.buf[4:4 + ln]; self.buf = self.buf[4 + ln:]
        return dec(body)
    def send(self, obj):
        data = frame(obj)
        if self.kind == "tcp": self.h.sendall(data)
        elif not pipe_write(self.h, data): raise ConnectionError("pipe write failed")
    def close(self):
        try:
            if self.kind == "tcp": self.h.close()
            else: k32.CloseHandle(self.h)
        except Exception: pass

def is_method(m, name):
    return isinstance(m, dict) and m.get("type") == "notify" and m.get("method") == name

def auto_pong(conn):
    def f(m):
        if is_method(m, "rpc.ping"): conn.send(notify("rpc.pong"))
    return f

def auto_engine_echo(conn):
    def f(m):
        if isinstance(m, dict) and m.get("type") == "req":
            conn.send(resp(m.get("id"), True, m.get("params")))
    return f

def collect(conns, seconds, hooks=None, keepalive=False, stop_when=None):
    """同时泵多条连接，返回 [(connIndex, msg)]；hooks[i](msg) 自动回包。
    stop_when(i, msg) 命中即早退——否则固定跑满 seconds 会制造 5s+ 静默，
    被编辑器 6s 心跳判掉线（本探针首版唯一假 FAIL 的根因）。"""
    hooks = hooks or [None] * len(conns)
    out, deadline, last_ka = [], time.time() + seconds, 0
    while time.time() < deadline:
        done = False
        for i, c in enumerate(conns):
            try: c.poll(0.01)
            except ConnectionError: pass
            m = c.pop()
            while m is not None:
                out.append((i, m))
                if hooks[i]: hooks[i](m)
                if stop_when and stop_when(i, m): done = True
                m = c.pop()
        if done: return out
        if keepalive and time.time() - last_ka > 1.0:
            for c in conns:
                try: c.send(notify("rpc.ping"))
                except Exception: pass
            last_ka = time.time()
        time.sleep(0.005)
    return out

def req(conn, method, params=None, timeout=5, hook=None):
    rid, m = request(method, params)
    conn.send(m)
    got = collect([conn], timeout, hooks=[hook],
                  stop_when=lambda i, x: isinstance(x, dict)
                  and x.get("type") == "resp" and x.get("id") == rid)
    for _, r in got:
        if isinstance(r, dict) and r.get("type") == "resp" and r.get("id") == rid:
            return r
    return None

def ok_of(r): return isinstance(r, dict) and r.get("ok") is True
def result_of(r): return r.get("result") if isinstance(r, dict) else None
def err_of(r): return (r.get("error") or {}) if isinstance(r, dict) else {}

def kill_all():
    for exe in ("editor.exe", "viewer.exe"):
        subprocess.run(["taskkill", "/F", "/IM", exe], capture_output=True)
    time.sleep(0.4)

# ================= 段 ①：真 viewer 走命名管道 =================
print("---- segment 1: real viewer over named pipe ----")
kill_all()
PIPE1 = "nl-e6-probe"
ps = PipeServer(PIPE1)
vproc = subprocess.Popen([EXE_VIEWER, "--scene", SCENE, "--editor-bridge", "--no-hud",
                          "--editor-endpoint", "pipe://" + PIPE1, "--view-id", "pipeA"],
                         cwd=BUILD, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    check("S1 pipe server accepted real viewer", ps.wait(30))
    c = Conn("pipe", ps.h, "viewer")
    got = collect([c], 6.0, hooks=[auto_pong(c)])
    anns = [m for _, m in got if is_method(m, "engine.announce")]
    ann = anns[0]["params"] if anns else {}
    check("S1 engine.announce received over pipe", bool(anns))
    check("S1 announce.viewId == pipeA", ann.get("viewId") == "pipeA", ann.get("viewId"))
    check("S1 announce.hwnd != 0", bool(ann.get("hwnd")), ann.get("hwnd"))
    caps = ann.get("caps") or {}
    check("S1 caps.backend == DX12", caps.get("backend") == "DX12", caps.get("backend"))
    check("S1 caps w/h present", caps.get("w", 0) > 0 and caps.get("h", 0) > 0, caps)

    r = req(c, "engine.echo", {"text": "pipe-echo"}, hook=auto_pong(c))
    check("S1 engine.echo over pipe", ok_of(r) and (result_of(r) or {}).get("text") == "pipe-echo", r)
    r = req(c, "scene.getState", hook=auto_pong(c))
    check("S1 scene.getState over pipe (count==5)", ok_of(r) and (result_of(r) or {}).get("count") == 5,
          (result_of(r) or {}).get("count"))
    r = req(c, "camera.getState", hook=auto_pong(c))
    st = result_of(r) or {}
    check("S1 camera.getState over pipe (fov/dist/yaw/pitch)",
          ok_of(r) and all(k in st for k in ("fov", "dist", "yaw", "pitch")), st)
    check("S1 viewer still alive after RPC", vproc.poll() is None)
    c.close()
except Exception as e:
    check("S1 no exception", False, repr(e))
finally:
    vproc.kill()
    ps.close()

# ================= 段 ②：TCP 默认回归 =================
print("---- segment 2: real viewer over default TCP (M-E1~E5 regression) ----")
kill_all()
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT)); srv.listen(2); srv.settimeout(30)
vproc = subprocess.Popen([EXE_VIEWER, "--scene", SCENE, "--editor-bridge", "--no-hud"],
                         cwd=BUILD, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    conn, _ = srv.accept()
    c = Conn("tcp", conn, "viewer-tcp")
    got = collect([c], 6.0, hooks=[auto_pong(c)])
    anns = [m for _, m in got if is_method(m, "engine.announce")]
    ann = anns[0]["params"] if anns else {}
    check("S2 engine.announce received over TCP", bool(anns))
    check("S2 default viewId == main (老语义)", ann.get("viewId") == "main", ann.get("viewId"))
    r = req(c, "engine.echo", {"text": "tcp-echo"}, hook=auto_pong(c))
    check("S2 engine.echo over TCP", ok_of(r) and (result_of(r) or {}).get("text") == "tcp-echo", r)
    r = req(c, "scene.getMaterials", hook=auto_pong(c))
    check("S2 scene.getMaterials 回归 (count==5)",
          ok_of(r) and (result_of(r) or {}).get("count") == 5, (result_of(r) or {}).get("count"))
    c.close()
except Exception as e:
    check("S2 no exception", False, repr(e))
finally:
    vproc.kill()
    srv.close()

# ================= 段 ③：真 editor 多视口（TCP + 2 假引擎） =================
print("---- segment 3: real editor with 2 engine clients (viewId routing) ----")
kill_all()
ELOG = os.path.join(ROOT, "out", "m-e6-editor-tcp.log")
os.makedirs(os.path.dirname(ELOG), exist_ok=True)
elog = open(ELOG, "w", encoding="utf-8", errors="replace")
eproc = subprocess.Popen([EXE_EDITOR, "--rpc-endpoint", "tcp://127.0.0.1:%d" % PORT],
                         cwd=BUILD, stdout=elog, stderr=subprocess.STDOUT)
def wait_log(path, text, timeout=12):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try: s = open(path, "r", encoding="utf-8", errors="replace").read()
        except OSError: s = ""
        if text in s: return s
        time.sleep(0.1)
    return ""

def log_text():
    try: return open(ELOG, "r", encoding="utf-8", errors="replace").read()
    except OSError: return ""

try:
    boot = wait_log(ELOG, "listening on tcp://")
    check("S3 editor listening on TCP", "listening on tcp://127.0.0.1:29110" in boot, boot[-120:])
    clients = []
    for i in range(2):
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        clients.append(Conn("tcp", s, "client%d" % i))
    hookA, hookB = auto_engine_echo(clients[0]), auto_engine_echo(clients[1])
    for i, vid in enumerate(("left", "right")):
        clients[i].send(notify("engine.announce", {
            "hwnd": 0xDEAD0001 + i, "pid": 9000 + i, "proto": 1,
            "caps": {"w": 320, "h": 240, "backend": "FAKE"},
            "viewId": vid}))
    got = collect(clients, 2.0, hooks=[hookA, hookB], keepalive=True)
    check("S3 two views announced in editor log",
          "announce view=left" in log_text() and "announce view=right" in log_text(),
          [l for l in log_text().splitlines() if "announce view=" in l][:3])

    r = req(clients[0], "editor.views", hook=hookA)
    views = result_of(r) or []
    check("S3 editor.views ok + 2 entries", ok_of(r) and len(views) == 2, str(views)[:200])
    ids = sorted(v.get("viewId") for v in views if isinstance(v, dict))
    check("S3 viewIds == [left, right]", ids == ["left", "right"], ids)
    hw = sorted(v.get("hwnd") for v in views if isinstance(v, dict))
    check("S3 hwnd per view preserved", hw == [0xDEAD0001, 0xDEAD0002], [hex(x) for x in hw])

    # 定向投递：只有 right 收到 editor.probe
    rid, m = request("editor.pingView", {"view": "right"})
    clients[0].send(m)
    got = collect(clients, 1.5, hooks=[hookA, hookB], keepalive=True)
    pr = [x for x in got if isinstance(x[1], dict) and x[1].get("type") == "resp" and x[1].get("id") == rid]
    check("S3 editor.pingView resp.sent == true",
          bool(pr) and (result_of(pr[0][1]) or {}).get("sent") is True, pr[:1])
    pb = [x for x in got if is_method(x[1], "editor.probe") and x[0] == 1]
    pa = [x for x in got if is_method(x[1], "editor.probe") and x[0] == 0]
    check("S3 only 'right' received editor.probe", len(pb) == 1 and len(pa) == 0,
          "toRight=%d toLeft=%d" % (len(pb), len(pa)))
    check("S3 probe params.view == right",
          bool(pb) and (pb[0][1].get("params") or {}).get("view") == "right")

    # 定向转发：editor.callView 把 engine.echo 转给 right，回包再回给 left。
    # 必须同时泵两条连接（editor 的 callTo 会阻塞等 right 的 resp）。
    rid, m = request("editor.callView",
                     {"view": "right", "method": "engine.echo", "params": {"tag": "routed"}})
    clients[0].send(m)
    got = collect(clients, 4.0, hooks=[hookA, hookB], keepalive=True,
                  stop_when=lambda i, x: i == 0 and isinstance(x, dict)
                  and x.get("type") == "resp" and x.get("id") == rid)
    r = next((x for i, x in got if i == 0 and isinstance(x, dict)
              and x.get("type") == "resp" and x.get("id") == rid), None)
    check("S3 editor.callView 定向转发 engine.echo",
          ok_of(r) and (result_of(r) or {}).get("tag") == "routed", str(r)[:180])

    # 负向：未知视图
    r = req(clients[0], "editor.pingView", {"view": "nope"}, hook=hookA)
    check("S3 pingView(未知视图) sent == false", ok_of(r) and (result_of(r) or {}).get("sent") is False, r)
    r = req(clients[0], "editor.callView",
            {"view": "nope", "method": "engine.echo", "params": {}}, timeout=3, hook=hookA)
    check("S3 callView(未知视图) -> error 103", r is not None and err_of(r).get("code") == 103, r)

    # 重复 viewId 应打警告但不拖死宿主
    clients[1].send(notify("engine.announce", {
        "hwnd": 0xDEAD0009, "pid": 9999, "proto": 1, "caps": {}, "viewId": "left"}))
    # 必须持续泵两条连接并回 editor 插件的自动刷新 req：否则插件 5s 阻塞
    # call 会把编辑器主循环冻住，announce 一直排不进来（首版假 FAIL 根因）。
    deadline = time.time() + 4.0
    while time.time() < deadline and "duplicate viewId" not in log_text():
        collect(clients, 0.1, hooks=[hookA, hookB], keepalive=True)
    check("S3 duplicate viewId warning", "duplicate viewId 'left'" in log_text(),
          [l for l in log_text().splitlines() if "announce view=" in l or "duplicate" in l][-4:])
    check("S3 editor still alive", eproc.poll() is None)
    for c in clients: c.close()
except Exception as e:
    check("S3 no exception", False, repr(e))
finally:
    eproc.kill()
    elog.close()

# ================= 段 ④：真 editor 走命名管道 server =================
print("---- segment 4: real editor over named pipe (server side) ----")
kill_all()
PIPE2 = "nl-e6-editor"
ELOG2 = os.path.join(ROOT, "out", "m-e6-editor-pipe.log")
elog2 = open(ELOG2, "w", encoding="utf-8", errors="replace")
eproc2 = subprocess.Popen([EXE_EDITOR, "--rpc-endpoint", "pipe://" + PIPE2],
                          cwd=BUILD, stdout=elog2, stderr=subprocess.STDOUT)
def log2():
    try: return open(ELOG2, "r", encoding="utf-8", errors="replace").read()
    except OSError: return ""
try:
    boot = wait_log(ELOG2, "listening on pipe://", 12)
    check("S4 editor listening on pipe", "listening on pipe://" + PIPE2 in boot, boot[-120:])
    h = pipe_client(PIPE2, 10)
    c = Conn("pipe", h, "pipe-engine")
    hook = auto_engine_echo(c)
    c.send(notify("engine.announce", {"hwnd": 0xDEADBEEF, "pid": 7777, "proto": 1,
                                      "caps": {"w": 800, "h": 600, "backend": "DX12"},
                                      "viewId": "pipeB"}))
    deadline = time.time() + 5
    while time.time() < deadline and "announce view=pipeB" not in log2():
        c.poll(0.05); time.sleep(0.05)
    check("S4 announce over pipe with transport=pipe",
          "announce view=pipeB" in log2() and "transport=pipe" in log2(),
          [l for l in log2().splitlines() if "pipeB" in l][:2])
    r = req(c, "editor.views", hook=hook)
    views = result_of(r) or []
    check("S4 editor.views over pipe == 1 view(pipeB)",
          ok_of(r) and len(views) == 1 and views[0].get("viewId") == "pipeB", str(views)[:180])
    check("S4 editor(pipe) still alive", eproc2.poll() is None)
    c.close()
except Exception as e:
    check("S4 no exception", False, repr(e))
finally:
    eproc2.kill()
    elog2.close()

# ================= 段 ⑤：负向 / 崩溃隔离 =================
print("---- segment 5: negative endpoints & crash isolation ----")
kill_all()
vproc = subprocess.Popen([EXE_VIEWER, "--scene", SCENE, "--editor-bridge", "--no-hud",
                          "--editor-endpoint", "pipe://nl-e6-no-server"],
                         cwd=BUILD, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(3.0)
check("S5 viewer survives missing pipe server (retry loop)", vproc.poll() is None,
      vproc.poll())
vproc.kill()

vlog = os.path.join(ROOT, "out", "m-e6-bad-endpoint.log")
with open(vlog, "w", encoding="utf-8", errors="replace") as f:
    p = subprocess.Popen([EXE_VIEWER, "--scene", SCENE, "--editor-bridge", "--no-hud",
                          "--editor-endpoint", "bogus://x"],
                         cwd=BUILD, stdout=subprocess.DEVNULL, stderr=f)
    time.sleep(3.0)
    alive = p.poll() is None
    p.kill()
bad = open(vlog, "r", encoding="utf-8", errors="replace").read()
check("S5 bad endpoint survives + logs diagnostic", alive and "bad endpoint" in bad, bad[-160:])

kill_all()
print("==== M-E6 probe: %d FAIL ====" % len(fails))
if fails:
    for f in fails: print("  - " + f)
sys.exit(1 if fails else 0)
