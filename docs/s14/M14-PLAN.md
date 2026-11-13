# M14 开发计划：S14 增强轨（A/B 并行 + C 串行，D 砍）

> 2026-11-13 用户拍板（三轮收敛：S14 四选一 → 并行性评估 → 「写吧」）：
> **A+B 两个 wt 并行、C 接力 B 串行、D 砍掉、kStage 冻结 S8.2 不 bump**。
> 本文件是 M14 轨道总纲；各子轨开工时（M14A0 / M14B0）按铁律补三处实读后写细化计划 `S14A-SOFTRAST-PLAN.md` / `S14B-RESTIR-PLAN.md`。
> 前置结论沿用：S14 = 可选增强四选一（`S14-PLAN.md` 原四分支）；2026-11-07 定案「软件光栅是算法教室 + S14 备胎」；2026-11-11 Q-WE2 worldgen 未占用 S14 第五分支名额。

---

## 0. 总体结构（拍板结论）

| 子轨 | 内容 | wt / 分支 | 节奏 | 收官 tag |
|---|---|---|---|---|
| **A 软光栅亚像素三角形** | 像素级小三角形走 compute 光栅（binning → 逐瓦片排序 → 逐像素原子写 vis buffer），`--mode soft-raster` | `E:/UE_STUDY_2026/nl-engine-s14-softrast` / `feat/s14-softrast` | 并行开 | `s14-softrast` |
| **B ReSTIR GI** | final gather 探针采样换 ReSTIR 重采样，`--mode restir` | `E:/UE_STUDY_2026/nl-engine-s14-restir` / `feat/s14-restir` | 并行开 | `s14-restir` |
| **C 探针自适应分辨率** | 探针按方差/置信度自适应加密，`--mode probe-adaptive` | **接力 B 的 wt**（同域文件，不另开） | B 收官后串行 | `s14-adaptive` |
| **D 简单场景编辑器** | — | 不开 | **砍**：已被 feat/editor 轨覆盖（M-E0~E3 merge `ac50c10`+`fc34176`：Inspector + 场景序列化存读盘），S14-PLAN 原文自述「就是 P8-E3 的提前量」，重复建设 |

**并行性依据**：A 动光栅路径（meshlet 光栅 / vis buffer / 分箱），B 动 GI 探针采样（final gather），模块不相交；C 与 B 同域（`probe.slang` 布点 / final gather 同一批文件）必撞，必须串行，且 C 的置信度权重可直接复用 B 的 ReSTIR 置信度。
**原计划口径修订**：`S14-PLAN.md`「别同时开两条分支」是主线单线时代口径；现在有 wt 隔离 + cases.json 语义合并（`merge-cases.py`），A/B 并行成立（2026-11-13 拍板覆盖）。

## 1. 纪律口径

- **B 类渲染版交代**：每步 example 卡 + cases.json（语义合并格式追加）+ 双后端回归；不进 A 类工具版通道。
- **kStage 冻结 `S8.2` 不 bump**（拍板）：新 `--mode` 不动 `renderer.cpp kStage`；`server.py --check` 只证旧阶段无回归，新卡验收 = `run.bat verify` + `regression.py --only <版本>` 双机械校验（M9.0 口径）。
- **S12 底线终身有效**：每分支单独可关（保留 S13 完成态作为默认模式）；做到一半动摇主线随时止损回退 tag。
- **开工闸**：M14A0 / M14B0 各自完成三处实读 + 待拍板清零并写回细化计划后，才允许落第一行代码。三处实读对照：
  - A 轨：bevy `visibility_buffer_software_raster.wesl`（软光栅 vis buffer）+ Scthe 软光栅系列博文（算法口径）+ UE Nanite 光栅路径（`Renderer/Private/Nanite*` 只读定向）+ zauonlok 光栅化器（2026-11-07 备胎结论指定素材）。
  - B 轨：bevy_solari `restir.wesl` vs `no_restir.wesl`（两条路线天然对照）+ UE `Lumen` final gather 对应模块（只读）+ NVIDIA RTXDI（答案册参考实现）。
- **占坑纪律**：A/B 两 wt 分别 claim `M14A0` / `M14B0`；收尾对称 release。
- **merge**：按各自收官先后 merge 回 master，不预设顺序；cases.json 语义合并兜底；A/B 交集只在 viewer mode 注册 + shader CMake `_ENTRIES_*` 登记处，冲突面小。收官 tag 后建 `milestone/<tag>` 分支。

## 2. A 软光栅轨里程碑（feat/s14-softrast）

| # | 做什么 | 验收 | tag |
|---|---|---|---|
| M14A0 | 三处实读 + 细化计划 `S14A-SOFTRAST-PLAN.md`；binning 方案定骨架（瓦片 8×8 vs 16×16 只留 profile 后定，不预拍死） | 计划落盘、待拍板清零 | `s14a-plan` |
| M14A1 | compute 光栅骨架：binning → 逐瓦片排序 → 逐像素原子写 vis buffer；`--mode soft-raster` 单 mesh | 双后端出图，mode 进 cases.json | `s14a-raster` |
| M14A2 | 小三角形正确性：与全硬件版逐像素数值对比（误差量化写进文档）；远景闪烁对比证据 | 数值差异可接受 + 证据图齐 | `s14a-correct` |
| M14A3 | profile 定瓦片尺寸 + 帧时间对比 + 收官交代 | 渲染版交代全过 | `s14-softrast` |

**坑（原计划已留）**：WebGPU 系教程的原子限制方案别照搬（本机 DX12/Vulkan 有真 64 位原子，方案可更直接）；分箱尺寸先 profile 再定。

## 3. B ReSTIR GI 轨里程碑（feat/s14-restir）

| # | 做什么 | 验收 | tag |
|---|---|---|---|
| M14B0 | 三处实读 + 细化计划 `S14B-RESTIR-PLAN.md`（reservoir 数据形态、重用半径、MSE 对比口径先定） | 计划落盘、待拍板清零 | `s14b-plan` |
| M14B1 | 最小实现：初始候选生成 + 时空重用（temporal + spatial），`--mode restir` | 双后端出图，mode 进 cases.json | `s14b-restir` |
| M14B2 | 置信度加权 + 同机位 MSE 数值对比（同等帧时间下 vs S12 滤波版误差明显下降，**数字写进文档**）+ 收官交代 | 渲染版交代全过 | `s14-restir` |

## 4. C 自适应探针轨（接力 B 的 wt）

| # | 做什么 | 验收 | tag |
|---|---|---|---|
| M14C1 | 探针按方差/置信度自适应加密（高差异区域多打射线/加密布点，预算恒定），`--mode probe-adaptive` + viz 自适应布点显示 | 同射线预算细节区 GI 质量提升（参考图对比）+ 总帧时间不升 | `s14-adaptive` |

三处参照：UE `LumenScreenProbeGather*` adaptive 部分源码（只读）；M10 收口备注「自适应加密列 S14 可选」即此分支的既有挂账。

## 5. 实施记录

- 2026-11-13：计划落盘（本文件）；wt `nl-engine-s14-softrast` / `nl-engine-s14-restir` 建于 master `1892866`（junction `.workbuddy` + `thirdparty/slang/dist` + skip-worktree 9 个 tracked `.workbuddy` 文件）；TRACKS 注册 `s14-softrast` / `s14-restir` active；claim `M14A0` / `M14B0`。
