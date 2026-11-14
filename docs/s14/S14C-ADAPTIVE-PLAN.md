# S14C 自适应探针计划（M14C1，接力 feat/s14-restir wt 串行）

> 立项依据：`M14-PLAN.md` §4（C 分支）+ `S14-PLAN.md` 分支 C + M10.0 台账挂账「自适应加密列 S14 可选」。
> tag `s14-adaptive`；kStage 冻结 S8.2 不 bump；M14B2 收官 commit `cb5bbca` 之上续作。
> 状态：**已拍板（2026-11-14 凌晨：C1-A + C2-A + C3-B）**，按 §5 组合落码。

## 1. 三处实读 + 开发历史（2026-11-13）

| 来源 | 文件 | 结论 |
|---|---|---|
| UE（只读） | `Lumen/LumenScreenProbeGather.cpp` L47-57/L795-905/L2226-2243 + `LumenScreenProbeGather.usf` Mark/Spawn/GetAdaptiveSampleCoord | ① 判据 = **插值覆盖不足**：每均匀探针 16 个 Hammersley 帧间 jitter 采样点，对周围 4 corner 算 upsample 插值权重和（与 gather 同一函数 `CalculateUpsampleInterpolationWeights`），`< MIN_PROBE_INTERPOLATION_WEIGHT(0.01)` → 打 mask（不是方差/置信度）；② Spawn 防重复：查相邻已 spawn 自适应探针的 corner 覆盖权重；③ 预算 = `NumUniformProbes × AdaptiveProbeAllocationFraction`（CVar 默认 **0.5**）——是**追加预算**（atlas 高度 +50%，帧时间随之上升），不是重分配；④ 自适应探针与均匀探针同池（同 atlas、同 trace、同 upsample，`bUseAdaptiveProbes` 分支）；⑤ Mark/Spawn shared-mask bit + 间接 args（`SetupAdaptiveProbeIndirectArgsCS`）喂数。 |
| bevy | `bevy_solari/src/realtime/*.wesl` 全目录检索 | per-pixel ReSTIR + world_cache 架构，**无屏幕探针概念**（per-pixel 天然回避布点问题）→ 自适应加密无等价物。 |
| Carrot | `engine/resources/shaders/` 全目录 | 仅 blit/material/particles/text 等基础 pass，**无 GI/探针**。 |
| 历史 | M10.0 台账（`MILESTONES.md`）+ M14-PLAN §4 + S14-PLAN 分支 C | M10.0 用户拍板「尽量按照 UE 的思路来」；M10.0 台账明记「UE 单级均匀网格 + 自适应加密，自适应加密列 S14 可选」；M14-PLAN 拍板口径：探针按方差/置信度自适应加密、**预算恒定**、`--mode probe-adaptive` + viz 自适应布点显示、验收 = 同射线预算细节区 GI 提升 + 总帧时间不升。 |

## 2. 引擎现状盘点（S10~S12 探针链）

- **布点** `probe.slang::csPlace`：4 级 cascade 均匀格点，每级 `res_l × res_l` 方形 RGBA32F atlas ×2 帧 ping-pong（rgb=世界位置 a=deviceZ，a=0=天空）；baseRes 滑条 4..32。
- **追踪** `probe_trace.slang`：每探针 10×10 octa texel = 100 条 RayQuery；irradiance atlas RGBA16F 每级一张；`--mode filter` 才有逐帧方向抖动（probe 模式确定性）。
- **消费** `probe_gather.slang::csGather`：全屏每像素 4-corner 双线性，UE upsample 权重口径（平面深度权重 exp2、MIN 0.01、Fallback、归一化）**已实现**——Mark 判据可整函数复用。
- **降噪** S12 `filter_temporal/spatial`：irradiance 域，**无显式 confidence 字段**（Q-C1-B 路线需新增跨帧方差累积器）。
- **viz** `probe_viz.slang`：探针点按级着色 L0红/L1绿/L2蓝/L3黄 + prevDelta 白点。

## 3. 关键发现（摆板核心）

计划原文两处口径与 UE 实测口径**不等价/冲突**：

1. **判据**：计划写「方差/置信度」，UE 实际是「插值覆盖不足」（gather 权重函数，无方差累积器）。方差路线需新增跨帧累积器；UE 路线复用现成函数、判据直接对准「GI 插值质量」。
2. **预算**：计划验收「预算恒定 + 总帧时间不升」（重分配语义），UE 是「追加 50% 预算换质量」（帧时间上升）。二者不可兼得，必须二选一或设计重分配变体。

## 4. 拍板项

> **拍板记录（2026-11-14 凌晨，用户逐项拍板）：Q-C1 = C1-A，Q-C2 = C2-A，Q-C3 = C3-B。** 判据与布点形态跟 UE 口径（M10.0「按 UE 思路」延续），预算走重分配守恒（M14-PLAN「预算恒定 + 帧时间不升」原验收原样执行，不改条款）。§5 实施草案即按此组合执行。

### Q-C1 自适应判据【已拍：C1-A】
| 选项 | 内容 | 代价 |
|---|---|---|
| **C1-A（推荐）** | UE 口径「插值覆盖不足」：每均匀探针 16 个 Hammersley jitter 采样点，复用 csGather 的 upsample 权重函数，权重和 < 0.01 打 mask | 新增 1 个纯采样 pass（零射线，成本 ~0.005ms 级）；判据与画面真实插值空白对齐；M10.0「按 UE 思路」既有拍板延续 |
| C1-B | 计划原文「方差/置信度」：filter 链加跨帧方差累积器，高方差格位加密 | 新增累积 buffer + 跨帧状态；filter_temporal 是 S12 冻结基线，改它须单独拍板；方差 ≠ 插值空白（噪声区≠需要的加密区） |

### Q-C2 自适应探针布点形态【已拍：C2-A】
| 选项 | 内容 | 代价 |
|---|---|---|
| **C2-A（推荐）** | UE 口径：place/irradiance atlas 每级高度 ×1.5（尾部预留区），Spawn 把选中采样点写预留区，trace/gather 同池消费 | atlas 重建逻辑改 1 处；trace/gather 各加分支；与 UE 同构 |
| C2-B | 不扩 atlas：自适应探针替换本格位均匀探针（严格 1:1 重分配） | gather 的 4-corner 双线性网格语义被破坏（角落探针缺失），改动反而更大；UE 无此形态先例 |

### Q-C3 预算口径（决定验收判据怎么写）【已拍：C3-B】
| 选项 | 内容 | 代价 |
|---|---|---|
| C3-A | UE 口径追加预算：`fraction` 默认 0.5，帧时间上升，验收改「细节区 GI 提升（参考图对比）+ 帧成本如实记账」 | 与 M14-PLAN 已拍验收「总帧时间不升」冲突，需改写计划验收条款 |
| **C3-B（推荐）** | 预算重分配：Mark 同时判「低价值均匀探针」（格位 16 采样点无一打 mask = 周边插值已覆盖良好）→ trace 对其整探针早退（省 100 射线）→ 省下预算给加密探针 → trace 总量守恒、帧时间不升 | trace pass 加早退分支；加密探针数上限 = 均匀探针数 × fraction（默认 0.5）；验收可按 M14-PLAN 原文执行 |
| C3-C | 只加密不重分配，验收放宽「帧时间增幅 ≤ 10%」 | 违背计划原文判据，且 M14B2 教训：验收判据模糊化必留后患 |

### 无需拍板项（既有拍板口径延续，直接执行）
- mode 名 `--mode probe-adaptive`（M14-PLAN 已拍）；viz 自适应探针 = 品红点（机械延续按级着色方案）；Mark/Spawn 合一间接 args 回填按 A2 `csArgsPatch` 先例；`--probe-adaptive-fraction` 滑条 clamp 0.25..1.0。

## 5. 实施草案（拍板后按 Q-C1A+C2A+C3B 组合落码）

1. `csAdaptiveMark`（新 pass，每级）：16 采样点复用 gather 权重函数 → mask buffer（每探针 1 uint，bit0..15 = 采样点位）+ 格位低价值 bit。
2. `csAdaptiveSpawn`（新 pass）：mask → 相邻防重复（UE SpawnCS 口径）→ 预算上限截断 → 写 place atlas 预留区 + args[0]=选中数。
3. `probe_trace`：dispatch 高度 ×1.5；尾部区段 = 自适应探针；均匀区段 mask 全 0 格位早退（C3-B 守恒面）。
4. `probe_gather`：corner 池并入归属格位的自适应探针（平面深度权重同口径，`bUseAdaptiveProbes` 分支）。
5. `probe_viz`：自适应探针品红点。
6. viewer/HUD/两后端/CMake/selftest 接线；`--mode probe-adaptive` + fraction 滑条。
7. 验收（M14-PLAN 原文口径）：① 细节区 GI 质量 = probe-adaptive vs baseRes 翻倍的纯均匀 probe 的 MSE（细节区裁剪窗口）逼近度；② 帧时间 = `--pass-timing` 记账 probe vs probe-adaptive 不升 + trace 总量守恒证明（早退数 + 加密数 = 原总量）；③ viz 布点显示（品红点集中在模型边缘区）；④ example 卡 + run.bat verify（确定性路径 bytes 门）+ regression `--only` + MILESTONES 四栏。

## 6. 冻结面

- `--mode probe` / `filter` / `restir` / `soft-raster` / `visbuf` 等不带新旗标的既有模式 bytes 保形（s10-* / s12-* / s14a-* / s14b-* 全部历史卡不漂）；新分支只在 `--mode probe-adaptive` 激活。
- `probe_trace.slang` 非自适应态（mask 恒 0）行为与现版逐位一致（早退分支以 mask 全 0 短路，probe/filter 态 mask buffer 不创建）。
- `filter_temporal.slang` / `filter_spatial.slang` / `world_cache*.slang` 一行不碰。
- kStage 冻结 S8.2 不 bump；新卡验收走 run.bat verify + regression `--only` 双闸（M9.0 澄清口径）。

## 7. 实码回写（2026-11-14，Q-P4「按实码回写」先例；落码后与 §5 草案的差异）

落码过程中按调试实锤收敛出**结构守恒变体**，与 §5 草案（fraction 滑条 + 低价值早退）的对应关系如实入档：

| 草案（§5） | 实码 | 差异依据 |
|---|---|---|
| 16 采样点/格位 Mark | **256 采样点/格位**（16×16 Hammersley 固定网格） | 调试实锤：16 点密度不够 → 插值空白漏判；256 点后 Mark 稳定 |
| Spawn 相邻防重复 + 预算上限截断（fraction 滑条） | **per-tile 至多 1 spawn**（无 fraction 滑条） | 结构守恒：res_eff=baseRes/2 时均匀网格减半省出的格位数 = 每格位 1 spawn 的加密数上限，天然防重复、无需截断参数 |
| trace dispatch 高度 ×1.5 | **atlas/irradiance 高度 ×2**，trace dispatch 线程数与 probe 模式**恒等**：`(res_eff·10) × (2·res_eff·10) = (baseRes·10)²` | 守恒证明改为结构性（构造恒等），「早退数+加密数=原总量」的运行时记账不再需要 |
| trace 均匀区段 mask 全 0 格位早退 | 均匀区段整格位早退保留（Mark 低价值位），trace 线程数不变的语义下早退只省射线遍历不省线程 | 与恒等 dispatch 一致 |
| gather corner 池并入自适应探针 | 落码 + 白斑修正：插值池 4→8 + spawn 探针权重限幅 | 调试实锤：4 corner 池混入 spawn 探针后权重溢出产生白斑 |
| 验收 ②「帧时间不升 + 运行时守恒记账」 | 守恒 = 构造恒等（上表第 3 行）+ `--pass-timing` csTraceProbe 实测对照（check-s14c-adaptive.py timing 门，1.5x 噪声裕量） | 运行时计数回读需要 GPU→CPU 同步，构造恒等 + 实测记账等价且零开销 |
| `--probe-adaptive-fraction` 滑条 | 不落地（无 fraction 概念） | 被 per-tile 1 spawn 结构守恒替代；调参面收敛为 `--probe-base-res`（4..32，0 级 cascade 分辨率） |

**Vulkan teardown 根修（2026-11-14 落码中实测）**：M14C1 初版把 Mark/Spawn 的 shader/layout/PSO/mask buffer/binding set 在 `initProbe`/`ensureProbeResources` 里**无条件创建**——probe/filter/restir 模式也背上这批资源，Vulkan 后端设备拆卸时 `ComputePipeline::destroy` → nvoglv64 驱动 C0000005（5/5 稳定复现；dmp 符号化 `viewer!nvrhi::vulkan::ComputePipeline::{dtor} → vk::Device::destroyPipeline`；vsm 同源 exe 无 M14C1 代码 probe 模式干净退出，实锤 M14C1 新资源为根因）。根修 = `setProbeAdaptiveMode(true)` 提到 `initProbe` **之前**（两后端对称），scene.cpp 五处创建按 `probeAdaptiveMode_` 守卫——非 adaptive 模式零新增资源，与 §6 冻结面「probe/filter/restir 态 mask buffer 不创建」的原意对齐（§5 草案第 3 条只写了 dispatch 保形，漏了资源创建面，实码补齐）。

## 8. 拍板 A：spawn 权重空间根修（2026-11-14 上午，用户拍板）

验收①实测发现 spawn 区 GI 被推离高质量参照（u8↔u32=338 → adaptive↔u32=552，spawn 区），差异热图呈 tile 条带污染。根因：初版 gather 8 池让 spawn 探针**借用所属格位 corner 的双线性权**，但探针实际位置在格位内部任意点（Mark 命中的失效采样点反投影 + 法线偏移）≠ corner 位置——空间权重错位。

**拍板方案 A（用户）**：spawn 权重改按其真实世界位置距离衰减，不借 corner 权。落码形态（probe_gather.slang）：

- `w_spawn = exp2(-4·(d/spacing)²) × exp2(-10000·rel²)`——d = 像素到 spawn 探针世界距，rel = 平面距离比（口径同均匀角）。
- `spacing` 自标定 = 相邻均匀角点世界距均值（4 邻对 (0,1)(2,3)(1,3)(0,2)，双侧有效才计），**零新 CB 常量、CB 布局不动**。
- 衰减标定：d=0.5·spacing → 0.5，d=1·spacing → 1/16（主影响域 = 本格位）。
- 均匀 4 角段数学逐值不变（非 adaptive 路径 bytes 保形）；50% 限幅与两级降级链不动。

验收判据（同口径重测）：spawn 区 adaptive↔u32 MSE 应回落至 ≤ u8↔u32 起点（338），且均匀模式回归卡 bytes 零漂。

## 9. 拍板记录（2026-11-14 下午）：213.8 根因实锤 + guard 转正 + 验收口径定稿

- **213.8 根因实锤**（diag19/20 全精度 RB 证据链，详见当日日志与 commit `cc69c7d`）：adaptive atlas 高 ×2（下半 spawn 预留区），L3 级 res=1 时 `(res,res)-2` **uint 下溢**使 tile 钳位失效，uniform corner01 ct=(0,1) 落预留区槽位——spawn 探针被当 uniform corner 双线性插值且与 k=4..7 spawn 路径同 texel 重复计权；u8 冻结路径 = OOB Load 返 0。L0-2 级 ct.y ≤ res-1 恒在均匀区，唯一受影响 L3。闭环：门槛翻转 7942px 与 sum 差 14263px 全部 ⊆ L3 26701px（0 例外）。
- **根修（拍板转正，commit `cc69c7d`）**：csGather uniform corner 循环加 `ct.x>=res||ct.y>=res` 越界守卫。guard 后 adp↔u8 gather RGBA16F 与 PNG 全帧**逐位一致**（MSE 213.8→0.00），u8 路径与基线逐位不变，spawn-on/off 两形态复验通过。
- **552 分解定案**：u8↔u32 结构差 338（均匀网格减半的固有逼近差，不受影响）+ 213.8（本 bug，已归零）。§8 的「回落至 ≤338」判据被本条取代。
- **验收口径定稿（拍板）**：① adaptive↔u8 同 res（adaptive 缺省 base 16 ↔ probe `--probe-base-res 8`，res_eff=8 同网格）**逐位一致**为验收门（run.bat `fc /b` bytes 级，双后端），替代已废 MSE/u32 参照门；② spawn GI 贡献本场景实测 0（机制保留，spawn-on 复测 bit-exact）；③ viz 品红门 + timing 守恒门 + present 门 + 负向沿用。
- **挂账（独立后续项，需单独拍板立项）**：spawn 增益验收场景——探针格稀疏/大尺度场景（均匀格欠采样时 spawn 才有正贡献）；现有 Suzanne 场景密度下增益恒 0，强行造景属于为验收而验收，不扩大本里程碑范围。
- **排查补记（2026-11-14 晚，用户拍板排查「增益为何为 0」；diag21/22 + readback 实锤）**：增益=0 的真机制**不是**「场景平坦/密度高」，而是三层结构：① **spacing 门全帧失败**（实测 spacing>0 像素=0，spawn 池整体未进入，已占位槽位 26/8/4/1 从未被读）；② **级-数据错配**（模型像素全部落 L2/L3——L2 uniform 0/4 全空、L3 仅 1 探针；L0 密网格的 7 uniform+26 spawn 只服务天空像素；全场景 GI 实际全部来自 L3 单探针）；③ **门与服务对象互斥**（Mark 在格点探针打空处放 spawn，该处均匀角点恰为天空 → spacing 门必死；UE 无此门）。上条「本场景密度下增益恒 0」的旧表述据此勘误。修复方向待拍板：spacing 门降级 / 维持挂账 / level 分带 {3,5,7} 重审（单独拍板）。

## 10. M14C2（2026-11-14 晚，用户拍板 A）：spacing 门降级落码 + spawn 激活 + 增益实测 + 验收口径 M14C2 重定

**落码（拍板 A = spacing 门降级）**，probe_gather.slang csGather 两处 + CB 两槽：

- **降级公式**：`spacingN==0`（无有效均匀角点对，覆盖不足格位 = spawn 的服务对象）时 `spacing = sceneDepth·g_gAdaptive.y / res_l`——线性视深 × 探针格屏幕角尺寸估算（`g_gAdaptive.y = 2tan(fovY/2)`，CPU 侧 `2/FrameParams.projYScale` 同源，144B CB 布局不动，[33]/[34] 原空槽）。量纲与自标定一致（探针格铺满视口，格距 ∝ 深度×角尺寸）；UE 无此门（自适应探针直接进 upsample 池按世界距衰减）。
- **50% 限幅修订（M14C1 收口修正的必要修订）**：原 `spawnSum > uniSum` 无条件限幅在均匀角全空格位 `scl = 0/spawnSum = 0` 把 spawn 一并清零——不修订则降级形同虚设。改为 **`uniSum > 0` 时才限幅**：混合格位保形（白斑前提不变），全空格位 spawn 独立生效（无双重计权，白斑机理不成立）。
- **spawn 池总开关** `--probe-adaptive-nospawn`（g_gAdaptive.z）：验收仪表，非功能旗标。
- **diag21 残留清理**：上轮排查的 TEMP-DIAG21 计数器（声明/赋值）全部移除。

**实测（Suzanne 缺省机位，DX12 全精度 RB + PNG）**：

| 项 | 数值 |
|---|---|
| spawn 池激活 | gather diff vs u8 = 20474 px（全单向增亮），M14C1 收口版（spawn 死）diff 同数——门降级生效实锤 |
| 保形 | `--probe-adaptive-nospawn` ≡ probe base 8 **逐位一致**（双后端 PNG fc /b）；u8 路径跨构建逐位不变（m2-u8 ≡ c22-u8） |
| 增益（PNG MSE vs u32 参照） | 全帧 u8 190.4 vs adaptive 197.7（略劣）；**分块混合**：改善 top2 块 522→204（−61%）、1937→1682（−13%），恶化块 10 个（最大 654→1203），全部**单向过亮**（6954 px 超 u32+40，0 过暗） |
| timing | verify 实测 ratio 0.71（adaptive trace 0.00512ms vs probe 0.00717ms，守恒门 ≤1.5） |

**增益混合的根因定性（挂账后续项）**：恶化全部来自 spawn 探针烘焙辐照度的**天空 miss texel 污染**（剪影/场景边缘 spawn 探针的外向 texel = 亮 miss 值，经宽 fallback spacing 服务到远端像素；u8 在这些区域 sum<kMin 输出 0）。修复方向（miss texel 烘焙侧处理 / spawn 位置质量门 / 权重再调）**单独拍板立项**，不在本里程碑展开。

**验收口径 M14C2 重定（拍板 A 内含授权）**：M14C1 的「adaptive↔u8 逐位一致」门被降级**按设计打破**（spawn 激活即有差），替代门组 = ① bytes 门（spawn-on 重拍 vs 冻结基线，双后端）；② **保形门**：`--probe-adaptive-nospawn` ≡ uniform-8 逐位一致（双后端，冻结路径保形证明）；③ **激活门**：spawn-on ≠ nospawn（fc /b 必须不同，spawn 池生效的机械证明）；④ timing/viz/present/负向沿用。run.bat verify ALL GREEN（12 门实测）。

**readback 基建限界 DX12-only（M14C1 交付项修正）**：M14C1 的 `--probe-place-readback` 在 VULKAN 上**预存在段错误**——staging 环形第 3 周期崩（先 depth 通道 map 崩，depth 限界后整体仍在 f5 dump 后崩；RGBA16F 同崩；slot0 三次全活；DX12 同代码全程正常）。定性 = NVRHI vk 侧 staging 复用内部问题（本仓不可修），M14C2 限界：vk 清空 readback 路径 + 一次性 notice，全部 copy/poll 块自动失效；M14C1 档案中「两后端」表述以此为准修正（当时只在 DX12 冒烟过）。

## 11. M14C3（2026-11-15 凌晨，用户拍板 A=探针级质量门）：spawn 天空 miss texel 污染修复收官

**§10 挂账修复兑现**。落码（probe_trace.slang + probe.slang + scene.cpp/hpp + gpu_timer.hpp + hud.cpp + renderer.hpp + main.cpp + run.bat）：

- **trace 侧 miss 标记**：`csTraceProbe` 里 spawn 区段 texel（`probeT.y >= res`，uniform 区恒 false 保形）irrad **a 通道写 miss 标记**（miss=0 / 命中=1；a 通道此前无任何消费端——gather/viz/dump 全只读 xyz，已核实）。
- **csAdaptiveSkyFixup pass**（probe.slang 新入口）：每 spawn 探针 1 线程统计 100 texel 的 a 均值，有效占比 < 0.4（`kFixupValidMin` 结构判定写死，非外观调参）→ 清 place atlas spawn 槽 `probe.w=0` → **gather 既有「probe.w<=0 权重 0」剔除语义自动接管，gather 零改动**。t7..t10 SRV（irrad）+ u0..u3 UAV（place atlas）独立 layout/set/PSO（probeAdaptiveMode_ guard 内创建）。
- **旗标**：`--probe-sky-fixup-off`（反向关，验收仪表）。
- **真 bug 修复（验收门抓出）**：csAdaptiveSkyFixup dispatch 条件漏 `probeAdaptiveMode_` → probe/filter/restir 模式 `setComputeState(null set)` 每帧 1 错（ref-u8 门 FAIL，deviceErrors=8）；补条件后 probe 模式 deviceErrors=0。**slang 消除实证**：dxc -dumpbin csplace/cstrace 绑定表无 t7..t10（全局声明 per-entry 消除成立，PSO 反射零漂实锤）。

**验收口径 M14C3（run.bat verify 19 门 ALL GREEN，DX12+VULKAN）**：bytes（spawn-on 重拍 vs 冻结基线 adaptive-*.png，双后端 identical）+ **NOFIX-FREEZE 门**（fixup-off ≡ M14C2 基线 adaptive-m14c2-*.png 逐位，双后端）+ **FIXUP-EFFECT 门**（fixup-on ≠ fixup-off 双后端）+ 保形门 + 激活门 + timing（ratio 1.00）+ viz（品红 1030px 双后端）+ present（mean 36.71/255）+ 负向。M14C3 基线定版 = 干净全量重建输出（adaptive-{dx12,vulkan}.png）。

**事故复盘：混合 obj 布局（本轮最大坑）**：落码后 M14C3 代码「确定性 0xC0000005」（M14C2 基线同参数 rc=0，崩点随增量构建漂移：GpuTimer::end←initBlas → probe cascade 创建段 → ensureProbeTargets+0x48c）。γ 系列实验定位链：α（kPassCount 回 17）仍崩 → β（env gate fixup 资源创建全关）仍崩 → γ1（shader 层回退 M14C2）仍崩 → γ4d 快照实锤单 8B 写 0x100000000 @ probeAtlas_[1][2]、邻居完好 → cdb 数据断点（bp ensureProbeTargets + `ba w8 @rcx+0x16a8`）实证「首写即触发」= 踩踏早于断点 → γ6（initProbe 入口哨兵）「已坏」→ **γ7 干净全量重建 rc=0 根因定案**：多轮 stash/checkout 后改布局类头文件（scene.hpp 5 成员 / renderer.hpp 数组 / gpu_timer.hpp kMaxPasses）的**增量构建产生新旧布局混链**——M14C2 布局 obj 构造 scene_ 按旧偏移写 `probeFirstFrame_=true` @0x16AC，M14C3 布局下该偏移 = probeAtlas_[1][2] 槽高 4 字节 → uint64 恰 0x100000000 → ensure 首写 operator= Release 崩。混合基线拍的 PNG 不可信（bytes 门基线重定版）。

**教训（工程纪律新增）**：① 改布局类头文件后**必须删全部 obj 全量重编**，增量构建不可信；② dump 单点崩栈不能定性踩踏者（崩点是「读被踩对象」非写入点），**哨兵时序 + cdb 数据断点**组合拳才能收敛；③ cdb 符号断点动态设数据断点（`bp 函数 "ba w8 @rcx+offset; g"`）是无 gflags 机器上抓堆踩踏的可行路径。
