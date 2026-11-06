# 步骤路线图：S0~S14

> 15 步、8 个阶段，总周期约 3~4 个月业余时间。
> **铁律：每一步都必须有可运行的源码产出和一条能亲眼看到的验证效果，验收不过不进下一步。**每完成一步打一个 git tag + 存一张截图。
> API 底座是 **NVRHI**（跨平台：Windows DX12/Vulkan、Linux Vulkan、macOS MoltenVK）。Carrot 是裸 Vulkan，所以本项目的性质是**把 Carrot 的架构移植到 NVRHI**——参考代码当思路读，API 层全部自己写。这正是本项目区别于「照抄 Carrot 历史」的价值所在。
> 卡住的对照资料分三层：知识库章节（`docs/kb/`）→ 本地参考代码（`E:\UE_STUDY_2026\` 下 Carrot 与 bevy）→ 作者博客。

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
| S7 vis buffer | `s7/S7-PLAN.md` | P8 引擎化 | 本页下方 P8 表 |

每份详细文档的结构统一：里程碑拆分（半天~两天一个）→ 每个里程碑带 checklist、自检标准、git tag → 常见坑 → 卡住翻哪（本地代码对照）→ 底线。

**Carrot 开发过程实况**：`docs/carrot-history.md`——从本地 Carrot 仓库 1180 个提交里抽出的 Nanite×GI 时间线（带 hash，可 `git show` 看实况 diff），逐行对齐 S1~S13，含「实况才看得到」的三条教训。

## 步骤 → 本地代码对照总表

| # | 本地参考文件（E:\UE_STUDY_2026\ 下） | 对应博客/资料 |
|---|---|---|
| S0 | `Carrot/engine/engine/vulkan/`（Vulkan 封装风格参考，别照抄体量） | vulkan-tutorial |
| S1 | `bevy/crates/bevy_pbr/src/meshlet/from_mesh.rs`（离线构建入口）、`meshlet/asset.rs`（资产结构） | JMS55 博客 0.14 篇 |
| S2 | `Carrot/engine/engine/render/ClusterManager.cpp`（聚组+简化+重切全套） | Carrot《Recreating Nanite: The Plan》 |
| S3 | `bevy/.../meshlet/cull_bvh.wesl`（bevy 用两级 BVH 而非 DAG——先想清楚要哪种） | JMS55 博客 0.15 篇（讲为什么弃 DAG） |
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

---

## 阶段 P0 · 渲染地基（约 1~2 周）

### S0 搭骨架
- **做什么**：接入后端，窗口 + glTF 加载 + ImGui 面板 + Slang 编译管线跑通
- **工程骨架（底子，一次搭对）**：

```
nl-engine/
├── CMakeLists.txt          ← 顶层：三个 target（core/renderer/viewer）+ nanite-tools 预留
├── core/                   ← 数学(GLM 封装)/日志/断言/窗口+输入/ImGui —— 引擎壳未来的家
├── renderer/               ← 唯一允许调 NVRHI 的模块（纪律所在）
├── viewer/                 ← main.cpp + --mode 分发，薄壳，不含图形调用
└── nanite-tools/           ← S1 起填
```

- **底子边界（S0 要的）**：窗口+输入+主循环（update/render 粗分工）、日志+断言、数学库、renderer 模块边界（S0 只包到 NVRHI 设备/swapchain 初始化）
- **底子边界（S0 明确不要的）**：ECS/场景图、事件总线、层系统、序列化、资产管理、反射、脚本——这些是 P8 引擎化的事，现在写就是范围失控
- **可运行产出**：`viewer` 打开窗口，画出带贴图的 Suzanne，相机可拖动，ImGui 可调参数
- **验收效果**：画面正确、可交互；开 NVRHI 验证层 **零报错**；一份 .slang 编译出 SPIR-V（和 DXIL）
- **涉及知识**：RHI 抽象（NVRHI）、Slang→SPIR-V/DXIL 编译、同步模型（NVRHI fence / frame in flight）、GLM、glTF 结构
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

## 最终交付清单

- 离线工具 `nanite-tools`（cluster/simplify/dag/stat 四个子命令 + `.nmesh` 资产格式）
- 运行时 `viewer`（10+ 个可开关模式，S0 普通渲染到 S13 完整 Nanite+Lumen 随时对照）
- 一个装满 git tag 和验收截图的仓库——**这个仓库本身就是最好的简历**

## 节奏提醒

S1~S3（离线）和 S10~S12（GI）是两个最容易陷进去的坑，各自预留缓冲；任何一步卡住超过三天，回 kb/index.html 的踩坑清单（08 章）或去翻对应开源实现源码，不要闭门造车。

## 验收心法

每一阶段都要能「单独关掉」这一层并看到画面退化。如果一个功能你没法关掉验证，说明它和别的层耦合太深，后面的调试会非常痛苦。
