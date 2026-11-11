# 天气变化实现 · 调研草案（已拍板，降级为附录）

> 状态：**2026-11-12 已拍板**。参数层（方案 A：M-WEA0~WEA3）立项安排进穿插项 track `weather`；高级（方案 B：大气 LUT + 体积云）列入远期（S13 之后）。**正式任务书见 `WEATHER-PLAN.md`**，本文件保留为调研附录（三处参考实读 + 开源参考 + 方案/代价矩阵全量留痕），不再作为待拍板文档。
> 原产出（2026-11-11）：三处参考实读 + 开源参考与许可证核查 + 方案/代价矩阵 + 验收草案。**零落码、零 shader、零 RT/UAV/槽位、零 tag、零 `kStage`、零 example 基线改动、不 claim/release 轨道、不跑回归**。
> 拍板记录见 `WEATHER-PLAN.md` §9；本文件 Q-WE* 已在该节结案，下方表保留原始待拍板表述仅作溯源。

---

## 0. 第一硬约束：天气 = Lumen 的时变输入，不是贴皮层

用户原话「别浪费 lumen 的强大能力」。据此定死一条方向（**这是已给的方向指令，不是待拍板项**）：

- 天气变化 = **一组随时间变化的光照 / 介质 / 表面参数**，交给现有 Nanite×Lumen 主线求解；画面明暗、反弹光、阴影色温、余晖衰减必须是**算出来的**。
- **禁止**的做法（判为「浪费 Lumen」）：换一张天空 cubemap + 屏幕空间距离雾 + 全局 ambient 乘系数 + 雨雪粒子贴片；即「关掉 GI 看起来也差不多」的实现。
- **验收必须含 GI 侧证据**：探针辐照度、世界缓存 cell 值、shadow ray 直光、多跳余晖随天气单调/连续响应；不能只交「看起来像下雨」的截图。
- **不加第二套 GI**、不改 Lumen 算法主体；每个天气层必须**单独可关**，关闭时冻结基线逐字节一致（ROADMAP「验收心法」+ S14-PLAN 底线终身有效）。

---

## 1. 本仓现状盘点（2026-11-11，MILESTONES 台账 + 当日日志实读）

| 现有能力 | 落点 | 与天气的接口面 |
|---|---|---|
| 解析渐变天穹 `skyGradient(dir)` | `renderer/shaders/sky.slangh`（M10.1，Q12-3），常量 zenith(0.16,0.30,0.58)/horizon(0.62,0.71,0.84)，地平线下 ×0.35 | **硬编码常量**，四处消费：探针 miss、`csCacheUpdate` miss、`reference-gi` 同口径、`--sky-bg` 背景。参数化的第一落点，也是基线重拍的第一雷 |
| 屏幕探针 GI | M10.0~M10.3 收官：`csPlace` / `csTraceProbe`（每探针 64 射线）/ `csGather`+`csGatherCompose`；`traceCB_` 64B（灯向/灯色强度/命中 albedo/shadowBias,tMax） | 天气参数直接进 `traceCB_` 与 `skyGradient`，无需新算法 |
| 世界缓存二跳 | M11.0/M11.1 收官：`csCacheUpdate`（DI shadow ray + 邻居射线查缓存，miss → `albedo×skyGradient`）、`csCacheBlend`（消费后清标记） | 「关灯余晖」「天光变暗后间接光跟随」的现成观测面 |
| 直光 / 阴影射线 / 参考图 | M9.1 shadow ray、M9.2 `--mode reference`、M10.3 `--mode reference-gi`（与探针逐项同口径） | 参考图必须与天气同源，否则尺子失真（Q14-1 口径） |
| TLAS / AS | M8.0 BLAS、M8.1 TLAS 每帧重建（DX12 常驻 `frameCmd_`） | 云层是否进 RT 场景 = Q-WE4，直接影响 AS 语义与 Q11-1 统计口径 |
| 共享 temporal 基建 | T0 收官：`hdrSceneColor_` RGBA32F、`velocity_` RG16F、history ping-pong；`setSharedDepth` 注入共享 D32 | 体积介质 raymarch 的深度接口现成；**S12 未落地**，天气渐变的历史滞后目前无处验 |
| 材质 / 表面 | M7.2 GPUMaterial 间接表；PBR 直光 | 湿地面 roughness/albedo 的落点（bevy_weather `GroundConditions` 同思路） |
| 外围 | GameClock 双帧（M-GL0）、console cvar、ECS、粒子轨道 M-PT0~PT5、驾驶城市方向（`GAME-DEV-ORDER.md`） | 天气状态层可做成纯 CPU 确定性工具版，与渲染基线零冲突 |

**冻结面（本草案一条未碰）**：`kStage` 仍 `S8.2`；Q4 方案 A「不落 GBuffer 纹理」；`example/` 冻结基线全部逐字节不动；`SceneConstants` 160B 禁整块 memcpy；UE-DIFFS D19 proxy 差异口径。

⚠️ **基线形态也是基线的一部分**（`.workbuddy/memory/MEMORY.md` M11.1 实锤）：`direct.slang` 的 `0xdeadbeef` 守卫正常恒 false，s10-refgi 冻结基线的 GI 项实为纯天穹；把它「顺手修好」= 30347 像素稳定回归。**改 `sky.slangh` / 探针 miss / 参考图任一处都属于改冻结基线路径，必须单独拍板 + 重拍（Q-WE5）。**

---

## 2. 三处参考代码实读（本轮定向，非复用旧证据）

| 来源 | 实读文件 | 它怎么说 | 对本项目的结论 |
|---|---|---|---|
| **Carrot**（节奏参照 / 从零起步） | `engine/engine/render/Skybox.hpp`（Forest/Meadow/Clouds 三选一静态 cubemap）；`resources/shaders/post-process/merge-lighting.slang:40-64`（sky 像素取 cubemap + **线性距离雾 lerp 到 fogColor**）；`resources/shaders/includes/lighting.glsl:663-665`（路径追踪 miss → `skyboxColor * beta`） | 天空是静态贴图、雾是合成期 lerp；**GI 侧确实吃同一张 cubemap**，但没有任何时变参数 | 反面教材：GI 远场源可以是天空，但 Carrot 的形态无法演示「天气变 → 间接光跟着变」。本项目不要复制它的「贴图 + 屏幕雾」层 |
| **bevy**（同代实证，本地 `E:/UE_STUDY_2026/bevy`，WESL/WebGPU） | `crates/bevy_solari/src/realtime/atmosphere.wesl:7-22`：`calculate_light_contribution` 里 **`light_contribution.radiance *= directional_light_transmittance(atmosphere, LUT, ...)`**；`crates/bevy_pbr/src/volumetric_fog/volumetric_fog.wesl:1-14,52-69`（raymarch AABB + shadow map 采样 + absorption/scattering/HG phase 参数结构）；`crates/bevy_pbr/src/fog.rs:56-76`（DistanceFog：color/directional light glow/falloff）；`crates/bevy_light/src/atmosphere.rs:37-58`（Atmosphere + `ScatteringMedium` 参数化） | ⭐ **大气透射率直接乘进 GI 的直光贡献**，不是后处理；雾分「距离雾（不与光照交互）」与「体积雾（物理参数化）」两层 | 与用户指令最同构的一条证据：天气参数应挂在 **Lumen 的光源项/辐照度输入** 上。本地 v0.16 快照为准；线上 bevy 0.18 release notes「Atmosphere Occlusion and PBR Shading」把透射率进一步接进 PBR 直射光，仅作方向印证，**不覆盖本地实读** |
| **UE 5.8.4**（工业终态，只读不抄） | `Renderer/Private/SkyAtmosphereRendering.cpp:85-107`（`r.SkyAtmosphere` / **`r.SkyAtmosphere.StateVersioning`：仅状态变化时更新 transmittance + multi-scattering LUT**）；`VolumetricCloudRendering.cpp:97-105`（cloud tracing → **Lumen update translucent lighting volume** → VFog/Cloud upsampling 的顺序耦合；cloud shadow/reflection ray 各自采样预算）；`VolumetricFog.cpp:36,67`（include `Lumen/LumenTranslucencyVolumeLighting.h` + `SkyLightSceneProxy`）；`Lumen/Lumen.cpp:228-235`（`ShouldHandleSkyLight`：`bRealTimeCaptureEnabled`）；`Lumen/LumenRadianceCache.cpp:966-968`（`FDynamicSkyLight` permutation） | 动态天光是 Lumen 的**一等输入**；天气层（大气/云/雾）与 Lumen 有明确先后顺序和独立预算；LUT 按状态变化重算而非每帧 | 工业口径确认「不浪费 Lumen」= 让 GI 通道消费动态天光；同时给出天气慢变的 LUT 更新节拍（不每帧重算） |

**三处口径不一致（必须摆给用户，不自行拍板）**：Carrot = 静态贴图 + 合成期雾（贴皮）；bevy = 大气透射率乘进 GI 直光（参数进 GI）、雾分两层；UE = 全套 LUT + 云/雾与 Lumen 顺序耦合 + 动态天光实时捕获。三者对应本项目的三个不同范围（方案 C / A / B），**不是同一个层级的实现细节**。

历史约定核对：`ROADMAP.md` S0~S14 + P8/P9 + 穿插项表**无任何天气条目**（2026-11-11 全仓 grep 实证）；`S14-PLAN.md` 是「四选一」可选增强（软光栅 / ReSTIR / 自适应探针 / 编辑器），**加天气 = 扩范围，须拍板（Q-WE2）**；`GAME-DEV-ORDER.md` 城市驾驶方向第 11 步「打磨波」是天气最自然的需求侧挂点，但也未列天气。

---

## 3. 开源参考例子（许可证已核，按选型硬闸 5 条留痕）

> 定位全是**思路 / 参数结构 / 验收方法参照**，本轮**不 vendored、不链接、不引入构建期依赖**；UE 源码只读不抄（EULA 与 MIT 不兼容，一行不进 repo）。

| 项目 | 版本 / 许可证 / 状态 | 可借鉴点 | 风险 / 边界 |
|---|---|---|---|
| **bevy_weather** | `0.2.0`（2026-09-16），**MIT OR Apache-2.0**，2 个 unstable releases，~7K SLoC / 430KB，依赖 bevy 0.19（`lib.rs/crates/bevy_weather`、`crates.io/crates/bevy_weather/0.2.0/code` 源码模块表实读） | ① **天气 = 时钟 + seed 的纯函数**（`ProceduralWeather` / `forecast()`），确定性、可存档复现、可回归；② `WeatherSystems::Simulate` → `Apply` 把状态推进 **lights / materials / fog / particles**；③ `GroundConditions.wetness` → 材质 roughness（地面雨停后仍湿）；④ **Cloud shadows 走 light cookie 进 DirectionalLight**（云影进照明而非贴片）；⑤ **日落颜色由大气透射产生，不能再把预红化的光交给大气**（双次消光 → 泥橄榄色）；⑥ `AtmosphereConfig::environment_map_size` 每帧重建、128 足够做 ambient light | unstable 且依赖 bevy 0.19 > 本地 0.16 快照；Rust/WESL 与 Slang/NVRHI 隔源 → 「抄思路不抄代码」物理强制；同名 GitLab 项目是接 Open-Meteo API 的 UI 应用，**不是同一个东西** |
| **Wicked Engine** | C++ 引擎，**MIT**（README/LICENSE.md；`other_licenses.txt` 列第三方子集），持续维护 | `WeatherComponent` 参数全集实证：sunDirection/sunColor/sunEnergy、skyExposure、horizon/zenith、ambient、fogStart/End、height fog、wind、两层体积云 coverage/shape/detail、`AtmosphereParameters`（rayleigh/mie/地球半径）；**volumetric clouds shadows「used for geometry and lighting」**（PR #636 WeatherWindow 实读） | 整引擎 MIT 但**第三方子集须逐文件核 `other_licenses.txt`**；DX11/12/Vulkan 自有 RHI，与 NVRHI 双后端纪律不同源；只作参数形态与「云影进光照」参照，不入库 |
| **Godot 4** | 引擎 **MIT**；`godot-demo-projects/3d/volumetric_fog` demo **MIT**（官方 asset library） | 官方 `volumetric_fog` 文档实锤：**传统雾不与光照交互 vs 体积雾** `GI Inject` / `Ambient Inject` / `Sky Affect` / Anisotropy / Length；**temporal reprojection 会让移动的 FogVolume / Light3D 拖影**；`PhysicalSkyMaterial` 暴露 rayleigh / mie / turbidity / sun disk / ground albedo / exposure，Sky radiance map 用于 ambient 与反射 | 其 SDFGI/VoxelGI 与 Lumen 非同源，只借「雾分两层 + GI 注入 + 时域拖影」三条口径；GDScript 层零参考 |
| **Bruneton precomputed atmospheric scattering** | 原始实现 **BSD-3-Clause**（INRIA 2008 / 2017 版）；DX11 port 同为 BSD-3-Clause；three.js port `jeantimex/precomputed_atmospheric_scattering` MIT | `atmosphere/model.h` 的 `GetSunAndSkyIrradiance(p, normal, sun_direction, out sky_irradiance)` = **大气直接给表面天空辐照度**，与本项目探针 / world cache 的辐照度口径同源；预计算 LUT 形态对应 UE StateVersioning 节拍 | BSD-3-Clause 宽松但与 MIT 不同条款（含「不得用作者名义背书」），**若入库必须 LICENSE 原文 + `VENDOR.md` + 逐文件 SPDX 核**（cosmoscout-vr 已明写「该插件并非全部源码 MIT」的先例）；本轮只作算法/参数参照 |
| **bevy_atmosphere** | `0.13.0`（2025-05），MIT OR Apache-2.0 | 轻量过程天空：参数 resource + 分相机 skybox + 分辨率档，可作「最小参数化天空」的粒度参照 | 对 bevy 版本强绑定；若走方案 A 的解析天空，只需借它的「参数集中化」而非插件结构 |

**选型硬闸结论**：天气算法属 nanite/lumen 渲染主线（**自研例外**成立，但须单独拍板 Q-WE1/Q-WE6）；以上项目**全部只作思路参照**，零构建期新依赖、零厂商 SDK、零运行时 DLL，本机 MSVC 14.38 / C++17 不受影响。如实声明：MIT 无显式专利授权条款，不宣称「绝对零风险」；UE 源码不参与代码层面复用。

---

## 4. 能力复用映射（「不浪费 Lumen」的落点，方案无关）

| 天气输入 | 复用现有通道 | 应自动出现的 GI 侧响应（验收观测面） |
|---|---|---|
| 太阳方位 / 色温 / 强度 | `traceCB_` 灯向灯色（M10.1）、M9.1 shadow ray、`csCacheUpdate` 直光 E_direct、`reference-gi` 同口径 | 阴影方向/软硬、探针八面体图的亮暗分布、世界缓存 C 值、多跳余晖衰减速度 |
| 天穹参数（zenith/horizon/turbidity/sun disk） | `sky.slangh` 单一事实源，四处消费端同源 | 朝天 texel 辐照度、暗面亮度、天光反弹色；**参考图必须同源**否则尺子失真 |
| 云层覆盖 / 透射率 | 直光项衰减 + 探针 miss 方向的天空遮挡（bevy 透射率口径） | 直光变暗时**间接光随之变暗且平滑**，不是画面整体乘系数；world cache 无突变 |
| 雾（吸收/散射/相函数） | 探针射线 / froxel raymarch + 共享深度 `setSharedDepth`（T0 已留接口）；shadow ray 代替 bevy 的 shadow map | 体积光柱、远场 GI 衰减；Godot `GI Inject` 口径（雾吃 GI，GI 也吃雾） |
| 地面湿度 / 积雪 | GPUMaterial 间接表 → 材质 roughness/albedo（M7.2 / PBR） | 反射与反弹光变化；bevy_weather `GroundConditions` 是「天空之外的累积状态」同款 |
| 闪电（瞬时强光） | 短时增强直射 + 现有 `csCacheBlend` 时间混合 | **世界缓存余晖**（M11.2 的「关灯余晖」验收面直接可复用），证明不是贴片闪光 |

**反面判据**：若关掉 `--probe-gather` / 世界缓存后画面「天气感」几乎不变，则该实现浪费了 Lumen，判不通过。

---

## 5. 三个可拍板方案（含代价矩阵）

### 方案 A（推荐）· 参数化天气驱动 Lumen
只做**时变参数层**：太阳位置/强度、天穹参数（zenith/horizon/turbidity + 简单太阳盘与透射近似）、云覆盖透射、单层高度雾近似、地面湿度；`skyGradient` 升级为参数化解析函数并集中到一份公共头（**所有消费端同源**）。不改 Lumen 算法结构、不做体积云 raymarch、不上 LUT。天气状态层为 CPU 纯时钟函数（seed + frame），确定性可回放。

### 方案 B · 大气 LUT + 体积介质
Bruneton 式 transmittance/multi-scattering LUT（**按状态变化重算**，UE StateVersioning 节拍）+ froxel 体积雾 raymarch（用 M9.1 shadow ray 替代 bevy 的 shadow map 采样）+ 云层 marching。视觉最完整，GI 参与介质散射；但新增 RT/UAV/槽位、shader 入口多、与 S12 时序强耦合、基线重拍面最大。

### 方案 C · 纯展示贴皮（**明确否决**）
换天空贴图 + 屏幕空间距离雾 + ambient 乘系数 + 雨雪粒子。**与用户「别浪费 lumen」直接冲突**：无 GI 响应、无探针/缓存证据，仅列作对照，不建议拍板。

| 维度 | A 参数化驱动 | B LUT+体积介质 | C 贴皮（否决） |
|---|---|---|---|
| 新增 RT / UAV / 槽位 | 0~1（仅参数 CB） | 多张 LUT + froxel 3D 纹理 + 多 pass | 0~1 |
| Slang 入口 / 公共头 | 1 份 `weather.slangh` + 现有入口改参数 | 新增若干 compute 入口 | 1 个后处理分支 |
| 基线重拍面 | 中（`sky.slangh` / refgi / probe 系） | 大（含 T0 主路径 / S12 时序） | 小 |
| 对 S12 / S13 的依赖 | 弱（S12 前可先验 GI 静帧） | 强（历史滞后必须与 S12 同期验） | 无 |
| 开发量（业余时间） | ~3~5 天 / 4 小里程碑 | ~2~3 周 | ~1 天 |
| **GI 侧证据强度** | **强**（探针/缓存/余晖全可观测） | 最强 | **无**（不合格） |
| 双后端风险 | 低（参数 CB 确定性） | 中高（LUT/march 浮点路径可能 bytes→near） | 低 |

---

## 6. 里程碑切分草案（仅当 Q-WE1 选中方案后生效）

按方案 A 建议切 4 个小步，每步独立可验收、独立可关、tag 名字制、不 bump `kStage`：

| 里程碑 | 内容 | 亲眼可见的用例 | 建议 tag |
|---|---|---|---|
| **M-W0** 天气状态层 | 纯 CPU：`(seed, frame/clock) → WeatherState` 纯函数 + cvar/CLI 手调 + 工具 dump（TSV/PNG 证据图由真实输出生成）；**零渲染改动、零基线影响**，A 类工具版 | 天气曲线图 + 时间轴对准 + 同 seed 重放逐字节一致 | `weather-w0` |
| **M-W1** 参数化天穹 + 太阳 | `weather.slangh` 单一事实源接入探针 / cache / reference-gi / `--sky-bg`；`traceCB_` 注入太阳与天穹参数；**冻结基线重拍按 Q-WE5 单独授权** | 晴→黄昏→夜的连续变化 + 探针辐照度 viz 同步变化 + 参考图同口径 | `weather-w1` |
| **M-W2** 云透射 + 雾 | 云覆盖解析透射进直光 + 单层高度雾近似；GI 侧响应证据（探针均值 / world cache C 值 / 关灯余晖） | 阴天间接光变暗、云影移动时缓存平滑跟随、无突变 | `weather-w2` |
| **M-W3** 湿地面 + 时域稳定性 | 湿度→材质 roughness/albedo；与 S12 历史帧联动：渐变天气无 GI 亮度滞后鬼影 | 雨停后地面仍湿、天气切换无拖影/无光斑残留 | `weather-w3` |

每步按既有纪律交代：渲染版 = `example/<版本>/` 基线 PNG + `run.bat` verify + `cases.json` 登记；工具版 = 展示台 tool 卡（usage / 正向 / 负向）+ 真实输出证据图 + `WEATHER-TOOLS-GUIDE.md`；双后端写明 `bytes` / `near` / `FAIL` 分档；收口四栏台账 + 固化文档。

---

## 7. 验收判据草案（GI 侧硬指标）

1. **GI 响应**：同一机位同一 seed，晴→阴→雨，`irradAtlas` 探针辐照度均值/方向分布单调且连续；world cache cell 的 C 值随直光衰减；关太阳后余晖自然衰减（复用 M11.2 判据）。
2. **参考尺同源**：`--mode reference-gi` 与探针 miss 共用同一份天气天空函数（Q14-1 口径），否则误差趋势判定无效。
3. **可关 + 零回归**：天气层默认关时，全部冻结基线 `bytes`；开启后老卡按需小子集 `--only` spot（默认不跑全量，全量只在 tag 收口跑一次）。
4. **双后端分档**：明确写 `bytes` / `near` / `FAIL`，禁笼统「一致」。**凡 GPU 输出数值进断言且依赖 AS/TLAS 布局 → 走统计口径**（Q11-1 拍板 A，M11.1 实锤）；CPU 天气状态层保持逐字节确定。
5. **时域**：天气渐变时无 GI 亮度滞后、无鬼影；若 S12 未落地，本项标「延后验收」而非假绿。
6. **确定性**：固定步长累加 float/double 同源（M-N6 教训）；天气 = 时钟纯函数（bevy_weather 实证），同 seed + 同帧号重放一致。

---

## 8. 已知风险 / 坑（提前留痕，拍板时一并核代价）

| 风险 | 依据 | 处置 |
|---|---|---|
| 改 `sky.slangh` / 探针 miss = 改冻结基线路径 | M11.1 `0xdeadbeef` 守卫事件：30347 像素稳定回归 | Q-WE5 单独拍板 + 重拍预算，禁止「顺手修」 |
| 新参数 CB 尺寸错 → shader 静默早退 | M11.0 `dxc -dumpbin` 实锤：CB 分配小于 cbuffer 声明 → dispatch 照常但全线程 early-out | 以反汇编 Size/offset 为准；CPU 常量数组长度 = 字节/4；`SceneConstants` 160B 禁整块 memcpy |
| 时间累积对慢变光照滞后 / 拖影 | Godot 官方：temporal reprojection 让移动 FogVolume/Light3D ghosting；bevy 自适应 `α=|ΔL|/L`（GI-RESEARCH-NOTES §3） | S12 落地后验；天气突变显式缩短历史或强制拒绝 |
| 云几何进不进 TLAS | M8.1 TLAS 每帧重建 + Q11-1 GPU 非 bit-exact | Q-WE4 拍板前不写 AS 相关代码 |
| 双后端精度 | T0 实测：任意浮点中间格式要保 `bytes` 必须 FP32 | 新 LUT/参数 CB 精度在拍板时算 ulp，别事后勘误 |
| HUD / 字符串 | HUD 面板高 ≤560、字符串 ASCII | 天气面板沿用既有约定 |

---

## 9. 待拍板项（**未清不许落码**，AGENTS.md 开工闸）

| 编号 | 问题 | 建议（**不是结论**） |
|---|---|---|
| **Q-WE1** | 范围/架构选 A / B / C | **A**（GI 证据最强、改动面最小、契合「别浪费 Lumen」）；C 建议直接否决 |
| **Q-WE2** | 阶段归属：穿插项（插在 S12 前 / 后）、S14 **第五分支**（扩范围）、独立 W 阶段、A 类并行轨道？ | 建议「穿插项 + M-W0 走 A 类工具版」；S14 原是四选一，加分支须显式拍板 |
| **Q-WE3** | 天空函数形态：解析参数化 vs 预计算 LUT（涉及绑定布局 / 资源格式 / 槽位，**不许先写**） | A 方案下取解析参数化，零新增纹理 |
| **Q-WE4** | 云/雾是否进 TLAS、是否 raymarch（RT 场景语义与 S13 接缝） | A 方案下取「不进 TLAS / 不 raymarch」，仅解析透射 + 高度雾近似 |
| **Q-WE5** | **冻结基线重拍授权**：`sky.slangh` / `direct.slang` 守卫 / s10-refgi 等是否允许重开并重拍 | 必须单独拍板，重拍清单与预算先列给用户 |
| **Q-WE6** | 是否引入任何第三方库（当前建议 **零 vendored**，天气算法自研例外） | 若拍板引入，按选型硬闸 5 条 + `thirdparty/<lib>/VENDOR.md` 逐项核查 |

拍板后动作（**本轮一条未执行**）：① 结论写回 `ROADMAP.md`（穿插项表 / S14 行）与新建的 `WEATHER-PLAN.md` 任务书，删掉任务书里的「待拍板」挂账句；② 全仓 grep 旧口径（当前「无天气条目」，需同步 `S14-PLAN.md` / `GAME-DEV-ORDER.md` / `MILESTONES.md` 下一步）；③ `SHADER-CONVENTIONS.md` 新增天气参数节（含 CB 尺寸实测、单一 BindingSet §2.7、纹理状态 §2.15）；④ `UE-DIFFS.md` 追加偏离台账（若采纳解析近似而非 UE 全套 LUT）；⑤ 才允许写第一行参数/资源代码。

---

## 10. 本轮边界与证据清单

**冻结面（一行未碰）**：`renderer/` 与所有 `.slang`/`.slangh`；`CMakeLists.txt`；`example/` 与 `cases.json`；`kStage`；所有 tag / 分支 / worktree；`MILESTONES.md` 台账；`ROADMAP.md` / `S14-PLAN.md` 等 canonical 文档；未跑回归、未打 tag、未 claim/release。

**本轮实读文件（可追责）**：
- 项目：`AGENTS.md`、`docs/ROADMAP.md`、`docs/s14/S14-PLAN.md`、`docs/s10/S10-PLAN.md`、`docs/s12/S12-PLAN.md`、`docs/GAME-DEV-ORDER.md`、`docs/SHADER-CONVENTIONS.md`（§2.19 / M10.1 表）、`docs/MILESTONES.md`（尾部 S11/M-LOG 段）、`docs/carrot-history.md`、`docs/bevy-history.md`、`.workbuddy/memory/MEMORY.md`、`.workbuddy/memory/2026-11-11.md`、`renderer/shaders/sky.slangh`。
- Carrot：`engine/engine/render/Skybox.hpp`；`resources/shaders/post-process/merge-lighting.slang`；`resources/shaders/includes/lighting.glsl`；`resources/shaders/includes/lights.glsl`。
- bevy：`crates/bevy_solari/src/realtime/atmosphere.wesl`；`crates/bevy_pbr/src/atmosphere/*`（glob）、`crates/bevy_pbr/src/volumetric_fog/volumetric_fog.wesl`、`crates/bevy_pbr/src/fog.rs`、`crates/bevy_light/src/atmosphere.rs`。
- UE（只读）：`Renderer/Private/SkyAtmosphereRendering.cpp`、`VolumetricCloudRendering.cpp`、`VolumetricFog.cpp`、`Lumen/Lumen.cpp`、`Lumen/LumenRadianceCache.cpp`（grep）。
- 联网核实：`lib.rs/crates/bevy_weather`、`crates.io/crates/bevy_weather/0.2.0/code`、`bevyengine.org/news/bevy-0-18`、`lib.rs/crates/bevy_atmosphere`、`github.com/JonahPlusPlus/bevy_atmosphere` README、`turanszkij/WickedEngine` README/LICENSE + PR #636 + DeepWiki 组件表、`docs.godotengine.org` volumetric fog / PhysicalSkyMaterial、`godotengine.org` asset library volumetric fog demo、Bruneton `atmosphere/model.h` 与 BSD-3-Clause 许可证文本、`jeantimex/precomputed_atmospheric_scattering`（MIT）。

> 用户实测结论优先于任何网页/官方文档信息；线上 bevy 0.18 新闻仅作方向印证，本项目以本地 `E:/UE_STUDY_2026/bevy` 实读为准。
