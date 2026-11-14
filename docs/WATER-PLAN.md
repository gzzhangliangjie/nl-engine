# 水体渲染实现 · 计划书（方案 B 已立项：Gerstner + SSR 反射 + 折射吸收）

> 状态：**方案 B（M-WAT0~WAT2）已立项**；方案 C（平面反射）/ D（FFT 海洋）列入远期。
> 2026-11-14 用户拍板：独立轨道 `feat/water` / 方案 B / 折射首版做 / 零 vendored。本计划书依据 `WATER-PLAN-DRAFT.md`（调研附录，三处参考实读 + 开源初筛 + 方案/代价矩阵全量留痕）定稿，删去待拍板挂账句。
> 归属：穿插项，track `water`（wt `nl-engine-water` / branch `feat/water`，M-WAT0 开工时 `wt_pool.py claim`），里程碑 `M-WAT0~WAT2`，tag 名字制 `water-t0`~`water-t2`，整轨收官 merge 后 tag `water-done`，**不占用 m0.* 编号、不 bump `kStage`**。
> 第一硬约束（用户口径，草案 §0）：水体 = **波形几何 + 反射/折射 + 吸收散射 + 透明排序**的全 pass 链，画面里的水体感必须是**算出来的**；禁止「一张反射贴图 + 屏幕雾」贴皮。验收必须含反射/折射侧证据——**关掉反射后水体感几乎不变即不合格**。

---

## 0. 第一硬约束：反射基建先于水体（依赖顺序铁律）

- 水体最贵的半边是**反射**。平面反射或 SSR 没落地就先写水面，反射半边必返工（草案 §6）。
- 已拍板（Q-WA3）：反射走 **SSR**，一步两用——SSR 本体服务全场景普通材质，水体只是它的第二个消费者。因此 **M-WAT0 = SSR 本体落地**，放在水体任何水面 pass 之前。
- SSR 现状（2026-11-14 核实）：插点 S7 之后已拍板（2026-11-09，`docs/s6/SSR-RESEARCH-NOTES.md` 预研齐）、T0 离屏 HDR RT + post-chain 就绪、S7 vis buffer 法线/粗糙度就绪、S6 深度金字塔就绪——**SSR 代码本体尚未落地**，由本轨 M-WAT0 承载，宿主从「主线穿插项」转入 water 轨道（ROADMAP 归宿行已同步）。
- 折射的「无水体场景」纹理复用 T0 `hdrSceneColor_` 思路，但须按 UE `RefractionMask` 同构做深度预渲分类（M-WAT2）。

---

## 1. 本仓现状盘点（2026-11-14 复核，可复用接口）

| 现有能力 | 落点 | 与水体的接口面 |
|---|---|---|
| vis buffer 反解 | S7 `gbuffer_common.slangh` 公共反解契约；Q4 方案 A 不落 GBuffer 纹理，法线/粗糙度即取即算 | SSR march 的命中判定与反射 ray 方向的输入源 |
| 深度金字塔 | S6（径向距离口径，SSR-RESEARCH-NOTES §3 论证成立） | Hi-Z march 步进加速；mip 增长只挂粗糙度（UE 口径） |
| 离屏 HDR RT + post-chain | T0 `hdrSceneColor_` + tonemap/合成 pass | SSR 反射合成落点；折射「无水体场景」RT 同构复用 |
| raster 主路径 | S4 GPU-driven 间接绘制 + S7 instanced 批次化 | Gerstner 顶点位移 pass 的挂点 |
| PBR 直光 | M0.x `pbr-direct`（Cook-Torrance GGX） | 水面高光（太阳 specular）与菲涅尔计算基础 |
| GameClock 双帧 | M-GL0 `GameClock::kStep` | 波形固定时间步进保 bytes（`--traffic-steps` 同款纪律） |
| 天空单一事实源 | `sky.slangh` → weather 参数化（M-WEA1） | SSR miss 天空采样**只调用现有公共函数**，不改采样路径（Q-WA5 预计不触发） |
| 粒子轨道 | M-PT0~PT5（已 merge） | 透明排序的既有半透明消费端；首版场景水体 = 唯一半透明面，复杂排序不进首版（随方案 B 隐式决定） |

**冻结面（本计划书一条未碰）**：`kStage` 仍 `S8.2`；Q4 方案 A 不落 GBuffer 纹理；`sky.slangh` 采样路径（M11.1 冻结）；`example/` 冻结基线逐字节不动；`SceneConstants` 160B 禁整块 memcpy。

---

## 2. 三处参考代码结论（详草案 §1，2026-11-11 定向实读）

| 来源 | 它怎么说 | 对本项目的结论 |
|---|---|---|
| **UE 5.8.4**（只读，权威） | `SingleLayerWaterRendering`：深度预渲 + 无水体场景渲（`SceneWithoutWaterTextures`）+ tile 分类反射（`FTiledReflection`）+ 折射掩码 + 速度，与体积 RT 强耦合；SSR march 口径在 `SSRT/SSRTRayCast.ush`（固定步长 + mip 按粗糙度 + 容差带 + bUncertain） | 算法口径唯一权威；水体 pass 链按此分解，首版裁剪体积 RT 交叉项 |
| **bevy**（轻量阶梯） | `transmission.wesl`：`refracted_ray()` Snell 折射 + thickness 出射点 → 屏幕 UV 偏移；深度拒绝；roughness 螺旋模糊。**只有薄玻璃透射，不是水面**；无波形/泡沫/水下体积 | 折射结构（UV 偏移 + 屏幕背景采样 + 深度拒绝）直接作 M-WAT2 阶梯 |
| **Carrot**（反面印证） | 全目录 grep 零水体；静态 cubemap 天空 + 线性距离雾 | 自研路线默认无水体，节奏参照 |

**每里程碑开工前按铁律重读本里程碑点名文件**（M-WAT0 = `SSRTRayCast.ush`/`SSRTReflections.usf`/`HZB.ush` + visbuf 反解；M-WAT1 = UE water vertex factory + osgw/iago gerstner 参照；M-WAT2 = `transmission.wesl` 重读 + UE `RefractionMask`/`SceneWithoutWater`），不许拿上一轮证据顶替。

---

## 3. 开源参考（许可证已初筛，**零 vendored**，详草案 §4）

- 波形参照：`CaffeineViking/osgw`（MIT，`gerstner.glsl` 解耦最干净）、`iago-cpalmer/Ocean-Wave-Simulator`（MIT，Gerstner+fBm 至 2048 波 + 解析 per-pixel 法线 + 泡沫 + Fresnel）。
- 反射/折射参照：`Yanyilucas/PurePool`（MIT，FBO 反射/折射 + Fresnel + Beer-Lambert）、`jmermar/Vulkan-Water-Renderer`（MIT，最贴 NVRHI/Vulkan 栈）。
- 架构参照：`ryzomcore` water_planar_reflection（平面反射远期）；⚠️ `deiss/fftocean` **GPL-3.0 禁入**；`kentril0/WaterSurfaceRendering` 依赖 FFTW+OpenMP **按选型硬闸禁入**。
- 口径：只读算法参照（抄思路不抄代码），不 vendored 入库，无需 `VENDOR.md`。MIT 无显式专利授权条款，不宣称「绝对零风险」。

---

## 4. 方案定案

### 方案 B（采用，2026-11-14 拍板）· Gerstner + SSR 反射 + 折射吸收
- **波形**：Gerstner 解析波（多波叠加），顶点位移 + 解析 per-pixel 法线，零纹理；固定时间步进保 bytes。
- **反射**：SSR（复用 S6 深度金字塔 + S7 vis buffer + T0 post-chain），miss 采现有天空公共函数。
- **折射**：渲「无水体场景」纹理 → 波形法线扭曲 UV 采样 + 深度差 → Beer-Lambert 吸收（深绿/浅蓝）。
- **合成**：Fresnel 混合反射/折射 + Gerstner 偏度/Jacobian 波峰泡沫。
- 复用最多、不碰 GI 冻结基线；日后要更真再升 C/D。

### 方案 C（远期）· 平面反射升级
镜像相机重渲场景；须先单独立平面反射里程碑。每帧多渲一遍场景，与 P9「超大场景 60fps」冲突。不排期。

### 方案 D（远期）· FFT 海洋
FFT-Tessendorf 需自实现 compute FFT（FFTW 禁入）；时间演化 + 时域重投影撞 M11.1 统计口径。不排期。

### 方案 A（否决）· 贴皮
静态法线图 + 反射贴图。验收必挂（关反射几乎不变），否决不排期。

---

## 5. 里程碑切分

| 里程碑 | 内容 | 亲眼可见的用例 | 建议 tag | track |
|---|---|---|---|---|
| **M-WAT0** SSR 反射本体 | Hi-Z march（UE 口径：固定步长 + mip 按粗糙度 + 容差带 + bUncertain）+ 法线/粗糙度 vis buffer 即取 + scene color GenerateMips + 离屏合成；**第一版 = 静态相机 + 无 jitter 确定性演示**（SSR-RESEARCH-NOTES §2 实锤：无时域累积只能这样，时域列为升级路径）；普通材质 + 水体两消费端一步两用 | 镜面地板/待水体面反射可见；关 SSR 画面明显变化（反射侧证据）；usage/正向/负向 tool 卡 | `water-t0` | `water` |
| **M-WAT1** Gerstner 波形 + 法线 | water plane mesh + 顶点位移 pass 进 raster 主路径；Gerstner 多波叠加 + 解析 per-pixel 法线；固定时间步进（`GameClock::kStep` 钉死）；波形参数 CPU 侧结构（波向/波高/波长/陡度） | 波形肉眼起伏 + 高光随法线扫动；固定帧号重放逐字节一致 | `water-t1` | `water` |
| **M-WAT2** 折射 + 吸收 + 泡沫收口 | 「无水体场景」渲（深度分类跳过水体，UE `RefractionMask` 同构）+ 扭曲 UV + Beer-Lambert 吸收 + Fresnel 混合 + 波峰泡沫；水体 pass 链完整合成 | 水下物体折射扭曲可见、深浅色吸收渐变、波峰白沫；**关反射水体感几乎不变即不合格**（整轨硬验收） | `water-t2`（整轨 merge 后 `water-done`） | `water` |

每个里程碑开工前：`wt_pool.py claim --track water --milestone M-WAT<n>`；收口走 `close`/`release --done` 硬闸（并行轨道口径）。

---

## 6. 验收判据（反射/折射侧硬指标）

1. **反射证据**：同一机位，SSR 开/关两张对比图差异显著（反射面占比、高光位置、天空镜像）；水体反射吃到场景几何（非纯天空采样）。
2. **折射证据**：水下物体位置/形态经折射偏移后肉眼可辨；Beer-Lambert 深浅吸收渐变单调。
3. **确定性**：Gerstner 固定步进 + 固定帧号重放逐字节一致；GPU 数值进断言且依赖 RT 布局时走统计口径（Q11-1 拍板 A）。
4. **可关 + 零回归**：水体/SSR 默认关时冻结基线 `bytes`；开启后老卡按需 `--only` 小子集，全量只在 tag 收口跑一次。
5. **双后端分档**：明确写 `bytes` / `near` / `FAIL`，禁笼统「一致」。
6. **example 交代**：每里程碑渲染版本卡（基线 PNG + run.bat + cases.json 登记）+ 工具版卡；`--check` 按 M9.0 澄清口径执行（不 bump kStage 时新卡以 `run.bat verify` + `regression.py --only` 双重机械校验）。

---

## 7. 已知风险 / 坑（提前留痕）

| 风险 | 依据 | 处置 |
|---|---|---|
| SSR miss 改天空采样路径 = 改冻结基线 | M11.1「基线形态也是基线」；`sky.slangh` 四处消费 | **Q-WA5 闸门**：M-WAT0 只调用现有公共函数（weather 参数化后单一事实源），预计不触发；若发现必须改 `sky.slangh` → 单独拍板 + 重拍预算，禁止「顺手改」 |
| SSR 无时域累积会闪 | SSR-RESEARCH-NOTES §2（UE 自认 8 帧轮换 + 会闪） | 第一版静态相机 + 无 jitter 确定性演示规避；时域（复用 S12/M-WEA3 history reject 基建）列升级路径 |
| 水体参数 CB 尺寸错 → 静默早退 | M11.0 `dxc -dumpbin` 实锤 | 新 CB 以反汇编 Size/offset 为准；CPU 数组长度 = 字节/4 |
| 双后端 bytes | T0 实测：保 bytes 必须 FP32 | 波形时间走固定步进整数帧号 → float 同源计算 |
| 半透排序 | 水体半透 + 粒子已 merge | 首版场景水体 = 唯一半透明面（随方案 B 隐式决定）；复杂排序进阶不排期 |
| 「无水体场景」RT 归属 | Q4 冻结不新增 GBuffer 纹理 | 折射 RT 走 T0 同款离屏 RT 口径，不进 GBuffer（草案 §7.3 建议随 B 隐式成立） |

---

## 8. 拍板记录（2026-11-14）

| 编号 | 问题 | 结论 |
|---|---|---|
| **Q-WA1** | 立项归属 | **独立轨道 feat/water**（照 M-WEA 先例：不占 m0.* 编号、tag 名字制、不挡主线）；里程碑 M-WAT0~WAT2 |
| **Q-WA2** | 波形 | **Gerstner 解析**（方案 B）；FFT（D）远期 |
| **Q-WA3** | 反射 | **SSR**（复用已拍板穿插项口径）；SSR 本体未落地，由 **M-WAT0 承载**；平面反射（C）须先单独立项，远期 |
| **Q-WA4** | 折射 | **首版做**：扭曲 UV + Beer-Lambert 吸收；水下体积合成留进阶 |
| **Q-WA5** | 冻结基线重拍授权 | 预计**不触发**（SSR miss 只调用现有天空公共函数）；若必须改 `sky.slangh` 采样路径 → 开工前单独拍板 |
| **Q-WA6** | 第三方 | **零 vendored**，只读算法参照；GPL/FFTW 依赖项禁入 |

**拍板后动作**：① 本计划书定稿，草案降级为调研附录 ✅；② `ROADMAP.md` 穿插项表补水体行 + SSR 归宿行更新宿主 ✅；③ `MILESTONES.md` 立项台账于 M-WAT0 开工时补；④ SHADER-CONVENTIONS 水体新节于 M-WAT1 落码前补。

---

## 9. 边界与证据清单

**冻结面（本轮纯文档，一行代码未碰）**：`renderer/` 与所有 `.slang`/`.slangh`；`CMakeLists.txt`；`example/` 与 `cases.json`；`kStage`；所有 tag / 分支 / worktree；未 claim/release、未跑回归、未打 tag。

**本轮实读文件（可追责）**：
- 项目：`AGENTS.md`、`WATER-PLAN-DRAFT.md`（全文）、`WEATHER-PLAN.md`（结构模板）、`PARALLEL-TRACKS.md` §1、`ROADMAP.md` 穿插项表 + SSR 归宿行、`SSR-RESEARCH-NOTES.md`（grep 定向）。
- 代码核实：`renderer/shaders/` grep `ssr|screen space reflect` 零命中（SSR 本体未落地实证）；主线 git log 天气轨收官（`86df748`）。
- 三处参考（引草案 2026-11-11 实读留痕）：UE `SingleLayerWaterRendering.h` / bevy `transmission.wesl` / Carrot 全目录 grep 零水体。

> 用户实测结论优先于任何网页信息；每个里程碑开工前按铁律重读本里程碑对应三处文件。

## 9b. M-WAT0 开工实读与任务书（2026-11-14，claim 后三处重读）

> claim：`wt_pool.py claim --track water --milestone M-WAT0`（2026-11-14 14:57）；wt `nl-engine-water` 建于 master `3004cb1`（junction .workbuddy + thirdparty/slang/dist + skip-worktree 13 files）。

**本轮实读（模块 = SSR march 本体，一一对应）**：
- UE（权威）：`Shaders/Private/SSRT/SSRTRayCast.ush`（march 主循环重读：UVz 三元组、4 texel 批量、`Level += (8/NumSteps)*Roughness`、容差带 hit、bUncertain 停、远平面拒绝、起点 95% 截断；tile 地平线加速段——首版不做，列升级）+ `HZB.ush`（HZB 绑定结构）+ `SSRTReflections.usf`（入口签名/`SampleScreenColor` 采样口径）。
- bevy：无 SSR（`SSR-RESEARCH-NOTES.md` §2 实锤缺位，本轮缺位照记）。
- Carrot：`resources/shaders/post-process/merge-lighting.slang`（全屏合成 pixel pass 结构 = SSR 合成落点参照；无 SSR 本体）。
- 本仓：`hiz.slang`（M6.0 径向距离 max 金字塔全貌——同像素视线单调论证见 `SSR-RESEARCH-NOTES.md` §3，容差用世界空间距离表达）+ `gbuffer_common.slangh`（反解契约 `gbInterpolateVisTriangle`）+ `scene.hpp/cpp`（hdrSceneColor_ RGBA16 + hdrSceneFb_ + velocity_ + blit/tonemap 链；**hdrSceneColor_ UAV 全替换有先例**（cache viz，scene.hpp §603/608 注释））。

**落码工程口径（全部有明文先例，零新拍板）**：
1. march 对齐 UE：固定步长 + mip 按粗糙度 + 容差带 + bUncertain；复用 S6 径向距离金字塔做 Hi-Z 加速（笔记 §3 论证成立）。
2. 法线/粗糙度 = **vis buffer 即取即算**（S16-PLAN §58 明文「SSR 亦依赖此口径」，Q4 冻结不新增属性纹理）；csSSR 绑 vis buffer 全家桶 + material 间接表，与 direct.slang 同构。
3. 场景色 mip 链：hdrSceneColor_ 建 mip levels + 均值 down（笔记 §3.1 点名「颜色是平均语义」）；若 NVRHI 双后端无统一 generateMips 则自写 csSSRMip（均值 2x2）。
4. 合成：march 输出独立 `ssrColor_`（反射色+强度）→ csSSRCompose 加权写回 hdrSceneColor_（UAV 全替换先例）；插点 = T0 链 tonemap 之前。
5. 第一版确定性：静态相机 + 无 jitter（笔记 §1 时域结论）；步数 8/12/16 档；`--ssr` 缺省关，关时冻结基线 bytes。
6. 零漂移闸：SSR 关 = 全链 bytes（s8.2/s10/s11/s12 spot）；SSR 开新卡 = water-t0（`run.bat verify` + `regression.py --only water-t0` 双重机械校验，`--check` 不覆盖新卡）。

**M-WAT0 文件清单**：`renderer/shaders/ssr.slang`（新）+ `scene.hpp/cpp`（ssrColor_/mip 链/双后端 PSO/drawSSR/`--ssr`）+ `renderer/CMakeLists.txt`（shader 编译项）+ `example/water-t0/`（卡 + run.bat）+ `example/cases.json`（python json 追加）+ `docs/water/WATER-TOOLS-GUIDE.md`（收口时）。

---

### 9b. M-WAT1 落码任务书（2026-11-16 前实读定稿；拍板 A 已写回 §5/SHADER-CONVENTIONS §2.23）

**三处实读结论（M-WAT1 点名文件）**：① UE Water 插件 `WaterVertexFactory.{h,cpp}` + `SingleLayerWater*.ush` —— SLW 是着色合成（M-WAT2 域），顶点位移在 vertex factory/WPO 上游，法线进 gbuffer 供全链；② osgw `gerstner.glsl`（多波显式数组：D/A/k/Q/ω 分量式）+ iago `ocean_shader.{vert,frag}`（fBm 递推 + 黄金角波向 + 解析法线求和定义在**未位移网格坐标** P 上 + pixel-footprint 抗锯齿因子）——M-WAT1 取 osgw 显式数组结构（任务书"波形参数 CPU 侧结构"字段对齐）+ iago 的 per-pixel 解析法线口径；③ 本仓 initCull/initVisbuf/cull.slang/direct.slang/visbuf.slang 全链 —— pulling = compute vertex pulling（vis=64b：deviceZ|clusterId24|triId8），clusterMeta 槽 3 = materialId，cull 用 clusterSpheres frustum 判定。

**接线（拍板 A）**：水网格 = initCull 内 CPU 程序化 grid（64x64 quad 覆盖 [-20,20]²，y=0 未位移平面）切 ≤128 tri 伪 cluster 追加进 vertexPool_/localIndices_/clusterMeta_/clusterSpheres_（radius 含 maxAmp 防位移出锥误剔），materialId = `kWaterMaterialId`（0xFFFFu 保留标记，GPUMaterial 表不占位）；`waterEnable` 布尔沿 init 参数链 viewer→backend→initVisbuf→initCull，缺省关 = 网格不追加、visIndex 段位空置。位移与法线 = `renderer/shaders/water_common.slangh`（新头，单源）：visbuf.slang `vsVisRaster` 位移（深度真实位移→SSR 命中面正确）+ direct.slang `csDirect`/`csDirectVsm` 水体分支（per-pixel 重算位移+解析法线+水体直光）+ ssr.slang 起点法线水体分支（M-WAT0 反解链零改动）。波形 CB = `WaterConstants`（新 volatile CB，8 波 × [D.xy,A,k / ω,Q] + time，160B），visbuf b2 / direct/ssr b7。时间 = `--water` 开时 viewer 固定步进（frameIdx × `GameClock::kStep`），FrameParams.waterTime 缺省 0。水体直光（M-WAT1 域）：深水基色 + N·L 漫反射 + 太阳 Blinn-Phong 高光（iago water_lighting_simple 口径，sky 反射/折射/泡沫留给 M-WAT2）。

**M-WAT1 文件清单**：`renderer/shaders/water_common.slangh`（新）+ `visbuf.slang`（VS 位移）+ `direct.slang`（水体分支 ×2 入口）+ `ssr.slang`（起点法线分支）+ `scene.{hpp,cpp}`（网格追加 + WaterConstants + 绑定）+ `renderer.hpp`（init 参数 + FrameParams.waterTime）+ `backend_dx12/vulkan.cpp`（参数透传）+ `viewer/main.cpp`（`--water`）+ `CMakeLists.txt`（water_common DEPS）+ `example/water-t1/`（卡 + run.bat + 基线）+ `example/cases.json`。SM66 白名单不动（无 NonUniformResourceIndex 新增）。

---

## 9c. M-WAT2 开工实读与任务书（2026-11-14，claim 后三处重读）

> claim：`wt_pool.py claim --track water --milestone M-WAT2`（2026-11-14 17:42）；wt `nl-engine-water` 复用（feat/water HEAD `113a766` = M-WAT1 台账收官）。

**本轮实读（模块 = 折射/吸收/泡沫，一一对应）**：
- UE（权威）：
  - `SingleLayerWaterRefractionCulling.usf` **全文**：`RefractionMaskPS` 按距离/Fresnel/深度差三段 smoothstep 渐隐折射（性能闸，首版不做 mask 渐隐，记升级路径）。
  - `SingleLayerWaterShading.ush` §60~250：折射主链——`IorWater = DielectricF0ToIor(Specular)`；扭曲 UV = `ComputeBufferUVDistortion`（法线+IOR）→ **折射量按水厚衰减** `BufferUVDistortion *= saturate((BehindDepth - WaterDepth) / 30)`；`WaterVolumeDepth = max(0, BehindDepth − WaterDepth)`（bilateral 从非水邻域估计）；**Beer-Lambert**：`Transmittance = exp(−ExtinctionCoeff × WaterVolumeDepth)`、in-scatter `SafeScatteringAmount = saturate(Scattering × (1−Transmittance) / ExtinctionSafe)`、最终 `Luminance = Scattered + Transmittance × BehindLuminance`（吸收与背景色分离合成）。
  - `SingleLayerWaterComposite.usf`：**水色合成在独立 composite pass**——`Reflection = SSR.rgb + ReflectionCaptures × (1−SSR.a)`（SSR 与环境反射混合先例），全 pass 对水像素整体替换。
  - `SingleLayerWaterRendering.cpp`：`SceneWithoutWater` = **copy 主 scene color/depth**（水不写主 scene color 的前提下零成本）；`RefractionMask` = R8 独立 RT。
- bevy：`crates/bevy_pbr/src/transmission.wesl` **全文重读**：`refracted_ray()` Snell（`eta = 1/IOR`，k 判全内反射）→ 出射点 `world + T × thickness` → clip → 屏幕 UV；**深度拒绝** = 采样点 prepass depth 与 frag z 比较，拒绝则 `a=0` 回退；roughness spiral blur（多 tap）首版不做。
- Carrot：无水体（§2 已实证留痕，本轮不重读）。
- 本仓（现状接线核实）：`material.slang` mode 5 水分支（218/256~273：waterGerstner + waterShadeDirect 直光色直接 return）+ `direct.slang` csDirect/csDirectVsm 水分支 + `water_common.slangh`（WaterConstants 288B：wcA/wcB[8] 波参 + wcC time/baseColor + wcD ambient/specPow/specGain/maxAmp）+ `ssr.slang`（csSSRCompose 写回 hdrSceneColor_ 先例）。

**核心发现（形态判定根因）**：折射需要「水像素背后的场景色」。UE 能 copy 主 scene color 当无水体场景，是因为**水色延迟到独立 composite pass 才写**；本项目 M-WAT1 已把直光水色写进 hdrSceneColor_（psMatResolve mode 5 / csDirect 分支）→ 水像素位置背景 miss，折射无处采样。**这是 M-WAT2 接线形态的架构判定，落码前必须拍板（Q-WA7）**。

### Q-WA7（已拍板 2026-11-14）：折射合成形态 = **A · UE 同构合成后移**（用户拍板，方案矩阵留痕）

| 方案 | 做法 | 代价 / 影响面 | 结论 |
|---|---|---|---|
| **A · UE 同构合成后移**（建议） | psMatResolve/csDirect 水分支改**黑占位**（不再写直光水色）；SSR compose 后 copy 一份 `sceneWithoutWater_`（水像素黑=天然无水体场景，同 UE copy 口径）；新增 `csWaterComposite`（SSR 后 tonemap 前）反解 vis buffer 判水像素：扭曲 UV 采背景 + 深度拒绝（bevy 口径）+ Beer-Lambert 吸收（厚度 = bilateral 非水邻域估计）+ Fresnel 混合 ssrColor_ 反射 + 直光 specular + Gerstner 波峰泡沫，整体替换水像素写回 | 改动 M-WAT1 收官两处水分支（直光逻辑迁移进 composite）；**water-t1 基线必变须重拍**；新增 1 RT + 1 copy pass + 1 compute pass；CS 绑定面与 csSSR 同构 | 唯一与 UE pass 链同构；折射背景保真；水色单一出口（pass 链完整合成 = 计划书 M-WAT2 验收原文） |
| B · psMatResolve 内嵌折射 | 保持 M-WAT1 形态，折射在 mode 5 水分支内做 | **不可行**：水像素场景色已是水色，背景色无源可采；MRT 第二目标也拿不到背景（反解到水伪簇） | 否 |
| C · 真重渲无水体场景 | 单独 raster+resolve 跳过水体渲背景 RT | 整条 vis buffer 链 ×2，UE 都用 copy 不重渲 | 否（成本无理由） |

**方案 A 落码口径**（拍板后细化，先记骨架）：
1. 占位对称改动：`material.slang` mode 5 水分支 + `direct.slang` csDirect/csDirectVsm 水分支 → 输出黑（保留 Gerstner 计算移进 composite）。
2. `water_common.slangh` 新增单源：`waterRefractDir`（Snell，bevy 口径）/ `waterAbsorb`（Beer-Lambert，UE 公式）/ `waterFresnel`（Schlick）/ 泡沫判据（Gerstner 偏度/陡度，iago 口径）。
3. WaterConstants 扩展（吸收系数 RGB / IOR / 折射强度 / 泡沫阈值）→ wcE 起新槽；**CB 尺寸以 `dxc -dumpbin` 反汇编为准**（M11.0 纪律），CPU 数组长度 = 字节/4。
4. `csWaterComposite` 绑定：vis buffer 全家桶 + clusterMeta/vertexPool/localIndices（几何反解拿未位移 worldPos → per-pixel Gerstner）+ WaterConstants + 主 depth + sceneWithoutWater_ + ssrColor_；新 binding layout（每 PSO 独立 desc，M-WAT0 先例），不动 refComputeLayout_ 六槽。
5. 确定性：全 float 同源（GameClock::kStep 时间），双后端 bytes 目标；bilateral 深度估计为固定 3×3 邻域确定性行走（无动态分支）。
6. 零漂移闸：`--water` 关 = 冻结基线 bytes 不变；water-t2 新卡 = `run.bat verify` + `regression.py --only water-t2`；**water-t1 基线重拍**（方案 A 画面必变，cases.json 基线 PNG 更新 + 台账记录重拍原因）。
7. 泡沫：首版 Gerstner 陡度判据（Q·A·k 峰值邻域白化），Jacobian 全量判据列进阶。

**M-WAT2 文件清单**：`material.slang` + `direct.slang` + `water_common.slangh` + `ssr.slang` 或新 `water_composite.slang` + `scene.hpp/cpp`（sceneWithoutWater_ RT + copy/composite 调度）+ `CMakeLists.txt` + `example/water-t2/` + `water-t1` 基线重拍 + `cases.json` + 台账。

**拍板结论（2026-11-14 用户拍板 Q-WA7 = A）**：折射合成走 UE 同构后移——水像素黑占位 + `sceneWithoutWater_` copy + `csWaterComposite` 统一合成；water-t1 基线重拍授权随拍板生效（台账记录重拍原因）。B（背景无源不可行）/ C（成本无理由）否决留痕。落码开始。
**拍板结论（2026-11-14 用户拍板 Q-WA7 = A）**：折射合成走 UE 同构后移——水像素黑占位 + `sceneWithoutWater_` copy + `csWaterComposite` 统一合成；water-t1 基线重拍授权随拍板生效（台账记录重拍原因）。B（背景无源不可行）/ C（成本无理由）否决留痕。落码开始。

**实现注记（2026-11-14 落码中实锤，§6.2 口径修正）**：
- **方案 A 盲区实锤**：池底在 vis buffer 中被水面深度遮挡 → copy 后水像素背后无内容（UE SceneWithoutWater 是真重渲才有水后物体；copy 只有水外环带/天空/水上物体可采）→ 陡视角水像素 3×3 邻域全水 → bilateral 全拒 → 折射只剩 in-scatter，§6.2「水下物体折射扭曲可见」在任何相机都不成立。
- **盲区补全（零新 pass/零新资源）**：池底是已知平面 → composite 内 B1 解析 ray-plane（y=wcG.y，命中 [-floorHalf,floorHalf]² → 命中点 xz×0.25 wrap 双线性采材质 0 贴图，thick = 真实光路程 → Beer-Lambert 深浅梯度天然成立）；B1 未命中（掠射出池）走 B2 屏幕空间扭曲 4-tap 兜底（原方案机制保留）。平面参数走 wcG.y/z（单源 kWaterFloorY/Half）。
- **折射 A/B 硬闸旗标**：`--water-norefract`（wcG.w）——折射路径整体关闭（bg 归零、厚 fallback），refract = in-scatter only；run.bat 加 full vs norefract DIFF 检查（与 §6.1 noreflect 对称）。
- **验收机位**：t2 主 case 维持 t1/t0 相机（pitch9/dist7，零漂移闸直接比 t0 基线 bytes）；池外机位（pitch8/dist30）另拍折射/环带证据图（池底贴图穿透水面可见）。

## 9d. M-WAT3 开工实读与任务书（2026-11-14，claim 后实读）

**三处实读**：
- **UE SLW（官方文档 4.26/4.27 + 论坛 5.5 实测）**：SLW = 均匀参与介质体积（Schlick 相函数）+ 表面 BSDF/BRDF 由 Opacity 混合；岸线软化官方路数 = **`Distance To Nearest Surface` 节点接 Opacity lerp**（近水面物体自然淡入），另一路 = Absorption 系数按深度调节；`Color Scale Behind Water` = 水后色度第 4 输入；折射读取前可选 downsample（waterBg copy 已同构）。
- **bevy**：SSR example 水体 = 4 张法线图叠加波纹 + deferred SSR（`water_material.wgsl`），SSR `edge_fadeout` 是屏幕边缘淡出与水体无关——**bevy 无水体岸线软化口径**，M-WAT3 纯 UE 口径。
- **本仓现状（M-WAT2 实测）**：csWaterComposite 整体替换水像素 → 水岸交界（池底环带/猴头穿出水面）为硬边；原位 3×3 bilateral 已算非水邻域径向差 diff（岸边邻域 diff 小），**复用即可得 fade，零新 pass/零新资源**。

**M-WAT3 方案骨架（待拍板范围）**：
1. **岸线软化 soft fade（核心）**：`fade = saturate(diff / kWaterFadeDist)`；diff = bilateral 邻域平均径向差（ok0）或扭曲 tap 加权差（B2 命中），ok0=false 且 B2 未命中 = 深水 fade=1。输出 `color = lerp(bg0, waterColor, fade)`（bg0 = bilateral 原位邻域色；ok0=false 直接 waterColor）。水岸硬边 → 按「距岸距离」渐变过渡，UE DistanceToNearestSurface 同构。
2. **岸线泡沫（可选）**：diff 窄带（如 < 0.8m）加白沫（复用 waterFoam，阈值换成 diff 判据），UE WaterBody 岸线泡沫同构。
3. **Color Scale Behind Water（可选）**：refract 的 bg 乘 wcH 色度系数（UE 第 4 输入同构，调水后色偏）。
4. **CB 扩展**：wcG 已满 → 新增 wcH float4（fadeDist / shoreFoam 参数 / behindWaterScale.rgb），336→352B（22×float4），CPU 数组 = 字节/4。

**验收口径（草案）**：水岸交界渐变可见（硬边消除）为软闸（截图对比）；bytes 零漂移 + 双旗标 A/B + verify 全套沿 M-WAT2 口径；跨后端 near 档。

**拍板结论（2026-11-14 用户拍板）**：**A+B**——岸线软化（fade = saturate(diffNear/fadeDist)，复用 bilateral 邻域 diff）+ 岸线泡沫（diff 窄带 shoreBand² 白沫）；C（Color Scale Behind Water）不做。**water-t1/t2 基线重拍授权随拍板生效**（composite 输出必变：岸边窄带像素 fade lerp）。参数 wcH = (fadeDist=1.2m, shoreFoamDist=0.6m, shoreFoamGain=0.7, noShore 旗标)；CB 336→352B（22×float4）。落码开始。
