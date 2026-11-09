# SSR 预研笔记：三处参考精读（S6 后穿插项前置，2026-11-09）

> 并行研究产物（用户拍板：只读预研 SSR，不建 wt、不写代码，照 2026-11-09 GI 预研先例）。
> 读了什么：UE `ScreenSpaceRayTracing.cpp/.h` + `SSRT/SSRTRayCast.ush`（march 核心全文）+ `SSRTReflections.usf`（入口绑定）+ `HZB.ush`（存储口径）；Carrot 与 bevy **均无 SSR 实现**（见 §2，如实记缺位）。
> UE 源码只读不抄。本文只记「口径 + 坑」，不含任何可直接粘贴的代码。

---

## 1. UE SSR 精读（SSRTRayCast.ush / SSRTReflections.usf / HZB）

### march 结构：不是教科书式变步长 Hi-Z，是「固定步长 + mip 随粗糙度增长」
- 射线全程携带 **UVz 三元组**（屏幕 UV + DeviceZ）， march 在屏幕空间均匀步进（`Step = 1/NumSteps`），每批采样 4 texel（SSRT_SAMPLE_BATCH_SIZE）。**没有经典 Hi-Z 的「逐级升 mip 加大步长」**——mip 增长只与粗糙度挂钩：`Level += (8/NumSteps) * Roughness`，即粗糙面用粗 mip 采（锥追踪风味），光滑面全程细 mip。
- **二分精修被 `#if 0` 弃用**（代码里留了完整实现但没启用），现行精修 = **线段求交**：取跨越命中前后两样本的深度差做线性插值（`TimeLerp = saturate(Diff0/(Diff0-Diff1))`）。光滑面（Roughness<0.2）的二分分支也只活在被弃用的死代码里。
- 命中判定不是严格交叉，是**容差带**：`|Diff + tol| < tol` 才算 hit；`Diff + tol < -tol` 记 **bUncertain**（射线可能已穿到几何背后），`bStopWhenUncertain` 时直接停——这是防止「穿墙取样」的核心，SSR 假阳性大头就在这。
- 远平面样本显式拒绝（`SampleDepth != FarDepthValue`）。
- 步进前先做**屏幕边界 + 世界距离双重裁剪**：朝相机方向的射线在起点表面前 95% 处截断（防自命中），远离相机方向的射线用 WorldTMax 截断。
- 步数与质量档：8/12/16 起步，多射线时 `min(NumSteps*NumRays, 24)` 封顶——UE 的 SSR 预算就是这么小，质量靠时域累积补。

### tile 分类预优化（SSRTTileClassification）
- tile（8×8 级）先算每个方向扇区的「地平线方程」（最近 DeviceZ + 方向斜率），march 循环里用射线方程与地平线求交**算出 UsefulMaxIteration**——射线注定在地平线之下的段直接不追。这是纯算法级剔除，不依赖任何 GPU 特性，我们可直接抄思路。

### 时域与随机化
- jitter 种子直接绑 TAA 采样序号（`TAAIndex * 1551`）；无 TAA 时退化为帧序号 mod 8——**UE 自己也承认无时域时只能靠 8 帧轮换 + 会闪**。印证：SSR 质量上限取决于时域累积，我们第一版（无 TAA/历史）只该做「静态相机 + 无 jitter」的确定性演示版。

### HZB 存储口径（HZB.usf）
- UE HZB 存 **DeviceZ**（非线性屏幕深度），且是**双链**：Furthest（远）+ Closest（近）各一条，fp16 下 Closest 向上取整保保守（`RoundUpF16`）。构建用 groupshared 共享内存做组内 min/max。
- DeviceZ 非线性带来的比较误差靠 **slope-scaled CompareTolerance**（起点/步进 z 差 × 斜率缩放系数，且随 mip 精度变化）兜。

## 2. Carrot / bevy 缺位记录（三处来源只有 UE 一处有 SSR）

- **Carrot**：post-process 只有 merge-lighting / premerge-lighting / taa / tone-mapping，无 SSR；反射走 raytracing 路线（`raytracing.slang`）。可用替代：其 **taa.slang**（历史缓冲 + 混合结构）作我们时域合成的结构参照；raytracing 反射当「正确答案对照图」。
- **bevy**：本机 bevy main 已**移除** `bevy_screen_space_reflections` crate（crates 全列表无 ssr/reflection 项），屏幕空间路线让位给 bevy_solari 光追反射。可用替代：solari 的「光追反射结果」同样可作对照；其时域重投影思路归 S12。
- 结论：SSR 的算法口径只能以 UE 为唯一权威，Carrot/bevy 提供的是「合成链结构」与「结果对照」，不是 march 口径。这与 GI 预研（三处各有本体）情形不同，如实记。

## 3. 与我们 M6.0 金字塔的对接分析（本轮核心产出）

### 径向距离口径对 SSR march **成立**（关键论证）
- 我们金字塔存 `distance(worldPos, eye)`（视点径向距离），UE 存 DeviceZ。担心点：反射射线原点不在眼点，径向距离沿射线不单调，能否做「射线在表面后」比较？
- **能**。比较发生在同一屏幕像素（= 同一视线上），而径向距离沿**视线**是严格单调的——同一像素上「射线点比表面远」⟺ 径向距离更大，与 DeviceZ/线性 Z 的比较语义完全等价。金字塔的 min 聚合（每 cell 最近）在径向度量下语义不破。
- 与 DeviceZ 相比径向距离还**免掉 slope-scaled tolerance**（线性量，非线性误差不存在）——这是我们的口径优势，不是劣势。
- 注意边界：跨 cell 的比较（mip 采样粗化后）与任何深度参数化一样要容差带，径向距离版容差可以用「世界空间距离容差」直接表达，反而更直观。

### 金字塔复用的真实缺口（两处基建待补）
1. **场景颜色没有 mip**：march 命中后按粗糙度粗 mip 采样颜色（UE `ReducePrevSceneColorMip` 专门给这个建上一帧颜色 mip 链）。颜色是平均语义，`GenerateMips` 可用（不像深度 min 必须 hand-written）——基建量小，但要在计划里点名。
2. **法线 + 粗糙度缓冲缺失**：UE SSR 吃 GBuffer 的 Normal/Roughness。我们 S6 状态是 forward 单 target + 深度 prepass，没有每像素法线输出。两条路：a) **S7 之后插**（vis buffer 材质 pass 白送法线/粗糙度，SSR 一步到位）；b) **S6 后先做简化版**——法线从深度重建（ddx/ddy），粗糙度当常数，效果打折但能提前验收 march 本体。
- **插点建议（待拍板）**：ROADMAP 原写「S6 之后插」；按上述依赖分析，**完整版插点实为 S7 之后**（依赖法线/粗糙度 + 离屏 RT 合成，与 post-chain 穿插项基建重叠），S6~S7 之间可插的只有「深度重建法线简化版」。是改 ROADMAP 口径还是保留原话加注，用户定。

### 合成落点
- SSR 是后处理 pass 不是 model shader 分支——M0.8 的 `--shading` 分支机制**不适用**；需要离屏 RT + 合成 pass（scene color + reflection 加权），这份基建与 post-chain 穿插项（tonemap/bloom 的离屏 RT）是同一份，**先插 SSR 会把 post-chain 的一部分基建提前建**，计划里要写清归属，避免重复建设。

## 4. 历史约定核对（不新增偏离）

- UE march 口径（固定步长+mip 按粗糙度）与教材/AMD胶囊实现（变步长 Hi-Z）不同——我们**对齐 UE**（唯一有 SSR 的参考源），变步长版列为升级路径（触发条件：光滑面 march 步数预算吃紧时）。
- 本笔记为零代码研究产物：不建 worktree、不动 kStage、不进渲染回归（照 GI 预研先例）。
- ~~下一步动作待用户拍板~~ → **已拍板（2026-11-09）**：①插点 = **S7 之后**（完整版，深度重建法线简化版不做），ROADMAP 归宿表 SSR 行已按此更新；②开工时的工程口径沿用本笔记 §3（径向距离金字塔直接复用、场景颜色补 GenerateMips、离屏 RT 合成与 post-chain 归属写清）。
