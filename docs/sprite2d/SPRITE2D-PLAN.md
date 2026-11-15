# M-SP0 — 2D 精灵批渲染（波 1 / B 类穿插轨道）

> 轨道 `sprite2d` / wt `E:/UE_STUDY_2026/nl-engine-sprite2d` / 分支 `feat/sprite2d`
> 立项 2026-11-15，用户拍板 `A A ①`：**2D 精灵簇 = A 严格切分**、轨道 = 两轨。
> 追加拍板（同日）：用户复核「只在界面上做是否 A′ 更省」后**仍定 A 原案**，
> 理由「不然 Brotato 没法做了」；最终指令「**按照可以落地 Brotato 的目标来**」
> → 本里程碑以「世界空间 2D 战场能画」为唯一目标，**界面桥接后置**。
> 本文件是冻结口径，无待定架构判定。

## 1. 目标与边界

给游戏②（Brotato 俯视波次 roguelite）一条独立 2D 绘制路径：几百个敌人/子弹/道具，
正交相机、共享 quad、按纹理分桶合批、atlas 帧动画。
目标是「波 1 结束时渲染底座可编译、可驱动、双后端 PSO 可建立」。

### 明确不做（范围外，不是待定项）

- **RmlUi 自定义 element / scissor 桥接**（后置到 M-SP1，用户拍板「按可以落地 Brotato 的目标来」）
- 2D 骨骼动画（cutout / 网格变形）——已两次拍板先不做
- ECS Sprite 组件 / render graph phase（bevy 形态，超出最小面）
- 精灵碰撞、文本渲染、UI 属性动效（RmlUi RCSS transition/@keyframes 承担）
- `viewer/main.cpp` 的 demo 旗标与 PNG 基线（波 0 闸，见 §5）
- `exports.api` 任何改动（波 2；**已立项 M-W2A**，见 `docs/script-api/WAVE2-PLAN.md`）

## 2. 三处证据（本轮定向实读，2026-11-15）

| 来源 | 实读文件 | 结论 |
|---|---|---|
| Carrot | `engine/engine/render/` + `resources/shaders/` | `Sprite` 直接持 `Texture::Ref`/Pipeline 写进 3D GBuffer，**无批处理**；同时公开头暴露图形类型 → 违反本项目 core 零图形依赖，是 B 方案原型，不取 |
| bevy | `crates/bevy_sprite_render/`（+ `bevy_pbr/meshlet`、`bevy_solari/realtime`） | 三要素可抄思路：**共享 quad + 实例缓冲 + 按纹理分桶**；无 flipbook、无动画状态机 |
| UE 5.8.4（只读） | `Plugins/2D/Paper2D`（非 Runtime）`UPaperFlipbookComponent` | FrameRun 时间轴、循环/反向/完成事件、播放速率 → flipbook 播放器口径 |

**综合口径**：渲染结构取 bevy 的三要素（但只做到 textureId 分桶的 CPU 侧提交，不引入 ECS
抽取链）；帧动画语义取 Paper2D 的 FrameRun；分层纪律按本项目铁律——
**core 只放纯 POD，全部图形符号留在 `renderer/`**。

## 3. 冻结结构

### 3.1 core（`core/include/nl/sprite/`，零图形符号）

| 文件 | 内容 |
|---|---|
| `atlas.hpp` | `AtlasGrid{dimX,dimY,addressing}` → `uvRect(index)`；越界 `Throw/Repeat/Clamp`（POD + 纯函数） |
| `flipbook.hpp` | `Clip{fps, vector<FrameRun{frame,repeat}>}`、`sample(t)→frame`、Play/Pause/Stop、loop/reverse、playRate、OnFinished |
| `sprite_desc.hpp` | `SpriteDesc{pos2D,rot,scale,uvRect,colorRGBA,textureId,layer}` 纯 POD；`SpriteBatch::sortByTexture()` 稳定排序 + 分桶区间 |

### 3.2 renderer（唯一图形符号）

| 文件 | 内容 |
|---|---|
| `renderer/shaders/sprite2d.slang` | 一份单源 → DXIL + SPIR-V；入口 `vsMain`/`psMain`；`register(b0/t0/s0)`，**禁 `[[vk::binding]]`**；实例数据构 quad，PS 采 atlas；直通 alpha |
| `renderer/src/sprite2d.hpp/.cpp` | `Sprite2DPass` 六件套（`init/ensurePipeline/invalidatePipeline/build/draw/shutdown`）；**无 VB/IB/input layout**——沿用 M-PT0 粒子已双后端验证的 `SV_VertexID`（6 顶点）+ `SV_InstanceID` 读 `StructuredBuffer`；`SpriteGpu` 64 B / `Sprite2DConstants` 16 B 带 `static_assert`；alpha blend + 深度关 |
| `renderer/include/nl/renderer/renderer.hpp` | `Sprite2DFrame` POD（`bool enabled=false` 打头，DEFAULT-OFF 纪律）；`Renderer::renderFrame` 新增尾参并带默认值 → **viewer 零改动** |
| `backend_dx12.cpp` / `backend_vulkan.cpp` | 各持一个 `Sprite2DPass` 成员，惰性 init（仅 `enabled` 帧建资源/PSO）；DX12 传 `"dxil"`、Vulkan 传 `"spv"`，入口名统一 `"main"`（particles 先例 §2.4）；`build()` 在 `beginFrame`、`draw()` 在 RmlUi 之后 / HUD 之前 |
| `renderer.cpp` | `kArtifacts[]` 追加 `sprite2d.vs/.ps` × 双目标 4 项（漏登 = selftest 假绿） |

**CMake 零改动**：`sprite2d.slang` 用默认入口名，`file(GLOB *.slang)` + `else()` 自动接管；
新增 `.cpp` 要登记进 `nl_renderer` 源列表。

## 4. 关键技术口径

- **按 textureId 分桶**：稳定排序（layer → textureId → sequence）后同一纹理连续，
  `SpriteBatch::buckets()` 产出桶区间；`draw()` 保留提交顺序（Brotato 需要确定遮挡），
  不做跨桶重排。
- **M-SP0 GPU 侧单次 draw（实施收敛，见 §8）**：CPU 侧分桶完整保留并由工具卡验收，
  但 GPU 提交为整批一次 `draw(6, spriteCount)`。原因是
  `DrawArguments::setStartInstanceLocation()` 在本仓无使用先例，且 DX12 的
  `SV_InstanceID` 不含 `StartInstanceLocation`、Vulkan 的 `gl_InstanceIndex` 含
  `firstInstance`——这是一条静默的双后端分歧路径，而 M-SP0 又没有 GPU demo 可验证它。
  每桶换 binding set 的 draw 与多图集注册一并后置到 **M-SP1**（与 demo/PNG 基线同批）。
- **正交投影**：复用 HUD 同款 4-float scale/offset，Y 取负；
  `Sprite2DFrame` 携带 `displayWidth/Height`（像素）、`cameraX/Y` 与 `pixelsPerUnit`。
- **双后端不对称不碰**：沿用 HUD/RmlUi 已验证的 `writeBuffer`（普通 VB/IB，Vulkan 禁 volatile
  非 CB buffer）、`nvrhi::Rect(minX,maxX,minY,maxY)`、Vulkan `setEntryName("main")`。
- **缺省关零漂移**：`Sprite2DFrame::enabled=false` 时 pass 不初始化、不建 PSO、不进绘制顺序，
  历史 PNG 基线逐字节不变；`kStage` 保持 `S8.2` 不 bump。

## 5. 冻结面（波 0 硬闸，已解锁）

`input` 轨曾活跃占用 `viewer/main.cpp` + `exports.api` 五产物 + `Bindings.cpp` +
`Wiring.g.cs` + `API.md`。**2026-11-15 波 0 闸已解锁**：input 轨已 merge 回 master
（`8f76591`），两条波 1 wt 均已 `git merge --ff-only master` 快进到 `28d3cda`，
零文件重叠、本地未提交改动保留。

但**范围不因此自动扩张**：`--sprite-demo` 旗标、**渲染截图**双后端 PNG 基线、
RmlUi 视觉卡仍按本冻结口径后置到 **M-SP1**，是否提前拉进 M-SP0
需用户重新拍板。`Renderer::renderFrame` 用带默认值的尾参，viewer 两处调用点
零改动。本里程碑的「可见用例」= A 类工具版：`example/sprite2d/` 下的
`run.bat` + 4 张由真实 TSV/日志渲染的证据图 + `cases.json` 展示台 tool 卡
（`example/sprite2d/` 因此是**工具卡目录**，不是视觉卡目录）。

## 6. 验收判据（本里程碑）

- `sprite-tools selftest`：atlas uvRect 边界三模式、flipbook 时间轴（loop/reverse/
  pause/stop/finished）、分桶稳定性与覆盖性，实测 log 锚 `SELFTEST PASS (4/4)`。
- 三组 TSV 真实回读断言（ctest 各 2 条）：
  `atlas-trace|atlas-check`、`flipbook-trace|flipbook-check`、`batch-trace|batch-check`；
  外加 `selftest bogus` / `atlas-trace --out` 两条负例（exit 2）。
- 双后端 BUILD OK（shader 四产物进 `kArtifacts[]`，由 `shaderArtifactsSelfTest()` 自检，
  实测总数 170 → **174/174**，`sprite2d.vs/.ps` 四个双目标产物落盘）。注意 selftest
  输出里硬编码的 `108 artifacts` 是既有陈旧文本，**本轮不动**（改它会波及 expectLog 锚）。
- `gen.py --check` 不适用（未碰绑定面）；`kStage` 不 bump。
- **工具版展示台卡已新增**（`cases.json` 版本 `sprite2d`，4 条 `kind:tool`，
  非渲染截图卡）：`example/sprite2d/run.bat verify` ALL GREEN +
  `regression.py --only sprite2d` 双后端冒烟；渲染侧「缺省关零漂移」另挑
  2~3 个既有基线用例验证（`--only <version[/case]>`）。

## 7. 交付物清单

`core/include/nl/sprite/{atlas,flipbook,sprite_desc}.hpp`、
`renderer/shaders/sprite2d.slang`、`renderer/src/sprite2d.{hpp,cpp}`、
`renderer/include/nl/renderer/renderer.hpp`（`Sprite2DFrame` + 尾参默认值）、
`renderer/src/backend.hpp`（纯虚签名）、
`renderer/src/{backend_dx12,backend_vulkan,renderer}.cpp`（接线 + `kArtifacts`）、
根 `CMakeLists.txt`（`nl_renderer` 源登记 + `add_subdirectory(sprite-tools)`）、
`sprite-tools/{main.cpp,CMakeLists.txt}`（7 条正向 + 2 条负向 ctest）、
`example/sprite2d/`（run.bat + 真实 TSV/日志 + 4 张证据图）、
`tools/make-sp0-evidence.py`、`example/cases.json`（`sprite2d` 工具卡 4 用例）、
`docs/sprite2d/SPRITE2D-PLAN.md` + `SPRITE2D-TOOLS-GUIDE.md`、
`docs/MILESTONES.md` 四栏、tag `sprite2d-sp0`。

**后置到 M-SP1（2026-11-15 已全部交付，用户拍板「全四子项一次做 / kStage 不 bump」；
实况见 §9 与 MILESTONES M-SP1 四栏）**：`--sprite-demo` 旗标、**渲染截图**双后端 PNG
基线、多图集注册 + 每桶换 binding set 的 draw、RmlUi element/scissor 桥接。

## 8. 实施记录（2026-11-15，不构成新拍板）

1. **波 0 解锁**：master `8f76591` 合入 input 轨，`28d3cda` 登记其 wt 拆除。
   两条 wt 从 `6170cd6` 快进，`sprite2d` 到 `0169335` 后再到 `28d3cda`；
   与本地未提交文件零重叠。
2. **mtime 闸**：`phys-query` 首次 DX12 构建 `rc=1` 报 `scriptapi freshness`，
   实为 checkout 同秒 mtime 误判；`gen.py --check` 显示 54 exports 内容全绿，
   `gen.py` 重写生成物后重建 `rc=0`（`SELFTEST PASS 16/16`）。非代码缺陷。
3. **入口名**：`ShaderDesc::setEntryName` 两后端统一 `"main"`（SPIR-V 恒 main，
   NVRHI 忽略 DXIL entryName，particles.slang 先例 §2.4），故 `init()` 不再接
   `vsEntry/psEntry` 参数。
4. **纹理上传**：内置 4×4 atlas 用 `CopyDest` 初态 + 首帧 `draw()` 内
   `writeTexture` + `setTextureState(ShaderResource)`（M-PT2 dummy 先例），
   不用独立 command list——DX12 的 `frameCmd_` 常驻（M8.1），init 期额外
   `executeCommandList` 不在冻结语义内。
5. **惰性 init**：两后端只在 `Sprite2DFrame::enabled=true` 的帧才建 shader/CB/纹理/PSO；
   缺省路径连 GPU 资源都不创建，历史 PNG 基线零漂移。resize 与 Vulkan swapchain
   重建两处都调 `invalidatePipeline()`；shutdown 清单同步进 `sprite2d_.shutdown()`。
6. **CMake**：`sprite2d.slang` 命中 `_ENTRIES_DEFAULT`（`vsMain|vs` + `psMain|ps`），
   `file(GLOB *.slang)` 自动收集；`build-m0.2.bat` 每次都跑 `cmake -S -B`，
   故新 shader 无需手工重新 configure。新增 cpp 必须显式登记进 `nl_renderer`。

## 9. M-SP1 实施记录（2026-11-15，用户拍板「两个都做 M-SP1 先 / 全四子项一次做 / kStage 不 bump」）

**逐桶 draw 的跨后端解法（核心设计）**：不用 `setStartInstanceLocation`（DX12
SV_InstanceID 不含 firstInstance、Vulkan gl_InstanceIndex 含——M-SP0 注记的静默
分歧），改为**每桶写 volatile CB 带 `g_baseIndex`**（CB 16B→32B），VS 索引
`g_sprites[iid + g_baseIndex]`——SV_InstanceID 在两后端都从 0 起，一份 shader
双后端逐语义一致。volatile CB `maxVersions` 提到 64（每桶一版本，桶数超限合并告警）。

**预乘 alpha 统一**：atlas 像素 ingest 时、sprite tint build 时 CPU 侧转预乘，
blend 改 One/InvSrcAlpha——与 rmlui_backend 同语义，M-SP1-d 纹理共享零转换。
直通 alpha 语义废止（M-SP0 的 blend 为 SrcAlpha/InvSrcAlpha）。

**多图集注册**：`Sprite2DFrame` 尾部追加 `atlases/atlasCount`（POD 数组，帧内
幂等重推）；`Sprite2DPass` 内 `unordered_map<TextureId, AtlasEntry>`（tex + set +
pending），纹理 `CopyDest` 建、pending 像素在 `draw()` 帧列表内上传晋升
（builtin M-PT2 先例，帧内禁第二 cmd list）。textureId 0 / 未注册 id /
kInvalidTextureId 全部落内置 4×4 兜底。textureId 语义 = SpriteDesc 原语义。

**RmlUi 桥接（M-SP1-d）**：`Renderer::rmlRegisterExternalTexture(key, tex)` 把
sprite atlas 纹理挂进 RmlUi texture 账本（key = `kRmlSpriteTexKeyBase + textureId`，
0 保留白像素兜底）；自定义 `<sprite>` element（`ElementInstancerGeneric`，OnRender
内 CompileGeometry→RenderGeometry→ReleaseGeometry 每帧零残留）+ demo.rml sprite
卡（`overflow:hidden` 走既有 SetScissorRegion 通道）。`renderer.hpp` 零 NVRHI
include 纪律保持：ITexture 只全局层前向声明（**前向声明必须放 nl::renderer 外
——放里面会变成嵌套 namespace nl::renderer::nvrhi 污染全部子命名空间查找，
r2 构建实证**）。

**M-SP0 投影语义实测澄清（demo 踩坑）**：`ndc.y = (y + cameraY)*sy + 1`（sy<0），
world 原点锚屏幕**顶**、y 正方向屏幕**向下**——与注释「y up on screen」直觉相反。
demo 用 `cameraX = -W/(2·ppu)`、`cameraY = +H/(2·ppu)` 把原点重定位到屏幕中心；
投影公式本体（M-SP0 冻结）一行未动。

**验收**：双后端 BUILD OK；ctest 176/176 ×2（DX12 首跑 `s1-nmesh-suzanne` 快进程
stdout 丢失竞态 3 连败——nanite-tools 零改动、手动跑锚齐、VULKAN 同条过、热身后
稳定过，重跑 176/176 定案，与本轮无关）；`--only sprite-m1` 4/4 新卡 + `--only
m0.5` 10/10 / `--only water-t3` 4/4 / `--only ecs-e1b` 8/8 旧卡零漂；run.bat
三路（usage=2 / normal=0 / scene=0）；`server.py --check` exit 0；bat 门禁 0
violations。cases.json `sprite-m1` 卡（2 条 image，164→165 版本）。
