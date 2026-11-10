# S9 详细开发路线图：GBuffer + 直光 + 阴影射线 + 离线参考模式

> 对应总路线 `docs/ROADMAP.md` 的 S9。预计 3 个里程碑、业余时间约 1 周。
> 两件事：① 每像素 1 条阴影射线的硬阴影直光（Lumen 的直光层）；② **64+ 采样/像素的离线参考模式**——它产出的「直光 / 硬阴影标准答案」是 M9.1 阴影射线与 VSM（S15）对照的裁判。
>
> **2026-11-10 开工重定义**：原计划 M9.0 的「直光框架」在 S7 / `pbr-direct` 穿插项里已实现（`material.slang` 的 mode 5 = vis buffer 反解 + Cook-Torrance GGX 直射光，`LightConstants` 80B + 材质间接表），照字面重做属重复劳动。三项用户拍板见文末「开工拍板记录」，本文件据此重写验收判据。

---

## M9.0 GBuffer 抽取 + 公共反解头（1 天）

- [x] 把 `material.slang::psMatResolve` 的 vis buffer 几何反解（vertex pulling → 透视正确 barycentrics → worldPos / normal / uv / uv 导数）抽进 **`renderer/shaders/gbuffer_common.slangh`** 公共头，表达式**逐字搬移、禁 branch/lerp 改写**（M-PT2/PT3 保形铁律），保证 S7 全部基线逐字节不变
- [x] 公共头同时提供 `gbReconstructWorldPos(pixel, deviceZ, size, invViewProj)`（bevy `gbuffer_utils.wesl` 口径），供 S10 / S12 的 GBuffer 重建复用
- [x] **Q4 已拍板：不新增独立 GBuffer 落盘**（方案 A，2026-11-10 用户）。M9.1 阴影射线在 **compute 8×8 单 pass** 内调 `gbInterpolateVisTriangle` 即取即算 → 发 shadow ray → 写 M8.2 已有的 `RGBA32_FLOAT` typed UAV；不写任何 RT / UAV 新增槽位。**落盘决策推迟到 S10 与 S12 历史帧属性需求一起拍**，届时 A/B/C 三方案代价矩阵见 `docs/s9/Q4-GBUFFER-OPTIONS.md`；写入端可复用本公共头，返工面只在资源层
- [x] 直光只做**一致性对照**，不重写 BRDF：公共头反解与旧内联路径的等价性已由 `s7.2/pulling` / `pbr-direct/pbr` / `pbr-direct/textured` / `s8.2/rt-dist` 双后端 8/8 `bytes` 证明；M9.1 compute 直光路径上线时再与 mode 5 做逐像素对照
- [ ] `--mode shadow` 的无阴影版先跑通（= compute 直光路径接上公共头反解，不发阴影射线）；**A 下不依赖任何 GBuffer 纹理**，绑定布局按 `Q4-GBUFFER-OPTIONS.md` 方案 A（扩现有 `rt` 单一 BindingSet：b0 / t0 TLAS / t1 vis / t2 vertexPool / t3 localIndices / t4 clusterMeta / u0 输出，shift 沿用 §2.5）。此项是 M9.0 硬验收第三条，拍 A 后**保留不丢**

**自检**：GBuffer 抽取前后 S7 / pbr-direct / s8.2 抽查基线 bytes 不变；`--mode shadow` 无阴影版与 mode 5 逐像素一致。tag：`s9-gbuffer`。

> **Q4 拍板后的范围口径（2026-11-10）**：方案 A 下 M9.0 的「独立 GBuffer 落盘」取消，交付 = 公共反解契约 + 保形验证 + `--mode shadow` 无阴影版对照卡；落盘形态不在本里程碑写死，S10 开工时与 S12 temporal 需求一并拍板。**硬验收三条原样保留**（见下）。

> **2026-11-10 第一铲实测（纯搬移已验证）**：双后端 `viewer --selftest` 16/16、`Slang 单源 64/64`；`s7.2/pulling`、`pbr-direct/pbr`、`pbr-direct/textured`、`s8.2/rt-dist` 四条 × DX12/VULKAN = **8/8 `bytes`**（near 0 / FAIL 0，报告 `build/regression/m9-spot-*.md`）。`psMatResolve` 字节码因内联改函数调用而 DIFF（spv 17188B / dxil 14884B），但画面逐字节保形，证实 M-PT2/PT3 的「逐字搬移」策略有效；VS 两目标逐字节不变。

## M9.1 阴影射线（1~2 天）

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
| 直光+阴影射线的完整写法 | `Carrot/.../shaders/lighting/direct-raytracing.slang` + `direct-base.slang`（`PixelInfo` 从 GBuffer 取属性 + `sampleLights` 能力抽象） |
| GBuffer 重建 / 打包属性 | bevy `solari/realtime/gbuffer_utils.wesl`（`gpixel_resolve` / `reconstruct_world_position`） |
| vis buffer 反解插值 | bevy `meshlet/visibility_buffer_resolve.wesl`（透视正确 barycentrics + 像素导数） |
| 阴影可见性射线 | bevy `solari/realtime/initial_path.wesl`（`trace_visibility` + `RAY_T_MIN/MAX`） |
| 采样与积分 | kb/graphics-handbook.html#path 的 G8（路径追踪器里程碑，同一套数学） |
| BRDF 细节 | kb/index.html#lum（02 章直光部分）；PBRT 第 14 章（当词典） |

## 底线

- **GBuffer 反解函数进公共头**：S10/S12 全靠它（本轮 M9.0 落地）
- 参考图工作流从今天建立：**调直光 / 阴影时同机位并排对比标准答案**；GI 调参的裁判另立，别拿直光尺子量间接光

---

## 开工拍板记录（2026-11-10，用户）

| 编号 | 问题 | 拍板 | 依据 |
|---|---|---|---|
| Q1 | M9.0 范围：S9-PLAN 字面「直光框架」已被 S7 / `pbr-direct` 实现 | **重定义为 GBuffer 抽取 + 公共反解头**，直光只做一致性对照 | `material.slang` mode 5 已有 vis 反解 + GGX；真缺口是没有独立 GBuffer 属性纹理供 M9.1 / S10 / S12 消费 |
| Q2 | 阴影射线几何：BLAS fallback 104 tri vs 光栅最细 3936 tri | **接受 proxy 差异并留痕**，改写验收判据 + 差异可视化图 + UE-DIFFS D19；几何对齐留 S13 | S3 fallback = 2.6%（UE `NaniteBuilder` 默认 ~2%）；UE Nanite RT 亦用 proxy mesh；S8.2 `s8.png` 已登记「轮廓不重合是设计本意」 |
| Q3 | 参考图积不积间接光（标题「GI 裁判」vs 正文「直光一跳」自相矛盾） | **只算一跳**，定位为直光 / 硬阴影标准答案，标题勘误；GI 尺子 S10 后另立 | S10-PLAN M10.3 写「与 S9 参考图并排对比」，但一跳图里没有间接光，量不了 GI |
| Q4 | **GBuffer 落盘形态**（MRT 几张 / packed UAV、compute vs 全屏 pass、绑定布局） | **已拍板：方案 A「不落盘，即取即算」**（2026-11-10 用户） | 三处实读：Carrot 7 张 MRT、bevy meshlet 不落盘 / solari 单张 packed uint4、UE GBuffer ≤8 槽 MRT 而 **Lumen HWRT 直光不读屏幕 GBuffer**（读 Surface Cache atlas）；本项目 vis buffer 即紧凑 GBuffer，公共头已抽走数学契约。落盘的真实需求方是 **S12 历史帧属性**，推迟到 S10 与 temporal 需求一起拍。A 下 M9.0 硬验收（8/8 `bytes`）零风险；B 的量化会破「mode 5 逐像素一致」、C 可能触发 ~50 张 HUD 基线重拍。决策材料与三方案代价矩阵见 `docs/s9/Q4-GBUFFER-OPTIONS.md` |

### Q4 拍板后动作（2026-11-10）

1. 结论已写回上表 Q4 行；M9.0 任务书第 3 条的「落盘形态待用户拍板」挂账句**已删除**，改为方案 A 口径。
2. 冻结解除范围：仅解除「与不落盘方案相容」的落码；**任何新增 RT / UAV / 绑定槽仍不许写**，除非 S10 重新拍板落盘。
3. 待办：`--mode shadow` 无阴影版（compute 直光路径）→ 与 mode 5 逐像素对照 → `example/s9-gbuffer/` 基线 + `run.bat` + `cases.json` 卡 + `server.py --check` → 四栏收官条 + `wt_pool.py close` + tag `s9-gbuffer`。
4. 选定方案的绑定布局 / 状态登记口径固化进 `docs/SHADER-CONVENTIONS.md`（新增 §2.18）。

**前置实读清单（本轮，符合 AGENTS.md 决策前置铁律）**：Carrot `lighting/direct-raytracing.slang` + `direct-base.slang`；bevy `meshlet/visibility_buffer_resolve.wesl` + `solari/realtime/gbuffer_utils.wesl` + `initial_path.wesl`；UE 5.8.4（对照总表 S9 行未点名，按功能就近读）`Lumen/LumenSceneDirectLighting.cpp` + `LumenSceneDirectLightingHardwareRayTracing.cpp`；开发历史 `MILESTONES.md`（`s8-done` 收官段）/ `carrot-history.md` / `bevy-history.md` / `.workbuddy/memory/MEMORY.md` / `2026-11-10.md` / 本计划 / `SHADER-CONVENTIONS.md` §2.14、§2.16、§2.17 / `UE-DIFFS.md` 对齐确认节。
