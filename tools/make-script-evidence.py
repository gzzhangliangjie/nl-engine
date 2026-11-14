# -*- coding: utf-8 -*-
"""P8-Script 工具版证据图生成器（M-SC0 收口段 + M-SC1 console 绑定段 + M-SC2 ECS 数据面段
+ M-SC3 脚本热更段 + M-SC4 绑定生成器段 + M-SC5 Time/Input/Log 域段 + M-SC6 Camera 域段
+ M-SC7 模型挂载段）。

铁律：所有内容从工具**真实输出**渲染（example/<dir>/*.log，
由 nl-script.exe / gen.py 实跑重定向落盘），不许手绘示意。产物进 example/<dir>/。
复现：example/<dir>/run.bat（重跑用例 -> 重新生成本图 -> exit 0 校验）。
用法：python make-script-evidence.py [m-sc0|m-sc1|m-sc2|m-sc3|m-sc4|m-sc5|m-sc6|m-sc7]   （缺省 m-sc0）
"""
import os
import sys

DIR_NAME = sys.argv[1] if len(sys.argv) > 1 else "m-sc0"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.join(ROOT, "example", DIR_NAME)
OUT = LOGS

from PIL import Image, ImageDraw, ImageFont

BG = (18, 20, 24)
FG = (222, 226, 232)
DIM = (130, 138, 148)
GREEN = (108, 220, 128)
CYAN = (86, 182, 224)
AMBER = (235, 185, 90)
RED = (235, 110, 100)


def font(size, bold=False):
    name = "consolab.ttf" if bold else "consola.ttf"
    return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)


def read_log(name):
    with open(os.path.join(LOGS, name), encoding="utf-8", errors="replace") as f:
        return f.read()


def color_for(s):
    t = s.strip()
    if "PASS" in t:
        return GREEN
    if "FAIL" in t:
        return RED
    if "run: ret=" in t or "check: PASS" in t or "run: FAIL" in t:
        return GREEN if "run: ret=" in t or "check: PASS" in t else AMBER
    if "exception" in t.lower() or "0xe0434352" in t:
        return AMBER
    if t.startswith("nl-script") or t.startswith("usage:") or t.startswith("$"):
        return CYAN
    if t.startswith("  ") and t.strip():
        return DIM
    if t.endswith(":") and not t.startswith(" "):
        return DIM
    return FG


def wrap_line(l, limit=118):
    # 超长行按字符折行（terminal 风格续行，前缀缩进对齐）
    if len(l) <= limit:
        return [l]
    out = []
    while len(l) > limit:
        out.append(l[:limit])
        l = "    " + l[limit:]  # 续行缩进
    out.append(l)
    return out


def render(log_name, png_name, title, note=None):
    text = read_log(log_name)
    lines = []
    for l in text.splitlines():
        lines.extend(wrap_line(l.rstrip()))
    if note:
        lines.append("")
        lines.extend(wrap_line(note))
    f = font(17)
    ft = font(22, bold=True)
    lh = 28
    pad = 28
    # 最大行宽决定画布（上限 1180）
    tmp = Image.new("RGB", (4, 4))
    td = ImageDraw.Draw(tmp)
    w = min(1180, max(
        [td.textlength(l, font=f) for l in lines] +
        [td.textlength(title, font=ft)]  # 标题参与宽度计算，防截断
    ) + pad * 2 + 40)
    h = pad * 2 + lh * (len(lines) + 2)
    img = Image.new("RGB", (int(w), h), BG)
    d = ImageDraw.Draw(img)
    y = pad
    d.text((pad, y), title, font=ft, fill=CYAN)
    y += lh * 1.5
    for l in lines:
        d.text((pad, y), l, font=f, fill=color_for(l))
        y += lh
    img.save(os.path.join(OUT, png_name))
    print("wrote %s (%dx%d)" % (png_name, img.width, img.height))


CASES_M_SC0 = [
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC0 acceptance (hosting kernel + hot rebuild)",
     "s1-s2 hosting (hostfxr 3-export + default-signature entry) / s3 return anchors / "
     "s4 managed exception at SEH boundary / s5 rebuild -> behavior update 42->43 in-process."),
    ("run-ok.log", "run-ok.png",
     "nl-script run --args ok -- positive (exit 0, ret=42)",
     "dotnet build -> hostfxr init -> load HelloScript.dll -> call Entry(\"ok\") -> ret 42."),
    ("run-throw.log", "run-throw.png",
     "nl-script run --args throw -- negative (exit 1, managed exception captured)",
     "Managed InvalidOperationException surfaces as SEH 0xe0434352 at the interop boundary; "
     "captured -> Result -> exit 1. Process never crashes."),
    ("check.log", "check.png",
     "nl-script check -- compile only (exit 0)",
     "dotnet build only; no runtime init. FDD contract: runtimeconfig.json generated "
     "via GenerateRuntimeConfigurationFiles."),
    ("usage.log", "usage.png",
     "nl-script (no args) -- usage error (exit 2)",
     "Exit-code convention: 0=PASS / 1=fail / 2=usage (showcase expectExit aligned)."),
]

CASES_M_SC1 = [
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC1 console binding (s6a-s6f, on top of M-SC0 s0-s5)",
     "s6a C# cvar.set fires C++ onChanged hook / s6b C#-registered command driven by console exec / "
     "s6c ExecCode 0/1/2/3 aligned across the binding (mask 31) / "
     "s6d managed exception in command -> SEH at boundary -> CommandFailed, console alive / "
     "s6e C++-set float read back from C# + cvar.list full view / s6f unregister -> UnknownCommand."),
    ("run-api.log", "run-api.png",
     "nl-script run --api -- demo (exit 0)",
     "Entry receives ApiTable* (6 native fn pointers): C# registers script.echo, sets script.count, "
     "execs it back, drives builtin cvar.set via exec, reads the value from C#, dumps cvar.list."),
    ("run-badtype.log", "run-badtype.png",
     "nl-script run --api without --type -- negative (exit 1, clean Result error)",
     "Default type name does not exist in ConsoleScript assembly -> getEntryPoint returns "
     "Result{code=5} surfaced as 'run: FAIL getEntryPoint'; no crash, exit 1."),
    ("check.log", "check.png",
     "nl-script check console example -- compile only (exit 0)",
     "dotnet build only; binding entry points require [UnmanagedCallersOnly] default signature."),
]

CASES_M_SC2 = [
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC2 ECS binding (s7a-s7g, on top of M-SC0 s0-s5 + M-SC1 s6)",
     "s7a C# spawn+Transform+fields -> C++ reads back exact / s7b C++ sets anchors via table, "
     "C# verifies / s7c invalid entity ops all rejected NoEntity, no crash / "
     "s7d deferred removal + flush asymmetry (Carrot) + table TransformGet safe reject / "
     "s7e parent roundtrip via table + getChildIDs view / s7f JSON roundtrip bytes equal / "
     "s7g unbound World -> all exports NoWorld (M-SC3 hot-reload window fallback)."),
    ("run-api.log", "run-api.png",
     "nl-script run --api on ecs example -- demo (exit 0)",
     "Entry receives ApiTable* (v2, 16 fn pointers): spawn root+leaf, setParent/getParent, "
     "addTransform, transformSet/Get, flush, remove root (children cascade), flush again."),
    ("run-badtype.log", "run-badtype.png",
     "nl-script run --api with wrong --type -- negative (exit 1, clean Result error)",
     "HelloScript type does not exist in EcsScript assembly -> getEntryPoint returns "
     "Result{code=5}; no crash, exit 1."),
    ("check.log", "check.png",
     "nl-script check ecs example -- compile only (exit 0)",
     "dotnet build only; binding entry points require [UnmanagedCallersOnly] default signature."),
]

CASES_M_SC3 = [
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC3 hot reload (s8a-s8h, on top of s0-s5 + s6 + s7)",
     "s8a ScriptHost::init (bootstrap 3 fn pointers) + console.reload-script registered / "
     "s8b first load via collectible ALC (LoadFromStream, file not locked) + Entry rebinding / "
     "s8c edit script source -> dotnet build overwrites same DLL name -> ScriptHost::reloadScript / "
     "s8d behavior updated (echo prefix v1 -> [v2]) / s8e C++ state intact "
     "(cvar values, entity tree, builtin commands, command ledger re-registered) / "
     "s8f consecutive reload x3, handle drift 0 / s8g unloadScript -> weakref dead "
     "(ALC fully collected) / s8h script commands gone, C++ exports still live."),
    ("reload.log", "reload.png",
     "s8 hot-reload section close-up (filtered from selftest.log)",
     "reload-script lifecycle: unbind old ALC thunks (scriptUnregisterAll) -> Unload "
     "(weakref dead after GC drain) -> reload same DLL -> Entry re-registers commands. "
     "Key pitfall fixed: unload and GC drain must live in separate (NoInlining) methods -- "
     "same-frame 'alc = null' is a JIT dead store, stack keeps rooting the ALC forever."),
    ("run-api.log", "run-api.png",
     "nl-script run --api on console example -- demo (exit 0)",
     "Entry (managed, collectible-reflectable) behind [UnmanagedCallersOnly] EntryNative: "
     "registers script.echo, sets script.count, execs back, drives builtin cvar.set, "
     "dumps cvar.list."),
    ("check.log", "check.png",
     "nl-script check bootstrap -- compile only (exit 0)",
     "Bootstrap is the non-collectible relay assembly (BootLoad/BootCall/BootUnload); "
     "FDD contract: runtimeconfig.json via GenerateRuntimeConfigurationFiles."),
]

CASES_M_SC4 = [
    ("gen-check.log", "gen-check.png",
     "gen.py --check -- golden self-verify (regenerate in-memory, byte-compare committed artifacts)",
     "Single source of truth: script/script-api/exports.api (25 exports = console 6 + ECS 10 + "
     "Time 2 / Input 5 / Log 2 + TransformData POD). --check exits 0 only when all four generated "
     "artifacts byte-match the committed ones: ApiTable.g.hpp / ApiTable.g.inc / "
     "ApiTable.g.asserts.inc / Wiring.g.cs."),
    ("gen-build.log", "gen-build.png",
     "gen.py -- regenerate artifacts from exports.api (TYPEMAP + lifetime ledger single source)",
     "One export line in exports.api -> typedef + table slot (C++) + delegate + Init offset (C#) "
     "+ static_assert signature guard. Manual surface: C++ implementation body + C# facade "
     "semantics only. Prior art: Flax API_ macros / csbindgen artifacts-in-git / "
     "UnrealSharp property translators."),
    ("asserts.log", "asserts.png",
     "ApiTable.g.asserts.inc -- compile-time drift gate (excerpt of real generated file)",
     "Per-export static_assert(is_same_v<decltype(&nl_xxx), declared signature>): a TYPEMAP mistake "
     "or hand-edited signature fails the build immediately (caught out u64->int* for ecsSpawn/"
     "GetParent/FindByName on first run). Build freshness gate: cmake -P timestamp check "
     "(exports.api newer than artifacts = build red)."),
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC4 acceptance: 16 legacy exports replayed through generated wiring",
     "s0-s5 hosting + s6 console + s7 ECS + s8 hot reload all green on generated ApiTable "
     "(zero hand-written table code left); reload-script path re-verified after the "
     "reloadScript getEntryPoint fix (M-GL4: OnTick rebinding must use bootstrap scriptTickFn, "
     "never getEntryPoint which loads the script into the default ALC)."),
]

CASES_M_SC5 = [
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC5 Time/Input/Log (s9a-s9b, on top of s0-s5 + s6 + s7 + s8)",
     "s9a Time: host GameClock tick x3 -> simTime 0.05 / tickCount 3; timeSetState sentinel "
     "(-1 = unchanged); unbound -> NoClock. s9b Input: ActionMap frozen values Bool/Float/Vec2 "
     "(W/D feed), type mismatch -> NoAction, KeyIsDown out-of-range guard, "
     "inputSetState/inputSetActionMap unbound -> NoSource; Log: [nl:info|script] channel, "
     "logSetFilter effect + invalid instruction explicit error."),
    ("s9.log", "s9.png",
     "s9 section close-up (filtered from selftest.log)",
     "New domain exports live-read the host instances: timeSetClock/inputSetState/"
     "inputSetActionMap follow the ecsSetWorld pattern (host sets live instance before entry, "
     "no implicit table creation). ApiTable grew 16 -> 25 via exports.api tail-append; "
     "TYPEMAP added f64/u32 primitives (double simTime/frameDt, uint32 ActionId)."),
    ("log-channel.log", "log-channel.png",
     "log channel anchor: [nl:info|script] on stdout (filtered from selftest.log)",
     "logWrite routes through nl::core::log writeCh with channel 'script' -- the stdout "
     "[nl:level|channel] contract gives regression cards a stable expectLog anchor. "
     "logSetFilter rejects invalid instructions with the parser reason surfaced."),
    ("check.log", "check.png",
     "nl-script check timeinput example -- compile only (exit 0)",
     "dotnet build only; s9 probes (BoundProbe/UnboundProbe) verified in selftest; "
     "nl-script links glfw PRIVATE because input.hpp pulls GLFW headers "
     "(nl_core glfw link is PRIVATE, does not propagate includes)."),
]

CASES_M_SC6 = [
    ("gen-check.log", "gen-check.png",
     "gen.py --check -- golden self-verify (v4: 29 exports = 25 + camera 4)",
     "Camera domain appended to exports.api (cameraGetPose/cameraSetPose/cameraGetFov/"
     "cameraSetFov, tail-append like every previous milestone) -> generated artifacts stay "
     "byte-identical after regenerate; declarations remain the only hand-written surface. "
     "GetFov unbound = NaN is the f32-no-rc-channel ledger exception, documented in "
     "exports.api itself."),
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC6 Camera (s10a-s10c, on top of s0-s5 + s6 + s7 + s8 + s9)",
     "s10a bound: canned pose readback (eye 1.5/2.5/3.5, yaw 30, pitch -10, fov 60, orbit) + "
     "SetPose rc Ok + BadArg on null eye + fov roundtrip 60->75->clamp 500->120 (C++ captures "
     "{4,5,6,45,15}). s10b track (mode=3): GetPose reads fine, SetPose -> ModeUnsupported "
     "(track data sovereignty), SetFov still ok (mode-uniform). s10c unbound: NoSource / "
     "GetFov NaN, no crash. CameraSource = 4 fn ptrs + ctx host adapter (nl_core must not "
     "depend on viewer; bare CameraRig would lose fov persistence + orbit control)."),
    ("s10.log", "s10.png",
     "s10 section close-up (filtered from selftest.log)",
     "cameraSetSource follows the ecsSetWorld pattern (explicit host set before entry, no "
     "implicit table creation); mode-aware SetPose semantics live in the viewer adapter "
     "(orbit=setOrbit(yaw,pitch,|eye|) / free=setPose / chase=snapEyeTo+pitch / "
     "track=ModeUnsupported). ApiTable grew 25 -> 29 via exports.api tail-append; "
     "Api.Version 0.6.0."),
    ("check.log", "check.png",
     "nl-script check camera example -- compile only (exit 0)",
     "dotnet build only; s10 probes (BoundProbe/TrackProbe/UnboundProbe) verified in "
     "selftest against the canned CameraSource; SetFov clamp [20,120] mirrors OrbitCamera "
     "so a script cannot fight the host's fov persistence."),
]

CASES_M_SC7 = [
    ("gen-check.log", "gen-check.png",
     "gen.py --check -- golden self-verify (v5: 32 exports = 29 + ecs model 3; Init-offset bugfix)",
     "Ecs domain tail-append (ecsAttachModel/ecsDetachModel/ecsModelInfo) -> 32 exports. "
     "Also fixed a real generator bug found by this milestone: C# Init offsets were "
     "assigned per first-appearance domain order with a running counter, so appending to a "
     "non-tail domain (Ecs) while Time/Input/Log/Camera follow silently misaligned every "
     "later domain's slot; offsets are now per-export global declaration indices (Ecs = "
     "6..15,29..31). Table struct/registration were always declaration-ordered and never "
     "affected."),
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC7 model attach (s11a-s11d, on top of s0-s5 + s6 + s7 + s8 + s9 + s10)",
     "s11a bound: spawn + AddTransform + AttachModel(cube.glb) -> ModelInfo readback "
     "(meshFile / default tint 1,1,1 / roughness 1 / metallic 0) + C++ capture: "
     "ModelComponent landed in canned world with meshFile=cube.glb. s11b error codes "
     "1-by-1: NoEntity=2 (attach/detach/info), BadArg=4 empty name, BadAsset=6 "
     "(../secret.glb / model.gltf / dir/model.glb rejected BEFORE touching the world), "
     "NoTransform=5 (no implicit table), Component=3 duplicate attach rejected without "
     "state change (World::addComponent duplicate = assert, binding must block first). "
     "s11c unbound: NoWorld x3, no crash. s11d detach: Model gone, Transform kept "
     "(C++-verified), re-detach = Component."),
    ("s11.log", "s11.png",
     "s11 section close-up (filtered from selftest.log)",
     "Attach semantics (2026-11-14 approved plan A): binding only mutates components + "
     "validates whitelist; the viewer compares the Model-set signature (entity id + "
     "meshFile) at frame boundary after tick and re-runs collection + loadEcsScene "
     "(initScene now re-entrant via shutdownEcsScene — second call used to leak the "
     "whole GPU resource set). Instance matrices keep flowing through the existing "
     "M-GL3 interpolation chain. ApiTable 29 -> 32; Api.Version 0.7.0."),
    ("check.log", "check.png",
     "nl-script check model example -- compile only (exit 0)",
     "dotnet build only; s11 probes (BoundProbe/ErrorProbe/UnboundProbe/DetachProbe) "
     "verified in selftest against the canned world; whitelist single-sourced into "
     "nl::scene::validAssetFileName (AssetName.hpp) replacing four drifting copies "
     "(renderer.cpp original + scene.cpp + viewer lambda + SceneGeo stricter variant)."),
]

CASES_M_SC8 = [
    ("gen-check.log", "gen-check.png",
     "gen.py --check -- golden self-verify (v6: 37 exports = 32 + physics vehicle 5)",
     "Physics domain tail-append (physicsAddVehicle/physicsRemoveVehicle/"
     "physicsSetVehicleInput/vehicleFullPose/teleportVehicle) -> 37 exports, "
     "Api.Version 0.8.0. buf4 (quaternion xyzw) is the first 4-float buffer type "
     "in the TYPEMAP (generic BUF_RE regex covers any width). Plan L107 wording "
     "kept: direct M-NET2 DebugScene surface, zero new adapter types."),
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC8 vehicle domain (s12a-s12b, on top of s0-s5 + s6 + s7 + s8 + s9 + s10 + s11)",
     "s12a bound (real DebugScene build(2), driveSetScene direct pointer per "
     "Q-SC8-1 A'): AddVehicle slot=1 (slot0 = built-in car) + 4-input write "
     "(Jolt handbrake float 0..1) + FullPose incl. out-NULL skip path + Teleport "
     "bit-exact readback (DontActivate) + RemoveVehicle tombstone -> FullPose/"
     "SetInput/Teleport all NoSlot, slot0 untouched. C++ capture: slot0 valid, "
     "script car tombstoned. s12b unbound: NoScene x5, no crash."),
    ("s12.log", "s12.png",
     "s12 section close-up (filtered from selftest.log)",
     "DriveScript probes (BoundProbe/UnboundProbe) verified against a real "
     "training ground: physics::DebugScene build(2) in nl-script selftest process, "
     "driveSetScene registers it (ecsSetWorld mode generalization). Viewer "
     "runNormal wiring: --script now also loads the script host in physics "
     "debug mode (scenario 2), script OnTick runs in the Physics tick group "
     "BEFORE the Jolt step (Q-SC8-2 A: registered-party steps)."),
    ("check.log", "check.png",
     "nl-script check drive example -- compile only (exit 0)",
     "dotnet build only; s12 probes (BoundProbe/UnboundProbe) verified in "
     "selftest against the canned DebugScene; C# facade Physics.* static class "
     "(AddVehicle/RemoveVehicle/SetVehicleInput/FullPose/TeleportVehicle), "
     "stackalloc triple-expression fix (Span cannot coerce to float*)."),
]

CASES_M_SC10 = [
    ("gen-check.log", "gen-check.png",
     "gen.py --check -- golden self-verify (v7: 39 exports = 37 + props 2)",
     "Props domain tail-append (propsPushBegin/propsDescAdd) -> 39 exports, "
     "Api.Version 0.9.0. Descriptor table = C#-reflected [Prop] static fields "
     "pushed as POD (lifecycle hard rule 1: FieldInfo cache stays managed-side "
     "dies with the ALC; C++ stores POD only). gen counter bumps on pushBegin = "
     "panel rebuild signal. Value read/write does NOT go through ApiTable: "
     "PropGet/PropSet optional contract resolved by name via the BootTickPtr "
     "protocol (same lifecycle as OnTick)."),
    ("selftest.log", "selftest.png",
     "nl-script selftest -- M-SC10 props panel (s13a-s13e, on top of s0-s5 + s6-s12)",
     "s13a loadScript -> descriptor table (4 rows, kind/Range/default bits per "
     "row) + channel attached. s13a2 value roundtrip float/int/bool/string via "
     "propGet/propSet + C# cross-source check (ReadField probe through bootstrap "
     "BootCall reads the live static field == 42). s13b negatives: idx out of "
     "range -> rc 8 (script-side 2), bad buffer -> rc 2 C++-side. s13c hot "
     "reload follow: add [Prop(\"Extra\", 0, 50)] field -> rebuild -> reload -> "
     "table 4 -> 5, channel re-resolved. s13d leak section: 10k string "
     "roundtrips + GC.Collect probe -> handle drift < 32 (s8f method extended, "
     "lifecycle ledger hard rule 2). s13e unload: channel nulled BEFORE "
     "scriptUnload, POD table persists C++-owned."),
    ("s13.log", "s13.png",
     "s13 section close-up (filtered from selftest.log)",
     "Panel data flow: C# PropsPush.PushStatic reflects [Prop] static fields "
     "(MetadataToken order = declaration-order stabilization) -> propsPushBegin/"
     "propsDescAdd -> C++ POD table. Viewer --prop-panel (additive, both "
     "runScene/runNormal) rebuilds rows on gen change, pulls initial values via "
     "propGet, pushes edits via propSet on dirty edge. State is a HudFrame "
     "VALUE member: viewer copies wholesale in before the frame and back after "
     "(M-T5 lesson: pointer alone renders zeros)."),
    ("check.log", "check.png",
     "nl-script check props example -- compile only (exit 0)",
     "dotnet build only; s13 probes (ReadField/GCCollect) verified in selftest "
     "through bootstrap BootCall. C# side: PropAttribute (DisplayName/Range, "
     "Default = field value snapshot at push time) + PropsPush helper "
     "(PushStatic/PropGet/PropSet one-line forwards), FieldInfo cache keyed by "
     "Type inside script-api assembly -> dies with the collectible ALC."),
]

CASES = {"m-sc0": CASES_M_SC0, "m-sc1": CASES_M_SC1, "m-sc2": CASES_M_SC2,
         "m-sc3": CASES_M_SC3, "m-sc4": CASES_M_SC4,
         "m-sc5": CASES_M_SC5, "m-sc6": CASES_M_SC6, "m-sc7": CASES_M_SC7,
         "m-sc8": CASES_M_SC8, "m-sc10": CASES_M_SC10}[DIR_NAME]


def main():
    os.makedirs(OUT, exist_ok=True)
    for log_name, png_name, title, note in CASES:
        render(log_name, png_name, title, note)
    print("%s evidence: %d PNGs -> %s" % (DIR_NAME, len(CASES), OUT))


if __name__ == "__main__":
    main()
