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

## 3. 待拍板清单（M14B0 开工闸，清零才落码）

### Q-B1 ReSTIR 语义落点（本轨最重的架构判定）
- **B1-A（推荐）**：**GI 域 per-pixel 射线候选**（RTXDI ReSTIR-GI 口径）：候选 = 余弦半球方向 + Ray Query 命中 + 命中点着色（直光/emissive + world_cache 间接）；reservoir 重用样本 = 「弹射方向 + 命中点」。真 ReSTIR（时空重用增益在 GI 噪声上），独立 mode 与 S12 干净对比；代价 = 每像素候选射线成本（M=1 起步 + 重用补偿）。
- B1-B：探针域内重采样——保持 probe 链，gather 时对**探针辐照度样本**做 reservoir 重用。便宜但样本池是探针级稀疏的，时空重用语义变形，与 bevy/RTXDI 机制对不上，验收「误差明显下降」难成立。
- B1-C：直光 ReSTIR-DI（bevy 同域，机制 1:1 移植）——最忠实 bevy，但 S14-PLAN 验收写的是 GI（vs S9/S12），直光已有 M9 真值链，属于换轨。

### Q-B2 初始候选构型
- **B2-A（推荐）**：M=1（每像素 1 初始候选），靠时域+空域重用凑池——bevy/RTXDI 起步口径，帧时间最可控；后续 M14B2 评估时才考虑 M=2/4。
- B2-B：M=2~4 起步——首帧质量更好，但帧预算翻倍，与「同等帧时间对比」验收相抵。

### Q-B3 reservoir 存储布局
- **B3-A（推荐）**：两条 `RWStructuredBuffer<Reservoir>` a/b ping-pong（bevy 同构）；`Reservoir = {float3 hitPos; uint packedNormal(八面体); float3 radiance; float3 dir; float confidence; float unbiasedW;}` ≈ 17 项对齐 72B，volatile CB 纪律不涉及（非 cbuffer）。
- B3-B：塞进既有 atlas 纹理——省 buffer，但 RGBA16F 精度不够存 weight/confidence，与 S11 纹理语义混淆，否。

**拍板前冻结面**：`restir.slang`、`ViewMode::Restir`、reservoir buffers、CMake 登记——一行不落；`probe.slang`/`probe_gather.slang`/`filter_temporal.slang` 冻结基线一行不碰；主树/master 一行不碰。
