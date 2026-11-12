# S12 详细开发路线图：时空滤波（降噪）

> 对应总路线 `docs/ROADMAP.md` 的 S12。预计 3 个里程碑、业余时间约 1 周。
> S10/S11 的 GI 本质是稀疏采样（1 射线/探针/帧），画面必带噪声：空间联合双边滤波抹平 + 时间累积放大样本数 + 历史拒绝防止拖影。Carrot 有现成 SVGF 实现对照。
> **状态：M12.0 T1~T8 已落码并验收全绿（2026-11-12），待提交 / tag。** 开工闸拍板 Q12-A / Q12-B（§0.3），落码期实测「静态结构性 ON≡OFF」后用户追加拍板 **Q12-E**（§0.3，含追因链）；双后端构建 selftest 16/16 + shader 106/106、`run.bat verify` ALL GREEN、新卡冒烟 + 3 条保形 `--only` 双后端 `bytes` 全绿。本文件为 S12 权威决策落点；`kStage` 保持 `S8.2`，工具见 `S12-TOOLS-GUIDE.md`。

---

## 0. 开工拍板记录（2026-11-12，按 `AGENTS.md` 开工闸）

### 0.1 三处定向实读（本轮，带路径）

| 来源 | 文件 | 结论 |
|---|---|---|
| Carrot | `engine/resources/shaders/lighting/svgf.slang` 全文 | `previousFrameGBufferInputs`（上一帧整套 GBuffer）+ color / moments / variance 共 8 张 RW 纹理；`tapFilter` 3×3 按深度差 0.1、法线 `dot<0.9`、entityID 拒绝；`temporal` 固定 `α=0.2` + historyLength；`estimateVariance` 历史 <4 帧转空间估计；à-trous 小波由 variance 引导。**时域对象是独立 noisy GI 图，不是整帧** |
| Carrot | `engine/resources/shaders/lighting/temporal-denoise.compute.glsl` 全文 | 6 张 image（noisy / lastFrame / output / historyLength ×2 / previousViewPos）；重投影有效性 `exp(-世界距离×5)`；5×5 邻域 min/max clamp；`α=min(0.99, 1-1/historyLength)` |
| bevy | `crates/bevy_anti_alias/src/taa/taa.wesl` 全文 | **整帧** `view_target` 时域；`history.a` 存 confidence（静止 +10 / 移动归 1）；5-tap Catmull-Rom + 3×3 YCoCg 方差 clip；blend `clamp(1/conf, 0.015, 0.1)`；越界拒史 |
| bevy | `bevy_solari/src/realtime/{bindings,restir,gbuffer_utils}.wesl` | 另有 `previous_gbuffer`（1 张 packed u32）+ `previous_depth_buffer`（binding 13/14）；`load_temporal_reservoir` 重投影后 `pixel_dissimilar`：切平面距离/viewZ > 0.003 或 `dot(n,n_prev)<0` 即拒绝 |
| UE 5.8.4（**只读，不抄**） | `Engine/Shaders/Private/Lumen/LumenScreenProbeGatherTemporal.usf/.ush` | history 是**多套 `RWTexture2DArray<float3>`**（DiffuseIndirect / Backface / RoughSpecular / ShortRangeAO / GI）+ `NumFramesAccumulated` 4bit 打包 + `EncodedHistoryScreenCoord` 双线性权重。**只对间接光分量做时域，直光不进** |

**三处口径不一致**：时域对象（独立 GI 图 / 整帧 / 多套 GI array）、history 形态（accumulation storage / 双纹理 ping-pong / Texture2DArray）、拒绝依据（完整上一帧 GBuffer / 切平面距离 / 帧数置信度）→ 按铁律摆证据请用户拍板，不自行择优。

`docs/carrot-history.md` / `docs/bevy-history.md` 已 Grep：对 S12 / SVGF / 降噪无专门条目，据实标注。`docs/s12/` 下无 `S12-TOOLS-GUIDE.md`（M12.0 交付时新建）。

### 0.2 历史约定与顺延项到期（本次真正拦路的）

- `S9-PLAN.md:156` / `MILESTONES.md` T0、M10.2 段 / `SHARED-INFRA-OPTIONS.md:44-50`：**Q4「GBuffer 落盘形态」顺延到「S10 与 S12 历史帧需求一起拍」**；S9 拍的是方案 A 不落盘，**顺延项本身没拍**。
- `T0-PLAN.md §0 Q-H2`：「属性落盘**第三次顺延至 S12 实测噪声证据再定**」；`§1`：「M12.2 法线差 > 阈值判据退化为深度差 + 当前帧法线，或到时补一张 previous normal（**属 S12 决策**）」；`§5`：「改写 Q4 结论，**S12 实测证据前不得改**」。
- `SHARED-INFRA-OPTIONS.md:179` 方案 A 风险③：「属性落盘第三次顺延（S9→S10→S12），若 S12 真要 `previous_gbuffer`，等于再建一轮」。
- `S12-PLAN.md` 自身无「待拍板」字样，但顺延项到期 + M12.0 第一行代码（history 读写 / 滤波挂点）会隐式拍死资源契约 → 按开工闸必须先拍板再落码。

### 0.3 拍板结论（2026-11-12 用户）

| 编号 | 问题 | 拍板 | 依据 |
|---|---|---|---|
| **Q12-A** | M12.0 history 累积对象与滤波挂点（现状：GI 已在 M10.2 compose 进 `hdrSceneColor_`，无独立 GI 缓冲） | **方案 A「GI 分量独立」** | 三处里 Carrot / UE 都只对 GI 分量做时域；整帧方案（bevy TAA）会把直光与天空一并平滑，污染 Q3 勘误后的对照链。关键实读：`gatherLinear_`（M10.2 全屏 `RGBA16F`）**本来就是 GI-only 中间缓冲**，`csGatherCompose` 才把它叠进 `hdrSceneColor_` → 方案 A 可零新增 RT 落地 |
| **Q12-B** | 三次顺延的 Q4「GBuffer 属性落盘」判定 | **方案 A'「M12.2 前按实测再拍」** | 符合 `T0-PLAN.md` 原文「S12 实测噪声证据再定」；不提前替 M12.2 定死资源契约。M12.0/M12.1 按 T0 口径「重投影深度差 + 当前帧法线」落码，拿到噪声 / 鬼影实测证据后在 M12.2 开工前再拍板；**Q4 结论不改写、冻结面维持 0 新增独立 GBuffer 资源** |
| **Q12-E**（落码期追加，2026-11-12） | 静态场景 `--mode filter` 的 ON / OFF / `--mode probe` / 32 帧四图 **MD5 全同** → M12.0 自检「静止噪声随时间明显收敛」不成立 | **方案 ①「仅 filter 态加帧间抖动」** | 实读 `probe_trace.slang`：`csTraceProbe` 每探针 64 条**固定**八面体方向，逐帧确定性无随机源 → `lerp(hist, cur, α)` 在静态场景数学上恒等于 `cur`（M11.2「结构性 ON≡OFF」同族坑）；运动场景 `--orbit-speed 5` 实测 ON≠OFF，证明 filter pass / 重投影 / ping-pong 本身正确。给 GI 引入逐帧采样抖动后，时间累积才有可观测的收敛。被否：② 如实记录 ON≡OFF 改走运动自比（验收口径退化，M12.1/M12.2 失去噪声尺子）、③ 人工噪声注入脚手架（不反映真实 GI 采样） |

**Q12-E 落码期追因链（实测，非推断）**：
1. 首版「切平面微扰 0.30f」：`--probe-atlas-dump` 证明 atlas 已变（filter 与 probe 的 atlas 图 MD5 不同），但四张合成图仍逐字节相同 → 朝外方向微扰不改变 hit/miss，而 `csGather` 只采外法线方向 texel（M11.2 `probe_atlas_dump.slang` 文件头实锤）。
2. 改「逐帧随机方向置换」（每个存储 texel 每帧重采一个 core texel 方向）后 atlas 与 gather 输入都带真噪声，合成图却仍全同 → 追到 **`--dist 7` 下 `csGather` 权重 `exp2(-10000·rel²)` 与 fallback `exp2(-1000·rel²)` 全灭**，`sum < kMin` → `result = 0` → `gatherLinear_` 恒 0（GI 直出图 960×540 全像素 0）。s10/s11 基线卡都用 `--dist 7`，其 GI 项本来就是 0，与 S12 无关。
3. 验收改走 **`--dist 4`（权重存活）+ 新增 `--filter-gi-dump`**（复用 M11.2 `csIrradDump` / `irradDumpLayout_` / `irradDumpPSO_`，零新增 shader / PSO / RT）把累积 GI 直出。双后端实测 `hf` OFF 1.216 → ON16 0.677（ratio 0.556 / 0.557 ≤ 0.88）、MAE 3.902 → 2.687（ratio 0.688 ≤ 0.90）、`diffPixels` 135409 / 135413；合成图补充证据 `hf` 1.873 → 1.493。
4. 工具口径：Q11-1 拍板 A → GPU 数值双后端非逐字节相同，断言走**统计不变量**（`tools/check-s12-temporal.py`，纯标准库无 PIL）；`example/s12-temporal/run.bat verify` 一键双后端 + 负向控制 + selftest。详见 `docs/s12/S12-TOOLS-GUIDE.md`。

**Q12-E 落码硬边界（保形）**：抖动**只在 `params.filterTemporal == true` 时启用**，非 filter 态 `g_traceBounce.z/w` 恒 0、抖动分支整块不执行、`dir` 仍是 `octaTexelToDir(octT, octRes)` 原表达式 → `s10-*` / `s11-*` 与 104 张卡 `bytes` 零漂（M11.2 同款 flag 门控先例，已实测 3 条 `--only` 双后端全绿）。复用 `g_traceBounce` 的**保留分量**（z=enable / w=frameIndex）→ `traceCB_` 仍 80B、`g_traceBounce.y`（probeCacheDiag）不动，**CB 尺寸零变化**（M11.0 CB 越界实锤）。`--filter-gi-dump` 只在显式开关下旁路 compose，默认 compose 块一字不动（M11.1 冻结基线形态）。

被否掉的方案留档：Q12-A 的「整帧时域」（直光被平滑）、「新增 GI-only RT」（+1 张纹理、动基线路径）；Q12-B 的「现在拍维持不落盘 / 补 previous normal / 补完整 previous_gbuffer」（分别 = 提前定死、Q4 范围扩展需全仓勘误、量化风险 + 双路径维护）。代价矩阵见本轮对话第一屏与 `docs/s9/Q4-GBUFFER-OPTIONS.md` §4/§5。

### 0.4 已拍板（7 条，属「已定计划内机械执行」）

1. T0 资源形态：`velocity_` `RG16_FLOAT` / `history0_`-`history1_` `RGBA32_FLOAT` ping-pong / 每 pass 单一 BindingSet / shift 沿用 §2.5 / ping-pong 不设 permanent。
2. Q4 方案 A「不落盘，即取即算」（S9 结论，本阶段不扩展范围）。
3. Q-H3：TSR / 时域挂在 tonemap 之前（UE 口径）。
4. M12.0 `α≈0.1` 起步，α 与历史长度 ImGui 可调；历史长度自适应可参考 bevy 亮度 delta 曲线 `alpha=|ΔL|/L`、`max_samples=mix(cap,1,alpha^(1/8))`（`GI-RESEARCH-NOTES.md` §3）。
5. M12.1 5×5 联合双边权重 = 空间距离 × 法线夹角 × 深度差（亮度差可选）。
6. 时间 / 空间 / 拒绝三层可单独开关；`kStage` 保持 `S8.2`，既有 104 张版本卡与 HUD 基线零重拍。
7. **Q12-E 帧间抖动**：仅 filter 态对 `csTraceProbe` 每条射线做 per-(coreTexel, probe, frame) **随机方向置换**（微扰版无效，见 §0.3 追因链）；抖动与「时间层开关」解耦（`--temporal-off` 时抖动仍开 → OFF 图 = 单帧噪声、ON 图 = 收敛后）；`traceCB_` 尺寸不变，帧号取模 64 保证确定性。验收对象 = `--filter-gi-dump` 直出的累积 GI（复用 M11.2 irrad-dump 管线），场景参数 **必须用 `--dist 4`**（`--dist 7` 下 gather 权重全灭、GI 恒 0）。

### 0.5 冻结面（拍板后仍冻结，落码时逐项解）

| 冻结项 | 状态 |
|---|---|
| 新增独立 GBuffer 纹理 / MRT / 落盘 UAV | **冻结**（Q12-B 判为 M12.2 前再拍） |
| `history0_/history1_` 的实际读写消费与 `t0Parity_` 语义 | 随 T3 解（T0 至今零消费：仅 `scene.cpp` 2509-2512 创建、7766-7767 释放、7772 复位） |
| `ViewMode::Filter` / `--mode filter` / cases.json 新卡 | 随 T1、T7 解（`modes.hpp:38` 注释仍写「S1 起依次追加：Cache, Filter, Final」） |
| M10.2 `csGatherCompose` 合成路径的**默认分支** | **冻结**（M11.1 实锤：冻结基线的代码形态也是基线的一部分）→ T5 以 filter 态旁路实现，默认路径一字不动 |
| M10.1 `csTraceProbe` 的**非 filter 分支** | **冻结**（Q12-E）→ 抖动必须用 `g_traceBounce.z` flag 门控，非 filter 态原方向表达式一字不动；`traceCB_` 80B 尺寸不变 |
| `direct.slang` 基线（Q11-2b 刚重开） | **冻结**，本阶段不碰 |
| 既有 104 张卡与全部 HUD 基线 | **冻结**（保形硬验收：`bytes`） |

### 0.6 M12.0 任务书（落码按 T1~T8 执行）

| # | 任务 | 落地口径 / 实读依据 |
|---|---|---|
| T1 | `ViewMode::Filter` + `--mode filter` 注册 | `modes.hpp` 枚举 + `viewer/main.cpp` 模式白名单 / 五处注册 / `runMode` case / 缺省 `Suzanne-materials.nmesh`（TLAS 需 NFLB fallback，同 `probe-viz`） |
| T2 | velocity 复用，**不另建 prepass** | `drawProbePlace` 内 `velocityFb_` + `vsVelocity`/`psVelocity`（`scene.cpp` 3744-3761）已产出完整逐像素 velocity；filter 态复用 probe 链基建（同 `cacheViz` 的「probe 链复用」先例），`prevUV = curUV - velocity`；M16.0 的 jitter / unjittered 剥离尚未落地，当前无 jitter 序列，口径自洽 |
| T3 | GI 分量时域累积，**零新增 RT** | 当前帧 GI 输入 = `gatherLinear_`（M10.2 全屏 `RGBA16F`，`csGather` 输出）；`history0_/history1_`（`RGBA32F`）作累积结果 ping-pong，`t0Parity_` 帧奇偶交换 read/write。**Q12-E 补**：filter 态在 `csTraceProbe` 加逐帧随机方向置换（`traceCB_` 保留分量 z/w，CB 尺寸不变）为时间累积提供真噪声；验收用 `--filter-gi-dump` 直出 GI 分量（复用 M11.2 irrad-dump 管线） |
| T4 | 新 `filter_temporal.slang` compute 8×8 | 读 velocity_ 回投 prevUV → 采样上一帧 history → `α≈0.1` 混合当前帧 `gatherLinear_` → 写本帧 history；α / 历史长度 ImGui 双向；三层独立开关（M12.0 只落时间层，空间 / 拒绝留可关 flag）；单一 BindingSet（§2.7） |
| T5 | **保形边界（最高优先）** | 非 filter 模式 `csGatherCompose` 的绑定与执行顺序**一字不动** → `s10-gather` / `s10-refgi` / `s11-*` 与既有 104 张卡 `bytes` 零重拍；filter 态才把 compose 读源切到时域累积结果（独立 set 或 shader 分支，落码时按实读定，不预设槽位号） |
| T6 | 状态登记 | §2.15 / §2.19：history0/1 不设 permanent、每帧 `beginTrackingTextureState` 如实声明、帧内禁第二个 immediate cmd list（§2.8）、clear 并入帧内；`SHADER-CONVENTIONS.md` §2.19 槽位同步表补 S12 行 |
| T7 | 交代物 | `example/s12-temporal/` 基线 PNG + run.bat verify + `cases.json` 新卡（**python json 库 indent=1 + 尾换行，追加后必须跑一次 regression 冒烟**，M11.2 卡漏 `id` 挡全量实锤）+ `docs/s12/S12-TOOLS-GUIDE.md`；`kStage` 不 bump → 按 `AGENTS.md` §3，新卡须 `run.bat verify` + `regression.py --only` 双验，`server.py --check` 只证旧阶段没回归 |
| T8 | 验收 / 收口 | 双后端构建 + selftest + shader 产物三处登记逐字同口径（CMake `_ENTRIES_*` + kArtifacts）；**Q12-E 后验收口径 = `--dist 4` + `--filter-gi-dump` 静态 ON/OFF/REF 自比**（`--temporal-off` = 单帧噪声 vs ON = 收敛后），`check-s12-temporal.py onoffref` 统计断言（Q11-1：非逐字节，走不变量）+ 负向控制 + selftest，`run.bat verify` 一键双后端；快速移动跟手用 `--orbit-speed` + `--screenshot-seq`（M11.3 工具化）；默认只跑 2~3 条 `--only` 保形 spot，**全量总闸只在 tag 收口跑一次**；tag `s12-temporal` |

### 0.7 非阻塞待拍板（归后续里程碑，不阻塞 M12.0 落码）

- **Q12-C**：M12.1「先时间后空间（**或交替迭代 1~2 次**）」括号内未定 → M12.1 开工前拍板，写回本文件。
- **Q12-D**：M12.2「与 **S12 自立的时域参考** 对比主观可接受」——参考图形态未定义（高采样离线参考 / 关滤波 N 帧平均 / 其它），M12.2 开工前拍板。Q3 勘误（S9 一跳图不含间接光，不是 GI 尺子）仍成立。
- **Q12-B 续拍**：M12.2 开工前，凭 M12.0/M12.1 的噪声与鬼影实测证据，在「维持不落盘 / 补 previous normal / 补完整 previous_gbuffer」间拍板；拍板后按 `AGENTS.md`「拍板后全仓 grep 旧口径同步勘误」更新 S9-PLAN / T0-PLAN / SHARED-INFRA-OPTIONS / ROADMAP / SHADER-CONVENTIONS / MILESTONES / UE-DIFFS。

---

## M12.0 Motion vector + 时间累积（2 天）

- [ ] Motion vector：复用 **T0 里程碑落地的完整逐像素 velocity GBuffer**（独立 `RG16_FLOAT` 纹理，当前 clip pos vs 上一帧 clip pos，用 unjittered 矩阵剥离 jitter，jitter 剥离口径见 `t0/T0-PLAN.md`），本步不另建一份。~~物体全静态时先只做相机版~~（2026-11-09 S16 预研修订：TSR 必须逐像素物体速度，相机版会让动目标 ghost 直接爆；ROADMAP 的「T0 喂 S12 复用 motion vector」指的是同一张完整 velocity GBuffer，见 `t0/T0-PLAN.md` 与 `s16/S16-PLAN.md` 预研 §4）。**2026-11-12 实读补充**：该 prepass 落在 `drawProbePlace`（probe 链）内，非全模式通用 → filter 态复用 probe 链基建，不另建（§0.6 T2）。
- [ ] 时间累积：**GI 分量独立**（Q12-A 拍板）——当前帧 `gatherLinear_` 与历史帧按 `α` 混合（α≈0.1 起步），历史位置 = motion vector 回投采样；history0/1 ping-pong 存累积 GI，合成时才叠进 `hdrSceneColor_`
  - 📋 预研证据（2026-11-09，`docs/s10/GI-RESEARCH-NOTES.md` §3）：bevy 世界缓存用**亮度 delta 自适应历史长度**——`alpha=|ΔL|/L`，`max_samples = mix(cap, 1, alpha^(1/8))`，变化快的区域自动短历史。像素级时间累积的历史长度可调可直接套这条曲线（出处 gboisse bsky，与 UE 探针重投影口径不冲突）
- [ ] `--mode filter` 接入，α 与历史长度 ImGui 可调；时间 / 空间 / 拒绝三层可单独开关

**自检**：静止时噪声随时间明显收敛（**Q12-E 后实测成立**：`hf` 1.216 → 0.677、MAE 3.902 → 2.687，见 §0.3 / T8 / `S12-TOOLS-GUIDE.md`）；移动画面基本跟手（拖影留到 M12.2 解决）；**非 filter 模式既有 104 张卡 `bytes` 零重拍**（§0.5 / T5 / Q12-E 保形硬验收，3 条 `--only` 双后端实测全绿）。tag：`s12-temporal`。

## M12.1 空间联合双边滤波（1~2 天）

- [ ] 5×5 联合双边：权重 = 空间距离 × 法线夹角 × 深度差（亮度差可选）
- [ ] 与时间累积串联：先时间后空间（或交替迭代 1~2 次 —— **Q12-C 待 M12.1 开工前拍板**）

**自检**：单帧噪声显著下降；边缘不被糊穿（法线/深度权重生效的直接证据）。tag：`s12-spatial`。

## M12.2 历史拒绝 + 验收（1~2 天）

- [ ] 历史有效性判定：重投影深度差 > 阈值 → 丢弃历史；**「法线差 > 阈值」按 Q12-B 拍板暂不落码**，本阶段口径 = 深度差 + 当前帧法线（`T0-PLAN.md §1` 退化口径），M12.2 开工前凭实测噪声 / 鬼影证据在 Q12-B 续拍里定是否补 previous 属性（disocclusion 区域重生噪声，允许短暂噪但**不允许鬼影**）
- [ ] 参数面板定型（滤波强度/历史长度/拒绝阈值）

**自检（S12 验收）**：静止与运动都无拖影鬼影；disocclusion 处无光斑残留；与 **S12 自立的时域参考**对比主观可接受（**Q12-D 待定**：参考图形态 M12.2 开工前拍板；Q3 勘误：S9 一跳图不含间接光，不能量 GI）。tag：`s12-done`，快速移动证据用 `--orbit-speed` + `--screenshot-seq` 确定性逐帧序列（M11.3 工具化），`docs/screenshots/s12_*.png`。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M12.0 时间累积 | 2 天 | `s12-temporal` |
| M12.1 空间滤波 | 1~2 天 | `s12-spatial` |
| M12.2 历史拒绝+验收 | 1~2 天 | `s12-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| 现成 SVGF 实现 | `Carrot/.../lighting/svgf.slang`（最直接的对照）+ `temporal-denoise.compute.glsl` |
| bevy 侧降噪 / 历史拒绝 | `bevy_anti_alias/src/taa/taa.wesl`（YCoCg 方差 clip + confidence）；`bevy_solari/src/realtime/{bindings,restir,gbuffer_utils}.wesl`（`previous_gbuffer` + `pixel_dissimilar`） |
| UE 侧时域形态（只读） | `Engine/Shaders/Private/Lumen/LumenScreenProbeGatherTemporal.usf/.ush` |
| 概念 | kb/index.html#lum（02 章 final gather 后处理节）；SVGF 原论文（只看权重设计） |

## 底线

- 滤波每一层（时间/空间/拒绝）必须能**单独开关**——「全开才对、关一个全崩」说明耦合坏了
- 拖影与噪声是跷跷板：调参时**以 S12 自立参考图为准绳**（Q3 勘误：S9 参考图只含直光一跳，不是 GI 尺子），别追求画面「感觉干净」
- **默认路径保形**：非 filter 模式一字不改（M11.1 实锤：冻结基线的代码形态也是基线的一部分）；任何「修一下 compose 顺序」的冲动先核是否破坏既有 104 张卡 `bytes`
- **Q4 冻结面**：M12.0/M12.1 零新增独立 GBuffer 纹理 / MRT / 落盘 UAV；补 previous 属性属 M12.2 续拍，须用户显式拍板 + 全仓旧口径勘误
