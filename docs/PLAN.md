# 方案（定稿）

> 结论先行：**NVRHI + Slang，参照 Carrot 的架构自己写一份。**
> 学习路线参考 Carrot、代码自己写一份——S0~S14 的每一步产出就是自己的实现，Carrot 是教材不是底座。
> 本机 N 卡 + N 家同源（NVRHI 与 Slang 同为 NVIDIA 出品，slangc 产物直接喂 NVRHI）；自动 barrier 可开关；Meshlet pipeline 原生支持；RTXDI/RTXGI/RTXMG 生态直接可用。

## 一、最终技术决策（四条要点）

1. **「参考 Carrot」是两件事，别混为一谈**：**搭建节奏**参考它从零起步的实况（`docs/s0/CARROT-BOOTSTRAP.md`：Init → 第二天出画面 → 抽象被重复逼出来 → 验证层第一天在），仅作参考、不做逐行对齐，他早期换底座 / 晚拆模块 / 相机硬搓等返工明确不复刻；**具体代码**才是「抄思路不抄代码」——先读它某模块怎么组织（ClusterManager / VisibilityBuffer / AS 管理），合上书自己默写，写不出来再回去看。Carrot 是裸 Vulkan，但 NVRHI 的 Vulkan 后端与之概念一一对应（barrier → NVRHI 自动跟踪，其余直接平移）。
2. **Slang 与 Carrot 同款**，shader 侧无缝对照；slangc 产物直接被 NVRHI 加载，零胶水。
3. **答案册**：RTXDI/RTXGI/RTXMG 源码（就跑在 NVRHI 上）——卡壳超过三天再精读，平时不碰。
4. **本机 N 卡**：N 家工具（Nsight Graphics / RTX 生态）按需取；**帧结构调试主用 RenderDoc**（同时覆盖 DX12 / Vulkan 双后端），Nsight / PIX 退到时间线佐证。工具全在进程外，不进构建链；四层计划见 `docs/s0/S0-PLAN.md`「可观测性与调试工具计划」。

## 二、框架选型（已定稿）

Nanite+Lumen 对框架有 4 条硬性检查项：**compute shader、indirect draw、64 位 buffer 原子操作、硬件光追查询（ray query）**。

| 框架 | 结论 |
|---|---|
| **NVRHI + Slang** | ✅ **主推（定稿，跨平台底座）**：Windows（DX12/Vulkan）+ Linux（Vulkan）+ macOS（MoltenVK，光栅化可用、硬光追不可用）；自动 barrier 可开关；Meshlet pipeline 原生支持；RTXDI/RTXGI/RTXMG 同栈直接可跑；与 Slang 同为 N 家，字节码直喂零胶水 |
| 裸 Vulkan 1.3（补课选项） | ⬇️ 备选：关键 pass 关掉 NVRHI 自动 barrier 手写对照，补同步/内存功底用；不作为主路线 |
| Diligent Engine | ⬇️ 降为备选：多后端优势在本场景用不上；只有「以后确定要 macOS/移动端商用」再回头考虑 |
| The Forge | ⚠️ 当资料读可以（可见性缓冲 2.0 + Wolfgang Engel 博客），当底座不建议（GI 中间件闭源、Windows 移除 Vulkan） |
| Falcor | ⚠️ 研究原型框架，替你做得太多，与「手撸」目标冲突；当 GI 算法参考书 |
| bgfx | ❌ 出局：无 64 位原子、无 ray query，硬伤不可绕 |

**为什么 Slang**：一份 shader 编译到 SPIR-V / DXIL / Metal LIB，天然跨后端；Carrot 的 shader 全是 .slang（`Carrot/engine/resources/shaders/`），语法对 HLSL 用户几乎零成本。

**后端对称（架构纪律，2026-11-06 定稿）**：既然是通用管线，构建链就**不依赖任何厂商图形 SDK**。D3D12 / OpenGL 的头、库、调试层来自 Windows SDK（系统自带）；Vulkan 头走 `KhronosGroup/Vulkan-Headers`（NVRHI 的 `NVRHI_FETCH_VULKAN_HEADERS` 自动拉），loader 是显卡驱动自带；Slang 独立获取，不从 `$VULKAN_SDK` 找。默认 DX12（Windows 零额外依赖），Vulkan 作为第二后端验证抽象；判据「调试层零报错」按当前后端走，不把 VVL 写成全项目前提。详细对照见 `docs/s0/S0-PLAN.md` M0.0「后端对称」表。

## 三、工程组织

- 只维护两个程序：`nanite-tools`（cluster / simplify / dag / stat）+ `viewer`（10+ 可开关模式）
- 资产格式 `.nmesh` 第一次就定好**带版本号**，后面加字段不破坏旧文件
- viewer 每一步加一个 `--mode`，旧模式永远保留当对照（退化可单独验证）

## 四、生态实战检阅（2026-11 查证）

用 NVRHI / Slang 造出来的开源项目：

- **RBDOOM-3-BFG** — NVRHI 的最完整真实游戏样本（前 AAA 引擎整体移植，PBR+GI+IBL+TAA）；作者选型时对比过 Diligent/bgfx 后选 NVRHI
- **NVIDIA RTX SDK 全家**（RTXDI/RTXGI/NRD/**RTXMG**）— 官方示例全跑 NVRHI+Donut；RTXMG 是「簇式几何加速 BVH 构建」的官方答案，与 S8 接口层直接对口
- **RTX Remix 运行时** — Portal with RTX 的底座，路径追踪管线工程活标本
- **Falcor** — Slang 官方参考实现之家（ReSTIR PT / RTXDI / NRD 集成）
- **Valve Source 2** — Slang 生产级最硬背书（非开源）
- **独立引擎一批**（Violet / prosper / Lune）— 均为裸 Vulkan + Slang，与本方案同栈

如实说：**「NVRHI+Slang 捆绑」无标杆项目**——两个生态平行壮大。本方案（裸 Vulkan + Slang）恰是社区验证最多、且 Carrot 可逐行对照的那条线。

## 五、配给体系

三级学习体系（拷贝在 `docs/kb/`，浏览器直接打开）：

```
graphics-handbook.html   地基：G0~G8，从零到实时渲染
        ↓ 毕业
index.html               专项：Nanite + Lumen 手撸（P0~P7 + 术语字典 + 踩坑清单）
        ↓ 落地
unity-handbook.html      宿主：Unity SRP 里做出来（U0~U8）
```

进度打卡在各 HTML 内，存浏览器 localStorage。
