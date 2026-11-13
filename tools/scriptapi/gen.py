#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""nl-engine 脚本绑定面生成器（M-SC4，docs/p8/P8-SCRIPT-PLAN.md「胶水层扩展」节口径）。

单源声明 script/script-api/exports.api → 生成 4 份接线（入 git，勿手改）：
  1. core/include/nl/script/ApiTable.g.hpp        POD 镜像 struct + ApiTable 结构
  2. core/src/script/ApiTable.g.inc               ApiTable 注册行（Bindings.cpp 内 include）
  3. core/src/script/ApiTable.g.asserts.inc       编译期签名防漂移 static_assert
  4. script/script-api/Wiring.g.cs                C# delegate/字段/Init/NativeMarshal/POD 封装

单源原则（2026-11-14 用户拍板）：
  - 类型映射（TYPEMAP）与生命周期账本规则只在本文件写一遍，全部导出统一走；
  - type 段只收纯 POD（Blittable 一档，禁字符串/嵌套引用，UnrealSharp 对照拍板）；
  - 容器不过界：动态数组走 count+指针协议（声明层不收容器类型）；
  - C++ 实现体与 C# facade 手写（语义本体），接线全生成。
  - 生成物入 git；CMake nl_scriptapi_freshness 目标做新鲜度闸（过期 = 构建红）。

用法：
  python tools/scriptapi/gen.py            # 重跑生成
  python tools/scriptapi/gen.py --check    # 自证：生成物与重跑输出逐字节对比
退出码：0 = ok / 1 = 生成物漂移（--check）/ 2 = 声明解析或校验失败
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DECL = REPO / "script" / "script-api" / "exports.api"
OUT_HPP = REPO / "core" / "include" / "nl" / "script" / "ApiTable.g.hpp"
OUT_INC = REPO / "core" / "src" / "script" / "ApiTable.g.inc"
OUT_ASSERTS = REPO / "core" / "src" / "script" / "ApiTable.g.asserts.inc"
OUT_CS = REPO / "script" / "script-api" / "Wiring.g.cs"

AUTOGEN = "由 tools/scriptapi/gen.py 从 script/script-api/exports.api 生成——勿手改；声明变更后重跑 python tools/scriptapi/gen.py"

# ---- TYPEMAP（生命周期账本的类型侧单源；所有权规则见计划「生命周期账本」）----

PRIM_CPP = {"int": "int", "u64": "std::uint64_t", "f32": "float"}
PRIM_CS = {"int": "int", "u64": "ulong", "f32": "float"}
PRIM_SIZE = {"int": 4, "u64": 8, "f32": 4}
BUF_RE = re.compile(r"^buf(\d+)$")
# type 段字段只收纯 POD（Blittable 一档）；str/ptr 禁止（非 POD 需求显式拆参数）
POD_FIELD_TYPES = {"int", "u64", "f32"}


def pascal(s: str) -> str:
    return "".join(p[:1].upper() + p[1:] for p in re.split(r"[_\s]+", s) if p)


def is_buf(ty: str):
    m = BUF_RE.match(ty)
    return int(m.group(1)) if m else None


class Param:
    def __init__(self, direction, ty, name, count):
        self.direction = direction      # "in" / "out"
        self.ty = ty                    # int/u64/f32/str/ptr/bufN/自定义 POD 类型名
        self.name = name
        self.count = count              # bufN 的 N；其余 None


class Export:
    def __init__(self, domain, name, ret, params, doc):
        self.domain = domain
        self.name = name
        self.ret = ret
        self.params = params            # [Param]
        self.doc = doc


class PodType:
    def __init__(self, name, fields):
        self.name = name
        self.fields = fields            # [(ty, name)]，ty ∈ POD_FIELD_TYPES ∪ bufN

    def cpp_size(self):
        total = 0
        for ty, _f in self.fields:
            b = is_buf(ty)
            total += 4 * b if b else PRIM_SIZE[ty]
        return total


# ---- 解析 ----

def parse_params(spec, lineno, pod_names):
    params = []
    for raw in spec.split(","):
        item = raw.strip()
        if not item:
            continue
        tokens = item.split()
        if len(tokens) == 3:
            direction, ty, name = tokens
        elif len(tokens) == 2:
            direction, (ty, name) = "in", tokens
        else:
            raise SystemExit(f"[scriptapi] exports.api:{lineno} 参数格式非法（应为 [in|out] 类型 名字）：{item!r}")
        if direction not in ("in", "out"):
            raise SystemExit(f"[scriptapi] exports.api:{lineno} 方向只能是 in/out：{item!r}")
        count = is_buf(ty)
        if count is None and ty not in PRIM_CPP and ty != "str" and ty != "ptr" and ty not in pod_names:
            raise SystemExit(f"[scriptapi] exports.api:{lineno} 未知类型 {ty!r}（词表：int/u64/f32/str/bufN/ptr 或 type 段 POD 名）")
        params.append(Param(direction, ty, name, count))
    return params


def parse_decl(path):
    pod_types = {}      # name -> PodType
    exports = []        # [Export]，声明序 = ApiTable 偏移序
    seen_names = set()
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("type "):
            # type Name = f32 A, int B, buf4 C, ...
            m = re.match(r"^type\s+(\w+)\s*=\s*(.+)$", line)
            if not m:
                raise SystemExit(f"[scriptapi] exports.api:{lineno} type 段格式非法：{line!r}")
            tname, field_spec = m.group(1), m.group(2)
            fields = []
            for item in field_spec.split(","):
                item = item.strip()
                if not item:
                    continue
                tokens = item.split()
                if len(tokens) != 2:
                    raise SystemExit(f"[scriptapi] exports.api:{lineno} type 字段格式非法（应为 类型 名字）：{item!r}")
                fty, fname = tokens
                if is_buf(fty) is None and fty not in POD_FIELD_TYPES:
                    raise SystemExit(
                        f"[scriptapi] exports.api:{lineno} type 段只收纯 POD（int/u64/f32/bufN），"
                        f"禁 {fty!r}（非 POD 需求显式拆 str/bufN 参数——Blittable 一档拍板）")
                fields.append((fty, fname))
            if not fields:
                raise SystemExit(f"[scriptapi] exports.api:{lineno} type 段无字段：{tname}")
            pod_types[tname] = PodType(tname, fields)
            continue

        parts = [p.strip() for p in line.split("|", 4)]
        if len(parts) != 5:
            raise SystemExit(f"[scriptapi] exports.api:{lineno} 导出行须 5 段（域|名|返回|参数|文档）：{line!r}")
        domain, name, ret, param_spec, doc = parts
        if not re.fullmatch(r"\w+", domain) or not re.fullmatch(r"\w+", name):
            raise SystemExit(f"[scriptapi] exports.api:{lineno} 域名/导出名只许标识符：{domain}/{name}")
        if name in seen_names:
            raise SystemExit(f"[scriptapi] exports.api:{lineno} 导出名重复：{name}")
        if ret not in ("int", "str", "u64", "f32"):
            raise SystemExit(f"[scriptapi] exports.api:{lineno} 返回类型只支持 int/str/u64/f32：{ret!r}")
        seen_names.add(name)
        exports.append(Export(domain, name, ret, parse_params(param_spec, lineno, pod_types), doc))
    if not exports:
        raise SystemExit("[scriptapi] exports.api 无导出声明")
    return pod_types, exports


# ---- 映射（TYPEMAP 本体）----

def cpp_param(p: Param, pod_names):
    b = is_buf(p.ty)
    if p.ty == "str":
        return "const char**" if p.direction == "out" else "const char*"
    if p.ty == "ptr":
        return "void*"
    if b is not None:
        return "float*" if p.direction == "out" else "const float*"
    if p.ty in pod_names:
        return f"{p.ty}*" if p.direction == "out" else f"const {p.ty}*"
    if p.ty in PRIM_CPP:
        return f"{PRIM_CPP[p.ty]}*" if p.direction == "out" else PRIM_CPP[p.ty]
    return None


def cpp_ret(ty):
    if ty == "str":
        return "const char*"
    return PRIM_CPP[ty]


def cs_param(p: Param, pod_names):
    b = is_buf(p.ty)
    if p.ty == "str":
        return ("out IntPtr" if p.direction == "out" else "IntPtr") + f" {p.name}"
    if p.ty == "ptr":
        return f"IntPtr {p.name}"
    if b is not None:
        return f"float* {p.name}"
    if p.ty in pod_names:
        return f"{p.ty}* {p.name}"
    base = PRIM_CS[p.ty]
    return (f"out {base} {p.name}" if p.direction == "out" else f"{base} {p.name}")


def cs_ret(ty):
    if ty == "str":
        return "IntPtr"
    return PRIM_CS[ty]


def esc_cs_doc(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# ---- 生成 ----

def gen_hpp(pods, exports):
    L = [
        "// <auto-generated>",
        f"// {AUTOGEN}",
        "// </auto-generated>",
        "#pragma once",
        "",
        "#include <cstdint>",
        "#include <type_traits>",
        "",
        "namespace nl::script {",
        "",
    ]
    if pods:
        L += [
            "// ---- POD 镜像（exports.api type 段单源；Blittable 一档，只许数值/定长字段）----",
            "// C# 侧同构封装在 Wiring.g.cs（partial，hand 扩展另落）。现 ABI 未直传 struct 的镜像",
            "// 供未来直接结构传参与 M-SC10 属性描述表复用。",
            "",
        ]
        for pod in pods.values():
            L.append(f"struct {pod.name}")
            L.append("{")
            for fty, fname in pod.fields:
                L.append(f"    {PRIM_CPP[fty]} {fname};")
            L.append("};")
            L.append(f'static_assert(sizeof({pod.name}) == {pod.cpp_size()}, '
                     f'"POD mirror drift: {pod.name}（exports.api type 段与镜像不一致）");')
            L.append("")
    L += [
        "// ---- C ABI api 表（随托管入口 args 传入；C# 侧按偏移 Marshal.ReadIntPtr 读）----",
        "// 字段顺序 = exports.api 声明序（64 位指针，逐个 8 字节）。只许尾部追加，禁中间插桩。",
        "struct ApiTable",
        "{",
    ]
    for e in exports:
        L.append(f"    // {e.domain}.{e.name} — {e.doc}")
        L.append(f"    void* {e.name};")
    L += ["};", "", "} // namespace nl::script", ""]
    return "\n".join(L)


def gen_inc(exports):
    L = [
        "// <auto-generated>",
        f"// {AUTOGEN}",
        "// 用法：Bindings.cpp consoleApiTable() 初始化列表内 #include 本文件。",
        "// </auto-generated>",
        "",
    ]
    for e in exports:
        L.append(f"reinterpret_cast<void*>(&nl_{e.name}),  // {e.domain}.{e.name}")
    return "\n".join(L) + "\n"


def gen_asserts(exports):
    L = [
        "// <auto-generated>",
        f"// {AUTOGEN}",
        "// 编译期签名防漂移：本文件须在 extern \"C\" 实现之后、namespace nl::script 内 include；",
        "// 声明（exports.api）与 C++ 实现签名不一致 = 编译失败（M-SC4 硬闸）。",
        "// </auto-generated>",
        "",
    ]
    for e in exports:
        sig_params = ", ".join(cpp_param(p, {}) for p in e.params) or "void"
        L.append(
            f'static_assert(std::is_same_v<decltype(&nl_{e.name}), '
            f'{cpp_ret(e.ret)}(*)({sig_params})>, "exports.api drift: {e.domain}.{e.name}");'
        )
    return "\n".join(L) + "\n"


def gen_cs(pods, exports):
    domains = []
    for e in exports:
        if e.domain not in domains:
            domains.append(e.domain)
    pod_names = set(pods)

    L = [
        "// <auto-generated>",
        f"// {AUTOGEN}",
        "// 类型映射/生命周期账本单源在 tools/scriptapi/gen.py；口径见 P8-SCRIPT-PLAN.md「胶水层扩展」。",
        "// </auto-generated>",
        "#nullable enable",
        "using System;",
        "using System.Runtime.InteropServices;",
        "",
        "namespace Nl.Script.Api",
        "{",
    ]
    for pod in pods.values():
        L += [
            f"    /// <summary>跨层 POD 镜像（exports.api type 段单源；C++ 侧同构在 ApiTable.g.hpp）。",
            f"    /// partial：hand 扩展（如工厂/便捷成员）另落，勿在本体加字段。</summary>",
            "    [StructLayout(LayoutKind.Sequential)]",
            f"    public partial struct {pod.name}",
            "    {",
        ]
        for fty, fname in pod.fields:
            L.append(f"        public {PRIM_CS.get(fty, 'float')} {fname};")
        L += ["    }", ""]
    L += [
        "    /// <summary>生命周期原语（TYPEMAP 账本类型侧单源；全部导出统一走，禁各 facade 私搭）。",
        "    /// 规则表：in str = 分配 + finally 必释放；out str = C++ 静态缓冲「下次调用前有效」→",
        "    /// 本类立即拷成托管串（C# 侧零分配零释放）。</summary>",
        "    public static unsafe class NativeMarshal",
        "    {",
        "        /// <summary>in str 分配（配对 Utf8Free；调用方 finally 必释放）。</summary>",
        "        public static IntPtr Utf8Alloc(string s)",
        "        {",
        '            byte[] bytes = System.Text.Encoding.UTF8.GetBytes(s ?? "");',
        "            IntPtr p = Marshal.AllocHGlobal(bytes.Length + 1);",
        "            Marshal.Copy(bytes, 0, p, bytes.Length);",
        "            Marshal.WriteByte(p, bytes.Length, 0);",
        "            return p;",
        "        }",
        "",
        "        /// <summary>in str 释放（与 Utf8Alloc 严格配对）。</summary>",
        "        public static void Utf8Free(IntPtr p) => Marshal.FreeHGlobal(p);",
        "",
        "        /// <summary>out str 读回：立即拷成托管串（null 透传）。</summary>",
        "        public static string? Utf8Read(IntPtr p) => p == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(p)!;",
        "",
        "        /// <summary>out str 读回（null 视作空串）。</summary>",
        '        public static string Utf8ReadOrEmpty(IntPtr p) => p == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(p)!;',
        "    }",
        "",
    ]
    offset = 0
    for domain in domains:
        dex = [e for e in exports if e.domain == domain]
        lo, hi = offset, offset + len(dex) - 1
        L += [
            f"    /// <summary>{domain} 域接线（生成物；ApiTable 偏移 {lo}..{hi} = exports.api 声明序）。</summary>",
            f"    internal static unsafe class {pascal(domain)}Wiring",
            "    {",
            "        // ---- C++ 侧函数指针（偏移序 = ApiTable 字段序）----",
            "        " + "public static IntPtr " + ", ".join("_" + e.name for e in dex) + ";",
            "",
        ]
        for e in dex:
            L += [
                f"        /// <summary>{esc_cs_doc(e.doc)}</summary>",
                "        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]",
                f"        public delegate {cs_ret(e.ret)} {pascal(e.name)}Fn({', '.join(cs_param(p, pod_names) for p in e.params)});",
                "",
            ]
        L += ["        /// <summary>由 ApiWiring.InitAll 调用（读本域偏移段）。</summary>",
              "        public static void Init(IntPtr table)",
              "        {"]
        for e in dex:
            L.append(f"            _{e.name} = Marshal.ReadIntPtr(table, {offset} * sizeof(IntPtr));")
            offset += 1
        L += ["        }", "    }", ""]
    L += [
        "    /// <summary>全域接线入口（声明序固化）。Engine.Init 转发；ExportOrder 供 selftest/",
        "    /// 工具卡断言声明序未漂移。</summary>",
        "    public static class ApiWiring",
        "    {",
        f"        public static readonly string[] DomainOrder = new[] {{ {', '.join(chr(34) + d + chr(34) for d in domains)} }};",
        f"        public static readonly string[] ExportOrder = new[] {{ {', '.join(chr(34) + e.name + chr(34) for e in exports)} }};",
        "",
        "        public static void InitAll(IntPtr table)",
        "        {",
    ]
    for domain in domains:
        L.append(f"            {pascal(domain)}Wiring.Init(table);")
    L += ["        }", "    }", "} // namespace", ""]
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(description="scriptapi binding generator (M-SC4)")
    ap.add_argument("--check", action="store_true",
                    help="自证模式：生成物与重跑输出逐字节对比，漂移 exit 1")
    args = ap.parse_args()

    pods, exports = parse_decl(DECL)
    texts = {
        OUT_HPP: gen_hpp(pods, exports),
        OUT_INC: gen_inc(exports),
        OUT_ASSERTS: gen_asserts(exports),
        OUT_CS: gen_cs(pods, exports),
    }

    if args.check:
        bad = []
        for path, text in texts.items():
            old = path.read_text(encoding="utf-8") if path.exists() else None
            if old != text:
                bad.append(path.name)
        if bad:
            print("[scriptapi] --check FAIL：生成物过期（先重跑 python tools/scriptapi/gen.py）：", ", ".join(bad))
            return 1
        print(f"[scriptapi] --check ok：{len(exports)} exports / {len(pods)} POD types / {len(texts)} files 与声明一致")
        return 0

    for path, text in texts.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
        print(f"[scriptapi] wrote {path.relative_to(REPO)}")
    print(f"[scriptapi] ok：{len(exports)} exports / {len(pods)} POD types")
    return 0


if __name__ == "__main__":
    sys.exit(main())
