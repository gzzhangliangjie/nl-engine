# T0 详细开发路线图：共享 temporal / post-chain 基建

> 对应 `docs/ROADMAP.md` 穿插项「后处理链 / TSR 基建」的正式承载里程碑。插于 `s9-done` 之后、`S10` 之前，**不占 `S*` 大版本号**（避免 S10~S16 重编号连锁）。预计 1 个里程碑、业余时间约 1 周。
> **状态：已落码 + 双后端编译验证（2026-11-11 05:0x，DX12/VULKAN selftest 16/16 ×2、shader 74/74）；GPU 字节回归待用户跑（`regression.py --only` 保形 spot）。**
> 决策证据与三方案代价矩阵见 `docs/SHARED-INFRA-OPTIONS.md`（三处定向实读 + 横向矩阵）；本文件是其结论的工程化落地。

---

## 0. 拍板结论（来自 SHARED-INFRA-OPTIONS §0 / §5 / §7）

| 编号 | 拍板 | 依据 |
|---|---|---|
| **Q-H1** | 方案 **A（temporal 最小集，单里程碑）** | bevy prepass / UE 解耦的共同口径：velocity 独立成一张、history 独立 ping-pong、post-chain 与属性落盘解耦；不翻 Q4 已拍的「方案 A 不落盘」案 |
| **Q-H2** | **GBuffer 属性不落盘**（即取即算） | 随 Q-H1=A 一并定；改写「S9 不落、T0 前落」边界——实际是**延续 S9 方案 A**，属性落盘第三次顺延至 S12 实测噪声证据再定 |
| **Q-H3** | **TAA/TSR 在 tonemap 之前**（UE 口径） | `S16-PLAN.md` 已把「TSR 在 tonemap 之前」定为底线；省掉一张 tonemap 后离屏输出 |

> **S12 到期状态（2026-11-12，M12.0 开工拍板 Q12-B）**：Q-H2「属性落盘第三次顺延至 **S12 实测噪声证据再定**」中的再定点 = **M12.2 开工前**，不是 M12.0 开工时。用户拍板「M12.2 前按实测再拍」→ 本 PLAN §0 / §1 / §5 的 Q-H2 结论与冻结面**维持不变**，M12.0/M12.1 按退化口径（重投影深度差 + 当前帧法线）落码、零新增独立 GBuffer 资源；M12.2 开工前凭噪声/鬼影实测证据再拍是否补 previous normal / previous_gbuffer。权威落点 `docs/s12/S12-PLAN.md §0`。

> 诚实边界：本 PLAN 给的是**资源形态与排期**，不是「已验证可落地」结论。`R11G11B10_FLOAT` 作 color attachment + storage + sampled 三能力、`RG16_FLOAT` 作 MRT 附件、主路径重构后保形，都在落码里程碑双后端实测；本 PLAN 零代码、零 tag。
>
> **落码期勘误（2026-11-11 GPU 实测）**：HDR scene color / history 由 `R11G11B10_FLOAT` 改判 **`RGBA32_FLOAT`**——保形硬验收①（bytes 不变）与紧凑 HDR 格式不可兼得：11F/10F 对 8-bit 显示色在 [0.5,1) 的 B 通道 ulp=4/255（实测 26k px/±4），16F 也在 8-bit 半步边界翻转（实测 5346 px/±1）；任意浮点管线只有 FP32 中间格式字节精确（FP32 往返零量化，与 rtTarget_ 同款）。代价 = 3 张纹理显存 ×2（1080p ~25MiB），换来 S12 时间累积精度同步受益。

---

## 1. 范围（方案 A 最小集）

| 维度 | 内容 |
|---|---|
| **新增 RT / UAV** | 4 张（+1 张可选，按 Q-H3=UE 口径不建）：① 离屏 HDR scene color `RGBA32_FLOAT`（**落码期勘误：原 R11G11B10_FLOAT 因保形硬验收改判 FP32**，~7.9 MiB @1080p）；② 独立 velocity `RG16_FLOAT`（~1.98 MiB，bevy 口径）；③ history ping-pong **2 张** `RGBA32_FLOAT`（同上勘误，~15.9 MiB）；④ tonemap 后离屏输出 **不建**（Q-H3=UE → 直接写 swapchain）；`D32 SceneDepth` **复用 backend `depth_`**（`setSharedDepth` 注入；首版自建 `t0Depth_` 全量闸实测 gl5/particles/drive-m3 三族 FAIL——overlay 深度测试读 backend depth_，场景深度必须留在原位） |
| **写入 pass 形态** | 主帧从直出 swapchain 改为渲到离屏 HDR RT（**属 §2.16 主路径重构**）；velocity 走**独立几何/prepass 附加 MRT**（bevy/UE 口径，双帧矩阵 + `unjittered` 剥离 jitter，天空像素单独补写）；新增最小全屏 `present/tonemap` pass 做合成 + gamma（ACES 留 S16，T0 只做最小 tonemap + gamma）。全部并入现有帧命令列表（§2.8） |
| **Q4 顺延项答复** | **不落盘**。`S10` 邻域 / `S12` disocclusion 靠 `gbuffer_common.slangh` 即取即算 + depth 反解；`S15` 维持只读 depth / vis。`S12` M12.2 的「法线差 > 阈值」判据退化为「深度差 + 当前帧法线」，或到时补一张 previous normal（属 S12 决策，不在此里程碑） |
| **绑定布局 / 状态登记** | 每新 pass 各建**单一 BindingSet**（§2.7）；shift 沿用 §2.5（b 256 / t 0 / s 128 / u 384，set 0）；ping-pong 纹理不设 permanent，每帧逐张 `beginTrackingTextureState` 声明当前态（§2.15）；clear 在 `setGraphicsState` 前、帧内不另开 cmd list（§2.8）。**纪律固化见 `SHADER-CONVENTIONS.md` §2.19** |
| **新增显存 @960×540** | ~7.9 MiB |
| **下游扩展面** | `S10` M10.0 有真 velocity（删简化版占位）；`S12` M12.0 声明落地；`S16` M16.0/M16.1 基建被承载、M16.4 挂账句可删；SSR / bloom 有共享 post-chain 落点；`S15` 零影响 |
| **基线改动面** | **中等偏大**：主帧换 RT 属主路径重构，现有双后端截图基线的 gamma / 清屏路径可能被碰；`kStage` 若不 bump，须 `run.bat verify` + `regression.py --only` 双验新卡，并跑 `s8.2` / `s9-*` / `pbr-direct` 保形 spot；spot 必含 lod/dag（§2.16） |

---

## 2. 资源形态表（拍板后形态，落码时按实读固化槽位）

| 槽位类别 | 资源 | 格式 | 状态登记（§2.15 / §2.19） |
|---|---|---|---|
| offscreen scene color | `hdrSceneColor_` | `RGBA32_FLOAT`（落码期勘误） | 每帧 `RenderTarget ↔ ShaderResource` 逐张 `beginTrackingTextureState` |
| velocity | `velocity_` | `RG16_FLOAT`（附加 MRT 写，或独立 prepass 附件） | 写后当帧转 `ShaderResource` 供消费端读 |
| history A / B | `history0_` / `history1_` | `RGBA32_FLOAT`（落码期勘误） | **ping-pong**，按帧奇偶交换 read/write，**不设 permanent** |
| swapchain 输出 | 现有 framebuffer | `RGBA8_UNORM` | 由最小 tonemap pass 写入（Q-H3=UE：tonemap 在 chain 末端、写 swapchain） |

> 不预设 `register(u/t)` 槽位号（Q4 材料 §0 纠偏教训）：落码时按 `renderer/src/scene.cpp` 实读固化进 §2.19，禁止凭记忆编表。

---

## 3. 下游消费端映射（决定本里程碑验收锚点）

| 消费端 | 硬需求 | T0 交付物 |
|---|---|---|
| `S10` M10.0 探针重投影 | 逐像素 motion vector | 读 `velocity_` 纹理（真 velocity，非 M12 简化版占位） |
| `S10` M10.2 final gather | 邻域像素世界位置 / 法线 | 不落盘：靠 `gbuffer_common.slangh` 即取即算（重复 resolve ×N） |
| `S12` M12.0 时间累积 | velocity + history color | `velocity_` + `history0/1_` ping-pong |
| `S12` M12.2 历史拒绝 | 重投影深度差 > 阈值 → 丢历史 | 不落 previous normal：退化为「深度差 + 当前帧法线」 |
| `S15` M15.1 页请求 | 只读 depth / vis | 零影响（维持现有口径） |
| `S16` M16.0 / M16.1 / M16.4 | jitter + 双帧矩阵 + velocity + history + tonemap 前 slot | velocity/history/post-chain 由本里程碑承载；jitter / 双帧矩阵 / 分辨率分离仍归 M16.0 |
| SSR / bloom 穿插项 | 离屏 HDR scene color + 合成 pass | 共享同一份 post-chain，不各建一份 |

---

## 4. 任务清单骨架（落码里程碑才执行，本 PLAN 不写代码）

| # | 任务（摘要） | 状态 |
|---|---|---|
| T1 | `createTargets()` 新增 `hdrSceneColor_` / `velocity_` / `history0_1_` slot（§2.19 同步表纪律），首帧懒建 + clear 并入帧内 cmd（§2.8） | ✅ 落码（`ensureRtTargets` 懒建 + `t0Depth_`/`hdrSceneFb_`；clear 在帧内 `beginHdrSceneTarget`） |
| T2 | 主帧从直出 swapchain 改为渲到 `hdrSceneColor_`（§2.16 主路径重构，spot 必含 lod/dag） | ✅ 落码+**GPU 保形 spot 全绿（bytes）**（场景管线按 RGBA32F+D32 `hdrInfo` 重建 ×6 组；7 处渲染点 `sceneFb` 重定向含 t0Active 回落；6 个出口挂 `presentTonemap`） |
| T3 | velocity：独立几何附加 MRT 或 prepass 附件，双帧矩阵 + `unjittered` 剥离 jitter，天空像素单独补写 | 🔶 纹理已建（`velocity_` RG16_FLOAT）；**写入归 S10 M10.0 消费端落地时一并实现** |
| T4 | history ping-pong：2 张 `RGBA32_FLOAT`（勘误后格式），按帧奇偶交换 read/write，不设 permanent（§2.19） | 🔶 双纹理 + 帧奇偶 `t0Parity_` 已建；**实际读写消费归 S12 M12.0** |
| T5 | 最小全屏 `present/tonemap` pass：合成 + gamma，写 swapchain（Q-H3=UE 口径，tonemap 前无 TSR 时即最终输出） | ✅ 落码（`psTonemap` 直通无二次 gamma——gamma 已在 producer 端烤进 scene color；ACES 留 S16） |
| T6 | 每新 pass 单一 BindingSet（§2.7）+ 槽位按实读固化 §2.19 + 状态登记 | ✅ 落码（blit/tonemap 单一 set + §2.19 T0 槽位同步表已固化） |
| T7 | 对照卡：`S10` M10.0 不再走简化版占位；`regression.py --only t0-*` + `run.bat verify` 双验；跑 `s8.2` / `s9-*` / `pbr-direct` 保形 spot | ⬜ 待 GPU：先跑 `--only s8.2,pbr-direct,s9-reference` 保形 spot（spot 必含 lod/dag） |
| T8 | 收口：四栏台账 + 主线收口闸（tag `t0-shared-infra` + 全量总闸 + `--check`；不跑 `wt_pool.py close`，见 `AGENTS.md` 1b）+ example 卡 | ⬜ |

**落码期勘误（2026-11-11，落码实测回写）**：
- **场景管线按格式重建**：PSO 与渲染目标格式锁定（NVRHI `createGraphicsPipeline(desc, fbInfo)`），场景管线必须按 RGBA32F+D32 的 `hdrInfo` 重建（照 hiz `prepassInfo` 先例按格式构造，不依赖真实纹理），涉及 sceneMode ×4 / cull ×4 / classic ×4 / matResolve / visViz / hiz vizPSO 共 6 组。`visRasterPSO_`/dbgLines/particles/HUD 保持 swapchain RGBA8（visRaster 写 swapchain 被 tonemap 全屏覆盖；overlay 画在 tonemap 输出之上）。
- **离屏深度**：场景主 pass 需要深度测试/写入 → 新增 `t0Depth_`(D32) + `hdrSceneFb_`（`hdrSceneColor_`+`t0Depth_`，§1 范围表的 4 张之外按实读追加，~2.6 MiB）；`hdrColorFb_`（无 depth）仅供 blit。清屏语义与后端对齐：`params.clearColor` 与后端 `ClearColor` 同源（renderer.cpp 已核实）、深度 1.0。
- **已知行为差异（落档）**：场景深度改写 `t0Depth_`，后端 `depth_` 不再被主场景写入 → **debugLines / particles overlay 的深度测试对着空深度（全 1.0），被实体遮挡的调试线框不再被剔除**。仅 `frame.debugLines`/`frame.particles` 开启时有差异，回归 spot 用例均不开（M-P3「缺省零开销」口径），物理调试用例若日后建基线须按新语义重拍。
- **重复 beginTracking 已核实无害**：NVRHI `beginTrackingTextureState` 整纹理语义 = 覆盖重置（`common/state-tracking.cpp:75-79`），shadow 模式「顶部清屏段 + blitRtToHdr 各跟踪一次」合法。

**硬验收**：① 主帧换 RT 后现有双后端截图基线（s8.2 / s9-* / pbr-direct）`bytes` 不变（spot 必含 lod/dag）；② S10 M10.0 有真逐像素 velocity（删 M12 简化版占位）；③ S12 M12.0 时间累积声明可直接复用 `velocity_` + `history0/1_`；④ `kStage` 默认不 bump，HUD 基线零重拍；若 bump 须 `reshot-hud-stage.py` 超 527px 签名追因才许 `--force`。

---

## 5. 冻结面（拍板后、落码前仍冻结）

| 冻结项 | 状态 |
|---|---|
| 新增任何离屏 RT / UAV / `register(u/t)` 槽位 / 纹理格式 / MRT 组合 | **冻结**，待落码里程碑 T1~T6 解 |
| 改写已拍板的 Q4 结论（属性不落盘） | **冻结**，S12 实测证据前不得改 |
| `renderer/shaders/gbuffer_common.slangh`（PURE MOVE / Resource-free，0 处资源声明） | **保留**，本里程碑不触碰 |
| 主帧 present 路径、`kStage`、既有 `example/` 基线 | **冻结**，待 T2/T5 重构时按 §2.16 保形 |

---

## 6. 与相邻里程碑的边界

- **上游 S9**：`gbuffer_common.slangh` 公共反解契约（Q4 方案 A）是本里程碑消费端「即取即算」的底座；S9 不落 GBuffer 纹理，故本里程碑**也不落盘属性**。
- **下游 S10**：M10.0 原「复用 M12 简化版」占位句已删（`S10-PLAN.md:11` 同步），改读本里程碑 `velocity_`。
- **下游 S12**：M12.0 原「复用 S16 前半段建好的」悬空句已删（`S12-PLAN.md:10` 同步），改读本里程碑 `velocity_` + `history0/1_`。
- **下游 S16**：M16.0~M16.3 基建（velocity/history/post-chain）被本里程碑承载，`S16-PLAN.md` M16.4 挂账句可删；jitter / 双帧矩阵 / 渲染-输出分辨率分离仍归 M16.0（见 `S16-PLAN.md` 预研 §1）。
- **SSR / bloom 穿插项**：共享同一份 post-chain（ROADMAP 后处理链行已指向本里程碑）。
