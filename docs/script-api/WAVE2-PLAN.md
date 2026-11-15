# M-W2A — 波 2 绑定域第一段（主线里程碑）

> 归属：主线 master（AGENTS §1b：**不建 track / 不 claim**；收口按主线口径 = annotated tag
> + `docs/MILESTONES.md` 四栏 + 全量总闸 + `server.py --check`）。
> 立项 2026-11-15，用户指令「A开始」→ 开工闸摆 ①/②/③ → 用户拍板「**按①来吧 写到文档**」。
> **本文件是冻结口径，计划内无待拍板架构判定。** 本轮只落文档，不落码；执行另开单。

## 0. 拍板记录（2026-11-15）

| 问题 | 结论（方案 ①） | 被否方案 |
|---|---|---|
| Q1 · Physics 查询绑定目标 | **绑 `nl::physics::QueryScene` 探针世界**（M-PQ0 交付的独立 fixture：5 个静态体，`build()` 自建）。脚本查得到这套确定性夹具，查不到 viewer 现役场景 | ② 先做 M-PQ1 把 `QueryScene` 接 live `DebugScene` 再绑（等于把绑定面与场景集成捆一起）；③ 本轮不做查询 |
| Q2 · Animation 绑定形态 | **薄面**：C++ 侧句柄表 + 按文件加载 clip + 参数驱动状态转移 / tick / pose 读回；`std::function` 不过 blittable 边界；零渲染、零 `viewer` 动画注入 | ② viewer skin 活实例注入 + 脚本动画驱动蒙皮（碰主循环、可能 bump `kStage` 重拍 PNG）；③ 整域后置 |
| Q3 · 范围批次 | **三域一次做**（Audio + Physics 查询 + Animation），单里程碑 M-W2A 一次收口 | ③ 先两域、Animation 留到 M-W2B |
| 里程碑编号 | **M-W2A**（波 2 绑定域第一段） | — |

上一轮「A」的文字口径需勘误两处（本文件 §3 写实，新单勿沿用旧说法）：
「已收官的 Physics 查询、Animation、Audio 接进 exports.api」听上去像三域都有现成活实例，实际
**QueryScene 是独立 fixture 世界**、**Animation 完全没有活实例**。方案 ① 就是在不动这两个前提的
前提下，把三域做成脚本可调用的最小绑定面。

## 1. 目标与边界

**目标**：游戏层 C#（`games/<name>/`）经 `Nl.Script.Api` 静态 facade 能调用三类通用能力——
物理空间查询、动画状态机采样、音频播放/空间定位；三域均可在 `nl-script` selftest 里逐位读回，
并有展示台 tool 卡。

**明确不做（范围外，不是待定项）**：

- `renderer/` 全部、任何 `.slang`、`kStage`、双后端 PNG 渲染基线（A 类铁律：零渲染符号、不进渲染回归）
- 既有 54 条导出的语义/顺序（只许 ApiTable **尾部追加**，禁中间插桩）
- M-PQ1：`QueryScene` 接 viewer 现役 `DebugScene`（live 查询）
- M-SP1：`--sprite-demo`、PNG 基线、多图集每桶换 binding set、RmlUi element/scissor 桥接
- Animation 驱动真实蒙皮 / ECS AnimationComponent / 2D 骨骼动画 / flipbook（`nl/sprite` 域，另案）
- Audio 全量 `AudioWorld` API（流解码、solo/freeze、缓存统计、离线 PCM dump）、音频文件转码
- 脚本侧物理步进、任意事件总线（`P8-SCRIPT-PLAN.md`「有意不做」维持）
- 脚本侧 UI 属性动效（RmlUi RCSS 承担）、LLM/TTS、NPC 行为

## 2. 三处证据（2026-11-15 本轮定向实读）

| 来源 | 实读文件 | 结论 |
|---|---|---|
| Carrot | `engine/script-api/Carrot/Audio/SFX.cs`（`Load` + `Play(SoundSource)`，按域拆 C# 类）、`Physics/Collider.cs`（`Raycast(start,dir,maxLen,ref RaycastInfo)`；`RaycastInfo` 带 `T`/`WorldPoint`/`WorldNormal`/`Collider`）、`engine/engine/scripting/CSharpBindings.h/.cpp`（中央句柄缓存 + 按域暴露） | 「按域拆 facade + 句柄失效安全」思路可抄；Mono internal call 底座不抄。**历史已否**：仿 Carrot 按域拆绑定文件（M-SC4 拍板沿用静态 facade + 单 `Bindings.cpp`） |
| bevy | `crates/bevy_animation/src/lib.rs:748` `AnimationPlayer` / `:882` `play(BlendNodeIndex) -> &mut ActiveAnimation`；`crates/bevy_audio/src/audio.rs:35` `PlaybackSettings` / `ONCE`/`LOOP`/`DESPAWN` | 播放器 = 实体组件 + 设置值，动画/音频都是「句柄/组件 + 显式参数」而非回调；无脚本 crate（据实标注，不硬凑证据） |
| UE 5.8.4（**只读不抄**） | `Engine/Classes/Engine/World.h:2161` `LineTraceSingleByChannel` / `:2210` `Multi` / `:2286` `SweepSingleByChannel`；`Components/AudioComponent.h:517` `Play` / `:583` `Stop`；`Animation/AnimInstance.h` 姿态 UFUNCTION | 三族查询与播放/停止是工业终态的公开面口径（M-PQ0 已据 World.h 定 3 族）；UE 无 C# 绑定（Blueprint/Python），据实标注无对应物 |

历史约定：`docs/MILESTONES.md`（M-PQ0 四栏 / M-A1~A5 / M-S0~S3 / M-GL2 / M-SC4~SC11）、
`docs/p8/P8-SCRIPT-PLAN.md`「胶水层扩展」+ M-SC9 行、`docs/p8/PHYQUERY-PLAN.md`、
`docs/sprite2d/SPRITE2D-PLAN.md`、`docs/PARALLEL-TRACKS.md`、`script/script-api/exports.api` 头部口径、
`AGENTS.md` 决策前置/开工闸/§1b。

## 3. 前提勘误与现状（新单开工前必读）

| 域 | 真实状态 | 对绑定的影响 |
|---|---|---|
| Audio | M-S3 已在 `viewer/main.cpp`（~5844）建 realtime `AudioWorld`（`offline=false`、spatialization 开，设备失败降级 offline），`audio.play` console 命令走 `ensureAudioWorld()` 惰性建；`P8-SCRIPT-PLAN.md` 早有既定 **M-SC9** 行（`audioPlay/audioStop/audioSetListenerPose`） | 最顺：只需宿主注入 `audioSetWorld(AudioWorld*)`，零新架构 |
| Physics 查询 | `core/include/nl/physics/query.hpp` 明写 `QueryScene` = **Fresh deterministic query world，NOT the engine's live DebugScene world；live 共享是 M-PQ1**；fixture = floor/boxA/boxB/boxC/sensor 5 个静态体。现有 `Physics` 域是 M-SC8 **载具** 5 条导出（`physicsAddVehicle` 等），与查询无关 | 绑探针世界 = 查固定夹具；viewer 现役场景查不到，**必须写进 API 文档与 GUIDE，不许暗示是场景查询**。查询导出追加到 `Physics` 域尾部（同域，不新造域） |
| Animation | `nl/anim` 六个头齐备（clip 采样 / 状态机 / blend space / skeleton / skinning / glTF 导入），但 M-A5 只做 viewer `--mode skin` 独立演示；**无 viewer 活实例、无 ECS 动画组件**。`AnimationStateMachine::transitionTo` 是 **private**（`state_machine.hpp:81`），`TransitionRule::canTake` 是 `std::function<bool(const ParamValues&)>` | 薄面自建 C++ 句柄表；状态转移改走「C++ 侧预置规则 lambda + 脚本写标量参数」，见 §5.3。**不要求改 `nl/anim` 公开面** |

⚠️ 上一轮方案表写「显式 `transitionTo`」，实读后发现它 private。方案 ① 的**薄面形态不变**，实现口径按
§5.3(a) 落地即可（不改 `nl/anim`、不过 `std::function`、不碰渲染）。若新单认为必须给 `nl/anim`
加 public `requestTransition()`，那属于**范围外的新架构判定**，须回拍板，不许顺手加。

## 4. 冻结面与改动面

**冻结面（一行不碰）**：`renderer/**`、`.slang`、`kStage` 与 `renderer/src/renderer.cpp` 的 `kArtifacts[]`、
既有 54 exports 声明行、`example/` 双后端 PNG 基线、其它两条 ACTIVE wt（`s14-softrast` / `worldgen`）、
`script/examples/tour/` 既有示例语义。

**改动面（新单落码范围）**：

| 文件 | 动作 |
|---|---|
| `script/script-api/exports.api` | 尾部追加三域声明（约 14~18 条；条数是机械细化，非架构判定） |
| `core/include/nl/script/ApiTable.g.hpp` / `core/src/script/ApiTable.g.inc` / `ApiTable.g.asserts.inc` / `script/script-api/Wiring.g.cs` / `docs/script-api/API.md` | `python tools/scriptapi/gen.py` **重写**生成物（禁手改）；`--check` 只用于定性 |
| `core/include/nl/script/Bindings.hpp` | 新增 `audioSetWorld` / `querySetScene` 注入点 + 三域错误码常量 |
| `core/src/script/Bindings.cpp` | 三域 `extern "C"` 实现 + Animation 句柄表（新 `core/src/script/AnimInstances.hpp/.cpp`，script 内部面，**不进 `nl/anim` 公开头**） |
| `viewer/main.cpp` | 仅注入 2 处（Audio 活实例 + 静态 `QueryScene` fixture），约 3~5 行；**不碰渲染路径**；`build()` 失败注入 `nullptr` |
| `CMakeLists.txt` | 新增 `.cpp` 源登记（若新建 AnimInstances.cpp） |
| `modules/nl-script/main.cpp` | selftest 新段 **s16 / s17 / s18**（现到 s15） |
| `script/examples/audio|query|anim/` | 三个示例脚本（C# `[UnmanagedCallersOnly]` 探针，同 s14/s15 的 BoundProbe/UnboundProbe 形态） |
| `example/cases.json` + `example/m-w2a*/` | 3 张 tool 卡（usage / 正向 / 负向，各含真实 stdout/TSV 证据图）+ `run.bat` ALL GREEN |
| `docs/script-api/SCRIPT-TOOLS-GUIDE.md`（或既有 GUIDE 新节） | M-W2A 三域归档 |
| `docs/MILESTONES.md` | 收口时补四栏台账 |

## 5. 三域设计（冻结口径）

### 5.1 Audio 域（沿用 M-SC9 既定行）

- **注入**：`nl::script::audioSetWorld(nl::audio::AudioWorld*)`（Bindings.hpp，前向声明即可，`nl_core`
  不反向 include viewer）；`nullptr` = 解绑 = 热更窗口兜底。viewer 在 `--script` 装载段、
  `ecsSetWorld/timeSetClock` 同批注入 `ensureAudioWorld()` 的结果；未开脚本/未建世界 = 未绑。
- **不隐式建表**：绑定层绝不自己 `new AudioWorld`（全族口径）。
- **导出草案**（6 条，落码期读 `AudioWorld.hpp` 实码微调签名，条数非架构判定）：
  `audioPlay(str wavPath, buf3 pos, int looping, out int sourceId)` /
  `audioStop(int id)` / `audioSetGain(int id, f32 gain)` /
  `audioSetSourcePosition(int id, buf3 pos)` /
  `audioSetListenerPose(buf3 pos, buf3 dir)` /
  `audioIsPlaying(int id, out int playing)`。
- **错误码**：`0=ok / 1=未绑 AudioWorld / 2=句柄无效 / 3=createSource 或文件失败（error 文本走 out str 静态缓冲） / 4=非法参数`。
- **文件口径**：运行时只吃 WAV PCM16；ogg→wav 离线转码工具在游戏侧，引擎零改动（三游戏边界拍板）。

### 5.2 Physics 查询域（追加到既有 `Physics` 域尾部）

- **注入**：`nl::script::querySetScene(nl::physics::QueryScene*)`；viewer 持一个静态 fixture `QueryScene`
  （`build()` 一次），与 Audio 同批注入；selftest 侧自建自注入。**绑定层不隐式建**。
- **导出草案**（5 条）：`queryRaycast(...)` / `querySweep(...)` / `queryOverlap(...)` 返回命中数 /
  `queryOverlapHitAt(int index, ...)` 逐条读回 / `queryBodyCount(out int count)`。
  输入 = `buf3` 起点/方向、`f32 maxT`、`u32 layerMask`、`u32 shapeKind`、`buf3 dims`/`buf4 quat`；
  命中结果 = `out int hit` + `out buf3 point` + `out buf3 normal` + `out f32 fraction/distance`
  + `out u32 bodyId/subShapeId/layer` + `out u64 owner`。**不新增 `type` 段 POD**（避免改生成器；
  `bufN`/标量全在现有 TYPEMAP 内）。
- **多命中容器协议**：`queryOverlap` 把 `std::vector<QueryHit>` 收进 C++ 静态表（「下次同线程调用前有效」），
  `queryOverlapHitAt(index)` 按序读回，绑 `maxHits` 上限（默认 32）；沿用 `P8-SCRIPT-PLAN.md`
  「容器 = count + 静态表 + C# 立即拷」口径，不把 vector 过界。
- **错误码**：`0=ok / 1=未绑 QueryScene / 2=索引越界或表空 / 3=build 未就绪 / 4=非法参数（方向零向量、maxT<=0、dims 越界等，先过 `validateRayQuery/validateSweepQuery/validateOverlapQuery`）`。
- **边界必须写进 API.md 与 GUIDE**：这是**确定性夹具查询**，不是 viewer 现役物理世界查询；live 化属 M-PQ1。

### 5.3 Animation 域（薄面句柄表）

- **落点**：新增 `core/src/script/AnimInstances.hpp/.cpp`（script 内部私有头，不放 `nl/anim` 公开面，
  与 M-SC10 Props 描述表「C++ 只存 POD」同构）。C++ 侧两张表：clip 表 `(path,index) -> AnimClip`、
  状态机表 `id -> { AnimationStateMachine, clips, ParamValues, 规则表, 最近 pose }`，句柄 >0，
  失效安全拒绝；`animRelease*` 显式回收，热更 unload 前置空。
- **clip 入口**：`nl::anim::loadGltfAnim(path, skeleton, clips, error)`（M-A4b 既有纯逻辑入口），
  按 `clipIndex` 取一条；路径走 assets 白名单校验（`nl/scene/AssetName.hpp` 同款防路径穿越）。
- **状态转移实现口径（a，本计划采用）**：`AnimationStateMachine::transitionTo` 是 private，故**不改
  `nl/anim`**；绑定层在 `animCreateStateMachine` 时于 **C++ 侧**按脚本给的「(from,to,参数下标,阈值,混合秒数)」
  规则表构造 `TransitionRule::canTake` lambda（纯 C++ 内部，`std::function` **不过 blittable 边界**），
  脚本只经 `animSetParam(smId, idx, f32)` 写标量、`animTick(smId, dt)` 调 `update(dt, params)` 触发
  优先级转移。规则声明顺序 = 优先级（`StateMachineDesc` 既定语义）。
  **范围外**：给 `nl/anim` 加 public `requestTransition()`（改 core 公开面 = 新架构判定，须回拍板）。
- **pose 读回**：`animGetPose(smId, boneIndex, out buf7 pose)` = translation(3) + quat xyzw(4)，
  下标对齐 `Skeleton::boneNames` 拓扑序；`animPoseCount` 给骨骼数。
- **导出草案**（6~7 条）：`animLoadClip` / `animCreateStateMachine(in ptr clipIds, in int count, in ptr rules, in int ruleCount, out int smId)` /
  `animSetParam` / `animTick` / `animGetPose` / `animStateInfo(out int current, out f32 alpha, out f32 duration)` /
  `animRelease`。`ptr` 数组走 count+ptr 调用期存活协议（C# 侧 pin，同 `consoleRegister` 先例）。
- **错误码**：`0=ok / 1=句柄无效 / 2=clip 加载或校验失败（validateClip / loadGltfAnim error 文本） /
  3=reset 失败（状态机 desc 非法）/ 4=非法参数 / 5=路径白名单拒绝`。
- **不碰渲染**：不调 `skinPositions`、不进 `viewer --mode skin`、不产生任何渲染交代物。

## 6. 交付物清单（工具版交代，A 类）

1. `gen.py` 生成的五产物 + 编译期 `static_assert` 签名防漂移全绿；`gen.py --check` 内容绿。
2. `nl-script` selftest 新段 s16/s17/s18：C# 写 → C++ 逐位读回断言 + **未绑错误码负向**
   + `reload-script` 后新域重绑定无损 + 万次级串往返 + `GC.Collect` 句柄漂移断言（生命周期硬规则②）。
3. 展示台：3 张 `kind:"tool"` 卡 × (usage / 正向 / 负向 3 起步)，每卡 `expectExit` 0/1/2，
   证据图由工具**真实 stdout/TSV** 生成（`tools/make-*-evidence.py` 同款），禁手绘示意；
   `example/m-w2a*/run.bat verify` ALL GREEN。
4. `docs/script-api/` TOOLS-GUIDE 新节（三域各一节）+ `docs/script-api/API.md` 由 gen.py 自动追加。
5. `docs/MILESTONES.md` 四栏写实 + annotated tag（主线口径，不跑 `wt_pool close`）。
6. 双后端 BUILD OK（DX12 / VULKAN 各一次）、`ctest --repeat until-pass:3`、
   `regression.py --only <新版本>`、`server.py --check` exit 0（**只证明 S8.2 旧卡无回归**，
   新卡齐备靠 `run.bat verify` + `regression.py --only` 双验，M9.0 口径）。

## 7. 验收判据（放行线）

| 项 | 判据 |
|---|---|
| 目标 | 三个域都能从 C# 静态 facade 调到引擎能力，且未绑/失效路径有确定性错误码 |
| 双后端 | DX12 / VULKAN 各 BUILD OK；selftest 段全绿；ctest 全绿（带 `--repeat until-pass:3`） |
| 零回归 | 既有 54 exports 语义不变、`kStage` 不动、PNG 基线零重拍、`--check` exit 0 |
| 展示台 | 3 卡全绿 + 真实证据图 + `run.bat verify` ALL GREEN |
| 文档 | PLAN / TOOLS-GUIDE / API.md / MILESTONES 四栏 / PARALLEL-TRACKS 与 MEMORY 口径同步 |
| 收口 | 主线口径：annotated tag + 四栏 + `server.py --check`；**不建 track、不 claim、不跑 `wt_pool close`** |

## 8. 后置挂账（范围外，不是本轮待拍板项）

M-PQ1（查询接 live `DebugScene`）、M-SP1（`--sprite-demo` / PNG 基线 / 多图集每桶换 binding set /
RmlUi element+scissor 桥接）、Animation 驱动真实蒙皮与 ECS 动画组件、Audio 全量 API、
三游戏开发顺序与 citygen 库化、广州地图范围、浮字方案、NPC 行人路网归属、
`ui-rmlui` 遗留目录（别轨，未动）、两个波 1 tag 推远端。

## 9. 新单开工须知（交接）

1. 开场声明里程碑编号 **M-W2A**；按 §1b 主线不 claim，收口不用 `wt_pool close`。
2. 第一屏报「待拍板 0 / 已拍板 6 / 冻结面清单」；本文件 §0 即拍板记录，无需重摆方案，
   但按决策前置铁律**三处参考仍需定向实读**（§2 是索引，不替代实读）。
3. 落码前先 `git status --porcelain`；主仓常有别线 staged 改动，提交只用精确 pathspec，
   命令顺序 `git commit -m "..." -- <paths>`。
4. 已知坑：`gen.py --check` 绿但构建报 `nl_scriptapi_freshness` = checkout 同秒 mtime 症状，
   **跑 `gen.py` 重写**即解；改 `.slang` 才需删 `build/{DX12,VULKAN}/viewer.exe`（本轮不碰 shader）；
   双后端构建错峰单跑防 `mspdbsrv` PDB `C1090`；bat 必须 CRLF（`check-bat-encoding.py --fix`）；
   `cases.json` 手工追加用 `json.dumps(ensure_ascii=False, indent=1)+'\n'` + UTF-8 + `newline='\n'`；
   tag/commit message 避开 `ctest`/`regression` 字样（OPS-GATE R2 扫命令文本误拦）。
5. 长任务（build / ctest / regression / 全量回归）**只准 `run_in_background`**，尾部写
   `.workbuddy/bg-last.log` 锚；完成通知到达立即查结果报数，禁阻塞等待。
6. 本文件落盘时主仓 HEAD = `649bb710`（M-MOD0.2）；**本轮只写文档，未改任何代码/生成物/基线**。
