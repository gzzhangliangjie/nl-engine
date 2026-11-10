# S9 详细开发路线图：GBuffer 公共反解 + 直光 / 阴影射线 + 离线参考模式

> 对应总路线 `docs/ROADMAP.md` 的 S9。预计 3 个里程碑、业余时间约 1 周。
> 两件事：① 每像素 1 条阴影射线的硬阴影直光（Lumen 的直光层）；② **64+ 采样/像素的离线参考模式**——产出「直光 / 硬阴影标准答案」，供 M9.1 阴影射线与 VSM（S15）对照。**只算直光一跳，不是 GI 裁判**（Q3 拍板）。
>
> **2026-11-10 开工重定义**：原计划 M9.0 的「直光框架」在 S7 / `pbr-direct` 穿插项里已实现（`material.slang` 的 mode 5 = vis buffer 反解 + Cook-Torrance GGX 直射光，`LightConstants` 80B + 材质间接表），照字面重做属重复劳动。三项用户拍板见文末「开工拍板记录」，本文件据此重写验收判据。

---

## M9.0 GBuffer 抽取 + 公共反解头（1 天）

**范围**：Q1（重定义为抽取）/ Q4（方案 A 不落盘）/ 无阴影版实现路径 ②（新建 compute 直光 pass）均已拍板。交付 = 公共反解契约 + 一条 compute 直光对照路径 + 保形证据；**不新增独立 GBuffer 纹理 / RT / UAV 槽位**，落盘形态推迟到 S10 与 S12 历史帧需求一起拍（依据与 A/B/C 代价矩阵见 `docs/s9/Q4-GBUFFER-OPTIONS.md`）。

### 任务书

| # | 任务 | 状态 |
|---|---|---|
| T1 | `gbuffer_common.slangh`：把 `material.slang::psMatResolve` 的 vis buffer 几何反解（vertex pulling → 透视正确 barycentrics → worldPos / normal / uv / uv 导数）**逐字搬移**进公共头，禁 branch / lerp / 代数改写（M-PT2/PT3 保形铁律） | ✅ |
| T2 | 公共头同时提供 `gbReconstructWorldPos(pixel, deviceZ, size, invViewProj)`，与 `rt.slang::csRt` 同源（perspectiveZO，z=0 近 / z=1 远） | ✅ |
| T3 | `CMakeLists.txt` 显式登记 `_GBUFFER_COMMON` 依赖（防改公共头不重编的假绿）；双后端构建 + selftest + 抽查回归 | ✅ |
| T4 | 新增 `--mode shadow` 开关：**枚举 / `core/src/modes.cpp` 注册表 / `viewer/main.cpp` `runMode()` 与模型白名单 / 两后端 `backend_{dx12,vulkan}.cpp` 分发**（该 mode 当前不存在，注册表末位是 `rt`）。无阴影版走 compute 8×8 直光 pass：`initVisbuf(..., pulling=true)` 产 vis buffer → `gbInterpolateVisTriangle` 即取即算 + **逐字搬移 mode 5 BRDF** → 写输出 → present；**不发阴影射线**。`kStage` **不 bump**（现有卡都不传新 mode，HUD 基线 0 改动） | ⬜ |
| T5 | 按 §2.7 给该 compute pass 建**单一 BindingSet**；槽位与状态登记**以实读代码为准**（`material.slang:39-72`、`scene.cpp:2171-2177`），固化进 `docs/SHADER-CONVENTIONS.md` §2.18。**任务书不预设槽位号**。输出复用 M8.2 的 `rtTarget_` 纹理对象（`RGBA32_FLOAT`，**不新建资源**）与 `psRtPresent` 呈现骨架；为此需把 present shader / layout / PSO 与 UAV 纹理创建从 `initRt` / `ensureRtTargets` 抽成两条链共用，并把依赖 TLAS 的 `rtComputeSet_` 创建留在 `drawRt` —— **纯结构重构，不改任何资源格式 / 槽位 / 画面语义**，`s8.2/rt-dist` 回归必跑 | ⬜ |
| T6 | 对照卡：`--mode shadow` 无阴影版 vs mode 5 **逐像素一致**；比对口径写清（同机位同参数两张 PNG、空像素写清屏色 `(0,0,0,1)` 对齐 `discard` 语义、`RGBA32_FLOAT` UAV 原值 → `psRtPresent` 输出 → `RGBA8_UNORM` framebuffer 只有一次 float→UNORM 转换、present 的 UV 取像素与 `SV_Position` 对齐）；**对照卡锁定 tag `s9-gbuffer` 的 exe**，M9.1 起同 mode 语义变更不重跑 | ⬜ |
| T7 | 交付物：`example/s9-gbuffer/` 基线 + `run.bat` + `cases.json` 卡 + `server.py --check` 退出 0 | ⬜ |
| T8 | 收口：四栏台账写实 + `wt_pool.py close` 硬闸 + tag `s9-gbuffer` | ⬜ |

**硬验收三条（原样保留，不因方案 A 缩水）**：① 抽取前后 `s7.2` / `pbr-direct` / `s8.2` 抽查基线双后端 `bytes` 不变（已过）；② 反解数学逐字搬移、禁 branch / lerp 改写（已过）；③ `--mode shadow`（**M9.0 阶段、无阴影版**）与 mode 5 逐像素一致（待 T4 / T6；对照卡锁定 tag `s9-gbuffer` 构建产物）。tag：`s9-gbuffer`（T1~T8 全绿后打）。

### 边界（防再写歪）

- **决策**只在文末「开工拍板记录」，**实测数字**只在「实测留痕」，**任务**只在上表；三者不互相塞。
- M9.0 不实现 M9.1 的射线（tMax / bias / 粗代理差异可视化见下一节）。② 的 compute 直光 pass 是**新管线**，按 §2.7 必须有自己的单一 BindingSet —— Q4 冻结的是「**独立 GBuffer 落盘资源**（新增纹理 / MRT / 落盘 UAV 的类型、格式与形态）」，**不是**这条管线的必备绑定集，也不是复用 M8.2 输出目标所需的纯结构重构；仍禁止新增任何独立 GBuffer 纹理 / MRT / 落盘 UAV。「不新增 RT / UAV」一律按此口径解读：`rtTarget_` 是 M8.2 已有成员，复用它不算新增资源。
- 落码前先核「落码前实读事实」，禁止再凭记忆编槽位表。

### 实测留痕（2026-11-10 第一铲）

- 双后端 `viewer --selftest` 16/16 ×2、`Slang 单源 64/64`；`material.psmatresolve.spv 17188B` / `.dxil 14884B` 重编。
- `s7.2/pulling`、`pbr-direct/pbr`、`pbr-direct/textured`、`s8.2/rt-dist` 四条 × DX12/VULKAN = **8/8 `bytes`**（near 0 / FAIL 0，报告 `build/regression/m9-spot-*.md`）。`psMatResolve` 字节码因内联改函数调用 DIFF，画面逐字节保形 → 逐字搬移成立；VS 两目标逐字节不变。
- 踩坑：只改 shader / CMake / 文档时 DX12 `viewer.exe` 未重链、mtime 落后 `material.slang` 37 分钟，触发回归闸 exe 新鲜度门禁；删 exe 强制重链解决（VULKAN 因 camera cpp 重编已重链）。

### 落码前实读事实（2026-11-10 补）

| 事实 | 出处 |
|---|---|
| `--mode shadow` / `reference` **当前不存在**于 `kModeNames`（末位 `rt`）；新增开关要改枚举 + 注册表 + 分发 + viewer 白名单 | `core/src/modes.cpp:8-22`、`viewer/main.cpp:2487-2491` |
| `rt` 走 `initRt → initDag`（BLAS/TLAS 链），**不产出 vis buffer**；`pulling` 走 `initVisbuf(..., pulling=true)`；两条链互斥 | `scene.cpp:2129`、`1501` |
| `rt` compute set 实际只有 3 项：b0 `RtConstants` / t0 TLAS / u0 `RGBA32_FLOAT`；present 端 t0 SRV | `scene.cpp:2171-2177`、`rt.slang:23-36,118` |
| mode 5 实际绑定：b0 `SceneConstants` / b1 `LightConstants` / b2 `MatResolveConstants` / t0 vis / t1 vertexPool / t2 localIndices / t3 clusterMeta / t4..t7 baseColor / t8..t11 mrTex / t12 `GPUMaterial` / s0 sampler | `material.slang:39-72` |
| 上一版任务书把 `rt` 的 t0(TLAS) 与 `material` 的 t0(vis) 混编成一张槽表、漏了贴图数组 / 材质表 / sampler / b1 / b2 → **已作废**；槽位以 §2.18 落码实读为准 | 本轮纠偏 |
| `rtTarget_` / `rtPresentSet_` 在 `ensureRtTargets` 懒建，但同函数里 `rtComputeSet_` 绑定 `tlas_`；`rtPresentLayout_` 与 `vsRtPresent_` / `psRtPresent_` 只在 `initRt` 创建。**vis 链下这些对象都拿不到** → T5 的纯结构重构是 T4 落地前提，不是可选项 | `scene.cpp:2154-2192, 2200-2245, 2248` |

## M9.1 阴影射线（1~2 天）

> **mode 语义分阶段**：M9.0 的 `--mode shadow` = compute 直光、**不发射线**（硬验收对照用）；M9.1 起同一个 mode 默认每像素发 1 条 shadow ray。硬验收第三条的对照卡以 tag `s9-gbuffer` 的 exe 为准，不要用 M9.1 之后的构建重跑。

- [ ] `--mode shadow`：直光采样点每像素发 1 条 shadow ray（朝光源方向），被挡→0，否则→BRDF
- [ ] 射线走 M8 的 TLAS；透明材质忽略（S9 不处理 alpha）
- [ ] **几何差异留痕**：BLAS 吃 S3 fallback 粗代理（Suzanne 104 tri），光栅主帧是最细 LOD（3936 tri），两者轮廓本来就不重合（UE-DIFFS D19）；另出一张「光栅几何 / 射线代理几何」差异可视化图，防 review 误判为 bug

**自检（按拍板改写）**：**在粗代理几何上**硬阴影边缘干净；光源方向旋转阴影正确跟随；区域级遮挡判定正确；猴耳 / 眼窝等细模细节的影子与画面对不上属已知口径，不判 FAIL。几何对齐按 ROADMAP 原排期留 **S13 簇级 BLAS 聚合**，不在 M9 提前做。tag：`s9-shadow`。

⚠️ 常见坑：
- shadow ray 的 tMax 设到光源距离即可，别无限远（性能与正确性双赢）
- 自相交（shadow acne）：ray query 的 origin 沿法线偏移（bias）要用**世界尺度**的经验值 + 光源方向点积调；UE `LumenSceneDirectLightingHardwareRayTracing.cpp` 的 `HardwareRayTracingShadowRayBias` 注释明确「防 proxy geo 自遮挡」，与本项目粗代理处境同源（只读对照，未抄代码）
- 射线命中结果来自 BLAS 几何，不是屏幕上的可见簇——M8.2 `s8.png` 口径注记沿用

## M9.2 离线参考模式（2 天）

- [ ] `--mode reference`：每像素 64+ 条**余弦重要性采样**射线求平均降噪（64 是采样条数，**不是反弹次数**），只算**直光一跳**，离线积分写 PNG 存盘
- [ ] 分块/进度显示（一次性离线跑，几十秒可接受）
- [ ] 存盘带相机参数元数据（同机位可重跑）

**自检（S9 验收）**：硬阴影正确；直光标准答案噪声极低（放大 400% 无明显颗粒）。这张图是 **M9.1 阴影射线与 VSM（S15）的直光 / 硬阴影裁判**，**不是 GI 裁判**——只算一跳的图里没有间接光，拿它量 S10~S12 的 GI 会「越调准反而差越远」。GI 的标准答案待 S10 之后另立。tag：`s9-done`，存 `docs/screenshots/ref_*.png`。

---

## 时间线与 tag

| 里程碑 | 预计 | tag |
|---|---|---|
| M9.0 GBuffer 抽取 + 公共头 | 1 天 | `s9-gbuffer` |
| M9.1 阴影射线（粗代理口径） | 1~2 天 | `s9-shadow` |
| M9.2 直光参考模式（一跳） | 2 天 | `s9-done` |

## 卡住翻哪

| 问题 | 翻 |
|---|---|
| compute 直光 / 射线 pass 的写法 | `Carrot/.../shaders/lighting/direct-raytracing.slang` + `direct-base.slang`（8×8 dispatch、`sampleLights` 抽象）。**只借 compute 组织方式**：方案 A 下不落 GBuffer，属性由公共头即取即算，别照着去找 `PixelInfo` 的属性纹理 |
| GBuffer 重建 / 打包属性 | bevy `solari/realtime/gbuffer_utils.wesl`（`gpixel_resolve` / `reconstruct_world_position`） |
| vis buffer 反解插值 | bevy `meshlet/visibility_buffer_resolve.wesl`（透视正确 barycentrics + 像素导数） |
| 阴影可见性射线 | bevy `solari/realtime/initial_path.wesl`（`trace_visibility` + `RAY_T_MIN/MAX`） |
| 采样与积分 | kb/graphics-handbook.html#path 的 G8（路径追踪器里程碑，同一套数学） |
| BRDF 细节 | kb/index.html#lum（02 章直光部分）；PBRT 第 14 章（当词典） |

## 底线

- **GBuffer 反解函数进公共头**：S10/S12 全靠它 —— M9.0 T1/T2 已落地（`gbuffer_common.slangh`，0 资源声明）
- 参考图工作流从今天建立：**调直光 / 阴影时同机位并排对比标准答案**；GI 调参的裁判另立，别拿直光尺子量间接光

---

## 开工拍板记录（2026-11-10，用户）

| 编号 | 问题 | 拍板 | 依据 |
|---|---|---|---|
| Q1 | M9.0 范围：S9-PLAN 字面「直光框架」已被 S7 / `pbr-direct` 实现 | **重定义为 GBuffer 抽取 + 公共反解头**，直光只做一致性对照 | `material.slang` mode 5 已有 vis 反解 + GGX；真缺口是**反解数学焊死在 `psMatResolve` 单个全屏 pass 里、没有可被 M9.1 / S10 / S12 复用的公共契约**。Q4 拍板 A 后确认缺的不是「独立 GBuffer 属性纹理」 |
| Q2 | 阴影射线几何：BLAS fallback 104 tri vs 光栅最细 3936 tri | **接受 proxy 差异并留痕**，改写验收判据 + 差异可视化图 + UE-DIFFS D19；几何对齐留 S13 | S3 fallback = 2.6%（UE `NaniteBuilder` 默认 ~2%）；UE Nanite RT 亦用 proxy mesh；S8.2 `s8.png` 已登记「轮廓不重合是设计本意」 |
| Q3 | 参考图积不积间接光（标题「GI 裁判」vs 正文「直光一跳」自相矛盾） | **只算一跳**，定位为直光 / 硬阴影标准答案，标题勘误；GI 尺子 S10 后另立 | **拍板时** `s10/S10-PLAN.md` M10.3 曾写「与 S9 参考图并排对比」，但一跳图里没有间接光，量不了 GI；该旧句已于 2026-11-10 18:1x 全仓勘误为「与 S10 自立的 GI 参考图并排」 |
| Q4 | **GBuffer 落盘形态**（MRT 几张 / packed UAV、compute vs 全屏 pass、绑定布局） | **已拍板：方案 A「不落盘，即取即算」**（2026-11-10 用户） | 三处实读：Carrot 7 张 MRT、bevy meshlet 不落盘 / solari 单张 packed uint4、UE GBuffer ≤8 槽 MRT 而 **Lumen HWRT 直光不读屏幕 GBuffer**（读 Surface Cache atlas）；本项目 vis buffer 即紧凑 GBuffer，公共头已抽走数学契约。落盘的真实需求方是 **S12 历史帧属性**，推迟到 S10 与 temporal 需求一起拍。A 下 M9.0 硬验收（8/8 `bytes`）零风险；B 的量化会破「mode 5 逐像素一致」、C 可能触发 ~50 张 HUD 基线重拍。决策材料与三方案代价矩阵见 `docs/s9/Q4-GBUFFER-OPTIONS.md` |

### Q4 拍板后动作（2026-11-10）

1. 结论已写回上表 Q4 行；M9.0 任务书按「任务 / 决策 / 实测 / 边界」四层重写为 T1~T8，删掉「落盘形态待用户拍板」挂账句与上一版编造的绑定槽位表；硬验收三条原样保留。
2. **冻结边界（澄清）**：禁的是「独立 GBuffer 落盘资源（新增 RT / UAV / 纹理 / 形态）」；compute 直光 pass 按 §2.7 必需的**单一 BindingSet 不在此列**，槽位以 §2.18 落码实读固化。
3. 待办 T4~T8 见任务书表；无阴影版实现路径按用户拍板 ②（新建 compute 8×8 直光 pass，逐字搬移 mode 5 BRDF，不碰 `material.slang`）。
4. 落盘形态推迟 S10 与 S12 历史帧需求一起拍；本里程碑**不新增任何独立 GBuffer 纹理 / MRT / 落盘 UAV**（compute 输出复用 M8.2 `rtTarget_` 成员，见 T5）。

**前置实读清单（本轮，符合 AGENTS.md 决策前置铁律）**：Carrot `lighting/direct-raytracing.slang` + `direct-base.slang`；bevy `meshlet/visibility_buffer_resolve.wesl` + `solari/realtime/gbuffer_utils.wesl` + `initial_path.wesl`；UE 5.8.4（对照总表 S9 行未点名，按功能就近读）`Lumen/LumenSceneDirectLighting.cpp` + `LumenSceneDirectLightingHardwareRayTracing.cpp`；开发历史 `MILESTONES.md`（`s8-done` 收官段）/ `carrot-history.md` / `bevy-history.md` / `.workbuddy/memory/MEMORY.md` / `2026-11-10.md` / 本计划 / `SHADER-CONVENTIONS.md` **§2.5 / §2.7 / §2.15 / §2.16**（Q4 决策材料 §2 实读；§2.18 待 T5 固化）/ `UE-DIFFS.md` 对齐确认节。
