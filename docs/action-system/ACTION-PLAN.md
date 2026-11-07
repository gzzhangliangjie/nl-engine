# ACTION-PLAN — 动作系统 / 动画控制器（A 类并行轨道，2026-11-08 立项）

> 轨道归属：`PARALLEL-TRACKS.md` §2.4（纯逻辑层，并行性最高的 A 类）。worktree = `E:/UE_STUDY_2026/nl-engine-action-system`，分支 `feat/action-system`。
> 性质：**纯 CPU 确定性逻辑层**——零 GPU、零渲染、不进 `kStage`、不进 bytes/near 渲染回归总闸。验收走**工具版交代**（展示台 tool 卡 + 确定性单测输出 + 真实输出证据图）。
> 范围澄清：本轨道是「动画动作」（clip 播放 / 状态机 / 混合树），**不是** gameplay 技能系统（CD/连招/buff）——那是另一条轨道。

---

## 1. 目标

给 nl-engine 建「驱动蒙皮的上层控制器」：

1. **片段（Clip）数据结构 + 曲线采样**——在动画曲线上按时间 t 取骨骼局部变换（TR），确定性可单测。
2. **混合**——线性混合（两 clip 按 weight）与加法混合（跑动中叠加持枪瞄准类）。
3. **动作状态机**——idle/walk/run/attack 状态 + 转移条件 + 跨状态混合时长（crossfade）。
4. **1D Blend Space**——按速度参数在多个 clip 间插值（2D 按需后置）。
5. （后挂）蒙皮就绪后接 2.3 骨骼蒙皮，做「看到角色动起来」的视觉验收。

## 2. 三处参考实读结论（2026-11-08，带证据）

### 2.1 bevy_animation（主参考，最贴数据组织；本地 checkout = 0.20.0-dev，RFC 51 已落地）

- **Clip 按目标组织**：`AnimationClip { curves: HashMap<AnimationTargetId, Vec<VariableCurve>>, duration }`（`crates/bevy_animation/src/lib.rs`）。骨骼寻址 = **全路径名 blake3 哈希 UUID**（`AnimationTargetId::from_names`），clip 与骨架解耦、天然支持重定向。
- **曲线**：`VariableCurve(Box<dyn AnimationCurve>)`，关键帧 `(f32, T)` 对（`BlendableKeyframeCurve`，内部 `UnevenCore`）；采样时间**钳制到曲线 domain**（`sample_clamped`）；**插值策略编码在曲线实现里**（gltf_curves.rs：Step / Linear / CubicSpline 三种，四元数采样后归一化）。
- **播放器**：`AnimationPlayer { active_animations: HashMap<BlendNodeIndex, ActiveAnimation> }`；`ActiveAnimation { weight, repeat, speed(负数=倒放), seek_time, paused }`。每帧系统链：advance_transitions → advance_animations（并行 tick）→ animate_targets（采样写回组件）→ 事件。
- **混合图**：`BlendGraph`（petgraph DAG），节点类型 `Clip / Blend(子权重归一化) / Add(不归一化、逐个累加)`。
- **crossfade**：`AnimationTransitions::play(new, duration)`——旧动画线性衰减 fade-out（`weight_decline_per_sec = 1/duration`），归零后 stop。**文档明说这是临时 API，状态机留给上层**；bevy **没有**状态机等价物（全 crate 仅 transition.rs:3-4 一句提及）。
- **骨骼层级**：无 Skeleton 资源，骨骼 = 普通实体层级 + 名字寻址，采样直接写骨骼实体的 Transform。
- **加法**：一等公民（`BlendNodeType::Add`）；`Quat` 加法语义 = `slerp(IDENTITY, rot, weight) * rotation`（相对旋转叠加）。

### 2.2 UE5 5.8.4（工业终态，只读对照）

- **轨道数据**：`FRawAnimSequenceTrack { PosKeys, RotKeys, ScaleKeys }`（`Public/Animation/AnimTypes.h:852`）——**key 不带时间戳**（均匀帧采样），每数组要么 N 帧、要么 1 个常量 key；外部单位一律**秒**，帧率只是元数据（`AnimSequence.h:731`）。插值在**解压/提取时**做：`GetKeyIndicesFromTime` 秒→帧 index+Alpha，两 key `Blend()`（`AnimationUtils.cpp:1051` / `AnimationRuntime.cpp:1617`），四元数插值后 Normalize。
- **两阶段纪律**：`FAnimNode` 的 `Update_AnyThread`（**只推进状态/定权重，不产 pose**）与 `Evaluate_AnyThread`（合成 local-space pose）严格分离（`AnimNodeBase.h:746/754`）。
- **状态机**：`FAnimNode_StateMachine`（`AnimNode_StateMachine.h:119` + Private/Animation/AnimNode_StateMachine.cpp）——**烘焙数据（FBakedAnimationStateMachine 状态/转移表）与运行时节点分离**；转移规则按优先级排序（`FBakedStateExitTransition`）；`ActiveTransitionArray` 支持同帧多次切换。
- ⭐ **防跳变细节**：crossfade 时长按**目标态既有权重**反向缩放（`CrossfadeDuration * CalculateInverseAlpha(BlendOption, 1 - ExistingWeightOfNextState)`，cpp:56-70），目标态已有权重时缩短剩余混合时长，避免二次跳变。
- **BlendSpace**：只存 sample 点 + **构建期预三角剖分**，运行时纯查表 + 重心坐标权重（1D=线性段，2D=三角形）；权重再做时间平滑（`BlendSpace.h:561/954`）。
- **加法**：资产差量标记 + `FAnimNode_ApplyAdditive` 独立节点，`AccumulateLocalSpaceAdditivePose` 逐骨累加（local space）。

### 2.3 Carrot（节奏参照）

- **动作逻辑层完全空白**（二次确认）：搜 `Blend/StateMachine/BlendTree/lerp/slerp/crossfade` 全部命中 Vulkan 状态或无果；动画栈止步于「GPU 采样预烘焙整副骨骼矩阵关键帧 + compute 蒙皮」（`core/core/render/Animation.h` 的 `Keyframe { timestamp, vector<glm::mat4> }`——**无通道、无插值**）。空白区即本轨道，且印证「别照 Carrot 的整副矩阵关键帧设计」。
- 可借的挂接面：`Transform { position, scale, quat }`（`engine/math/Transform.h`）+ `Skeleton::Pose = unordered_map<BoneName, glm::mat4>`（`core/core/render/Skeleton.h:65`）——**骨骼名 → 局部变换** 是与蒙皮对接的天然出口。
- 玩法参考：`AnimatedSprite` 的 play/pause/speed/timer 播放状态管理范式。

### 2.4 三处综合 → 本轨道选型

| 决策点 | 三处口径 | 本轨道拍板（沿用 2.4 既定阶梯，不另创） |
|---|---|---|
| 骨骼寻址 | bevy=路径哈希 UUID / UE=索引 / Carrot=骨骼名 map | **骨骼名字符串**（哈希表索引）；不做 UUID 重定向——纯逻辑层先保简单，重定向留到真有多骨架资产时 |
| Clip 结构 | UE=per-bone 轨道 key 无时间戳 / bevy=per-target 曲线 (t,v) / Carrot=整副矩阵 | **per-bone 通道曲线，keyframe (t, value) 对**（bevy 口径）；时间单位**秒**（UE/bevy 一致）；域外 **clamp**（bevy 语义） |
| 插值 | UE=提取时 lerp+slerp / bevy=策略在曲线实现里 | **Linear（lerp/slerp+归一化）+ Step** 两种起步；Cubic 按需后置（gltf CubicSpline 罕用） |
| 两阶段 | UE=Update/Evaluate 严格分离 / bevy=advance→sample 链 | **保留分离**：`update(dt)` 推进时间与权重、`evaluate()` 采 pose 纯函数——确定性单测好写 |
| 状态机 | bevy=无（留上层）/ UE=烘焙表+运行时分离 | **轻量单层状态机**（状态→clip + 转移条件 + crossfade 时长），抄 UE 的「目标态已有权重→缩剩余时长」防跳变细节；不做烘焙表（单用户逻辑层无此需求） |
| BlendSpace | UE=sample 点+预剖分+重心 / bevy=图节点 | **1D 段插值起步**（按速度），2D 重心三角后置 |
| 加法 | 两处皆一等公民，Quat=相对叠加 | M-A2 一并实现，语义照 bevy：`slerp(IDENTITY, rot, w) * rot` |
| 范围 | bevy 明说不做状态机 / UE 全家桶 | 不做：2D blend space、动画事件、mask 部分混合、动画层叠 Layer、重定向——按需再议 |

## 2.5 网上生态调研（2026-11-08，用户指示「先找找再动手」）

| 候选 | 状态 | 对本轨道意味着什么 |
|---|---|---|
| **ozz-animation** | MIT，0.17.0（2026-08 活跃），C++17 纯标准库、渲染器/引擎无关、SoA+SIMD 数据导向 | 开源骨骼动画运行时事实标准。覆盖：压缩轨道 Animation、Skeleton rest pose、SamplingJob（可缓存 Context 帧相干优化）、BlendingJob（多层加权/部分混合/加法层/rest-pose 兜底/归一化）、LocalToModelJob、双骨 IK/look-at、root motion（0.16+）、离线 gltf2ozz 优化管线。**刻意不含状态机/混合树/blend space**——正留给我们 2.4 这一层。定位 = **2.3 蒙皮数学层的头号候选**（立项 2.3 时再议 vendored），不改变 2.4 纯逻辑自建路线 |
| Lumix Engine | MIT 开源，层级骨骼+关键帧采样+动画状态机+混合树+root motion+FABRIK IK+重定向 | 自建状态机的**中量级完整参照**（比 UE 小、比 bevy 全，C++ 同语言可读实现） |
| 设计文章（GDC 类） | crossfade 时长经验：快动作 0.05~0.1s / 移动 0.2~0.4s；ease-in-out 优于线性；**Inertialization**（过渡瞬间拍姿态快照+阻尼衰减，只更新一个状态，网易文实测状态更新省 ~10%，UE 推荐默认）；pose matching/相位同步防滑步；**过渡中断规则**（动作游戏必备） | M-A3 状态机设计输入：crossfade 防跳变细节已从 UE 源码拿到（目标态既有权重缩时）；Inertialization 列为后置可选项；中断规则进状态机语义 |
| 小型库（Cryonix / animate.hpp 等） | 个人项目级 | 无可借鉴，忽略 |

**结论（2026-11-08 用户拍板：A 路 · 自建）**：网上没有「现成的动作状态机/混合树 C++ 库」可直接 vendored——该层业界全留给引擎自建（bevy 明说、ozz 不做、UE 是引擎内蓝图）。现成物只覆盖**下层**（采样/混合/蒙皮数学 = ozz）。故 2.4 路线维持自建（现行计划 M-A1~M-A4 不变），ozz/Lumix 只作参照阅读；ozz vendored 与否留给 2.3 立项时决策。

---

## 3. 里程碑阶梯（编号 M-A*，对齐 `PARALLEL-TRACKS.md` §2.4 五步）


| 里程碑 | 内容 | 验收判据（全部确定性） |
|---|---|---|
| **M-A0 立项** | 本计划落盘 + 依赖选型（数学库）拍板 | 用户确认计划 |
| **M-A1 曲线采样** | `AnimClip`（per-bone 平移/旋转通道关键帧）+ `sample(t)`：lerp/slerp/Step、域外 clamp、循环归一 | 确定性单测：固定输入断言输出数值（含 clamp 端点、循环 wrap、quat 归一化）；输出骨骼变换序列 MD5 落证据 |
| **M-A2 混合** | `blend(a, b, w)` 线性混合 + `add(base, additive, w)` 加法混合 | 单测：w=0/0.5/1 三个锚点数值断言；加法 quat 语义（IDENTITY 参照）断言 |
| **M-A3 状态机** | 状态 + 转移条件（阈值式）+ crossfade 时长 + UE 式防跳变缩时 | 单测：事件序列驱动的确定性状态序列断言（idle→walk→run→idle），过渡期 pose 混合权重曲线断言 |
| **M-A4 1D blend space** | 按速度参数多 clip 段插值（预建段表，运行时查表） | 单测：速度扫过全参数域的采样序列断言 |
| **M-A5 视觉验收（后挂）** | 接 2.3 蒙皮 + rigged GLB，驱动可见骨骼动画 | GPU 截图（此时才碰渲染；A 类属性不变，走一次性对照不进总闸） |

## 4. 代码落点与测试载体（待 M-A0 拍板两项）

- **代码**：`core/include/nl/anim/*.hpp` + `core/src/anim/*.cpp`——零图形符号（守「图形符号只在 renderer/」铁律），不链 meshopt/METIS/glfw。
- **测试载体（已拍板）**：仿 `material-graph-tools/` 先例，新建 `anim-tools/` 独立 exe（只链 nl_core），`anim-tools selftest <case>` + ctest 注册；工具版交代 = 展示台 tool 卡实跑 + 从真实输出生成证据图。
- **数学库（已拍板）**：自写最小 `vec3/quat` 数学（lerp/slerp/normalize，百行内）放 `core/include/nl/anim/anim_math.hpp`——零新依赖、纯逻辑层数学面极窄、确定性单测可控。

## 5. 交代纪律（A 类专属，复用不重写）

- 不进 `kStage` / 不动 S0~S14 编号 / 不进 bytes-near 渲染回归总闸。
- 验收 = 确定性单测全绿 + 展示台 `kind:"tool"` 用例（usage / 正向 / 负向三条起步，`expectExit` 0/1/2）+ 真实输出证据图（`cases.json` preview，不许手绘）+ 本 PLAN / `MILESTONES.md` 四栏写实。
- merge 回 master 前：本轨道验收纪要写齐（`PARALLEL-TRACKS.md` §7.2 文档闸）+ 双向 `git status` 自查（§7.3 假绿防护）。
