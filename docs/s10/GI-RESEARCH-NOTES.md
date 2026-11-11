# GI 预研笔记：三处参考精读（S10~S12 前置，2026-11-09）

> 并行研究产物（用户拍板：主线推进 S6 前，先做零代码冲突的研究类并行）。
> 读了什么：UE `LumenScreenProbeGather.usf`（定向）+ bevy_solari `prepare.rs` / `initial_path.wesl` / `world_cache_*.wesl`（全文）+ Carrot `modules/gi.slang` / `update-cells.slang` / `read-screen-probes.slang`（全文）。
> UE 源码只读不抄；Carrot/bevy 抄思路不抄代码。本文只记「口径 + 坑」，不含任何可直接粘贴的代码。

---

## 1. S10 屏幕探针（UE LumenScreenProbeGather 对照）

### 探针布点（M10.0）
- UE 是「均匀网格 + 自适应加密」两层：均匀探针下采样深度求出；自适应探针在**均匀 4 探针插值权重和不足**（低于 MIN_PROBE_INTERPOLATION_WEIGHT）的像素上按 tile 掩码补种（Mark→Spawn 两个 CS，组内 InterlockedOr 聚掩码 + InterlockedAdd 分配槽位）。我们的 M10.0 只做均匀 4 级 cascade，**自适应加密列为可选升级**（对应 S14「探针置信度与自适应分辨率」）——升级路径成立，触发条件 = 硬阴影/几何边缘处 gather 灰斑明显。
- 插值权重公式（UE `CalculateUniformUpsampleInterpolationWeights`）：**双线性 corner 权重 × 平面深度权重**。平面权重 = `exp2(k * (planeDist/sceneDepth)²)`，planeDist 是探针世界位置到像素「法线+位置」定义的平面之距离；另有指数更弱的 FallbackWeights，当主权重和不足时降级使用。**不是简单距离权重**——法线平面距离才是消除「背景漏光」的关键，我们的 M10.2 final gather 权重应按此实现（距离+法线一致性的具体化）。
- 像素 jitter：探针格子对全分辨率像素有 BlueNoise jitter，且 jitter 位置先过一遍平面深度一致性检验（DepthWeight > 0.01 才采纳），避免 jitter 跨几何。

### 八面体图（M10.1）
- UE 探针八面体图带 **1 texel border ring**（`ScreenProbeGatherOctahedronResolutionWithBorder`），边界 texel 经 `OctahedralMapWrapBorder` 回绕到对侧——seam 采样无缝。这验证了我们 PLAN 里「边界 texel 卷绕要做对」的坑点，且给出了具体做法：**边框在生成 mip 前就回绕填充**（filtering.usf 里 mip 链也基于 WithBorder 尺寸逐级生成）。S10-PLAN 补强：八面体图按「core + 1texel border」布局，mip 生成在带边框的图上做。

### 探针追踪与辐照度来源（M10.1）
- bevy 的对应物（initial_path.wesl）是 ReSTIR PT 风格的完整路径追踪器，比我们的「每探针每 texel 1 条方向射线（8×8=64 条，2026-11-11 Q12-1 拍板，UE TracingOctahedronResolution=8 口径）」激进得多；我们 S10 只做一跳，口径以 UE 的 Screen Probe 单探针少射线 + bevy 的世界缓存兜底为参照。bevy 给了两个可抄的思路：
  1. **cache 终止门**（`terminate_into_cache`）：只有 BRDF 采样立体角足够宽（lobe_solid_angle ≥ π）才允许路径提前终止进缓存——粗糙面才信缓存，光滑面继续追；且射线必须**穿出当前 cell 的对角**（t > √3·cellSize）否则不信（防 cell 跨遮挡漏光）。这两个门我们 S11 二跳时直接可用。
  2. **max_gi_ray_distance 截断的漏光警告**：bevy 注释明确说射线截断处 miss ≠ 真 miss（可能远处有面），截断太小会漏天光。我们 S10 探针射线设 tMax 时要留这个余量。

## 2. S11 世界缓存（bevy_solari + Carrot HashGrid 对照）

两处实现结构差异大，都是「hash 定位 + 原子累加 + 帧衰减回收」，但细节口径互补：

| 维度 | Carrot（桶式） | bevy（扁平 CAS） | 我们 S11 取舍 |
|---|---|---|---|
| 结构 | 1024·256 桶 × 16 cell/桶（4M cell） | 2^20 项扁平数组（84B/项 ≈ 84MB 单块 storage） | 扁平 + 线性探测起步（S11-PLAN 原案），桶式为扩容备选 |
| key/校验 | pcg 定桶 + xxhash32 作 tag（双独立哈希） | pcg 定 key + iqint 作 checksum（0 保留为空） | 同构：**key 与 checksum 必须用两个独立哈希**，防同 key 假命中 |
| 抢占 | 桶内扫 16 槽，`InterlockedCompareExchange(0→hash2)` 抢空槽；桶满返回 none | `atomicCompareExchangeWeak(checksum)`：空则初始化、同则命中、异则 key+1（最多 3 步） | bevy 的 3 步探测上限简单够用；Carrot 桶满是真实边界（要处理「查无返回 0」） |
| 辐照度累加 | int3 量化（1/16384）`InterlockedAdd` 进独立 accumulation buffer，帧末 mergeRadiance 一次性 lerp（max 64 samples） | float 经 blend pass 逐 cell mix（无原子） | 量化原子累加是 GPU hashmap 标配坑：**int 量化 + 双缓冲累加 + 帧末合并**，别在 radiance 上直接原子加 float |
| 时间混合 | `lerp(old, new, 1/totalSampleCount)`，cap 64 | 亮度 delta 自适应：`alpha=|ΔL|/L`，`max_samples = mix(cap, 1, alpha^(1/8))`——变化快的 cell 混合系数大（出处 gboisse bsky） | bevy 的自适应混合更先进且便宜，S11 采用（对应 S11-PLAN「cell 生命周期」） |
| 回收 | `lastTouchedFrames + 20 < frame` → clearCell（decayCells 全表扫）；touch 用 `InterlockedExchange` 返回首触 | 每帧全表 life-1，0 则清 checksum；查询命中时重置 life（原子 Max/Store 可选） | 两处一致：**回收 = life 衰减到 0 清槽，查询即续命**。无墓碑（墓碑是 S11-PLAN 原表述，实际两家都不用墓碑，直接清零重用） |
| cell 尺寸 LOD | 公式写了但**被强制关掉**（cellSizeStep=0，固定 0.15m）——Carrot 实际没用 LOD | `base·2^lod`，lod 按 `log2(1+dist/lod_scale)` 且 fract³ 概率进位（软化 LOD 跳变） | Carrot 关掉 LOD 是个「实况教训」：LOD 尺寸混合容易闪烁。我们第一版固定 cell 尺寸，LOD 列 S14 升级 |
| 防漏光 | key 混入 `rayLength < cellSize` 标记（短射线单独 hash）；查询点切平面 jitter | 终止门要求 t > √3·cellSize；查询点切平面 jitter（引 tomclabault ReGIR 博客） | **查询点 jitter 两家都做**（模糊格子感）；短射线防漏光 Carrot 用 hash 分流，bevy 用终止门——我们用 bevy 的门（更简单） |
| 预算控制 | 无（桶满丢弃） | 软目标：每 cell 以 `target/count` 概率决定本轮是否更新（控制每帧射线数） | bevy 软目标直接可用：**每帧只更新一部分 cell**，把射线预算摊平 |

### S11-PLAN 需要修正的表述
- 「GPU hashmap（线性探测+墓碑）」→ 实际两家回收都不用墓碑，改为「线性探测 + life 衰减清零重用」。
- 补坑：radiance 原子累加必须 int 量化 + 双缓冲帧末合并；key/checksum 双哈希；软目标控预算；cell 尺寸 LOD 先不做（Carrot 实测关掉的教训）。

## 3. S12 时空滤波（bevy_solari 降噪 + UE temporal 对照）

- UE 探针级时域在 `LumenScreenProbeGatherTemporal.usf`（ScreenProbeTemporalReprojectionCS），是**探针重投影**（probe→probe），像素级滤波另有链路；我们 S12 的「空间联合双边 + 时间累积 + 历史拒绝」按 SVGF 思路自建，口径不变。
- bevy 世界缓存的 luminance-delta 自适应混合（见上表）同样适用于像素级时间累积的历史长度控制——S12 的「历史长度 ImGui 可调」参数化时可直接参考 `alpha^(1/8)` 曲线。
- Carrot 有现成 `svgf.slang` / `denoise-direct.slang` 可作 S12 结构参照（过一遍入口名即可，实现期再精读）。
- motion vector：~~S10-PLAN 已注「复用 M12 的 motion vector，先做简化版（同屏位置直接挪）」~~ **已勘误（2026-11-11 M10.0 开工拍板）**：旧案作废，改为 T0 已建好的 `velocity_`（RG16_FLOAT）逐像素真 motion vector；探针重投影走 probe→probe 查 velocity（UE 的 ScreenProbeTemporalReprojectionCS 同为先重投影探针格、非逐像素滤波）。

## 4. 历史约定核对（不新增偏离）

- 三处口径不一致处（回收策略、LOD、混合策略）均已在 §2 表内逐项给出取舍理由，属「计划内选择」而非偏离 UE 官方——Lumen 本体没有世界缓存 hashmap（Lumen 用 surface cache 卡片流，S11 的 hashmap 方案来自 bevy/Carrot 两条个人引擎路线），故 S11 整体定性为「参考两条开源路线的综合实现」，与 ROADMAP「翻 04 章；Carrot GI 长文」的原计划一致。
- S10 的权重公式按 UE 平面深度权重实现，是**向官方口径对齐**（原 PLAN 的「距离+法线一致性加权」即此意的粗写，本次具体化）。
- 本笔记为零代码研究产物：不建 worktree、不动 kStage、不进渲染回归（2026-11-09 用户拍板「只做研究类并行」）。
