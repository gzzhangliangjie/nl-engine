# S14B ReSTIR GI 计划（M14B0~M14B2）

> 轨道：`feat/s14-restir`，wt `E:/UE_STUDY_2026/nl-engine-s14-restir`；总纲 `docs/s14/M14-PLAN.md`（2026-11-13 拍板：A+B 并行 / C 接 B / D 砍 / kStage 冻结 S8.2）。
> 本文件 = M14B0 产出。§1 三处实读结论；§2 里程碑；§3 待拍板清单（**清零后才允许落码**，AGENTS.md 开工闸）。

---

## 1. 三处实读结论（2026-11-13，M14B0）

### bevy_solari `restir.wesl`（365 行全文精读）+ `no_restir.wesl`（对照）——**主参考**
- **关键定性：bevy 的 `restir.wesl` 是 ReSTIR-DI（直光重采样）**，不是 GI——reservoir 选的是 `light_sample.light_id`（光源三角形），GI 在 bevy 走探针/world cache 路径。**把 ReSTIR 移植到 GI 域是本项目对标 RTXDI「ReSTIR GI」的自设计扩展**（S14-PLAN 原文「final gather 的探针采样换成 ReSTIR 式重采样」），bevy 提供的是重采样**机制**，不是 GI 语义本身——此定性已写进 Q-B1 拍板项。
- **机制全件**（可直接移植）：
  - 两 pass 结构：`initial_and_temporal`（初始候选 + 时域重用，写 `reservoirs_b`）→ `spatial_and_shade`（空间重用 + 着色，写 `reservoirs_a`），**a/b ping-pong**，8×8 workgroup。
  - 时域：motion vector 重投影 → 越界回退本像素旧 reservoir → **pixel permute**（同位置错位采样防自相关）→ `pixel_dissimilar` 几何拒绝 → 光源 id 跨帧翻译（GI 域无需）→ **confidence cap**（`min(confidence, cap)`）。
  - 空域：5 邻居 disk 采样（半径 30px）+ 逐邻居几何拒绝 + **Jacobian 重投影权重**（`phi_r·ql²/phi_q·rl²`，NaN/Inf → 0）+ **Jacobian 野值拒绝**（<0.125 或 >8.0 → 权重清零）。
  - 合并：target function = luminance(BRDF×radiance)，**defensive balance heuristic**（空域合并时 `t_c = canonical_conf/total_conf`），`unbiased_contribution_weight = weight_sum / target`。
- `no_restir.wesl` = 30 行单 pass 直着色，天然对照组结构（本项目对应 S12 filter 版）。

### UE `LumenScreenProbeGather.cpp`（定向读）
- UE 无 per-pixel ReSTIR GI（探针域走 uniform + adaptive 加密，`FScreenProbeAdaptivePlacementMarkCS/SpawnCS`，adaptive 属 C 轨素材：`NumAdaptiveProbes` cvar + `AdaptiveProbeAllocationFraction=0.5` 预算切分口径）。
- **UE 对 ReSTIR 的口径在 RTXDI 集成（答案册）**：Reservoir 结构 = (position, normal, radiance, direction, ...) + 无偏权重，与 bevy 机制一致；本轨以 bevy 为机制权威、RTXDI 为 GI 语义参照（本地无 RTXDI 源码，凭公开算法口径，落码前如需细节再补 web 实读）。

### Carrot（据实标注）
无 ReSTIR / reservoir 等价；时域结构参照已在 S12 用过（history ping-pong + adaptive alpha + 拒绝层），本轨复用其**工程挂点纪律**（T5 保形：新 pass 插在既有链之间不动绑定）。

### 项目现状（wt `feat/s14-restir` 代码实读）
- 探针链：`probe.slang`（place，4 级 atlas ping-pong）→ `probe_trace.slang`（探针射线 → 辐照度 atlas RGBA16F）→ `probe_gather.slang`（csGather 全屏 gather + csGatherCompose RMW 写 `hdrSceneColor_`，`g_gatherOut` u0）。
- S12 基建（B 轨直接复用）：`velocity.slang`（velocity.xy = curUV-prevUV 唯一消费契约）、`history0_/1_` RGBA32F ping-pong（a=累积帧数）、`filter_temporal.slang` 挂点纪律（插 csGather 与 compose 之间，非本 mode 零改动）。
- Ray Query：`rt.slang`（TLAS from S8 initDag 资产链）+ `world_cache.slang`（S11 flat hashmap，命中点间接光 query 口径已有）。
- Mode：`ViewMode` Filter=20 后追加 `Restir`。

## 2. 里程碑

| # | 内容 | 验收 | tag |
|---|---|---|---|
| **M14B0** | 本计划落盘 + 待拍板清零 | §3 全拍板写回 | `s14b-plan` |
| **M14B1** | 最小 ReSTIR GI 链：`restir.slang` 两 pass（initial_candidates+temporal → spatial+shade）+ `--mode restir`；候选 = per-pixel 余弦半球射线（Ray Query 打 TLAS，命中点直光+emissive+world_cache 间接 ≈ `probe_trace` 着色逻辑逐像素化）；reservoir ping-pong 两条 `RWStructuredBuffer`；复用 velocity/几何拒绝 | 双后端出图；同机位与 `--mode probe`（S12 滤波版）并排出图；example 卡 + cases.json | `s14b-restir` |
| **M14B2** | 置信度加权 + 评估：confidence cap / defensive MIS 打开；**MSE 数值对比**（同机位、同帧时间预算下 vs S12 滤波版，裁判 = `--mode reference-gi` 参考图，数字写进 README）；帧时间记账（`--pass-timing`）+ 收官交代 | MSE 明显下降（同等帧时间）；渲染版交代全过；MILESTONES 四栏 | `s14-restir` |

**C 轨接力预告**（M14C1，B 收官后）：探针自适应加密走 UE `Mark→Spawn` 口径（§1 UE 实读），预算切分对齐 `AdaptiveProbeAllocationFraction`；B 的 confidence 权重直接复用。细节届时补 `S14C-ADAPTIVE-NOTES.md`，不提前拍死。

## 3. 拍板记录（2026-11-13 用户「全部按推荐」拍板，开工闸已清零）

### Q-B1 ReSTIR 语义落点 → **B1-A（拍板）**
GI 域 per-pixel 射线候选（RTXDI ReSTIR-GI 口径）：候选 = 余弦半球方向 + Ray Query 命中 + 命中点着色（直光/emissive + world_cache 间接）；reservoir 重用样本 = 「弹射方向 + 命中点」。真 ReSTIR（时空重用增益在 GI 噪声上），独立 `--mode restir` 与 S12 干净对比；M=1 起步 + 重用补偿控帧时间。bevy restir.wesl = DI 定性成立，只移植机制不移植语义。

### Q-B2 初始候选构型 → **B2-A（拍板）**
M=1（每像素 1 初始候选），靠时域+空域重用凑池（bevy/RTXDI 起步口径）；M14B2 评估时才考虑 M=2/4。

### Q-B3 reservoir 存储布局 → **B3-A（拍板）**
两条 `RWStructuredBuffer<Reservoir>` a/b ping-pong（bevy 同构）；`Reservoir = {float3 hitPos; uint packedNormal(八面体); float3 radiance; float3 dir; float confidence; float unbiasedW;}` ≈ 72B 对齐；非 cbuffer，volatile CB 纪律不涉及。

**冻结面解除**：M14B1 允许落码 `restir.slang`、`ViewMode::Restir` 追加、reservoir buffers、renderer 接线、CMake 登记；`probe.slang`/`probe_gather.slang`/`filter_temporal.slang` 冻结基线一行不碰；主树/master 一行不碰。


## 4. M14B2 验收记录（2026-11-13 收官，tag `s14-restir`）

### 4.1 裁判链重定义（用户拍板 option A）
M14B0 计划预设 `--mode reference-gi` 作 MSE 裁判，实测它是 Q14-1 口径的 **104-tri 粗代理**（不同几何 + 不同 albedo + 探针级联语义），restir vs reference-gi MSE 0.1187 无意义。经用户拍板改为**自收敛累积裁判**：

- restir 长跑 2048 帧 / filter 长跑各当自收敛真值；
- 短跑比「逼近自收敛态程度」+ 帧成本记账；
- reference-gi 判据缺陷如实入档（本节）。

### 4.2 自收敛评估数字（DX12，960x540，yaw 12 / pitch 9 / dist 7）

| 对比 | 结果 | 定性 |
|---|---|---|
| restir f128 vs f2048 真值 | MSE = 0.0194 | 已在噪声底（8-bit tonemap 域） |
| restir f256 vs f2048 | MSE = 0.0061 | 同上 |
| restir f512 vs f2048 | MSE = 0.0143 | 同上（reservoir 链微抖，非单调收敛） |
| filter f4/f16/f64/f128 vs f1024 | MSE = 0（MD5 逐位相同） | filter 输出确定性、无收敛维度，帧数裁判不适用 |
| restir vs filter 互差 | MSE ≈ 116 | GI 语义域不同（per-pixel 单弹射 vs 探针级联插值），非质量裁判 |

**M14B0 原验收「MSE 明显下降（同等帧时间）」在 option A 口径下的结论**：restir 128 帧短跑已达自收敛噪声底（≤0.02 MSE），但 filter 无收敛维度、两者语义域互差不构成质量排序 → 原判据在本场景不可判，按 option A 改记「短跑逼近自收敛态 + 帧成本」。两轨真实差异 = 帧成本 3.0x 换 per-pixel 单弹射语义。

### 4.3 MIS 开/关自比（defensive MIS，misLambda cap = 0.5）

| 对比 | MSE |
|---|---|
| MIS-off f128 vs MIS-off f2048 | 0.0177 |
| MIS-on f2048 vs MIS-off f2048（收敛态互差） | 0.0107 |
| MIS-on f128 vs MIS-off f128（短跑互差） | 0.0272 |

结论：defensive MIS 在收敛态贡献 ≤0.011 MSE（噪声底量级），短跑差异 0.027 略高于噪声底 —— 与置信度加权方向一致（低置信期无偏化），本场景增益可见但有限。

### 4.4 帧成本记账（--pass-timing，128 帧，首解锚 + 稳态 GPU 计时）

| 通道 | ms |
|---|---|
| restir 稳态 GPU 全帧（NVRHI timer avg，unthrottled） | 0.656 |
| filter 稳态 GPU 全帧 | 0.219（3.0x 差距） |
| restirTemp / restirSpatial（首解） | 0.121 / 0.837 |
| filter GI 链六 pass 合计（首解） | ≈ 0.097 |
| MIS-off 稳态 GPU 全帧 | 0.614（defensive MIS 增量 ≈ 0.04 ms） |

### 4.5 非确定性根因（重要踩坑 + 待拍板）

restir 模式**同 exe 新进程双跑 MD5 不同**（825~1491 像素 / 0.16% / max 23~55）。二分定位：

- 诊断禁用 restir 模式下的 `drawCacheFrame`（cache 衰减/填表/更新/混合四 pass）→ 双跑**逐字节一致**；
- world_cache 链各 pass 的 rng 种子（cell_index + frame_index）均确定性；唯一竞态面 = S11 hashmap 插槽 `InterlockedCompareExchange`（world_cache.slang:153）的 warp 调度序 → 存活 cell 集合 run-to-run 漂移 → restir 命中查缓存（cache bounce direct + fallback，Q-B1 语义）吃到漂移的 radiance；
- 该链在 S11 自己的模式（probe/filter 卡）下 bytes 级稳定（历史回归全绿），只在 restir 模式暴露 —— restir 帧路径少了 probe trace 的表内容约束。

**处置**：验收门走统计档（B1「随机采样路径预期」先例）：re-shot diff-pixel ≤ 1.0%（实测 dx12 0.127% / vulkan 0.260%）+ meanDiff ≤ 0.5/255（实测 0.002/0.004），`tools/check-s14b-restir.py stability/present/selftest` 纯标准库。根因修复挂**待拍板**（触碰 S11 冻结基线或 Q-B1 world_cache 间接语义）。

### 4.6 渲染版交代
`example/s14b-restir/`：双后端基线 PNG + json 侧车（mode=restir）+ run.bat verify（shot/锚/统计门/present 门/负向五段）+ cases.json 第 118 张卡（tool `restir-gpu` + manual `restir-verify`）；regression `--only s14b-restir` 双后端 2/2 ALL GREEN。kStage 冻结 S8.2 不 bump。
