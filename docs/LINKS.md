# 学习链接

> 按用途分类。标 ⭐ 的优先看。本地离线手册在 `docs/kb/`（浏览器直接打开，含打卡进度）。

## 1. 目标画面（先建立预期）

| 资源 | 链接 | 什么时候用 |
|---|---|---|
| ⭐ 《Lumen in the Land of Nanite》UE 官方宣传片 | https://www.youtube.com/watch?v=qC5KtatMcUw | 开工前看一遍，知道终点长什么样 |
| nanite-webgpu 在线交互 demo | https://scthe.github.io/nanite-webgpu/ （Chrome） | 玩每个滑块开关：软光栅/LOD 阈值/剔除逐项开关，直观感受各模块权重 |
| three-virtual-geometry demo | https://elad12390.github.io/three-virtual-geometry/ | 另一套浏览器 demo，调 errorThreshold |

## 2. 必读架构资料（S0 之前啃完）

| 资源 | 链接 | 什么时候用 |
|---|---|---|
| ⭐ Karis《Nanite: A Deep Dive》SIGGRAPH 2021 | https://advances.realtimerendering.com/s2021/ （Karis Nanite PDF） | 所有开源实现的共同蓝本，cluster DAG / 误差 / 两遍光栅全在这 |
| GDC 2021《The Technology Behind Lumen in the Land of Nanite》 | GDC Vault 搜标题 | Lumen screen/surface cache 架构 |
| ⭐ JMS55 博客（Bevy 0.14/0.15 虚拟几何 + Solari） | https://jms55.github.io/ | 最完整的「我踩过的坑」实录，S1~S5 配套 |
| ⭐ jglrxavpok《Recreating Nanite》系列 + GI 长文 | https://blog.jglrxavpok.eu/ | 唯一双撸编年史，S2/S7/S8/S10/S11 配套 |
| Scthe 软光栅系列博文 | https://scthe.github.io/ | S14 软光栅时的 WGSL 踩坑记录 |
| Understanding Nanite（Deep Dive 解读） | 搜标题 | 觉得 slides 太密时先看这个 |

## 3. 本地参考代码（已在 E:\UE_STUDY_2026\）

| 仓库 | 链接 | 关键目录 |
|---|---|---|
| ⭐ Carrot（C++/Vulkan，双撸完成） | https://github.com/jglrxavpok/Carrot | `engine/engine/render/`、`engine/resources/shaders/`（.slang GI 全家桶） |
| ⭐ bevy（meshlet + solari） | https://github.com/bevyengine/bevy | `crates/bevy_pbr/src/meshlet/`、`crates/bevy_solari/src/realtime/` |
| meshoptimizer | https://github.com/zeux/meshoptimizer | `src/meshlet.cpp`（S1）、`demo/nanite.cpp`（分层 LOD 工具链） |
| Danite（mesh shader 学生项目，METIS 用法参考） | https://github.com/CDDing/Danite | S2 METIS 用法 |
| Wbiu 虚拟几何渲染器（入门友好） | https://github.com/Wbiu/Custom-Virtualized-Geometry-Renderer | 演示视频 youtu.be/D8U2XRY2Bt8 |
| yuphin/Lumen（ReSTIR GI / DDGI 现成实现） | https://github.com/yuphin/Lumen | S10~S12 算法对照 |

## 4. NVIDIA「答案册」（卡壳超三天再翻，全部 MIT）

| 仓库 | 链接 | 对应步骤 |
|---|---|---|
| RTXDI（ReSTIR 直接光） | https://github.com/NVIDIA-RTX/RTXDI | S14 ReSTIR |
| RTXGI（DDGI/表面缓存） | https://github.com/NVIDIA-RTX/RTXGI | S11 世界缓存 |
| **RTXMG**（簇式几何加速 BVH——Nanite×RT 接口层官方答案） | https://github.com/NVIDIA-RTX/RTXMG | S8/S13 |
| NRD（降噪器库：ReBLUR/SIGMA/ReLAX） | https://github.com/NVIDIA-RTX/NRD | S12 滤波 |
| NVRHI | https://github.com/NVIDIA-RTX/NVRHI | 换底座备选；RBDOOM-3-BFG 是其最完整用例 |
| Falcor（Slang 研究框架） | https://github.com/NVIDIAGameWorks/Falcor | GI 算法实验/论文代码宿主 |
| RBDOOM-3-BFG（NVRHI 真 AAA 用例） | https://github.com/RobertBeckebans/RBDOOM-3-BFG | 工程组织参考 |

## 5. 工具链

| 工具 | 链接 |
|---|---|
| Slang（shader 语言+编译器） | https://shader-slang.org / https://github.com/shader-slang/slang |
| Vulkan 规范 / Vulkan-Headers（**只查资料，不装 SDK**） | https://docs.vulkan.org/ · https://github.com/KhronosGroup/Vulkan-Headers |
| vulkan-tutorial.com | https://vulkan-tutorial.com/ |
| vkguide.dev | https://vkguide.dev/ |
| Nsight Graphics（本机 N 卡抓帧） | NVIDIA 官网下载 |
| meshoptimizer（工具链依赖） | https://github.com/zeux/meshoptimizer |

## 6. 基础补课（对应 kb/graphics-handbook.html 的 G0~G8）

| 资源 | 链接 | 什么时候用 |
|---|---|---|
| LearnOpenGL（中文版） | https://learnopengl-cn.github.io/ | G2~G6 的主力教程 |
| GAMES101 闫令琪（B 站搜「GAMES101」） | B 站官方账号 | 系统过一遍图形学 + 数学 |
| GAMES202 高质量实时渲染 | B 站搜「GAMES202」 | 阴影/GI/降噪，S6/S10/S12 之前看 |
| Ray Tracing in One Weekend | https://www.realtimerendering.com/raytracing/ | S9 离线参考模式直通车 |
| RTR4《Real-Time Rendering 4th》 | 图书 | 当词典查，不通读 |
| PBRT（在线免费） | https://www.pbr-book.org/ | BRDF/采样理论卡住时查 |

## 7. UE 官方（路线 B 用）

| 资源 | 链接 |
|---|---|
| UE 官方文档 Nanite / Lumen | https://docs.unrealengine.com/ 搜 Nanite / Lumen |
| SIGGRAPH Advances 合集（历年） | https://advances.realtimerendering.com/ |

> 注意：GitHub 链接在本机浏览器可能直连打不开（网络原因），源码已拉到 `E:\UE_STUDY_2026\`，链接主要当路径索引用。
