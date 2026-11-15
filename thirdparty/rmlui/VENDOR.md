# Vendored third-party: RmlUi 6.3

## Identity

| item | value |
|---|---|
| upstream | mikke89/RmlUi (https://github.com/mikke89/RmlUi)，libRocket 后继 |
| version | **6.3**（2026-08-22 发布；tarball `archive/refs/tags/6.3.tar.gz`） |
| license | MIT（`LICENSE.txt` 原文随库）— 与本仓 MIT 兼容 |
| vendored on | 2026-11-15，M-UI0.0（feat/ui-rmlui） |
| obtained | gh-proxy 镜像直链 tarball（本机 GitHub 直连被墙，curl `https://gh-proxy.com/https://github.com/mikke89/RmlUi/archive/refs/tags/6.3.tar.gz`） |

## What is vendored

- `Include/` — 公共头子集：`RmlUi.h`、`Config.h`、`RmlUi/Config/`、`RmlUi/Core/`、
  `RmlUi/Debugger/`（含 `Debugger.h`）。**不收** `RmlUi/Lua|SVG|Lottie` 及对应
  `Lua.h`/`SVG` 头（可选模块未选）。
- `Source/Core/`、`Source/Debugger/` — 上游客户端库与调试器源码（231 + 26 项；剔除
  各自的上游 CMakeLists.txt，本仓自建 `nl_rmlui` target）。**目录布局保持上游原样**
  （`Source/Core`、`Source/Debugger`、`Include` 平级）：上游源码用
  `../../Include/RmlUi/...` 相对路径 include，扁平化编译直接失败（M-UI0.0 实测教训）。
  上游版本串经编译定义注入：`RMLUI_VERSION="6.3"`（上游 `Core.cpp:79` 有 `"custom"`
  兜底，本仓显式传入保溯源真实）。
- `LICENSE.txt` — 上游原文，零改动。
- `VENDOR.md` — 本文件（nl-engine 溯源记录）。

Intentionally **not** vendored：`Backends/`（官方后端参考件——渲染接口签名以
`Include/RmlUi/Core/RenderInterface.h` 为权威，「抄思路不抄代码」口径）、`Samples/`、
`Tests/`、`CMake/`、`Dependencies/`（上游自带 freetype 下载逻辑，本仓自 vendored
freetype）、`Utilities/`、Lua/SVG/Lottie 可选模块、上游 `CMakeLists.txt`。

## Build integration（master `CMakeLists.txt`）

- 纯 CPU + 可选 FreeType：零厂商 SDK、零联网（架构纪律同 Jolt 条目）。
- 编译定义：`RMLUI_STATIC_LIB`（静态链）、`RMLUI_VERSION="6.3"`。
- include 路径：`thirdparty/rmlui/Include` + `thirdparty/freetype/include`。
- Debugger 一并入 `nl_rmlui`（`Source/Debugger` 6 文件级开销，换 F8 调试器能力）。

## 字体依赖

RmlUi core 的默认字体引擎（`RmlUi/Core/FontEngineDefault`）硬依赖 FreeType——本仓
vendored `thirdparty/freetype/`（见该目录 VENDOR.md）。运行时字体文件在
`assets/ui/fonts/`（Noto Sans Regular，SIL OFL 1.1，许可同目录）。

## Upgrade discipline

版本**锁在 6.3**。升级是独立里程碑：重新裁剪、重看 RenderInterface 破坏性变更
（v6 起渲染接口已大改，v7 需逐方法核对）、重拍 `--rmlui-demo` 证据卡、重跑 script
Ui 域 selftest。不许静默 bump。
