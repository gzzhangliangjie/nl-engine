# 方案（定稿）

> 结论先行：**裸 Vulkan 1.3 + Slang，参照 Carrot 的架构自己写一份。**
> 学习路线参考 Carrot、代码自己写一份——S0~S14 的每一步产出就是自己的实现，Carrot 是教材不是底座。

## 一、最终技术决策（四条要点）

1. **「参考 Carrot」= 抄思路不抄代码**：先读它某模块怎么组织（ClusterManager / VisibilityBuffer / AS 管理），合上书自己默写，写不出来再回去看——逐行复制等于白学，理解后重写才算产出。
2. **Slang 与 Carrot 同款**，shader 侧无缝对照；不习惯也可以 HLSL + DXC，随你。
3. **本机 N 卡的正确用法**：Nsight Graphics 对 Vulkan 程序逐 draw 抓帧调试（这比选什么框架重要得多）；NVRHI/RTXDI/RTXGI 源码当「答案册」，卡壳超过三天再精读，平时不碰。
4. **渲染器模块纪律**：图形调用不散落到业务代码。哪天目标从学习转向产出，把模块换成 NVRHI 或 Diligent，Nanite/Lumen 业务代码不动。

## 二、框架选型（已定稿）

Nanite+Lumen 对框架有 4 条硬性检查项：**compute shader、indirect draw、64 位 buffer 原子操作、硬件光追查询（ray query）**。

| 框架 | 结论 |
|---|---|
| **裸 Vulkan 1.3 + Slang** | ✅ **主推（定稿）**。与 Carrot 逐行对照零翻译；barrier/同步/内存亲手写本身就是 S0~S8 的课程内容；vulkan-tutorial / vkguide 全部直接适用 |
| wgpu（Rust） | ✅ 备选 B：想跟 bevy 参考代码同语言同 API 时用；RT 为实验特性；WGSL 无 64 位标量原子（vis buffer 要改双 32 位 CAS） |
| NVRHI（NVIDIA） | 📚 **参考答案库，不是底座**：RTXDI/RTXGI/RTXMG 全建其上（MIT），卡壳超三天再读；Meshlet pipeline 原生支持但学习目标下不启用 |
| Diligent Engine | ⬇️ 降为备选：多后端优势在本场景用不上；只有「以后确定要 macOS/移动端商用」再回头考虑 |
| The Forge | ⚠️ 当资料读可以（可见性缓冲 2.0 + Wolfgang Engel 博客），当底座不建议（GI 中间件闭源、Windows 移除 Vulkan） |
| Falcor | ⚠️ 研究原型框架，替你做得太多，与「手撸」目标冲突；当 GI 算法参考书 |
| bgfx | ❌ 出局：无 64 位原子、无 ray query，硬伤不可绕 |

**为什么 Slang**：一份 shader 编译到 SPIR-V / DXIL / Metal LIB，天然跨后端；Carrot 的 shader 全是 .slang（`Carrot/engine/resources/shaders/`），语法对 HLSL 用户几乎零成本。

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
