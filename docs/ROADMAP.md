# 步骤路线图：S0~S14

> 15 步、8 个阶段，总周期约 3~4 个月业余时间。
> **铁律：每一步都必须有可运行的源码产出和一条能亲眼看到的验证效果，验收不过不进下一步。**每完成一步：本地提交一波 + 打一个 git tag + 存一张截图 + 在 `docs/MILESTONES.md` 追加一条（动作 / 提交版本 / 完成情况 / 踩坑情况，写实不写空话）。
> API 底座是 **NVRHI**（跨平台：Windows DX12/Vulkan、Linux Vulkan、macOS MoltenVK）。Carrot 是裸 Vulkan，所以本项目是**把 Carrot 的架构移植到 NVRHI**——API 层全部自己写，搭建**节奏**参考 Carrot 从零起步的实况（见 `docs/s0/CARROT-BOOTSTRAP.md`：Init → 第二天出画面 → 抽象被重复逼出来 → 验证层天天在）。**仅作参考、不做逐行对齐，他早期的返工不必复刻**。区别于「照抄 Carrot 成品」的地方在这里。**构建链不依赖任何厂商图形 SDK**：D3D12 / OpenGL 走 Windows SDK、Vulkan 头走 `KhronosGroup/Vulkan-Headers`、Slang 独立获取；默认 DX12，Vulkan 为第二后端（见 `s0/S0-PLAN.md` M0.0 的后端对称表）。
> 卡住的对照资料分三层：知识库章节（`docs/kb/`）→ 本地参考代码（`E:\UE_STUDY_2026\` 下 Carrot 与 bevy）→ 作者博客。

## 决策前置铁律（Agent 协作门禁，2026-11-07 固化 → 完整权威落点见仓库根 `AGENTS.md`）

> **每开始一轮新任务，必须先同时读过 Carrot / bevy / UE5 官方 三处参考代码 + 本项目开发历史（`MILESTONES.md` / `carrot-history.md` / `.workbuddy/memory/MEMORY.md` / 当日日志 / 当前 `S*-PLAN.md`），带着证据做决断。任何超出「已定计划内机械执行」的判定（架构 / 范围 / 选型 / 约定 / 与历史决策冲突 / 三处口径不一致需二选一），不许自己拍板——先摆三处证据 + 历史约定，再请用户定。** 违反即视为失职，用户可随时追责。当前步骤对应哪几份文件见下方「步骤 → 本地代码对照总表」。

## 详细开发文档索引（每步一份，按里程碑拆到可直接照着敲的粒度）

| 步骤 | 文档 | 步骤 | 文档 |
|---|---|---|---|
| S0 骨架 | `s0/S0-PLAN.md` | S8 BLAS/TLAS | `s8/S8-PLAN.md` |
| S1 切簇 | `s1/S1-PLAN.md` | S9 直光+参考图 | `s9/S9-PLAN.md` |
| S2 QEM 简化 | `s2/S2-PLAN.md` | S10 屏幕探针 | `s10/S10-PLAN.md` |
| S3 DAG | `s3/S3-PLAN.md` | S11 世界缓存 | `s11/S11-PLAN.md` |
| S4 剔除+indirect | `s4/S4-PLAN.md` | S12 时空滤波 | `s12/S12-PLAN.md` |
| S5 DAG 遍历 | `s5/S5-PLAN.md` | S13 接缝聚合 | `s13/S13-PLAN.md` |
| S6 Hi-Z | `s6/S6-PLAN.md` | S14 增强（四选一） | `s14/S14-PLAN.md` |
| S15 VSM（虚拟阴影贴图）⤴**插入 S9 后** | `s15/S15-PLAN.md` | S16 TSR（时域超分）⤴**基建 S7 后 / 算法 S7 后轻量验证** | `s16/S16-PLAN.md` |

> **2026-11-08 新增研究轨道说明**：用户拍板「前三个研究模块」（VSM / GPU-Driven 间接绘制骨架 / TSR）中，**GPU-Driven 间接绘制骨架已 = 本表 S4**（硬件光栅+GPU视锥剔除+indirect draw，原文"从本步起进入 GPU-driven……这是后面 S5/S6 的骨架"），**不重复建**；VSM、TSR 为真正新增，排 S15/S16。编号保留 S15/S16 不变以避免重排 S0~S14 引发连锁改动；**2026-11-08 用户已拍板提前插入主线**：VSM 插入 **S9 之后**（作阴影阶段 rasterized 对照路线，合并进 `shadow-map` 穿插项，依赖 S4+S7/S9 在 S9 已全就绪）；TSR **拆为两段**——velocity/history 基建在 **S7 之后**先建（喂给 S12 时空滤波复用 motion vector），完整算法在 **S7 之后**轻量直光场景隔离验证，最终集成 slot 仍在 tonemap 前（Lumen 之后主帧）。
| S7 vis buffer | `s7/S7-PLAN.md` | P8 引擎化 | 本页下方 P8 表 |

每份详细文档的结构统一：里程碑拆分（半天~两天一个）→ **阶段目标 + 能亲眼看到的用例** → 验收判据 → checklist / git tag → 常见坑 → 卡住翻哪 → 底线。**验收通过才放行下一阶段，tag 在验收之后打**（S0 门禁表见 `s0/S0-PLAN.md` 开头）。

## 里程碑「交代」规范（2026-11-07 固化）

> 铁律里「能亲眼看到的验证效果」**不限于可运行 exe 的截图**——每个里程碑都必须有交代物，按版本类型分两路，全部挂展示台网页（`tools/showcase/`，数据源 `example/cases.json`）：

| 版本类型 | 交代物 | cases.json 用例类型 | 总闸覆盖 |
|---|---|---|---|
| **渲染版本**（改 viewer / shader） | `example/<版本>/` 基线 PNG + `run.bat`，网页上看图、现场重跑比对基线 | `image` / `pair` / `selftest` | PNG×基线逐字节比对（bytes / near / FAIL） |
| **工具版本**（nanite-tools 等不产 GPU 截图） | 展示台工具卡：点「运行工具」现场实跑、**输出直接展开显示**；**一张从工具真实输出生成的证据图**（`preview` 静态图，见下方约定）；另归档一份 `docs/s*/…-TOOLS-GUIDE.md`（usage + 实跑输出 + 负向路径与退出码） | `tool` | 文本用例：`expectExit` + `expectLog` 校验 |

**工具用例（`kind:"tool"`）约定**：
- `args` 只允许来自 cases.json 清单（服务端不执行任意命令）；引用仓库内文件用 `{root}` 占位；可变参数走 `params` 白名单（`type:"select"` 只许枚举值，防路径穿越）。
- **退出码语义**：`0` 成功 / `1` 用法错误或运行失败 / `2` 已声明未实现（防静默假成功）。用例用 `expectExit` 声明预期退出码，`expectLog` 声明输出必须包含的行；两者由展示台 `/api/run` 与 `tools/regression.py` **同一套语义**校验。
- 每个工具版本至少三条用例：**一条正向**（核心功能实跑输出）、**一条 usage/总览**、**一条负向**（非法输入或未实现路径的显式退出码）。
- **补图要求（2026-11-07 用户追问「有图么」后固化）**：工具版本不产 GPU 截图，但**必须有一张肉眼可验的证据图**，且必须由工具的**真实输出**生成，不许手绘示意、不许拿别的版本截图凑数。做法：工具提供几何/数据导出（如 `simplify --obj`），本地离线渲染脚本（如 `tools/qem_preview.py`）出 PNG → 放 `example/<版本>/` → 用例上写 `"preview": "example/<版本>/<图>.png"`，展示台只读经 `/shots/` 白名单（`server.py declared_shots()` 收集 `preview`）显示在卡片上；同目录放 `run.bat` 一键复现。首个样板：`example/s2-qem/qem-suzanne.png`（左右同机位同尺度的 QEM 简化前后线框，红点=锁定边界点）。静态图不进总闸比对，回归仍只校验文本输出。
- 工具版本不动 kStage（渲染阶段号只在渲染版本前进时更新）；`server.py --check` 依旧按 kStage 校验渲染用例归位，工具版本的交代挂在展示台自己的版本卡上。
- **工具卡 exe 恒指当前构建**（`build/{dx12,vulkan}/nanite-tools.exe`，2026-11-07 定案）：工具用例无截图基线可比、每次现场实跑，当前 exe 输出即权威；`rebuild.py` 只负责渲染 viewer 卡的历史快照（exe 映射含 `build/showcase/{tag}/` 的版本卡），不为工具 exe 建历史快照。
- 纯工具版本的「渲染零影响」证明 = 全量回归 ALL GREEN（历史基线逐字节不动）。

**收口回复格式（2026-11-09 用户点名扩充，与 AGENTS.md 里程碑交付纪律 7/9 条同口径）**：
- 每段工作收口，回复里三项**缺一不可**：①**提交版本**（commit 短哈希 + tag，写实不许漏）；②**修改文件清单**（按类别列出改动的每个文件 + 一句话说明，`git show --stat` 核对——不许只报「N 个文件 +N 行」的数量）；③**固化内容**（写进了哪些 canonical 文档/台账/注册表，各补/改了什么）。
- 收口前**四项自查**逐项过了才许宣布收口 / 打 tag：① 渲染交代物（基线肉眼核验 + run.bat + cases.json + `server.py --check` 退出 0）② 工具交代物（无产出时写明「本轮无工具交代物」）③ `MILESTONES.md` 四栏台账 ④ 固化文档。收口回复里逐项报告状态，让用户一眼可验。
- 背景：M6.0 收口（2026-11-09）三项全漏（example 卡、台账、文件清单），tag 都打了才被用户两次点名补——清单没打勾就宣布收口视为失职。

**Carrot 开发过程实况（两份，别搞混）**：
- `docs/s0/CARROT-BOOTSTRAP.md`——**2020-11 起从空仓库到能看画面的前 21 天**，节奏仅作参考、不做逐行对齐；第三节明确列了他早期不复刻的弯路（换底座、晚拆模块、相机硬搓、resize 修 4 次）。
- `docs/carrot-history.md`——2023 年之后的 Nanite×GI 时间线（带 hash，可 `git show` 看实况 diff），逐行对齐 S1~S13，含「实况才看得到」的三条教训。

**UE 源码使用规矩（2026-11-07 定；完整源码在 `E:/UnrealEngine/5.8.4/`，Nanite/Lumen 模块齐全）**：
- **只读、不抄**：UE 的 LICENSE（EULA）与本项目 MIT 不兼容，**任何 UE 代码不许复制进 nl-engine 仓库**——许可证污染。参考的是「结构和边界情况怎么想全」，一行代码都不进 repo；这比 Carrot 的「参考节奏不复刻弯路」更严一档。
- **定向读，不漫游**：UE Renderer 上万文件，90% 与本项目无关。只读步骤对照表里点名的那几个文件（S5 `NaniteCullRaster.cpp`、S10 `LumenScreenProbeGather*`，路径前缀 `Engine/Source/Runtime/Renderer/Private/`），每次打开前先带着具体问题。
- **分工**：Carrot = 从零搭建的**节奏**（S0~S1）；bevy = meshlet **数据结构与 shader**（S1~S4）；**UE = 工业终态的满分答卷**——S4~S13 每步卡壳时翻它，看边界情况（接缝、crack、fallback、异步构建预算）是怎么处理全的，这正是我们验收判据最容易漏的部分。穿插项研究项目（zauonlok / Brioche 等）与 UE 互不替代：一个管算法可读性，一个管工业完备性。

## 步骤 → 本地代码对照总表

| # | 本地参考文件（E:\UE_STUDY_2026\ 下） | 对应博客/资料 |
|---|---|---|
| S0 | ⭐ `docs/s0/CARROT-BOOTSTRAP.md`：Carrot 从零搭建实况 `46799759` Init → `152edabe` 三角形（第二天）→ `d11508e3` staging 抽象 → `d25a1cc9` 模型；本地 `Carrot/thirdparty/` 的 glfw / glm / imgui / tinygltf / stb 源码可直接复用（零网络） | vulkan-tutorial；NVRHI / Donut 官方示例（gh-proxy 拉） |
| S1 | `bevy/crates/bevy_pbr/src/meshlet/from_mesh.rs`（离线构建入口）、`meshlet/asset.rs`（资产结构） | JMS55 博客 0.14 篇 |
| S2 | `Carrot/engine/engine/render/ClusterManager.cpp`（聚组+简化+重切全套） | Carrot《Recreating Nanite: The Plan》 |
| S3 | `bevy/.../meshlet/cull_bvh.wesl`（bevy main 现状：**DAG 管 LOD 层级、BVH 管 GPU 遍历剔除**，两者配合不是替代；博客期 DAG 一直保留） | JMS55 博客 [0.14](https://jms55.github.io/posts/2024-06-09-virtual-geometry-bevy-0-14/) / [0.15](https://jms55.github.io/posts/2024-11-14-virtual-geometry-bevy-0-15/) / [0.16](https://jms55.github.io/posts/2025-03-27-virtual-geometry-bevy-0-16/) + `docs/blogs/jms55-bevy-meshlet.md` |
| S4 | `bevy/.../meshlet/cull_instances.wesl → cull_clusters.wesl → fill_counts.wesl`（三级剔除链，照抄结构） | — |
| S5 | `bevy/.../meshlet/cull_bvh.wesl`；UE `NaniteCullRaster.cpp`（已拉源码里） | Karis《Nanite: A Deep Dive》 |
| S6 | `Carrot/engine/resources/shaders/post-process/visibility-buffer-debug/` | Carrot《The Plan》 |
| S7 | `Carrot/.../render/VisibilityBuffer.cpp`；`bevy/.../meshlet/visibility_buffer_hardware_raster.wesl`、`visibility_buffer_resolve.wesl`、`material_shade_nodes.rs` | Carrot《A basic material pass》 |
| S8 | `Carrot/.../render/raytracing/AccelerationStructure.cpp`、`RaytracingScene.cpp`（工业级 AS 管理+聚合，本项目最值钱的文件之一） | Carrot《Recreating Nanite: Raytracing》；卡壳超三天再翻 NVIDIA **RTXMG**（簇式几何 BVH 官方实现，MIT） |
| S9 | `Carrot/.../shaders/lighting/direct-raytracing.slang`（阴影射线直光） | — |
| S10 | `Carrot/.../lighting/gi/read-screen-probes.slang`（final gather）；`bevy_solari/src/realtime/prepare.rs` + `initial_path.wesl` | Carrot GI 长文；UE `LumenScreenProbeGather*` |
| S11 | `Carrot/.../lighting/gi/update-cells.slang`（世界缓存 cell 更新）；`bevy_solari/.../world_cache_update.wesl`、`world_cache_query.wesl`、`world_cache_compact.wesl` | Carrot GI 长文 |
| S12 | `Carrot/.../lighting/svgf.slang`（现成 SVGF 实现）+ `temporal-denoise.compute.glsl` | — |
| S13 | `Carrot/engine/resources/pipelines/compute/compute-rt-clusters.json`（逐簇 BLAS 聚合管线定义） | Carrot《Raytracing》（此题唯一详解） |
| S14 | `bevy_solari/.../restir.wesl` vs `no_restir.wesl`（两条路线对照）；`bevy/.../meshlet/visibility_buffer_software_raster.wesl`（软光栅） | Scthe 软光栅系列博文 |
| S15 | `UE5 .../VirtualShadowMaps/VirtualShadowMapArray.cpp`（`FVirtualShadowMapArray`、物理页池 `PhysicalPagePool`/`MaxPhysicalPages`）+ `VirtualShadowMapClipmap.cpp`（级联 clipmap）+ `VirtualShadowMapProjection.cpp`（OnePassProjection 投影采样）+ `VirtualShadowMapCacheManager.cpp`（页请求/缓存） | bevy/Carrot 无 VSM 等价实现（待开工按铁律二次确认）；**插入位置：S9 之后** |
| S16 | `UE5 .../PostProcess/TemporalSuperResolution.cpp`（`HistoryUpdate` 历史重投影 / `RejectHistory` 拒绝掩码 / `ShadingRejection` 启发式 + 历史元数据 R11G11B10 编码） | bevy/Carrot 无 TSR 等价实现（待开工按铁律二次确认）；**拆分：velocity/history 基建 S7 后先建喂 S12，完整算法 S7 后轻量验证，最终 slot tonemap 前** |

---

## 阶段 P0 · 渲染地基（约 1~2 周）

### S0 搭骨架
> 当前进度（2026-11-07）：**S0 全部完成**——M0.5 总验收通过（tag `s0-done`），能力补齐序列 M0.6 GPU 计时 / M0.7 高模资产 / M0.8 调试着色 ✅，下一步 **M0.9 回归总闸 → 进 S1**。逐阶段实况见 `docs/MILESTONES.md`，可回放用例见 `example/`，调试工具计划见下方专节。

- **做什么**：接入后端，窗口 + glTF 加载 + ImGui 面板 + Slang 编译管线跑通
- **工程骨架（底子，一次搭对）**：

```
nl-engine/
├── CMakeLists.txt          ← 顶层：三个 target（core/renderer/viewer）+ nanite-tools 预留
├── core/                   ← 日志/断言/窗口+输入/轨道相机（M0.4 手写 4×4，未引 GLM）/ImGui —— 引擎壳未来的家
├── renderer/               ← 唯一允许调 NVRHI 的模块（纪律所在）
├── viewer/                 ← main.cpp + --mode 分发，薄壳，不含图形调用
└── nanite-tools/           ← S1 起填
```

- **底子边界（S0 要的）**：窗口+输入+主循环（update/render 粗分工）、日志+断言、数学库、renderer 模块边界（S0 只包到 NVRHI 设备/swapchain 初始化）
- **底子边界（S0 明确不要的）**：ECS/场景图、事件总线、层系统、序列化、资产管理、反射、脚本——这些是 P8 引擎化的事，现在写就是范围失控
- **可运行产出**：`viewer` 打开窗口，画出带贴图的 Suzanne，相机可拖动，ImGui 可调参数
- **验收效果**：画面正确、可交互；**DX12 / Vulkan 两个后端**的调试层都 **零报错**；一份 .slang 同时编译出 SPIR-V 与 DXIL 且两后端都能加载（通用管线的验收核心）
- **涉及知识**：RHI 抽象（NVRHI）、Slang→SPIR-V/DXIL 编译、同步模型（NVRHI fence / frame in flight）、4×4 矩阵与轨道相机（M0.4 现为 `core/camera.cpp` 手写，GLM 未引入）、glTF 结构（tinygltf）
- **卡住翻哪**：NVRHI / Donut 官方示例；kb/graphics-handbook.html#path 的 G2

## 阶段 P1 · 离线管线（Nanite 数据侧，约 2~3 周）

### S1 切簇
- **做什么**：meshlet 构建，128 tri/簇，算包围球与初始误差
- **产出**：`nanite-tools cluster model.gltf` → `.nmesh`；`stat` 打印簇数/平均簇大小；`viewer --mode cluster` 按簇随机着色
- **验收**：每簇三角数 ≤128 且近似均匀；调试图簇边界清晰、无长条畸形簇
- **知识**：meshlet 构建、包围球（Ritter）、初始误差度量（边界边投影）
- **翻哪**：kb/index.html#nanite（01 章）；meshoptimizer src/meshlet.cpp

### S2 聚组 + 组内 QEM 简化 + 重切
- **产出**：`nanite-tools simplify`；`viewer --mode lod N` 手动切换第 N 层渲染
- **验收**：每层三角数约减半；**任意相邻层拼接无裂缝**（边界锁定生效的直接证据）
- **知识**：METIS 图分割、QEM 二次误差、边界顶点锁定、重索引
- **翻哪**：kb/index.html#algo（04 章）；Danite 仓库（METIS 用法）

### S3 构建 cluster DAG + fallback mesh
- **产出**：`nanite-tools dag` → DAG 写回 `.nmesh`；`viewer --mode dag-top` 只渲染 DAG 最顶层
- **验收**：顶层渲染结果 = 一个正确粗模；DAG 遍历能从根走到任意叶
- **知识**：图合并、误差自底向上传播（父=max 子）、合并后重分裂
- **翻哪**：01 章；JMS55 博客 0.14 篇；Carrot《The Plan》

## 阶段 P2 · 运行时光栅（GPU-driven，约 3~4 周）

### S4 硬件光栅 + GPU 视锥剔除 + indirect draw
- **产出**：`viewer --mode cull`：全簇常驻显存，compute 剔除写 indirect 命令，一次 ExecuteIndirect 绘全部可见簇；HUD 显示剔除统计
- **验收**：相机背对/移出视锥时 HUD 绘制簇数骤降；1M+ 三角场景稳定 60fps
- **知识**：GPU-driven 管线、indirect 命令、持久剔除、pipeline barrier
- **翻哪**：01 章；bevy meshlet culling 代码

### S5 compute 遍历 DAG，按屏幕误差选 cut
- **产出**：`viewer --mode dag`；HUD 实时显示当前总三角数
- **验收**：相机拉远三角数**连续**下降；固定视角下数字稳定不抖（时滞防抖生效）
- **知识**：世界误差→屏幕像素投影、遍历策略、LOD 抖动抑制
- **翻哪**：04 章 §4.1；UE `NaniteCullRaster.cpp`

### S6 Hi-Z 两遍遮挡剔除
- **做什么**：深度 pass → 手写 min 金字塔 → 第二遍剔除绘制
- **产出**：`viewer --mode hiz`；ImGui 一键开关 Hi-Z 对比
- **验收**：室内/被遮挡场景帧时间明显下降；关掉开关立即回退（退化可单独验证）；遮挡边缘无漏画
- **知识**：深度金字塔（compute 手写 min，**不能**用 GenerateMips）、保守盒测试、两遍光栅
- **翻哪**：04 章；kb/unity-handbook.html#pits（GenerateMips 坑同源）

## 阶段 P3 · 可见性缓冲与材质（约 1~2 周）

### S7 Visibility buffer + 材质解析 + PBR 直光
- **做什么**：64 位 atomic 编码 vis buffer → 材质 pass vertex pulling 反解 → 多材质 PBR
- **产出**：`viewer --mode visbuf`；调试图可切 cluster/三角着色
- **验收**：多材质模型着色与直接画 GBuffer 的对照组**逐像素一致**（模式开关对比截图）
- **知识**：atomicMin 位打包、vertex pulling、材质 ID 间接表、深度精度
- **翻哪**：01 章；Carrot《A basic material pass》

## 阶段 P4 · 光追地基（Lumen 前置，约 2 周）

### S8 BLAS/TLAS + ray query 最小闭环
- **产出**：`viewer --mode rt`：fallback mesh 预建 BLAS、TLAS 每帧重建；compute 里发调试射线，命中距离可视化成灰度图
- **验收**：调试射线可视化与场景深度吻合；HUD 显示 AS 构建/更新耗时
- **知识**：NVRHI RayTracing 接口（BLAS/TLAS/ray query）、AS 构建预算、队列同步
- **翻哪**：02 章；NVRHI / Donut 光追示例；nvpro ray tracing samples

### S9 GBuffer + 直接光 + 阴影射线 + 离线参考模式
- **产出**：`viewer --mode shadow`（每像素 1 条阴影射线）；`--mode reference`（每像素 64+ 采样积分「真值图」存盘）
- **验收**：硬阴影正确；参考图噪声极低——这张图是 P5 调 GI 时**唯一的裁判**
- **知识**：BRDF 与余弦采样、阴影射线、tone mapping、参考图工作流
- **翻哪**：02 章；kb/graphics-handbook.html#path 的 G8

## 阶段 P5 · Lumen 核心（约 4~6 周，最难的一段）

### S10 屏幕探针一跳 GI
- **做什么**：4 级 cascade 布点 + 八面体图 + final gather
- **产出**：`viewer --mode probe`（探针 pass + 1 条余弦采样射线/探针 + gather 插值）；`--mode probe-viz` 可视化探针辐照度
- **验收**：天光与一次弹射可见；与 S9 参考图并排对比误差趋势一致；探针辐照度方向合理
- **知识**：八面体映射、余弦重要性采样、cascade 布局参数、探针插值权重
- **翻哪**：02 章；UE `LumenScreenProbeGather*`；bevy_solari

### S11 世界缓存（surface cache）
- **做什么**：GPU hashmap + 每帧更新 + 二跳
- **产出**：`viewer --mode cache`（hash cell 存 irradiance，每帧直光射线+邻居射线更新；gather 二跳查缓存）；`--mode cache-viz` 显示 cell 占用
- **验收**：**红白棋盘颜色渗透出现**；关灯后余晖自然衰减（多跳涌现）；cell 回收无闪烁
- **知识**：GPU hashmap（线性探测+墓碑）、cell 生命周期、irradiance 编码（RGBE/SH）
- **翻哪**：04 章（含回收坑）；Carrot GI 长文

### S12 时空滤波
- **做什么**：空间联合双边 + 时间累积 + 历史拒绝
- **产出**：`viewer --mode filter`；滤波强度/历史长度 ImGui 可调；可单独关空间或时间对照
- **验收**：静止与运动都无拖影鬼影；disocclusion 处无光斑残留；与参考图对比主观可接受
- **知识**：SVGF 思想、motion vector、历史有效性（法线/深度差异阈值）
- **翻哪**：02 章；bevy_solari 降噪 pass

## 阶段 P6 · 接缝：虚拟几何 × 光追（约 1~2 周）

### S13 簇级 BLAS 聚合 + 延迟一帧回读
- **做什么**：可见簇列表回读（延迟一帧）→ 拷贝预建 per-cluster BLAS 聚合 → TLAS；光追几何与渲染几何对齐
- **产出**：`viewer --mode final`：GI 反射用的就是当前 cut 的几何
- **验收**：LOD 切换处 GI/反射**无跳变闪烁**；时间线截图证明无 CPU-GPU 同步 stall
- **知识**：AS clone/重建、readback 延迟隐藏、BLAS 内存池
- **翻哪**：kb/index.html#bridge（03 章）；Carrot《Raytracing》（此题唯一详解）

## 阶段 P7 · 打磨与增强（可选，持续）

### S14 按需挑一项
- 软光栅亚像素三角形 / ReSTIR GI / 探针置信度与自适应分辨率 / 简单场景编辑器
- **验收**：软光栅：远景闪烁消失（对比录屏）；ReSTIR：同等帧时间下参考图误差明显下降
- **翻哪**：01 章 / 06 章；Scthe 软光栅博文；`bevy_solari restir.wesl vs no_restir.wesl` 对照

---

## 阶段 P8 · 引擎化（渲染路线跑完之后，按需推进）

> 定位：**不是第二个项目，是这个项目的「包装阶段」**。viewer 里的窗口/ImGui/glTF 加载就是引擎壳的种子，core/ 目录从 S0 起就按引擎预备役组织。铁律不变：每步有可运行产出 + 验收。

| # | 做什么 | 可运行产出 | 验收效果 |
|---|---|---|---|
| E1 | ECS-lite 场景：多物体、多光源、变换层级 | viewer 加载场景文件，画 N 物体 M 灯 | 50 物体 + 8 个动态灯，GI 正确响应（被 GI 测试需求拉出来，不是拍脑袋） |
| E2 | 资产管理器 + 热重载 | .nmesh/贴图统一注册表，改动自动重载 | 改模型文件不重启生效 |
| E3 | 序列化 + Inspector | 场景存/读盘；ImGui 面板升级为属性编辑器 | 改个灯颜色/位置，存盘重开保持 |
| E4（可选） | 物理/音频/脚本 | 由最终想做的游戏类型决定取舍 | 按游戏需求定义 |

**到此为止就是完整引擎**：core（壳）+ renderer（Nanite+Lumen）+ scene + assets + editor。物理/音频/脚本要不要，等你想清楚拿它做什么游戏再说——不要为「像 UE」而堆功能。

---

## 阶段 P9 · 真实资产落地验收（主线 S0~S14 + P8 之后，按需推进）

> 定位：**研究主线的「真刀真枪」验收关，也是项目的立身之本**。P0~P8 全用 Suzanne / 球 / 合成 50 物体验证算法正确性；这一关换成**用户后续提供的真实资产（含超大场景）**，证明整条 Nanite×Lumen 管线在真实数据上跑得通。**大场景加载是这个项目的意义所在，不是可选项**——S4 百万面硬件剔除、S5 DAG 屏幕误差 LOD、S6 Hi-Z 遮挡从一开始就为大场景服务，P9 把它落到真实超大资产上。资产由用户提供，不阻塞主线进度（前 8 阶段不依赖真实资产即可验收）。

| # | 做什么 | 可运行产出 | 验收效果 |
|---|---|---|---|
| V1 | 真实资产接入：单 / 少数真实模型（多材质 / PBR 贴图 / 可能含破面·孤立顶点·法线缺失）过完整离线管线（cluster → simplify → dag → fallback） | 真实资产 `.nmesh` + `viewer` 全模式可加载 | 离线三阶段无报错退出；分层画廊无黑斑无裂缝无洞（沿用 M2.3a 验收口径）；双后端画面 bytes/near |
| V2 | 真实资产全链路渲染：剔除 / LOD / DAG 遍历 / Hi-Z / visbuf / PBR / 直光 / GI 在**超大场景**上正确 | `viewer` 各模式实跑真实超大场景 | 各模式结果与参考 / 对照组一致；HUD 统计合理；**超大场景稳定 60fps**（沿用 S4 性能基准，首版即须跑通，不以流式为前提） |
| V3 | 多物体真实场景：真实资产组合成多物体世界（承接 P8 E1） | `viewer` 加载真实多物体场景 | GI / LOD 在多物体真实场景正确响应 |
| V4 | **完整流式落地**：`.nmesh` 分页 + renderer streaming 接口由「全常驻」替换为「分页 residency」 | `viewer` 实跑超显存超大场景 | 按页加载 / 卸载，帧时间平稳，无 pop-in 爆闪；**超显存的超大场景也能流畅加载** |

**证据依据（三处来源，2026-11-08 复读确认，P9 判定非拍脑袋）**：
- **Carrot**（`engine/engine/render/ClusterManager.cpp:144-150,161,182-186`）：簇顶点**按簇自包含存储**（`vertices.resize(firstVertexIndex + meshlet.vertexCount)` 每簇独立拷贝，buffer 命名 "Virtual geometry vertex buffer %llu meshlets"）。但 Carrot **本身未实现流式**（render 目录下无任何 streaming 模块）——印证流式是公认的后期重活，不是我们偷懒。
- **bevy**（`aa626e4f0b` Per-meshlet compressed vertex data）：Objective 第一行即 "Prepare for streaming"，顶点按 meshlet 自包含 + 位流压缩；`bdaf47c72b` Zero-Day 大场景示例即本阶段 V2/V3/V4 的现成参照。
- **UE5**（`Nanite/NaniteStreamOut.h` 的 `FStreamOutMeshDataHeader` 用 `VertexBufferOffset / IndexBufferOffset / NumVertices / NumIndices` 偏移寻址；`kb/index.html:820` 点名 `NaniteStreamingManager*.cpp` = 分页与驻留策略）：工业终态的流式 = 「偏移寻址大 buffer + 独立 residency 管理器」，V4 直接对标此模型。
- 三处一致结论：**顶点自包含 + 偏移寻址 + 独立 page/residency 管理** 是流式正路；`kb/index.html:475` 警告的「全局共享顶点池焊死」三处来源无一采用。本阶段 **V1 起**就把 `.nmesh` 与离线管线按此设计，V4 不返工。

**边界（首版约定，沿用 ROADMAP 既有口径 + 用户 2026-11-08 拍板）**：
- **真实资产含超大场景（可能超显存）**：V2 起超大场景**首版即须跑通**（整模型常驻或分块加载）；完整流式（V4）在此之上进一步支持「超显存 N 倍的超大场景」，**流式非可选、非推远，是 P9 必做收尾**。
- **提前预留流式接口（用户拍板）**：离线管线与 `.nmesh` 格式设计需**分页友好**——cluster 自包含、顶点可分页组织、支持后续按簇 / 页独立加载，不为全局共享顶点池焊死结构（`kb/index.html:475` 指出的后期返工点，现在就避开）；renderer 层预留 streaming 抽象接口（asset residency / page manager），**V4 由「全常驻」替换为「分页 residency」**。
- **资产预处理**：真实资产常见脏数据（非流形 / 破面 / 孤立顶点 / 法线缺失）由离线管线 cleanup / weld / 法线继承消化（沿用 S2 简化器 `rebuildMeshlets` 法线继承口径），不另立资产清洗工具。
- **资产来源**：由用户提供，本计划不负责获取 / 制作真实资产。

**此阶段不阻塞主线**：S0~S14 与 P8 各自验收通过后才进本阶段；真实资产到位后走 V1→V4 验收，复用现有 `example/cases.json` 用例库 + `tools/regression.py` 总闸。

---

## 穿插项（2026-11-07 定：不挡主线，卡壳 / 换脑子时插）

> **M0.x 必做序列冻结在 M0.9，做完立刻进 S1**——不往 0.x 里继续加版本，也**不为渲染研究单开第二条并行线**（并行线会让 `kStage` 阶段检查、展示台 exe 映射、M0.9 回归总闸全部变成两份，互相打架）。想做的研究一律降级为「穿插项」：硬规则不变（example 归位 + cases.json + `--check` 退出 0），但插在主线任意两步之间，做完就回主线。tag 用名字（如 `pbr-direct`、`toon`），**不占用 m0.* 编号**。

| 穿插项 | 内容 | 建议插在哪 | 与主线的关系 | 详细计划 |
|---|---|---|---|---|
| PBR 直射光 + 贴图采样 | glTF 贴图进 GPU（`t0` baseColor sRGB + `t1` ORM 线性、`s0` 共用 sampler）、新开 `b1` LightConstants、Cook-Torrance GGX 直射光 + 常数环境光；走 `--shading pbr`（M0.8 的 shader 分支机制现成） | **S1~S6 之间插一次** | **S7 的前置**：S7 本来就含「PBR 直光」，提前做掉，S7 只剩 vis buffer + vertex pulling + 材质解析 | `s0/PBR-DIRECT-PLAN.md` |
| 卡通着色（ramp + 描边） | `--shading toon`：三段阶梯漫反射 + 阶梯高光 + rim；描边走背面外扩 hull（同一 render pass 第二个 draw：`CullMode::Front` + view-space 法线外扩，先画写深度），零离屏 RT | 随时；或排 P7 与 S14 同类 | 与 Nanite/Lumen 研究目标零耦合，纯展示性；成本最低（只改 shader 分支 + 多一个 draw，pipeline 组合表扩一档） | `s0/TOON-PLAN.md` |
| 光栅阴影（ShadowMap / PCF / PCSS） | 深度 pass + 阴影贴图采样 + PCF 软化 / PCSS 半影估计 | **已由 VSM（S9 后插入）升级替代**，见下方 VSM 行 | 主线 S9 走**光追阴影射线**，光栅阴影为对照路线；VSM 是其工业级升级版 | GAMES202 hw1 拆解，开工时补计划 |
| PRT 球谐环境光（静态场景） | 离线预计算 cubemap → SH 系数，运行时 SH(dot(n, wi)) 环境光 | `pbr-direct` 之后 | **PBR 穿插项「不做 IBL」缺口的廉价补法**；也可当 S9 参考图工作流的降级对照 | GAMES202 hw2 拆解，开工时补计划 |
| 屏幕空间 AO（SSAO） | 深度/法线 buffer + 半球采样（或 GTAO 近似） | `pbr-direct` 之后 | 主线 S10 GI 才补间接光，SSAO 先填直光阶段的接触暗角；PhasmaEngine 用 FidelityFX CACAO 可对照 | 开工时补计划 |
| 后处理链（tonemap / AA / bloom） | 离屏 RT + ACES tonemap → FXAA/TAA → bloom | **S7 之后**（前置 = 离屏 RT 基建，与 vis buffer 材质 pass 共享） | TAA 的 motion vector 与 S12 时间累积共享；PBR/卡通最终都要 tonemap 出片 | 开工时补计划 |
| VSM（虚拟阴影贴图） | clipmap 级联 + 物理页池 + 页表，rasterized 阴影路线（替代 S9 光追阴影射线，作对照） | **S9 之后**（合并升级原 `shadow-map` 穿插项） | S15 正式计划；依赖 S4 簇光栅 + S7/S9 GBuffer，S9 已就绪 | `s15/S15-PLAN.md` |
| TSR 基建（velocity/history） | 运动向量 GBuffer + 历史 ping-pong，喂 S12 时空滤波复用 motion vector | **S7 之后** | S16 前半段；避免 S12 重复建 motion vector 基建 | `s16/S16-PLAN.md` |
| `--profile` 按 pass 拆分 GPU 计时 | M0.6 整帧计时扩成按 pass | 视需要 | 见 `s0/S0-PLAN.md`「可观测性与调试工具计划」L2 | — |

**外部技术点归宿对照（2026-11-07 对齐，不单开新线，防重复建设）**

| 来源 | 技术点 | 归宿 |
|---|---|---|
| GAMES202 | ShadowMap / PCF / PCSS | 穿插项 `shadow-map`（上表；级联阴影 CSM 是其进阶） |
| GAMES202 | PRT / 球谐环境光 | 穿插项 `prt-sh`（上表） |
| GAMES202 | SSR | **排 S6 之后插**：Hi-Z SSR 复用 S6 的深度金字塔基建，一鱼两吃；P7/S14 扩选 |
| GAMES202 | Kulla-Conty BRDF | 并入 `pbr-direct` 进阶（能量补偿，不单开） |
| GAMES202 | SVGF / 降噪 | **主线 S12 本体**（时空滤波就是 SVGF 思想），不穿插 |
| PhasmaEngine | SSAO（FidelityFX CACAO 可对照） | 穿插项 `ssao`（上表） |
| PhasmaEngine | 级联阴影 CSM（GPU-culled cascades） | 并入 `shadow-map` 进阶 |
| PhasmaEngine | 后处理链（tonemap/AA/bloom/DoF） | 穿插项 `post-chain`（上表） |
| PhasmaEngine | TAA + RCAS upscale | 并入 `post-chain`；motion vector 与 S12 时间累积共享 |
| PhasmaEngine | Forward+ 分块光源剔除 / 渲染图 / shader 热重载 | 归 **P8 引擎化**（E1 多光源 / E2 热重载时才有意义，穿插期不做基建） |
| PhasmaEngine | 两相 temporal Hi-Z 遮挡剔除 / meshoptimizer 簇层级 / visibility buffer / MaterialX→Slang | **主线 S6 / S1~S5 / S7 本来就有**——Phasma 实证了这条路可走；MaterialX→Slang 远期不归档 |
| 作者其他项目（2026-11-07 二次调研） | PepcyCh `opengl-cs-rasterizer`（compute shader 光栅器 + Hi-Z culling） | **主线 S6 + S14 直接参考**——比 CPU 软渲更贴 GPU 路线的「compute 光栅」活样本 |
| 作者其他项目 | bisemutum Wavefront 路径追踪 / PepcyCh `cuda-path-tracer` | **S9 离线参考图模式**的对照实现 |
| 作者其他项目 | HanetakaChou `Voxel-Cone-Tracing` | GI 另一条路线，**S14 备选视角**对照 Lumen（S10/S11），不单开 |
| 作者其他项目 | Brioche SSS/LTC/SHaRC/NeRF-NRC、ANARI-USD Hydra、Console-Tutorial(PS5)、christoskaramou `RenderLens`(Godot 性能 MCP) | 与 Nanite×Lumen 主线无关，**远期不归档**；做卡通人物项目时再翻 Brioche 的 SSS/脸影 |

**边界**：穿插项不做 IBL / 阴影贴图 / 后处理链（bloom、tonemap）——PBR 补全到那一步就够 S7 用；卡通不做 SDF 脸影等进阶项，那是要真做卡通项目时另立门户的事。

**渲染研究参考项目（2026-11-07 调研；融合方式：不看它走硬走软，看它哪一层最可读）**：
- **融合判据（先定这个再看代码）**：学**算法本身**（BRDF / 阴影 / 滤波为什么这么算）→ 翻软渲（zauonlok，C89 零 API 噪音）；学**GPU 工程化**（pass 组织 / 多后端 / 同步）→ 翻硬件项目（Brioche-Puppet / bisemutum）；**落地一律过自己的关**（slangc 单源双目标 + 绑定约定 + example 回归），谁都不照抄。主线已定硬件（NVRHI），软件光栅不是路线选项——它是算法教室 + S14 备胎（Nanite 本尊就是软硬混合，S14 选「软光栅亚像素」时 zauonlok 光栅化器 + bevy `visibility_buffer_software_raster.wesl` 才升级为移植素材）。
- `zauonlok/renderer`——C89 从零软件渲染器，Blinn-Phong / PBR 双工作流 / IBL / 阴影贴图 / ACES，20+ 场景按 CLI 名选用例（与本项目 example/ 用例库同思路）。**融合点**：`pbr-direct` / `shadow-map` 的算法参照（公式逐行对拍）；用例组织已同构，不另学。
- `HanetakaChou/Brioche-Puppet`——自写渲染层 **D3D12/Vulkan/Metal 三后端**，PBR + 卡通（赛璐璐脸 + 矢状面 SDF 影）+ ReSTIR。**融合点**：多后端对称的工程组织对照；`toon` 穿插项与卡通进阶（SDF 脸影）的效果标杆。
- `PepcyCh/bisemutum-engine`——C++20 个人 RHI（Vulkan + D3D12）+ 渲染图 + PBR 前向/延迟/路径追踪/DDGI。**融合点**：RHI 双后端上再长渲染图的活样本——**P8 引擎化（渲染图）时翻**，穿插期不碰基建。
- `christoskaramou/PhasmaEngine`——Vulkan + DX12 学习引擎，**shader 用 Slang**、MaterialX→Slang、visibility buffer + meshlet 簇。**融合点**：S1~S7 各步的验证思路（归宿已并入上表对照）；Slang 的生产级用法参考。
- GAMES202 各作业仓库——一作业一个技术点，当主线技术点拆分清单用（归宿已对齐，见上表）。

---

## 最终交付清单

- 离线工具 `nanite-tools`（cluster/simplify/dag/stat 四个子命令 + `.nmesh` 资产格式）
- 运行时 `viewer`（10+ 个可开关模式，S0 普通渲染到 S13 完整 Nanite+Lumen 随时对照）
- 一个装满 git tag 和验收截图的仓库——**这个仓库本身就是最好的简历**

## 调试与性能工具链（S1~S13 共用）

> 权威明细在 `docs/s0/S0-PLAN.md` 的「可观测性与调试工具计划」，这里只记路线图层面必须知道的三件事，避免两处漂移。

| 什么时候 | 用什么 | 为什么 |
|---|---|---|
| 每一步的验收证据 | `--screenshot` GPU 回读 + `example/` 基线 + 展示台（L0） | 铁律「能亲眼看到」，已就位 |
| S4 起查 draw call / 资源状态 | **RenderDoc** 外部启动（零代码改动），需要精确帧号再加 `--capture-frame N` 的进程内 API（L1） | 它同时覆盖 DX12 / Vulkan；Nsight 偏 N 卡、PIX 只管 D3D12，都不适合当双后端通用管线的主工具 |
| S4 之前、S13 验收 | NVRHI timestamp query 填进 HUD 的 GPU 耗时曲线（L2）；S13 再用 Nsight / PresentMon / Tracy 做 CPU-GPU 时间线佐证（L3） | 当前 HUD 的 frame ms 只是 CPU 帧间隔，不是 GPU 耗时；S4 之后每一步都要能看出性能退化 |

**工具全是进程外或 NVRHI 抽象层内**：不进 CMake、不链接、不读厂商 SDK 路径，图形 API 符号仍只在 `renderer/`。本机（2026-11-07 实测）RenderDoc / Nsight / PIX 均未安装，按需再取；S13 验收里「时间线截图证明无 CPU-GPU 同步 stall」据此走 L3。

## 节奏提醒

S1~S3（离线）和 S10~S12（GI）是两个最容易陷进去的坑，各自预留缓冲；任何一步卡住超过三天，回 kb/index.html 的踩坑清单（08 章）或去翻对应开源实现源码，不要闭门造车。

## 验收心法

每一阶段都要能「单独关掉」这一层并看到画面退化。如果一个功能你没法关掉验证，说明它和别的层耦合太深，后面的调试会非常痛苦。
