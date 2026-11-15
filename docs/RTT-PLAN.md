# RTT 后视镜（M-RT0）开发计划

> 立项：2026-11-16（用户「后视镜做吧」+「按默认」）。归属主线 master，按 AGENTS §1b
> 不建 track / 不 claim。本文件 = 开工闸拍板记录，拍板后落第一行码。

## 0. 拍板记录（用户 2026-11-16「按默认」，五项全取推荐）

| # | 架构项 | 拍板结论 |
|---|---|---|
| Q1 | 镜像相机 | **A 独立后向相机（真镜像）**——从现役相机 yaw+180 派生 viewProj，不是主相机水平翻转 |
| Q2 | 写入 pass | **A 复用主 pass draw 逻辑换相机+RT**——Scene 承载独立 mirror pass，画 sceneBatches_ 到半分辨率 RT |
| Q3 | RT 格式/分辨率 | **A 半分辨率**（格式落码期勘误 RGBA8→RGBA32F：scene PSO 锁 RGBA32F+D32，同格式复用零新管线，半分辨率仍省 3/4 像素；见 §4） |
| Q4 | 消费端显示 | **A 直接 quad blit 到屏幕角落**——右下角小 viewport 贴纹理，不动 RmlUi / sprite2d 语义 |
| Q5 | kStage | **A bump** 到 `S8.3`（渲染里程碑惯例）；旗标 `--mirror` 缺省关 → 旧基线零影响，bump 重拍面按 `reshot-hud-stage.py` 实测锁 |

方向拍板 = 方案②（真镜像 viewport），推翻 2026-11-10 camera 轨「后视镜不做」的候选池结论——
那是当时的排期判断，非永久禁令；本轮用户明确要做，属新里程碑，历史留痕保留不抹。

## 1. 三处参考 + 本项目历史（开工闸定向实读，只读不抄）

| 来源 | 证据 | 取用 |
|---|---|---|
| UE 5.8.4 | `Components/SceneCaptureComponent2D.h:80` `TextureTarget` + `:301 UpdateSceneCaptureContents` | 独立后向相机渲到纹理 = Q1 工业终态形态 |
| bevy | `bevy_render/src/view/mod.rs` `ViewTarget`/`ManualTextureView`、`camera.rs` 多相机 target | 相机→RT 抽象，双缓冲可选（本轮单 RT 起步） |
| Carrot | `engine/render/RenderGraph.cpp` / `RenderPass.cpp` | pass 图编排；本轮不引图，沿用 Scene 现成 draw 段 |
| 本项目 T0 | `SHADER-CONVENTIONS.md:285` `hdrSceneColor_` 状态表；`scene.cpp:3470 ensureRtTargets` | 离屏 RT 创建/懒建/逐帧 RenderTarget↔ShaderResource 状态跟踪现成范式 |
| 本项目 compare | `scene.cpp:4401 drawCompareImage` + `rt.pscompare` | 角落贴片 pass 现成范式（vsRtPresent 复用） |

## 2. 现状勘误（开工闸实测）

- **通用 render-to-texture 早已存在**：T0 的 `rtTarget_`(RGBA32F) → `hdrSceneColor_` → blit → tonemap → swapchain，双后端在码（`scene.cpp:2863..2920`）。本轮**不重建 RT 基建**，只加一个消费端（镜面相机）与角落贴片。
- **exports 72 条零 RT 导出**：本轮不碰 exports / Bindings（A 类渲染能力，不进脚本绑定面；若后续游戏层要调再立绑定里程碑）。
- **kStage 重拍面实测**：`cases.json` 615 用例里 `exe=current` = **0**，历史卡全走 tag 快照 exe。`reshot-hud-stage.py` 自动枚举 `--hud` + exe=current 用例——本轮新增 mirror 卡用新 exe tag，旧卡零影响；bump 后按脚本实测锁，不预判「1026 全重拍」（该数字已被开工闸勘误）。

## 3. 冻结面（本轮不碰）

- `renderer/shaders/model.slang` / `rt.slang` 既有入口语义；不新增 shift 偏移。
- T0 post-chain（blit/tonemap/bloom/godrays）既有 pass 顺序与 bytes。
- 既有 72 条 exports、`Bindings.cpp`、`Wiring.g.cs`、`API.md`。
- sprite2d / RmlUi 既有语义（Q4 选直接 blit，不接这两个）。
- 两条 ACTIVE wt（`s14-softrast` / `worldgen`）。

## 4. 改动面（逐文件）

| 文件 | 改动 |
|---|---|
| `renderer/include/nl/renderer/renderer.hpp` | `FrameParams` 加 `bool mirrorEnable` + `float mirrorMvp[16]`（缺省关/零，不读即无副作用） |
| `renderer/src/scene.hpp` | mirror RT/fb/CB/PSO/set 成员 + `initMirror`/`drawMirror`/`presentMirror`/`ensureMirrorTargets`/`shutdownMirror` 声明 |
| `renderer/src/scene.cpp` | 独立半分辨率 RGBA8 RT + D32 depth + fb；复用 `vsScene_/psScene_` 建 PSO（换 RT 格式）；`drawMirror` = 写 mirrorMvp 到独立 CB → 画 sceneBatches_；`presentMirror` = 角落 viewport quad blit；进 `shutdown()` 清单（S15 UAF 铁律） |
| `renderer/shaders/` | 不改既有入口；mirror 呈现复用 `rt.vsrtpresent` + 新增 `rt.psmirror`（texture sample + 可选水平翻转） |
| `renderer/src/renderer.cpp` | `kStage` → S8.3；kArtifacts 登记 `rt.psmirror` 双目标 |
| `renderer/src/backend_dx12.cpp` / `backend_vulkan.cpp` | init/ensure/invalidate/draw/shutdown 五段接线；仅 `frame.mirrorEnable` 时执行 |
| `viewer/main.cpp` | `--mirror` flag 缺省关；后向相机 viewProj（yaw+180）填 `fp.mirrorMvp`；两个 renderFrame 调用点一致 |
| `tools/` + `example/mirror-rtt/` | 证据图脚本 + run.bat + cases.json 新卡（双后端 PNG 基线） |
| `docs/MILESTONES.md` / `docs/RTT-TOOLS-GUIDE.md` | 四栏 + 工具指南 |

## 5. 验收判据

- 双后端 BUILD OK；selftest 16/16；ctest 双后端全绿。
- `--mirror` 开：mirror RT 真实后向画面（log 锚 `mirror pass: ...`），右下角贴片肉眼可见；DX12/VULKAN 基线 PNG 各自落盘。
- `--mirror` 关：全套旧用例逐字节不变（`regression --only <旧版本>` 抽查 + `showcase --check` rc=0）。
- `example/mirror-rtt/run.bat` verify 全绿；`regression --only mirror-rtt` 双后端 ALL GREEN。

## 6. 后置挂账（范围外）

- Audio/Camera 脚本绑定面暴露 mirror（需 exports 改动，另立里程碑）。
- 多相机 target / 画中画 multi-view（bevy ViewTarget 双缓冲形态）。
- 后视镜曲率畸变（真凸面镜 UV 变形）——本轮平面镜。
