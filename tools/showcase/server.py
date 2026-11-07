#!/usr/bin/env python3
"""nl-engine S0 showcase: serve the gallery and run each milestone's real viewer.

Zero third-party dependencies (Python standard library only).

    python tools/showcase/server.py --port 8787
    python tools/showcase/server.py --port 8787 --open
    python tools/showcase/server.py --check    # iteration gate, exit 1 if example/ is stale

The case catalog lives in example/cases.json (single source of truth), and the
baseline PNGs live next to it in example/<version>/. Every new iteration must
add its successful cases there; --check compares the kStage declared in
renderer.cpp against the catalog and exits non-zero when they are out of sync.
The /api/run endpoint never
executes arbitrary commands: every argument comes from that catalog, and
user-supplied values are validated against the per-case parameter spec
(range bounds / select options) before substitution.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import mimetypes
import os
import re
import subprocess
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
EXAMPLES = ROOT / "example"
RUNS = ROOT / "build" / "showcase" / "runs"
CASES_PATH = EXAMPLES / "cases.json"

with CASES_PATH.open(encoding="utf-8") as f:
    DATA = json.load(f)


def declared_shots() -> set[str]:
    """Only PNGs named in example/cases.json are served (no path traversal).

    Includes `preview`: static evidence images for tool milestones (e.g. the
    s2-qem QEM before/after render), which no viewer.exe produces at runtime.
    """
    out: set[str] = set()
    for v in DATA["versions"]:
        for c in v["cases"]:
            for key in ("baselines", "beforeBaselines"):
                for rel in (c.get(key) or {}).values():
                    if rel and rel.startswith("example/"):
                        out.add(rel[len("example/"):])
            rel = c.get("preview")
            if rel and rel.startswith("example/"):
                out.add(rel[len("example/"):])
    return out


ALLOWED_SHOTS = declared_shots()

# A viewer creates a real GPU window; serialize runs so two of them never fight
# over the same adapter / swapchain at the same time.
RUN_LOCK = threading.Lock()

VERSIONS = {v["id"]: v for v in DATA["versions"]}


def md5(path: Path) -> str | None:
    if not path.is_file():
        return None
    h = hashlib.md5()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def exe_for(version: dict, backend: str) -> Path:
    tmpl = version.get("exe", {}).get(backend)
    if not tmpl:
        return ROOT / "__missing__"
    return ROOT / tmpl.replace("{tag}", version["tag"])


def baseline_for(case: dict, backend: str) -> Path | None:
    rel = (case.get("baselines") or {}).get(backend)
    return ROOT / rel if rel else None


def before_baseline_for(case: dict, backend: str) -> Path | None:
    rel = (case.get("beforeBaselines") or {}).get(backend)
    return ROOT / rel if rel else None


def shot_url(path: Path) -> str:
    return "/shots/" + path.relative_to(EXAMPLES).as_posix()


def compare_pixels(path_a: Path, path_b: Path) -> dict:
    """Compare two PNGs pixel-wise. Pillow is used when available (fast);
    otherwise a small stdlib decoder handles the RGB/RGBA8 non-interlaced PNGs
    that stb_image_write produces. Returns max channel delta and pixel count."""
    try:
        from PIL import Image, ImageChops
        a = Image.open(path_a).convert("RGBA")
        b = Image.open(path_b).convert("RGBA")
        if a.size != b.size:
            return {"sameSize": False, "maxDelta": None, "diffPixels": None}
        d = ImageChops.difference(a, b)
        max_delta = max(hi for lo, hi in d.getextrema())
        # Per-channel max, so a 1-step difference in any channel is counted.
        r, g, b, al = d.split()
        mx = ImageChops.lighter(ImageChops.lighter(r, g), ImageChops.lighter(b, al))
        hist = mx.histogram()
        return {"sameSize": True, "maxDelta": int(max_delta),
                "diffPixels": int(sum(hist[1:]))}
    except Exception:
        pass  # Pillow missing or unreadable PNG -> stdlib decoder below

    def decode(path: Path):
        import struct
        import zlib
        buf = path.read_bytes()
        if buf[:8] != b"\x89PNG\r\n\x1a\n":
            raise ValueError("not a PNG")
        pos, idat = 8, bytearray()
        while pos < len(buf):
            length = struct.unpack_from(">I", buf, pos)[0]
            typ = buf[pos + 4:pos + 8]
            data = buf[pos + 8:pos + 8 + length]
            if typ == b"IHDR":
                w, h, depth, ctype = struct.unpack_from(">IIBB", data, 0)
            elif typ == b"IDAT":
                idat += data
            pos += 12 + length
        if depth != 8 or ctype not in (2, 6):
            raise ValueError("only 8-bit RGB/RGBA PNGs supported")
        bpp = 4 if ctype == 6 else 3
        stride = w * bpp
        raw = zlib.decompress(bytes(idat))
        pixels = bytearray(h * stride)
        prev = bytearray(stride)
        off = 0
        for y in range(h):
            ft = raw[off]
            line = bytearray(raw[off + 1:off + 1 + stride])
            off += 1 + stride
            if ft:
                for i in range(stride):
                    a = line[i - bpp] if i >= bpp else 0
                    b = prev[i]
                    c = prev[i - bpp] if i >= bpp else 0
                    if ft == 1:
                        pred = a
                    elif ft == 2:
                        pred = b
                    elif ft == 3:
                        pred = (a + b) >> 1
                    elif ft == 4:
                        p = a + b - c
                        pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                        pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                    else:
                        raise ValueError(f"bad PNG filter {ft}")
                    line[i] = (line[i] + pred) & 255
            pixels[y * stride:(y + 1) * stride] = line
            prev = line
        return w, h, bpp, pixels

    wa, ha, bppa, pa = decode(path_a)
    wb, hb, bppb, pb = decode(path_b)
    if (wa, ha, bppa) != (wb, hb, bppb):
        return {"sameSize": False, "maxDelta": None, "diffPixels": None}
    max_delta, diff_pixels = 0, 0
    for i in range(0, len(pa), bppa):
        local = max(abs(pa[i + k] - pb[i + k]) for k in range(bppa))
        if local:
            diff_pixels += 1
            if local > max_delta:
                max_delta = local
    return {"sameSize": True, "maxDelta": max_delta, "diffPixels": diff_pixels}


def file_info(p: Path | None) -> dict:
    return {
        "path": p.relative_to(ROOT).as_posix() if p else None,
        "url": shot_url(p) if p and p.is_file() else None,
        "exists": bool(p and p.is_file()),
        "md5": md5(p) if p else None,
    }


STAGE_FILE = ROOT / "renderer" / "src" / "renderer.cpp"
STAGE_RE = re.compile(r'kStage\s*=\s*"((?:M|S)\d+(?:\.\d+)?)')


def stage_sync() -> dict:
    """Gate: every new iteration must land its successful cases in example/.

    The current iteration is declared by kStage in renderer.cpp, so a milestone
    that only changes code (without adding cases to example/cases.json) shows up
    here as "not yet landed". Run `server.py --check` before tagging a release.
    """
    reasons: list[str] = []
    stage = None
    version_id = None
    try:
        m = STAGE_RE.search(STAGE_FILE.read_text(encoding="utf-8"))
        if m:
            stage = m.group(1)
            version_id = stage.lower()
    except OSError as e:
        reasons.append(f"读不到 {STAGE_FILE.relative_to(ROOT).as_posix()}: {e}")
    if not stage:
        reasons.append("renderer.cpp 里没有可识别的 kStage（M0.x / Sx.y）")

    v = VERSIONS.get(version_id) if version_id else None
    if not v:
        reasons.append(
            f"example/cases.json 缺版本 `{version_id or stage}`：本轮迭代的成功用例还没归位")
    visible = [c for c in (v["cases"] if v else []) if c.get("kind") != "manual"]
    if v and not visible:
        reasons.append(f"`{version_id}` 没有可运行 / 可展示的用例")

    baselines = 0
    for c in visible:
        for key, getter in (("baselines", baseline_for),
                            ("beforeBaselines", before_baseline_for)):
            for backend in ("dx12", "vulkan"):
                rel = (c.get(key) or {}).get(backend)
                if not rel:
                    continue
                p = getter(c, backend)
                if p and p.is_file():
                    baselines += 1
                else:
                    reasons.append(f"基线缺失：{rel}")
        # 工具版本不产 GPU 截图，但必须有从工具真实输出生成的证据图
        # （M2.1 固化约定：preview 静态图 + run.bat 复现）。缺失或文件不存在
        # 都算「没有基线证据」。
        pv = c.get("preview")
        if pv:
            if (ROOT / pv).is_file():
                baselines += 1
            else:
                reasons.append(f"证据图缺失：{pv}")
        script = c.get("script")
        if script and not (ROOT / script.split()[0]).is_file():
            reasons.append(f"复现脚本缺失：{script.split()[0]}")
    if v and baselines == 0:
        reasons.append(f"`{version_id}` 没有任何基线截图")

    return {"ok": not reasons, "stage": stage, "version": version_id,
            "cases": len(visible), "baselines": baselines, "reasons": reasons}


def decorate() -> dict:
    """cases.json plus live facts: which exe/baseline files exist, hashes."""
    out = json.loads(json.dumps(DATA))  # cheap deep copy
    out["stageSync"] = stage_sync()
    for v in out["versions"]:
        v["status"] = {
            b: {"exe": str(exe_for(v, b)).replace(str(ROOT) + "\\", ""),
                "ready": exe_for(v, b).is_file()}
            for b in ("dx12", "vulkan")
        }
        for c in v["cases"]:
            for key, getter in (("baselineInfo", baseline_for),
                                ("beforeInfo", before_baseline_for)):
                c[key] = {b: file_info(getter(c, b)) for b in ("dx12", "vulkan")}
    return out


def validated_params(case: dict, supplied: dict) -> dict[str, str]:
    """Return only declared params, each checked against its own spec."""
    spec = case.get("params") or {}
    result: dict[str, str] = {}
    for name, cfg in spec.items():
        raw = supplied.get(name, cfg.get("default"))
        value = str(raw)
        if cfg.get("type") == "select":
            if value not in cfg["options"]:
                raise ValueError(f"param {name}: {value!r} not in {cfg['options']}")
        elif cfg.get("type") == "range":
            try:
                num = float(value)
            except ValueError as e:
                raise ValueError(f"param {name}: not a number") from e
            if not (float(cfg["min"]) <= num <= float(cfg["max"])):
                raise ValueError(f"param {name}: {value} out of range")
            value = ("%g" % num)
        else:
            raise ValueError(f"param {name}: unknown spec")
        result[name] = value
    return result


def build_argv(case: dict, params: dict[str, str], out: Path | None) -> list[str]:
    argv: list[str] = []
    for token in case.get("args", []):
        if token == "{out}":
            if out is None:
                raise ValueError("case args need {out} but this is a text-only run")
            argv.append(str(out))
        else:
            def sub(m: re.Match[str]) -> str:
                key = m.group(1)
                if key == "root":
                    return str(ROOT)   # 工具用例引用仓库内文件的唯一白名单方式
                if key not in params:
                    raise ValueError(f"unresolved placeholder {{{key}}} in case args")
                return params[key]
            argv.append(re.sub(r"\{(\w+)\}", sub, token))
    return argv


def run_one(version: dict, case: dict, backend: str, supplied: dict) -> dict:
    started = time.time()
    exe = exe_for(version, backend)
    if not exe.is_file():
        return {"ok": False, "backend": backend,
                "error": f"该版本 {backend} 的 exe 未构建，无法现场运行：{exe.relative_to(ROOT).as_posix()}",
                "log": "", "ms": 0}

    params = validated_params(case, supplied)
    kind = case.get("kind")
    out = None
    if kind in ("image", "pair"):
        out = RUNS / f"{version['id']}-{case['id']}-{backend}-{int(started * 1000)}.png"
        for stale in (out, out.with_name(out.name + ".before.png")):
            if stale.exists():
                stale.unlink()

    try:
        argv = build_argv(case, params, out)
    except ValueError as e:
        return {"ok": False, "backend": backend, "error": str(e), "log": "", "ms": 0}

    try:
        with RUN_LOCK:
            proc = subprocess.run(
                [str(exe), *argv],
                cwd=str(exe.parent),
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=120,
            )
        log = (proc.stdout or "") + (proc.stderr or "")
        code = proc.returncode
    except subprocess.TimeoutExpired as e:
        log = (e.stdout or "") + (e.stderr or "")
        if isinstance(log, bytes):
            log = log.decode("utf-8", "replace")
        return {"ok": False, "backend": backend, "error": "运行超时（120s）",
                "log": log[-8000:], "ms": int((time.time() - started) * 1000)}
    except OSError as e:
        return {"ok": False, "backend": backend, "error": f"无法启动进程：{e}",
                "log": "", "ms": int((time.time() - started) * 1000)}

    result: dict = {
        "ok": True,
        "backend": backend,
        "exit": code,
        "log": log[-8000:],
        "ms": int((time.time() - started) * 1000),
        "params": params,
        "command": exe.name + " " + " ".join(
            ("<out.png>" if a == str(out) else a.replace(str(ROOT), "<root>")) for a in argv),
    }

    m = re.findall(r"deviceErrors=(\d+)", log)
    result["deviceErrors"] = int(m[-1]) if m else None
    # 工具用例（kind:tool）约定非零退出码也是合法预期：expectExit 声明（如
    # usage 的 1、未实现子命令的 2），防静默假成功而不是把失败当成功。
    expected_exit = case.get("expectExit", 0)
    if code != expected_exit:
        result["ok"] = False
        result["error"] = (f"退出码 {code}" +
                           (f"（预期 {expected_exit}）" if expected_exit else ""))
    if case.get("expectLog") and case["expectLog"] not in log:
        result["ok"] = False
        result["error"] = f"日志里没找到 {case['expectLog']!r}"

    if out is not None:
        if not out.is_file():
            result["ok"] = False
            result["error"] = (result.get("error", "") + " / 没有生成 PNG").strip(" /")
        else:
            tag = str(int(started * 1000))
            result["image"] = "/runs/" + out.name + "?t=" + tag
            result["_path"] = str(out)
            result["md5"] = md5(out)
            baseline = baseline_for(case, backend)
            if baseline and baseline.is_file():
                bmd5 = md5(baseline)
                result["baselineMd5"] = bmd5
                result["baselineImage"] = shot_url(baseline)
                result["baselineMatch"] = (bmd5 == result["md5"])
            bb = before_baseline_for(case, backend)
            if bb and bb.is_file():
                result["beforeBaselineImage"] = shot_url(bb)
            before = out.with_name(out.name + ".before.png")
            if before.is_file():
                result["beforeImage"] = "/runs/" + before.name + "?t=" + tag
    return result


def do_run(payload: dict) -> dict:
    vid = payload.get("version", "")
    cid = payload.get("case", "")
    backend = payload.get("backend", "dx12")
    if vid not in VERSIONS:
        return {"ok": False, "error": f"未知版本 {vid!r}"}
    version = VERSIONS[vid]
    case = next((c for c in version["cases"] if c["id"] == cid), None)
    if case is None:
        return {"ok": False, "error": f"未知用例 {cid!r}"}
    if case.get("kind") == "manual":
        return {"ok": False, "error": "这是交互用例（需要真实窗口 + ESC 退出），请在命令行手动运行。"}
    if backend not in ("dx12", "vulkan", "both"):
        return {"ok": False, "error": f"未知后端 {backend!r}"}

    supplied = payload.get("params") or {}
    if not isinstance(supplied, dict):
        return {"ok": False, "error": "params 必须是对象"}

    try:
        if backend == "both":
            a = run_one(version, case, "dx12", supplied)
            b = run_one(version, case, "vulkan", supplied)
            results = [a, b]
            identical, pixel_diff = None, None  # text-only runs: no comparison
            if a.get("md5") and b.get("md5"):
                if a["md5"] == b["md5"]:
                    identical = "bytes"
                else:
                    pixel_diff = compare_pixels(Path(a["_path"]), Path(b["_path"]))
                    if pixel_diff.get("sameSize") and pixel_diff.get("maxDelta") == 0:
                        identical = "pixels"
                    elif pixel_diff.get("maxDelta") is not None and pixel_diff["maxDelta"] <= 1:
                        identical = "near"  # GPU sampling/rounding noise
                    else:
                        identical = False
            for r in results:
                r.pop("_path", None)
            return {"ok": a["ok"] and b["ok"], "results": results,
                    "identical": identical, "pixelDiff": pixel_diff}
        r = run_one(version, case, backend, supplied)
        r.pop("_path", None)
        return {"ok": r["ok"], "results": [r], "identical": None}
    except ValueError as e:
        return {"ok": False, "error": str(e)}


def do_interactive(payload: dict) -> dict:
    """开交互窗口：启动该版本 viewer.exe（--mode normal --hud，无 --screenshot）。

    与 /api/run 的截图回归不同，这里不等待进程结束、不比对基线——窗口保持打开，
    由用户手动关闭（ESC 或点 X）。一次最多起两个窗口（双后端时）。
    """
    vid = payload.get("version", "")
    if vid not in VERSIONS:
        return {"ok": False, "error": f"未知版本 {vid!r}"}
    version = VERSIONS[vid]
    backend = payload.get("backend", "both")
    if backend not in ("dx12", "vulkan", "both"):
        return {"ok": False, "error": f"未知后端 {backend!r}"}
    backends = ["dx12", "vulkan"] if backend == "both" else [backend]

    spawned = []
    for b in backends:
        exe = exe_for(version, b)
        if not exe.is_file():
            return {"ok": False, "error": (
                f"该版本 {b} 的 viewer.exe 未构建，无法开交互窗口："
                f"{exe.relative_to(ROOT).as_posix()}（历史版本先跑 tools/showcase/rebuild.py）")}
        flags = 0
        if os.name == "nt":
            flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS
        try:
            proc = subprocess.Popen(
                [str(exe), "--mode", "normal", "--hud"],
                cwd=str(exe.parent),
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                stdin=subprocess.DEVNULL, creationflags=flags,
            )
        except OSError as e:
            return {"ok": False, "error": f"无法启动进程：{e}"}
        spawned.append({"backend": b, "pid": proc.pid})
    return {"ok": True, "spawned": spawned,
            "note": "交互窗口已启动，窗口保持打开直到手动关闭（ESC 退出）；"
                    "不会写基线，也不参与 MD5 比对"}


class Handler(BaseHTTPRequestHandler):
    server_version = "nl-showcase/1.0"

    def log_message(self, fmt: str, *args) -> None:
        # Keep the console readable: only API calls and errors.
        if "/api/" in (self.path or ""):
            super().log_message(fmt, *args)

    def _send(self, code: int, body: bytes, ctype: str, cache: bool = False) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store" if not cache else "max-age=5")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj: dict, code: int = 200) -> None:
        self._send(code, json.dumps(obj, ensure_ascii=False).encode("utf-8"),
                   "application/json; charset=utf-8")

    def _file(self, path: Path, cache: bool = False) -> None:
        if not path.is_file():
            self._send(404, b"not found", "text/plain; charset=utf-8")
            return
        ctype = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        if path.suffix in (".html", ".js", ".css", ".json"):
            ctype += "; charset=utf-8"
        self._send(200, path.read_bytes(), ctype, cache)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        if path == "/":
            return self._file(HERE / "index.html")
        if path in ("/app.js", "/style.css"):
            return self._file(HERE / path.lstrip("/"), cache=True)
        if path == "/api/cases":
            return self._json(decorate())
        if path.startswith("/shots/"):
            rel = path[len("/shots/"):]
            if ".." in rel or "\\" in rel or rel not in ALLOWED_SHOTS:
                return self._send(403, b"forbidden", "text/plain")
            return self._file(EXAMPLES / rel, cache=True)
        if path.startswith("/runs/"):
            name = path[len("/runs/"):]
            if "/" in name or "\\" in name or ".." in name:
                return self._send(403, b"forbidden", "text/plain")
            return self._file(RUNS / name, cache=True)
        self._send(404, b"not found", "text/plain; charset=utf-8")

    def do_POST(self) -> None:
        path = urlparse(self.path).path
        if path not in ("/api/run", "/api/interactive"):
            return self._json({"ok": False, "error": "unknown endpoint"}, 404)
        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            if not isinstance(payload, dict):
                raise ValueError("body must be an object")
        except (ValueError, json.JSONDecodeError) as e:
            return self._json({"ok": False, "error": f"bad request: {e}"}, 400)
        if path == "/api/interactive":
            return self._json(do_interactive(payload))
        return self._json(do_run(payload))


def main() -> int:
    ap = argparse.ArgumentParser(description="nl-engine S0 showcase")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--open", action="store_true", help="open the browser on start")
    ap.add_argument("--check", action="store_true",
                    help="verify this iteration's cases landed in example/; exit 1 if not")
    args = ap.parse_args()
    if args.check:
        s = stage_sync()
        print(f"[example-sync] {'OK' if s['ok'] else 'FAIL'}: "
              f"stage={s['stage']} version={s['version']} "
              f"cases={s['cases']} baselines={s['baselines']}")
        for r in s["reasons"]:
            print("  - " + r)
        return 0 if s["ok"] else 1
    RUNS.mkdir(parents=True, exist_ok=True)

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    url = f"http://{args.host}:{args.port}/"
    print(f"nl-engine showcase -> {url}")
    for v in DATA["versions"]:
        ready = [b for b in ("dx12", "vulkan") if exe_for(v, b).is_file()]
        print(f"  {v['label']:<6} {v['tag']:<13} exe ready: {', '.join(ready) or 'NONE'}")
    print("Press Ctrl+C to stop.")
    if args.open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        srv.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
