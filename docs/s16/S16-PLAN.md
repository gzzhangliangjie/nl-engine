# S16 详细开发路线图：TSR（Temporal Super Resolution，时域超分）

> 对应总路线 `docs/ROADMAP.md` 的新增研究轨道 S16。**拆分：velocity/history 基建在 S7 之后先建（喂 S12 时空滤波复用 motion vector），完整算法在 S7 之后轻量直光场景隔离验证，最终集成 slot 仍在 tonemap 前（Lumen 之后主帧）**。预计 5 个里程碑、业余时间 2~2.5 周。
> 目标：在 tonemap 之前的 HDR scene color 上做时域上采样，替代 TAA，把渲染分辨率降到 50~66% 仍接近全分辨率画质。全程 compute pass，完美契合本项目 Slang 单源双后端。

---

## 三处参考口径（2026-11-08 按铁律定向读）

- **UE5（唯一权威，只读不抄）**：`Engine/Source/Runtime/Renderer/Private/PostProcess/TemporalSuperResolution.cpp`
  - 关键 pass：`HistoryUpdate`（历史更新/重投影）、`RejectHistory`（拒绝掩码/去鬼影）、`ShadingRejection` 启发式（`r.TSR.ShadingRejection.*`）。
  - 历史元数据编码（`r.TSR.History.R11G11B10` 省带宽）、输入分辨率 `r.TSR.History.ScreenPercentage`。
- **bevy**：无 TSR 等价实现（仅概念近似 TAA）。
- **Carrot**：无 TSR 实现。

> 以 UE 为唯一 read-only 对照；bevy/Carrot 仅作否决检查。UE 代码一行不进 repo。

## 前置（需新建/已具备，拆分：velocity/history 基建 S7 后先建喂 S12，完整算法 S7 后轻量场景验证，最终 slot tonemap 前，不依赖 Lumen）
- 需新建：**velocity / 运动向量 GBuffer**（当前 MVP 用 volatile CB 已有；写上一帧 clip pos 即可算）。这是 TSR 唯一新 GBuffer。
- 已具备：depth/normal GBuffer（S7 vis buffer 或 S9 GBuffer）、HDR scene color（tonemap 前）。
- 与 S12 时空滤波共享 motion vector 概念，但 S12 是 GI 降噪、TSR 是上采样，互不替代。

## M16.0 velocity pass + 半分辨率渲染 + 朴素双线性（2~3 天）
- [ ] 新增 velocity GBuffer：当前 clip pos vs 上一帧 clip pos（用已有 volatile MVP CB）。
- [ ] scene color 降到 50% 分辨率渲染。
- [ ] 双线性上采样到全分辨率（对照基线）。
- [ ] velocity 当颜色可视化，确认移动物体正确。

**自检**：半分辨率双线性图内容正确但模糊；velocity 可视化正确（轮廓/移动方向对）。tag：`s16-vel`。

## M16.1 历史重投影（2 天）
- [ ] 上一帧 upscaled 结果存为 history 纹理（ping-pong）。
- [ ] 用 velocity 把 history 重投影到当前帧。
- [ ] 相机静止时结果时域稳定（不抖）。

**自检**：静止场景 TSR 输出与全分辨率参考逐像素接近、无时间抖动；相机切场景时重置 history（无残留鬼影）。tag：`s16-reproj`。

## M16.2 联合双边空间放大（2~3 天）
- [ ] 用 depth+normal 引导的 5×5 联合双边滤波替代双线性。

**自检**：边缘比双线性更锐、shimmer 更少；与全分辨率参考误差下降。tag：`s16-bilateral`。

## M16.3 邻域钳制（APG）+ 拒绝掩码 + dilate（2~3 天）
- [ ] 邻域钳制抑制重影拖尾（对位 UE `RejectHistory` / `ShadingRejection` 思路）。
- [ ] 深度/法线不连续处弃用 history（disocclusion），dilate 好样本。

**自检**：移动物体不拉尾迹；物体露出背景处不 smear；关掉钳制/拒绝立即回退（退化可单独验）。tag：`s16-reject`。

## M16.4 锐化 + 最终混合 + 双后端（2 天）
- [ ] 锐化补偿联合双边带来的模糊（对位 UE 锐化系数）。
- [ ] 输出写 history，接入主帧（tonemap 前）。
- [ ] DX12 / Vulkan 同图，分档 bytes/near。

**自检**：锐度接近全分辨率参考、客观误差（PSNR vs 全分辨率基线）可接受；`regression.py` 全绿；kStage bump 后重拍 m0.9/s2.0 HUD 基线。tag：`s16-done`。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M16.0 velocity+半分辨率 | 2~3 天 | `s16-vel` |
| M16.1 历史重投影 | 2 天 | `s16-reproj` |
| M16.2 联合双边 | 2~3 天 | `s16-bilateral` |
| M16.3 钳制+拒绝 | 2~3 天 | `s16-reject` |
| M16.4 锐化+双后端 | 2 天 | `s16-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| 历史更新/重投影 pass | UE `PostProcess/TemporalSuperResolution.cpp`（`HistoryUpdate`） |
| 拒绝掩码/去鬼影 | UE 同上（`RejectHistory` / `ShadingRejection` 启发式） |
| 历史编码/分辨率 | UE 同上（`r.TSR.History.*` cvar） |
| motion vector 概念 | S12 时空滤波（共享），但 TSR 在 tonemap 前、S12 在 GI 后 |

## 底线
- TSR 在 **tonemap 之前、Lumen GI 之后** 插入主帧（确认 `renderer/src/scene.cpp` 该 slot 存在，沿用渲染约定）。
- velocity 精度是第一大病源（世界/屏幕空间、上一帧矩阵、轮廓精度）；M16.0 可视化验收不可省。
- 「看着还行」是主观的：M16.0 起建全分辨率参考 + 客观误差指标，接 `regression.py`。
- 每次 kStage bump 重拍 m0.9/s2.0 HUD 基线（M3.2 固化④）；UE 代码一行不进 repo。
