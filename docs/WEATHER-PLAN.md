# 天气变化实现 · 计划书（参数层已立项，高级远期）

> 状态：**参数层（方案 A：M-WEA0~WEA3）已立项安排**；**高级（方案 B：大气 LUT + 体积云）列入远期（S13 接缝聚合之后）**。
> 2026-11-12 用户拍板：「参数层可以直接在天气那里安排 高级的放远期计划」。本计划书依据 `WEATHER-PLAN-DRAFT.md`（调研附录，三处参考实读 + 开源参考 + 方案/代价矩阵全量留痕）定稿，删去待拍板挂账句。
> 归属：穿插项，track `weather`（wt `nl-engine-weather` / branch `feat/weather`），里程碑 `M-WEA0~WEA3`，tag 名字制 `weather-w0`~`weather-w3`，**不占用 m0.* 编号、不 bump `kStage`**。
> 第一硬约束（用户指令「别浪费 lumen 的强大能力」）：天气 = 一组**时变光照 / 介质 / 表面参数**，交给现有 Nanite×Lumen 主线求解，画面明暗 / 反弹光 / 阴影色温 / 余晖必须用**算出来的**；禁止「换天空贴图 + 屏幕雾 + ambient 乘系数 + 雨雪贴片」式贴皮。验收必须含 GI 侧证据。

---

## 0. 第一硬约束：天气 = Lumen 的时变输入，不是贴皮层

- 天气变化 = 一组随时间变化的光照 / 介质 / 表面参数，交给现有 Nanite×Lumen 主线求解；画面明暗、反弹光、阴影色温、余晖衰减必须是**算出来的**。
- **禁止**：换一张天空 cubemap + 屏幕空间距离雾 + 全局 ambient 乘系数 + 雨雪粒子贴片；即「关掉 GI 看起来也差不多」的实现。
- **验收必须含 GI 侧证据**：探针辐照度、世界缓存 cell 值、shadow ray 直光、多跳余晖随天气单调/连续响应；不能只交「看起来像下雨」的截图。
- **不加第二套 GI**、不改 Lumen 算法主体；每个天气层必须**单独可关**，关闭时冻结基线逐字节一致（ROADMAP「验收心法」+ S14-PLAN 底线终身有效）。

---

## 1. 本仓现状盘点（2026-11-11/12 实读，可复用接口）

| 现有能力 | 落点 | 与天气的接口面 |
|---|---|---|
| 解析渐变天穹 `skyGradient(dir)` | `renderer/shaders/sky.slangh`（M10.1，Q12-3），常量 zenith/horizon，地平线下 ×0.35 | **硬编码常量**，四处消费：探针 miss、`csCacheUpdate` miss、`reference-gi` 同口径、`--sky-bg` 背景。参数化的第一落点 |
| 屏幕探针 GI | M10.0~M10.3：`csPlace`/`csTraceProbe`（每探针 64 射线）/`csGather`+`csGatherCompose`；`traceCB_` 64B | 天气参数直接进 `traceCB_` 与 `skyGradient`，无需新算法 |
| 世界缓存二跳 | M11.0/M11.1：`csCacheUpdate`（DI shadow ray + 邻居射线查缓存）/ `csCacheBlend`（消费后清标记） | 「关灯余晖」「天光变暗后间接光跟随」的现成观测面 |
| 直光 / 阴影射线 / 参考图 | M9.1 shadow ray、M9.2 `--mode reference`、M10.3 `--mode reference-gi` | 参考图必须与天气同源，否则尺子失真（Q14-1 口径） |
| TLAS / AS | M8.0 BLAS、M8.1 TLAS 每帧重建 | **方案 A 下云/雾不进 TLAS、不 raymarch**（Q-WE4 随 A 隐式决定） |
| 共享 temporal 基建 | T0：`hdrSceneColor_`/`velocity_`/history ping-pong；`setSharedDepth` 注入共享 D32 | 体积介质 raymarch 的深度接口现成（仅方案 B 用） |
| 材质 / 表面 | M7.2 GPUMaterial 间接表；PBR 直光 | 湿地面 roughness/albedo 的落点（bevy_weather `GroundConditions` 同思路） |
| 外围 | GameClock 双帧（M-GL0）、console cvar、ECS、粒子轨道 M-PT0~PT5 | 天气状态层可做成纯 CPU 确定性工具版，与渲染基线零冲突 |

**冻结面（本计划书一条未碰、M-WEA1 才触碰）**：`kStage` 仍 `S8.2`；Q4 方案 A「不落 GBuffer 纹理」；`example/` 冻结基线全部逐字节不动；`SceneConstants` 160B 禁整块 memcpy；UE-DIFFS D19 proxy 差异口径。

⚠️ **基线形态也是基线的一部分**（`.workbuddy/memory/MEMORY.md` M11.1 实锤）：`direct.slang` 的 `0xdeadbeef` 守卫正常恒 false，s10-refgi 冻结基线的 GI 项实为纯天穹；把它「顺手修好」= 30347 像素稳定回归。**改 `sky.slangh` / 探针 miss / 参考图任一处都属于改冻结基线路径 → 走 `Q-WE5` 单独拍板 + 重拍闸门（M-WEA1 开工前必清）。**

---

## 2. 三处参考代码结论（详 `WEATHER-PLAN-DRAFT.md` §2，本轮定向实读）

| 来源 | 它怎么说 | 对本项目的结论 |
|---|---|---|
| **Carrot** | 静态 cubemap 天空 + 合成期线性雾；GI 吃同一张 cubemap 但无时变参数 | 反面教材：贴图 + 屏幕雾层，无法演示「天气变 → 间接光跟着变」 |
| **bevy** ⭐ | `atmosphere.wesl`：**大气透射率直接乘进 GI 的直光贡献**；雾分「距离雾（不与光照交互）」与「体积雾（物理参数化）」两层 | 与用户指令最同构：天气参数挂在 **Lumen 的光源项/辐照度输入** 上 |
| **UE 5.8.4**（只读） | 动态天光是 Lumen 的一等输入；天气层（大气/云/雾）与 Lumen 顺序耦合、独立预算；LUT 按状态变化重算非每帧 | 工业口径确认「不浪费 Lumen」= 让 GI 通道消费动态天光；慢变 LUT 更新节拍（方案 B 远期才用） |

---

## 3. 开源参考（许可证已核，思路参照、零 vendored，详草案 §3）

`bevy_weather 0.2.0`（MIT/Apache，天气=时钟+seed 纯函数、`Simulate→Apply` 推 lights/materials、湿地面、云影进 DirectionalLight、日落色不预红化光源）、Wicked Engine（MIT C++，`WeatherComponent` 参数全集 + 云影进 lighting）、Godot 4（MIT，体积雾 `GI Inject`/`Ambient Inject` 分层、时域拖影）、Bruneton（BSD-3-Clause，`GetSunAndSkyIrradiance` 与探针辐照度同源）、bevy_atmosphere（MIT/Apache）。UE 源码只读不抄；MIT 无显式专利授权条款，不宣称「绝对零风险」。

---

## 4. 能力复用映射（「不浪费 Lumen」的落点，方案无关）

| 天气输入 | 复用现有通道 | 应自动出现的 GI 侧响应（验收观测面） |
|---|---|---|
| 太阳方位 / 色温 / 强度 | `traceCB_` 灯向灯色（M10.1）、M9.1 shadow ray、`csCacheUpdate` 直光 E_direct、`reference-gi` | 阴影方向/软硬、探针八面体图亮暗分布、世界缓存 C 值、多跳余晖衰减速度 |
| 天穹参数（zenith/horizon/turbidity/sun disk） | `sky.slangh` 单一事实源，四处消费端同源 | 朝天 texel 辐照度、暗面亮度、天光反弹色；**参考图必须同源**否则尺子失真 |
| 云层覆盖 / 透射率 | 直光项衰减 + 探针 miss 方向天空遮挡（bevy 透射率口径） | 直光变暗时**间接光随之变暗且平滑**，非画面整体乘系数；world cache 无突变 |
| 雾（吸收/散射/相函数） | 方案 A = 单层高度雾近似（解析）；方案 B = froxel raymarch + 共享深度 | 体积光柱、远场 GI 衰减；Godot `GI Inject` 口径 |
| 地面湿度 / 积雪 | GPUMaterial 间接表 → 材质 roughness/albedo | 反射与反弹光变化；bevy_weather `GroundConditions` 同款「天空之外的累积状态」 |
| 闪电（瞬时强光） | 短时增强直射 + 现有 `csCacheBlend` 时间混合 | **世界缓存余晖**（M11.2「关灯余晖」验收面直接复用），证明不是贴片闪光 |

**反面判据**：若关掉 `--probe-gather` / 世界缓存后画面「天气感」几乎不变，则该实现浪费了 Lumen，判不通过。

---

## 5. 方案定案

### 方案 A（采用）· 参数化天气驱动 Lumen
只做**时变参数层**：太阳位置/强度、天穹参数（zenith/horizon/turbidity + 简单太阳盘与透射近似）、云覆盖透射、单层高度雾近似、地面湿度；`skyGradient` 升级为参数化解析函数并集中到一份公共头（**所有消费端同源**）。不改 Lumen 算法结构、不做体积云 raymarch、不上 LUT。天气状态层为 CPU 纯时钟函数（seed + frame），确定性可回放。**零新增纹理、零 raymarch、不进 TLAS。**

### 方案 B（远期）· 大气 LUT + 体积介质
Bruneton 式 transmittance/multi-scattering LUT（按状态变化重算，UE StateVersioning 节拍）+ froxel 体积雾 raymarch（用 M9.1 shadow ray 替代 bevy 的 shadow map）+ 体积云 marching。视觉最完整，GI 参与介质散射；但新增 RT/UAV/槽位、shader 入口多、与 S12 时序强耦合、基线重拍面最大。**列入远期（S13 接缝聚合之后），不在本轮排期。**

### 方案 C（否决）· 纯展示贴皮
换天空贴图 + 屏幕空间距离雾 + ambient 乘系数 + 雨雪粒子。与「别浪费 lumen」直接冲突，无 GI 响应、无探针/缓存证据，否决不排期。

---

## 6. 里程碑切分

### 参数层（方案 A，已立项）

| 里程碑 | 内容 | 亲眼可见的用例 | 建议 tag | track |
|---|---|---|---|---|
| **M-WEA0** 天气状态层 | 纯 CPU：`(seed, frame/clock) → WeatherState` 纯函数 + cvar/CLI 手调 + 工具 dump（TSV/证据图由真实输出生成）；**零渲染改动、零基线影响**，A 类工具版 | 天气曲线图 + 时间轴对准 + 同 seed 重放逐字节一致 | `weather-w0` | `weather` |
| **M-WEA1** 参数化天穹 + 太阳 | `weather.slangh` 单一事实源接入探针 / cache / reference-gi / `--sky-bg`；`traceCB_` 注入太阳与天穹参数；**冻结基线重拍按 Q-WE5 闸门单独授权** | 晴→黄昏→夜连续变化 + 探针辐照度 viz 同步变化 + 参考图同口径 | `weather-w1` | `weather` |
| **M-WEA2** 云透射 + 雾 | 云覆盖解析透射进直光 + 单层高度雾近似；GI 侧响应证据（探针均值 / world cache C 值 / 关灯余晖） | 阴天间接光变暗、云影移动时缓存平滑跟随、无突变 | `weather-w2` | `weather` |
| **M-WEA3** 湿地面 + 时域稳定性 | 湿度→材质 roughness/albedo；与 S12 历史帧联动：渐变天气无 GI 亮度滞后鬼影 | 雨停后地面仍湿、天气切换无拖影/无光斑残留 | `weather-w3` | `weather` |

### 高级（方案 B，远期条目，S13 之后才排期）
- 大气 LUT（Bruneton transmittance/multi-scattering，按状态变化重算）+ froxel 体积雾 raymarch + 体积云 marching。
- 立项条件：S13 接缝聚合收官、S12 历史帧落地（时域拖影有可验基线）、且当前参数层已全收口并被用户确认「视觉仍不够」。
- 届时单独出 `WEATHER-PHASE2-PLAN.md`，重走三处实读 + 选型闸 + 冻结基线重拍预算。

---

## 7. 验收判据（GI 侧硬指标）

1. **GI 响应**：同一机位同一 seed，晴→阴→雨，`irradAtlas` 探针辐照度均值/方向分布单调且连续；world cache cell 的 C 值随直光衰减；关太阳后余晖自然衰减（复用 M11.2 判据）。
2. **参考尺同源**：`--mode reference-gi` 与探针 miss 共用同一份天气天空函数（Q14-1 口径），否则误差趋势判定无效。
3. **可关 + 零回归**：天气层默认关时，全部冻结基线 `bytes`；开启后老卡按需小子集 `--only` spot（默认不跑全量，全量只在 tag 收口跑一次）。
4. **双后端分档**：明确写 `bytes` / `near` / `FAIL`，禁笼统「一致」。**凡 GPU 输出数值进断言且依赖 AS/TLAS 布局 → 走统计口径**（Q11-1 拍板 A，M11.1 实锤）；CPU 天气状态层保持逐字节确定。
5. **时域**：天气渐变时无 GI 亮度滞后、无鬼影；若 S12 未落地，本项标「延后验收」而非假绿。
6. **确定性**：固定步长累加 float/double 同源（M-N6 教训）；天气 = 时钟纯函数（bevy_weather 实证），同 seed + 同帧号重放一致。

---

## 8. 已知风险 / 坑（提前留痕）

| 风险 | 依据 | 处置 |
|---|---|---|
| 改 `sky.slangh` / 探针 miss = 改冻结基线路径 | M11.1 `0xdeadbeef` 守卫事件：30347 像素稳定回归 | **Q-WE5 闸门**：M-WEA1 开工前单独拍板 + 重拍预算，禁止「顺手修」 |
| 新参数 CB 尺寸错 → shader 静默早退 | M11.0 `dxc -dumpbin` 实锤：CB 分配小于 cbuffer 声明 → dispatch 照常但全线程 early-out | 以反汇编 Size/offset 为准；CPU 常量数组长度 = 字节/4；`SceneConstants` 160B 禁整块 memcpy |
| 时间累积对慢变光照滞后 / 拖影 | Godot 官方 temporal reprojection ghosting；bevy 自适应 `α=|ΔL|/L` | S12 落地后验；天气突变显式缩短历史或强制拒绝 |
| 双后端精度 | T0 实测：任意浮点中间格式要保 `bytes` 必须 FP32 | 新参数 CB 精度在拍板时算 ulp，别事后勘误 |
| HUD / 字符串 | HUD 面板高 ≤560、字符串 ASCII | 天气面板沿用既有约定 |

---

## 9. 拍板记录（2026-11-12）

| 编号 | 问题 | 结论 |
|---|---|---|
| **Q-WE1** | 范围/架构选 A / B / C | **A**（参数化驱动 Lumen，GI 证据最强、改动面最小） |
| **Q-WE2** | 阶段归属 | **参数层排穿插项（track `weather`）**；高级（方案 B）列入远期（S13 后） |
| **Q-WE3** | 天空函数形态 | 随 A = 解析参数化，零新增纹理（无需另拍板） |
| **Q-WE4** | 云/雾是否进 TLAS / raymarch | 随 A = 不进 TLAS / 不 raymarch，仅解析透射 + 高度雾近似（无需另拍板） |
| **Q-WE5** | 冻结基线重拍授权 | **保留为 M-WEA1 开工闸门**：改 `sky.slangh`/探针 miss/参考图前必须单独拍板 + 重拍预算 |
| **Q-WE6** | 是否引入第三方库 | 随 A = **零 vendored**，天气算法自研例外（无需另拍板） |

**拍板后动作（已执行）**：① 结论写回 `ROADMAP.md` 穿插项表（参数层 + 高级远期两行）、新建本计划书、草案降级为调研附录；② `MILESTONES.md` 补天气立项台账；③ `GAME-DEV-ORDER.md` 第 11 步打磨波标注天气渲染穿插项可在此消费。
**拍板后动作（M-WEA1 开工时补）**：④ `SHADER-CONVENTIONS.md` 新增天气参数节（CB 尺寸实测、单一 BindingSet §2.7、纹理状态 §2.15，紧贴 M10.1 天穹头）；⑤ `s5/UE-DIFFS.md` 追加偏离台账（采纳解析近似而非 UE 全套 LUT）；⑥ 全仓 grep 旧「无天气条目」口径同步（S14-PLAN/GAME-DEV-ORDER/MILESTONES）。

---

## 10. 边界与证据清单

**冻结面（一行未碰）**：`renderer/` 与所有 `.slang`/`.slangh`；`CMakeLists.txt`；`example/` 与 `cases.json`；`kStage`；所有 tag / 分支 / worktree；`MILESTONES.md` 既有台账（仅追加天气立项节）；`ROADMAP.md` / `S14-PLAN.md` 既有内容（仅穿插项表追加两行）。未跑回归、未打 tag、未 claim/release。

**本轮实读文件（可追责，详草案 §10）**：
- 项目：`AGENTS.md`、`ROADMAP.md`、`S14-PLAN.md`、`S10-PLAN.md`、`S12-PLAN.md`、`GAME-DEV-ORDER.md`、`SHADER-CONVENTIONS.md`（§2.19/M10.1）、`MILESTONES.md`、`carrot-history.md`、`bevy-history.md`、`MEMORY.md`、`2026-11-11.md`、`sky.slangh`。
- Carrot：`Skybox.hpp`、`merge-lighting.slang`、`lighting.glsl`、`lights.glsl`。
- bevy：`atmosphere.wesl`、`volumetric_fog.wesl`、`fog.rs`、`atmosphere.rs`。
- UE（只读）：`SkyAtmosphereRendering.cpp`、`VolumetricCloudRendering.cpp`、`VolumetricFog.cpp`、`Lumen.cpp`、`LumenRadianceCache.cpp`。
- 联网核实：bevy_weather / bevy_atmosphere / Wicked Engine / Godot 4 / Bruneton（许可证与参数结构，见草案 §3）。

> 用户实测结论优先于任何网页/官方文档信息；线上 bevy 0.18 新闻仅作方向印证，本项目以本地 `E:/UE_STUDY_2026/bevy` 实读为准。
