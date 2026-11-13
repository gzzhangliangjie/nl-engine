# PERF-PLAN — Shader 性能检查（M-PERF0 / M-PERF1）

> 拍板权威落点。2026-11-13 用户拍板「按建议来」（Q-P1~P5 全按推荐项）。主线 master 直接做、不建 track / 不 claim（M-CRASH0 先例：编号 + tag，§1b 口径）。

## 0. 三处实读结论（2026-11-13，本轮模块对应）

| 来源 | 文件 | 形态 | 结论 |
|---|---|---|---|
| Carrot | `engine/render/RenderGraph.h` | 每 pass 一对 timestamp query：pool `MaxPassTimingQueries=256` + `MAX_FRAMES_IN_FLIGHT` 环 + ImGui `drawPerfDebugPanel()` 逐 pass 面板 | M-PERF1 per-pass 计时的直接同构参照（query pool + 帧环 + 面板） |
| bevy | `bevy_render/src/diagnostic/internal.rs` | scope begin/end timestamp（query set + resolve buffer + readback，wgpu-profiler 同款）；`tracy_gpu.rs` 可选 Tracy GPU context 上报 | scope 语义同构；Tracy 上报已有 tracy-m0~3 轨道，本轨不重复 |
| UE | `RHI/Public/GPUProfiler.h` | event stream + `FRHIGPUFrameTimeHistory` per-pass 聚合（Stat GPU 口径） | 工业终态参照，只读思路 |

**slangc 实测（vendored dist，2026-11-13）**：
- `-validate-uniformity` / `-trace-coverage` 全家族支持；`-report-perf-benchmark` 是编译器自身性能（与 shader 运行时性能无关，排除）。
- 10 个代表入口（direct/csReferenceGI、filter_temporal、filter_spatial、probe_trace、probe_gather、cull、material×2、visbuf×2）实跑 `-validate-uniformity` **rc=0 零 uniformity 警告**（hiz/dag 的 E30081 隐式转换警告与 uniformity 无关）。
- 波操作/梯度操作进发散分支的经典违规模式在该 slang 版本下未触发警告——**过滤器按「warning 行含 uniform 关键词 → uniformity 违规」设计**（不依赖具体警告码），干净基线实锤可门禁化。
- 项目内现状：只有整帧 GPU 计时（`renderer/src/gpu_timer.hpp`，4 slot 环 `ITimerQuery`，backend begin/end 包整帧），无逐 pass 细分、无静态指标。

## 1. 拍板记录（Q-P1~P5，2026-11-13 用户「按建议来」）

| 问 | 结论 |
|---|---|
| Q-P1 范围 | **A + B**；C（trace-coverage）挂后续单独拍板 |
| Q-P2 编号归属 | **M-PERF0（A）→ M-PERF1（B）**，主线 master 直接做、不建 track / 不 claim，tag `perf-m0` / `perf-m1` |
| Q-P3（A）门禁强度 | uniformity 警告出现 → **FAIL（exit 1）**（现状零警告基线干净，防退化）；非 uniformity 警告（E30081 隐式转换等）→ informational 只报告不挡 |
| Q-P4（B）pass 清单 | 12 个命名 pass：`csCull`（视锥剔除）/ visbuf 簇光栅 / material 反解 / hiz build / `csDirect` / `csReferenceGI` / `csPlace` / `csTraceProbe` / `csGather` / `csFilterTemporal` / `csFilterSpatial` / `csGatherCompose` / 主绘制（若落码时发现清单与实际调度有出入，按实码修正并回写本表） |
| Q-P5（B）输出形态 | `--pass-timing` CLI 开关（**默认关**）+ HUD 面板逐 pass 行 + log 可断言锚；零基线扰动（开关关 = 渲染路径零变化，冻结基线 bytes 不动） |

**C 挂后续理由**：`-trace-coverage` 需宿主侧分配/绑定 `__slang_coverage` buffer + 读回 + manifest JSON 映射回源码行，集成最重；插桩字节码与冻结基线不同源，必须隔离成独立模式/产物目录。价值场景真实（死代码/未覆盖分支定位）但不阻塞主线，待单独拍板。

## 2. 任务书

### M-PERF0（候选 A：静态 uniformity 门禁）
1. `tools/check-shader-uniformity.py`（纯标准库）：解析 `CMakeLists.txt` 的 `_ENTRIES_*` / 文件映射链 / `_SM66_ARTIFACTS`，逐 artifact × 双 target（dxil `-dxc-path` + SM66 profile；spirv + fvk shift）跑 `slangc -validate-uniformity`；`--jobs` 并行；selftest 子命令。
2. `tools/check-shader-uniformity.bat`：薄包装（调 PATH 的 python），供 showcase/regression 卡以 exe 形态执行（bat 编码门禁 CRLF/GBK 达标）。
3. 交代物：cases.json 第 111 张卡 `perf-m0`（kind:tool：usage / 正向全量 / 负向 --only 未知名，case 级 exe 覆盖 + expectLog 以实测 log 为准）+ `docs/perf/PERF-TOOLS-GUIDE.md` + `example/perf-m0/run.bat verify`。
4. 验收：工具双 target 全量 PASS（零 uniformity 警告）+ selftest 全过 + regression 冒烟 + 全量总闸一次 + tag `perf-m0`。

### M-PERF1（候选 B：per-pass GPU 计时）
1. `gpu_timer.hpp` 扩展：per-pass 多槽（命名 pass 各一对 begin/end，NVRHI `ITimerQuery` 多实例，复用 4-slot 环语义：懒建 / poll 不阻塞 / 无空闲槽本帧跳过）。
2. scene.cpp 12 个 pass 边界插桩（开关关闭时零开销路径）；viewer `--pass-timing` 默认关；HUD 面板逐 pass 行（开关开时）；log 汇总行可断言锚。
3. 交代物：`example/perf-m1/` 版本卡 + run.bat verify + cases.json 卡 + TOOLS-GUIDE 补章。
4. 验收：`--pass-timing` 双后端 deviceErrors=0 + log 锚 + 各 pass ms 合理；开关关 = `--only` 保形抽查 bytes 全绿；全量总闸一次 + tag `perf-m1`。

## 3. 变更记录

- 2026-11-13：立项 + Q-P1~P5 拍板落档（用户「按建议来」）。
