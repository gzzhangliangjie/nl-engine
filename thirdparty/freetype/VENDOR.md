# Vendored third-party: FreeType 2.13.3

## Identity

| item | value |
|---|---|
| upstream | freetype/freetype (https://github.com/freetype/freetype，官方镜像) |
| version | **2.13.3**（tag `VER-2-13-3`） |
| license | FreeType License (FTL) — `LICENSE.TXT` 原文随库（GPL-2.0 双许可择 FTL 消费，与本仓 MIT 兼容；FTL 无显式专利授权条款的事实如实记录） |
| vendored on | 2026-11-15，M-UI0.0（feat/ui-rmlui） |
| obtained | gh-proxy 镜像直链 tarball（curl `https://gh-proxy.com/https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz`） |

## Why vendored

RmlUi 6.3 默认字体引擎（`FontEngineDefault`）**硬依赖** FreeType——无替代的轻量路径，
自写字体引擎属过度设计（选型闸：关联系统优先 vendored 成熟库）。

## What is vendored

- `include/` — 完整公共头树。**含 `include/freetype/config/ftmodule.h`**（repo 版直接
  携带，无需自写模块表——与本仓 CMake 编译集逐项对应，见下）。
- `src/` — 全部模块源码。**剔除** `src/tools/`（构建期辅助工具）与 `src/dlg/`
  （可选日志后端，`FT_CONFIG_OPTION_USE_DLG` 缺省关）。
- `LICENSE.TXT` — 上游原文，零改动。
- `VENDOR.md` — 本文件。

## Build integration（master `CMakeLists.txt`）

`nl_freetype STATIC`，编译集 = `ftmodule.h` 登记模块的单文件聚合入口（上游 CMake
baselib 清单口径）：

- base：`ftbase/ftbbox/ftbdf/ftbitmap/ftcid/ftfstype/ftgasp/ftglyph/ftgxval/ftinit/
  ftmm/ftotval/ftpatent/ftpfr/ftstroke/ftsynth/ftsystem/fttype1/ftwinfnt/ftcache`
- drivers：`truetype/type1/cff/cid/pfr/type42/winfnt/pcf/bdf`
- support：`autofit/psaux/psnames/pshinter/sfnt/gzip/ftlzw`
- renderers：`smooth/raster/sdf/svg`

编译定义：`FT2_BUILD_LIBRARY`（standalone build 惯例）；`ftsystem` 用 stdlib 版
（`src/base/ftsystem.c`），零平台特化。**不编译 `bzip2`**（外部 bzlib 依赖）。

## Upgrade discipline

版本**锁在 2.13.3**。升级是独立里程碑：重裁剪、核对 `ftmodule.h` 模块表、
`--rmlui-demo` 证据重拍（字形栅格化结果会随版本漂移，截图 bytes 预期变化需登记）。
不许静默 bump。
