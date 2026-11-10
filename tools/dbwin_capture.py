# -*- coding: utf-8 -*-
"""Capture OutputDebugString (DBWIN protocol) from viewer.exe to get the
D3D12 debug layer's last messages before the execution-time fail-fast.
Pure Win32 via ctypes: DBWIN_BUFFER file mapping + DBWIN_DATA_READY event.
"""
import ctypes, ctypes.wintypes as wt, subprocess, sys, threading, time

k32 = ctypes.windll.kernel32
adv = ctypes.windll.advapi32

FILE_MAP_ALL_ACCESS = 0xF001F
EVENT_MODIFY_STATE = 2
SYNCHRONIZE = 0x100000
INVALID_HANDLE = -1
WAIT_OBJECT_0 = 0
INFINITE = 0xFFFFFFFF

SECURITY_ATTRIBUTES = None  # default (widely shareable in practice)

INVALID_HANDLE_VALUE = ctypes.c_void_p(-1)
k32.CreateFileMappingW.restype = wt.HANDLE
k32.CreateFileMappingW.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, wt.DWORD, wt.DWORD, wt.LPCWSTR]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.CreateEventW.restype = wt.HANDLE
k32.CreateEventW.argtypes = [wt.LPVOID, wt.BOOL, wt.BOOL, wt.LPCWSTR]
k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
k32.SetEvent.argtypes = [wt.HANDLE]

# Create the DBWIN_BUFFER shared section: 4KB, first DWORD = process id, rest = text
mapping = k32.CreateFileMappingW(INVALID_HANDLE_VALUE, None, 0x04, 0, 4096, "DBWIN_BUFFER")
if not mapping:
    print("CreateFileMapping failed", ctypes.get_last_error()); sys.exit(1)
buf = k32.MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)
if not buf:
    print("MapViewOfFile failed"); sys.exit(1)

data_ready = k32.CreateEventW(None, False, False, "DBWIN_DATA_READY")
buffer_ready = k32.CreateEventW(None, True, True, "DBWIN_BUFFER_READY")
if not data_ready or not buffer_ready:
    print("CreateEvent failed"); sys.exit(1)

view = ctypes.cast(buf, ctypes.POINTER(ctypes.c_char * 4096)).contents

def drain(stop_evt, out):
    while not stop_evt.is_set():
        w = k32.WaitForSingleObject(data_ready, 500)
        if w == WAIT_OBJECT_0:
            pid = ctypes.cast(buf, ctypes.POINTER(ctypes.c_uint32)).contents.value
            text = bytes(view[4:]).split(b"\x00")[0].decode("utf-8", "replace")
            out.append((pid, text))
            k32.SetEvent(buffer_ready)

msgs = []
stop_evt = threading.Event()
t = threading.Thread(target=drain, args=(stop_evt, msgs), daemon=True)
t.start()

argv = sys.argv[1:]
print("run:", " ".join(argv), flush=True)
p = subprocess.run(argv, capture_output=False)
print("exitcode=%s" % p.returncode, flush=True)
time.sleep(1.0)
stop_evt.set(); t.join(timeout=2)

for pid, text in msgs:
    print("[ods pid=%d] %s" % (pid, text), flush=True)
print("total ods lines:", len(msgs))
