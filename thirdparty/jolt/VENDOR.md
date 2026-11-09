# Vendored third-party: Jolt Physics 5.3.1

## Identity

| item | value |
|---|---|
| upstream | jrouwe/JoltPhysics (https://github.com/jrouwe/JoltPhysics) |
| version | **5.3.1** (`project(JoltPhysics VERSION 5.3.1)` in `Build/CMakeLists.txt`; `JPH_VERSION_MAJOR 5 / MINOR 3 / PATCH 1` in `Jolt/Core/Core.h`) |
| license | MIT (`LICENSE`, upstream SPDX `MIT`) — compatible with nl-engine MIT |
| vendored on | 2026-11-09, P8 M-P0 |
| local source | `E:/UE_STUDY_2026/Carrot/thirdparty/JoltPhysics` (Carrot's unchanged upstream submodule checkout — **third-party library bytes, not Carrot engine code**) |
| obtained | local copy, **zero network** (GitHub direct access is blocked on this machine) |

## What is vendored

- `Jolt/` — complete upstream library sources (442 files; 133 `.cpp` + headers/inl/natvis), no edits.
- `Build/CMakeLists.txt` — upstream CMake entry, included via `add_subdirectory(thirdparty/jolt/Build EXCLUDE_FROM_ALL)`.
- `LICENSE`, `README.upstream.md` — upstream license and readme, no edits.
- `VENDOR.md` — this file (nl-engine-owned provenance record).

Intentionally **not** vendored: `Samples/`, `UnitTests/`, `TestFramework/`, `PerformanceTest/`, `HelloWorld/`, `JoltViewer/`, `Assets/`, `Docs/`, `Doxyfile`, `.github/`, `.git`. Those are upstream demo/test apps, not library code; `HelloWorld/HelloWorld.cpp` was **read only** as the rhythm reference for the minimal world (copy-ideas-not-code rule).

## Build integration (master `CMakeLists.txt`)

Jolt is pure CPU: no vendor graphics SDK, no `find_package(Vulkan)`, no network — the architecture rule in the root CMake comment is preserved. Options forced before `add_subdirectory`:

- `USE_STATIC_MSVC_RUNTIME_LIBRARY=OFF` — keep the same CRT (`/MD`) as `nl_core` / `viewer`, avoid LNK4098 /MT-vs-/MD mismatches. (Carrot does the same.)
- `CPP_RTTI_ENABLED=ON`, `CPP_EXCEPTIONS_ENABLED=ON` — match MSVC defaults used by the rest of the project.
- `INTERPROCEDURAL_OPTIMIZATION=OFF`, `ENABLE_INSTALL=OFF`, `BUILD_SHARED_LIBS=OFF`, `USE_ASSERTS=OFF`, `FLOATING_POINT_EXCEPTIONS_ENABLED=OFF`, `ENABLE_ALL_WARNINGS=OFF` — keep the sub-build small, quiet, and link-compatible.

The upstream `Build/CMakeLists.txt` only enables samples/tests when it is the **root** project; as a subdirectory it builds the single `Jolt` static target. Compiler flags it sets are scoped to that subdirectory.

## Determinism contract

`nl/physics/World.cpp` uses `JPH::JobSystemSingleThreaded` (one job system, zero worker threads) and a fixed `1/60` step with `collisionSteps = 1`. Thread-count is therefore part of the frozen determinism contract: two runs of the same `DropConfig` must match sample-by-sample bit-for-bit. Switching to `JobSystemThreadPool` later is a separate M-P milestone with a re-baseline.

## Upgrade discipline

Version is **locked at 5.3.1**. Upgrading is a standalone M-P milestone: re-vendor, re-run `phys-tools selftest`, regenerate the drop TSV + evidence PNG, and re-verify the showcase card. Never silently bump.
