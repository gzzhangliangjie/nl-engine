# 穿插项计划：卡通着色 ramp + 描边（tag：`toon`）

> 状态：**计划已预写（2026-11-07），开工时按本页执行，动手前如需调整先改这里**。
> 定位：主线「穿插项」，与 Nanite/Lumen 研究目标零耦合、纯展示性，随时可插（或排 P7 与 S14 同类）。
> 成本最低：只改 shader 分支 + 同一 render pass 里多一个 draw，pipeline 复用 M0.8 的组合表扩一档。
> 硬规则不变：example 归位 + cases.json + `--check` 退出 0；tag 用名字 `toon`，不占 m0.* 编号。

## 为什么是它（背景）

- 「多类型渲染展示」是框架的验收目标之一：同一份 `.slang` 出 SPIR-V + DXIL、双后端渲染**风格化画面**，比 PBR 更能证明管线不绑死「物理渲染」一种语义。
- M0.8 的 shading 分支（`g_options.x`）+ pipeline 组合表已把地基打好：卡通着色 = 新分支 + 描边壳 = 新 pipeline，两处都是纯增量。

## 做什么

### 1. `model.slang`：shading mode 5 = `toon`

- 阶梯漫反射：`NdotL` 经**三段阈值**量化（`0.0 / 0.5 / 0.85`，暗/中/亮三档），阈值写死 shader 常数——保证确定性（截图回归可用）。
- 阶梯高光：Blinn-Phong `pow(NdotH, spec)` 后 `step(0.5, ...)` 二值化。
- 边缘光 rim：`pow(1 - saturate(NdotV), 3)` 加进亮部。
- 光方向/颜色复用 **`b1` `LightConstants`**（与 PBR 穿插项共享同一份 cbuffer 基建；**若 toon 先开工，由 toon 建这个 b1，PBR 沿用**——谁先做谁铺，别各建一份）。**不塞进现有 `SceneConstants`**（严格 160 B + 禁整块 memcpy 的老规矩）。

### 2. 描边：同一 render pass 第二个 draw（背面外扩壳）

- 顶点外扩：view space `pos.xyz + normal * width` 再投影——**等世界宽度**，实现最简、确定性最好；等屏宽（clip space `pos.xy += normalView.xy * width * pos.w`）留作可选进阶，先不做。
- Pipeline：新增第 5 套 `outline`（`CullMode::Front`——项目剔除约定是反直觉的：CCW 外表面被判 back，描边壳要画背面壳，验收时先拿一个球确认露出来的是轮廓再上 Suzanne）。
- 绘制顺序：**outline 先画并写深度，正常面后画盖掉内侧**——只露轮廓；顺序反了内侧描边会残留。
- 描边颜色写死 shader 常数（近黑），不进 cbuffer。

### 3. 参数与 HUD

- `--shading toon` 合法值入枚举，非法值照旧 exit 1；`--outline <宽度>` 默认 0 = 关（与 `--shading toon` 独立：描边也能叠在 textured/PBR 上展示）。
- HUD：shading Combo 加 `toon` 项；outline 复选框视面板高度决定（上限 510，加完 960×540 实拍确认不裁底——M0.6~M0.8 三连教训）。

### 4. example 归位（老流程）

- `example/toon/`：`--shading toon` 双后端基线 + `--shading toon --outline 0.02` 描边对照 + `run.bat` + cases.json；`kStage` 改 `toon`；`server.py --check` 退出 0。

## 验收判据

1. `--shading toon`（含描边开/关）双后端出正确画面、`deviceErrors=0`、调试层零输出。
2. **不带新参数行为与 M0.8（或当时的最新主线版本）完全一致**：Suzanne `--no-hud` 基线 MD5 不变，selftest 全过。
3. 描边宽度参数变化肉眼可见且随距离缩小（等世界宽的预期行为）；三段色阶边界清晰。
4. `model.slang` 单源出 SPIR-V + DXIL，魔数 selftest 照旧覆盖。
5. `example/toon` 归位 + cases.json + `--check` 退出 0。

## 踩坑预判

- **硬边裂缝**：Suzanne 有硬边（顶点法线在棱处被拆分），顶点沿各自法线外扩会在硬边处撕开描边。属已知现象，**先记录不修**——按位置索引合并法线是 S1 meshlet 的事，穿插项不提前铺。
- **剔除方向**：项目 `setFrontCounterClockwise(true)` 的语义下，`CullMode::Front` 才是画背面壳；先用球体验证，再上带硬边的模型。
- **深度顺序**：outline 必须先画（写深度），反了内侧残留描边。
- **确定性**：阈值/描边宽度/光向全部常数或 CLI 固定值，无常数传播问题，截图回归可用。
- **HUD 面板高度**：加 combo 项不增行，复选框/滑条会增行——加完必拍 960×540。

## 不做什么（边界）

- 不做 SDF 脸影、分材质 ramp、顶点色控制线宽（Brioche-Puppet 级进阶，真做卡通项目时另立门户）。
- 不做后处理描边（Sobel on depth/normal）——需要离屏 RT，那是 S7+ 的基建。
- 不做描边抗锯齿 / 抖动；三段色阶不做平滑过渡（smoothstep 混合留作可选进阶）。

## 卡住翻哪

- 卡通整体做法与描边：`HanetakaChou/Brioche-Puppet`（卡通节）；Unity Chan / UTS 文档当通用做法对照。
- 光栅光照基础（NdotL/NdotH/NdotV 全在）：zauonlok/renderer 的 Blinn-Phong shader，C89 可逐行读。
- NVRHI pipeline / cull 语义：`thirdparty/nvrhi/doc/ProgrammingGuide.md` + 本项目 `docs/SHADER-CONVENTIONS.md` §2（剔除方向老坑已写死）。
