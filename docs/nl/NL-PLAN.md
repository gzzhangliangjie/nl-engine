# 夜景灯光任务书（NL-PLAN，2026-11-14 定稿）

> 上游依据：`docs/NIGHTLIGHTING-PLAN-DRAFT.md`（2026-11-12 调研轮，§1 三处实读 + §5 方案矩阵 + §8 待拍板）。
> 本轮（2026-11-14）拍板已全清（见 §1），本任务书是唯一落码依据；与草案冲突处以本文件为准。

## 1. 拍板记录（2026-11-14，用户逐项确认）

| 项 | 结论 |
|---|---|
| Q-NL1 + Q-NL2 立项归属与定位 | **串联验收场景**：独立任务书（本文件）、不占 m0.* 编号、tag 用名字、不挡主线 |
| Q-NL3 体积光柱路线 | **A 方案：屏幕空间 God Rays**（radial blur 3-pass；B 体积 raymarch 否决——天气 B 组 froxel 尚未立项） |
| Q-NL4 emissive-as-light | **首版不做**：自发光只进最终着色（`model.slang:689` 现状）+ bloom 辉光；日后要「霓虹照亮街道」再单独立项 |
| Q-NL5 冻结基线重拍授权 | **消解**：天气线 M-WEA0~3（merge `4090d94`）已含 `sky.slangh` 9 行改动且走 Q-WE5 零漂移闸，night-sky 用例已存在；夜景只复用其产出、**不改 `sky.slangh` 采样路径**。emissive 进 GI 半条随 Q-NL4 一并消解。若落码中发现必须动采样路径，单独申请重拍，不许顺手改 |
| Q-NL6 第三方 | **零 vendored**：city-of-wires / screen-space-godrays / glsl-godrays / bevy volumetric_fog / UE LightRendering 只作算法与参数参照，一行源码不入库 |

## 2. 现状（2026-11-14 实测）

- **post-chain（T0）**：`rtTarget_`(RGBA32_FLOAT) → `psBlitToHdr` → `hdrSceneColor_`(R11G11B10_FLOAT) → `psTonemap` 直通 → swapchain(RGBA8_UNORM)。gamma 已在 producer 端烤进 scene color，tonemap 不二次 gamma（`rt.slang:155~185`）；TSR/post 插入点已预留注释。
- **自发光**：PBR direct 有 emissive 通道（glTF emissiveFactor + 贴图，`model.slang:679~689`），只进最终色。
- **夜空/夜间雾**：天气线已收官（`--weather` 参数化天穹，M-WEA0 曲线含夜因子/晚霞/夜间压暗）。
- **bloom**：零命中，未落码。**God Rays**：无。
- 结论与草案 §1 一致：新做只有 ③ God Rays 与 bloom 基建，其余全是拼接。

## 3. 子里程碑

### M-NL0：bloom（post-chain 辉光）

- **目标**：自发光/高亮区域溢出成柔和光晕，`--bloom` 旗标开启，**缺省关**。
- **Pass 结构（4 pass，全部挂 post-chain，插入点 = hdrSceneColor_ 之后、psTonemap 之前）**：
  1. `psBloomExtract`：从 `hdrSceneColor_` 半分辨率亮部提取（亮度阈值 CB 可调，起步 ~0.7）→ `bloomA_`（半分辨率，R11G11B10_FLOAT，与 hdrSceneColor_ 同格式）
  2. `psBloomBlurH`：分离高斯水平方向（半分辨率）
  3. `psBloomBlurV`：分离高斯垂直方向 → `bloomB_`（半分辨率）
  4. composite 并进 `psTonemap`：加 `t1 = bloomB_` SRV + CB 旗标常量；**旗标关时加法权重为 0，输出像素与现行直通逐字节一致**
- **bytes 保形（M-WEA0 先例）**：`--bloom` 缺省关时新 pass 全部跳过；`psTonemap` 的 t1 无条件声明（M15.0 教训：静态引用必须齐槽，缺省态 CB 权重 0 直通）→ 缺省态输出保形，历史用例 bytes 全绿。
- **参数（CB，随用例可调）**：threshold / 权重（composite 加法系数起步 ~0.5）/ 模糊半径。
- **shader 产物**：`rt.slang` 新增 psBloomExtract/psBloomBlurH/psBloomBlurV 三入口 × 双后端 = 6，**必须同步登记 selftest 清单**（M7.1/M8.1/M15.0 漏登三犯教训）。

### M-NL1：屏幕空间 God Rays（体积光柱首版）

- **目标**：点光源经场景遮挡形成径向光束，`--godrays` 旗标，缺省关。
- **算法（GPU Gems 3 *Volumetric Light Scattering as a Post-Process* 口径，math-araujo/Erkaman 参照）**：
  1. **遮挡预渲 pass**：论文原版口径——场景几何以极简 PS 渲黑、光源 quad 渲白 → `occlusionMask_`（半分辨率 R8_UNORM 或 R11G11B10，落码前按 NVRHI 支持定）；**不用深度派生**（深度只有最近表面，「光源被挡但几何在光源前」场景会错）
  2. **radial blur pass**：以光源屏幕 UV 为中心径向采样累积（density/weight/decay/exposure 四参数 CB 可调，GPU Gems 3 默认起步）
  3. **叠加**：加法混回 post-chain（同 M-NL0 composite 路径，或独立 composite 入口——落码时以「psTonemap 单点改动最小」为准，两法都不改直通语义）
- **光源选择（首版）**：单光源 = 场景最强点光源（CPU 侧算屏幕投影进 CB）；多光源列入后续项，不进首版。
- **产物**：`godrays.slang`（occlusion ps / radial blur ps）入口 × 双后端，登记 selftest 清单。

### M-NL2：夜景串联验收 demo（收口）

- **内容**：`--weather night`（天气线夜间态）+ 自发光城市/街道资产（灯牌、路灯、车灯）+ `--bloom --godrays` 全开 → 「夜景组合成立」的亲眼可见证据。
- **验收**：
  - 新卡 `example/night-demo/`：基线 PNG + `run.bat`，登记 `cases.json`（带 case 级 `"exe"` 双后端字典）；四态对比证据 = 白天基线 / 夜+自发光 / 夜+bloom / 夜+bloom+godrays，**关 bloom 几乎不变的贴皮验收必挂（草案 C 方案否决口径）**。
  - 缺省态保形：不挂旗标跑 s10/s11/s12/s15/weather 关键用例抽测 bytes 一致。
  - `regression.py --only` 抽测 2~3 条 + 全量总闸一次（tag 收口），双后端分档写实。
- **收口**：tag `nl-done`（annotated）+ `MILESTONES.md` 四栏台账 + `server.py --check`；轨道收口走 `wt_pool.py close/release`（track 注册见 §4）。

## 4. 轨道与执行约定

- **轨道**：`feat/nightlighting` 并行轨道（注册进 `wt_pool.py` TRACKS + wt junction 建树，落码轮开场 `claim`，收口 `release` 对称）。里程碑编号 M-NL0 / M-NL1 / M-NL2。
- **kStage**：不 bump（保持 S8.2），新卡验收走 `run.bat verify` + `regression.py --only` 双重机械校验（M9.0 澄清口径）。
- **落码前核实清单（NL0 开工时做，结果写进当日日志）**：
  1. post-chain 管线创建/布局代码实读（BindingSet 槽位、rt 离屏 RT 创建处），确定 bloom RT 尺寸与池化方式
  2. `psTonemap` 现有 CB 结构（加旗标常量的落点）
  3. 点光源数据通路（`Lo_point` 的 CB 数组），确认「最强点光源 + 屏幕投影」可行
- **冻结面（本轮一行未碰）**：`sky.slangh`、探针/world cache（S10/S11）、GBuffer 布局（Q4）、VSM（S15）、脚本/交通/粒子各轨道。

## 5. 参照与边界（重申草案铁律）

- 只读参照：UE `LightRendering.cpp`（r.LightShafts 口径）/ bevy `volumetric_fog.wesl`（HG 相函数——首版屏幕空间不用，留 B 方案）/ math-araujo & Erkaman（radial blur 参数）/ GPU Gems 3 论文（算法权威）。
- C++20 实现与 GLSL 源码一律不入库；UE 一行不进 repo。
- 自研边界：God Rays 与 bloom 属渲染主线（nanite/lumen 手写诉求的邻接面），自研成立，无需 vendored。
