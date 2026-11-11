# PARTICLES-PLAN — 粒子系统穿插项（track `particles`，B 类渲染穿插项）

> **能力现状与扩展点登记的唯一维护处在 `EFFECTS-STATUS.md`**——本文各里程碑的
> 「已知边界」为收口时点的历史记录，后续演进以 EFFECTS-STATUS 登记表为准。

> 立项：2026-11-10（ROADMAP 穿插项行 + PARALLEL-TRACKS §7.1 B 类专列 + wt_pool `particles`/M-PT0）。
> 起点 master `50574e3`，worktree `E:/UE_STUDY_2026/nl-engine-particles`，分支 `feat/particles`。
> tag 命名：`particles-pt0`、`particles-pt1` …（名字制，不占 m0.* 编号）。

## 三处口径（2026-11-10 已实读，结论随落码定向复核）

| 来源 | 结论 | 对本计划的落点 |
|---|---|---|
| Carrot | `render/particles/` 约 580 行：CPU 侧发射器/系统（`ParticleSystem.cpp` / `ParticleEmitter.cpp`）+ GPU 蓝图渲染（`RenderableParticleBlueprint`），compute 模板 `particles_template.slang` + `raster/draw_particles.slang` | **M-PT0 主参照**（抄思路不抄代码）：CPU 模拟 + GPU 绘制分层结构；开工时定向实读该目录与三份 slang |
| bevy | 无官方粒子系统（社区 bevy_hanabi，不在本体） | 反面印证：自研要克制起步，不上 compute 模拟 |
| UE 5.8.4 | 粒子非独立 pass：`NiagaraRendererSprites::GetDynamicMeshElements` 构建 FMeshBatch 注入主流水线（sprite 走 Translucency），shader 侧自定义 VertexFactory 读 GPU sim buffer | 印证「粒子画进主帧现有流程」；本项目起步用 instanced billboard 等价落法 |

## 格式层决策（2026-11-10 用户拍板，路线 B）

- **目标**：吃现成特效资产；**机制 = 离线转换器 → 自定义 `.nfx`**，运行时只认 `.nfx`（UE「导入转自家资产」同款姿势；`.nmesh`「离线管线 → 自有格式」项目先例同款思路）。
- **不 vendored Effekseer runtime 进运行时**（自带渲染管线撞适配成本与依赖边界）；转换器对 Effekseer **MIT 源码**的解析参照/借用合法——若转换器直接复用其加载代码，按选型闸 `thirdparty/effekseer` 子集入库 + `VENDOR.md` 溯源 + LICENSE 原文随入。
- **`.nfx` v1 模块子集**：sprite 发射器；位置/速度/寿命/重力 + 曲线 alpha 淡出 + flipbook 贴图帧序列。**子集外模块（ribbon/trail/ring/网格/扭曲/程序模型/噪声力场）转换期显式报「不支持模块」失败**，禁止静默降级出错误画面。
- Flipbook 序列帧不再单列里程碑（就是 `.nfx` 内的贴图元数据字段）。

## M-PT0 · 运行时最小闭环（✅ 收官 2026-11-10，tag `particles-pt0`）

### 内容
- `renderer/` 新增粒子模块（CPU 模拟 + 数据上传）：
  - 固定容量粒子池（起步 4096），粒子结构 `{pos, vel, life, maxLife, size, color}`；
  - 发射器：定点喷泉（发射率、初速锥形向上、重力 −9.8、size/alpha 生命周期曲线）；
  - **模拟跑固定逻辑步长（与 M-GL 物理同款纪律）**——确定性是截图基线逐字节复现的前提，不随渲染帧率漂。
- GPU 侧：每帧写 staging buffer（**§2.15 buffer 每帧写回状态登记**）→ instanced billboard quad 绘制（per-instance pos/size/alpha/color）；alpha blend，**深度测试开、深度写入关**；画进主帧场景之后、HUD 之前。
- viewer 接入：`--particles` 开关（默认关，零波及既有用例）；HUD 加 `Particles : N alive` 行，**仅 `--particles` 模式显示**（同 BLAS 行只在 dag/hiz 显示的先例，避免 HUD 基线全量重拍波及）。
- shader：`renderer/shaders/particles.slang`（vs 顶点展开 billboard + ps 采样/淡出），绑定按 §1 SPIR-V shift 约定，单源双后端。

### 验收判据（亲眼可见 + 机器断言）——✅ 已全部达成（2026-11-10 收口，tag `particles-pt0`）
- `example/particles-pt0/`：6 基线（三卡×双后端）+ `run.bat verify` 断言（双后端稳态锚点 `particles steady: alive=567`，落在发射率×平均寿命理论区间）+ 负向卡。**偏差如实记录**：双后端 PNG **不逐字节互检**（dx `6bd87baa` vs vk `6f36def7`），按 phys-p3/p4 薄线噪声先例改「各自对各自基线」，DX12 侧确定性由同参两次实跑 `fc /b` 逐字节同承担（MD5 `6bd87baad5381dc8e47c1810d3584583`）。
- 同帧两次实跑 PNG 逐字节同 ✅（固定步长确定性证明，DX12 实测）。
- ctest 双后端 **51/51 全绿** ✅ + `server.py --check` exit 0 ✅。
- 渲染版交代全套 ✅（cases.json particles-pt0 版本卡 + 3 用例卡）；tool 版交代 M-PT1 才有，M-PT0 **无工具交代物**（本行即写明）。

### 边界（起步不做，违反即扩权）
compute GPU 模拟、软粒子、碰撞、深度/不透明排序优化、体积类、ribbon/trail、`.nfx`/转换器（归 M-PT1）、post-chain 集成（离屏 HDR RT 建成后再迁半透明合成，起步直画 LDR 主帧）。

## M-PT1 · vfx-tools 转换器 + `.nfx` v1（✅ 收官 2026-11-10，tag `particles-pt1`）

- `vfx-tools/` 离线工具：`.efkefc` → `.nfx`（JSON 描述 + 贴图路径记录），子集见上；不支持模块 exit 2 显式失败。✅ 落地：vendored Effekseer core（MIT，b570008，Network 排除）+ `vfx-tools/main.cpp`（convert/selftest，SKFE chunk v1500/v1610 老版本迁移实测生效）+ `assets/efx/` 真实样例（正例 CullingSprite/square_r-g-b + 负例 GpuParticles_* ×2）+ NFX1 schema（VFX-TOOLS-GUIDE.md 归档）。**已知边界（M-PT2 候选）**：贴图路径记录但运行时未采样（程序化软圆盘回退；上游贴图是 git-lfs 指针、镜像拉不到真体）；sizeY 记录未用；rate 区间取均值。
- 运行时加载端并入（用户拍板「同里程碑」）：`Particles::loadNfx`（NFX1 JSON → 模拟参数面，box 初速 + 常加速度 nfx 路径；**legacy 喷泉逐位不动零回归**，pt0 spot 回归 6/6 全绿）+ additive 变体管线懒建 + viewer `--nfx`（隐含 --particles）。确定性实测：nfx 模式 DX12 同参两跑 PNG 逐字节同（MD5 7fd2a033…）、双后端 alive=101 同值命中理论稳态。
- 交代：✅ 展示台 tool 卡 3 条（usage exit 2 / selftest exit 0 / convert 负向 exit 2 带实测原因清单，case 级 exe 覆盖 vfx-tools）+ `docs/particles/VFX-TOOLS-GUIDE.md` 归档 + 真实输出生成的证据图（nfx-hud 渲染基线 = 转换产物实跑画面）。
- 里程碑开工时按铁律重读三处对应模块 ✅：Effekseer 源码解析层（EfkEfcFactory/SKFE chunk/参数结构面逐字段实读）、Carrot 资产与蓝图拆分（ParticleBlueprint = 预编译 SPIR-V + 贴图索引，离线生成运行时零解析）、UE 导入转换姿势（Niagara authored → FNiagaraEmitterCompiledData 编译产物，运行时零解析 = 工业口径同构）。

## M-PT2 · 贴图采样端 + sizeY 非方形（✅ 收官 2026-11-10，tag `particles-pt2`）

- 贴图采样端 ✅：`.nfx` texture 字段生效（v1 记录未用→消费）。stb_image 声明头入 `thirdparty/stb/`（与 tinygltf 内嵌 v2.21 同版本副本；`STB_IMAGE_IMPLEMENTATION` 仍只在 nl_core mesh_io.cpp 展开一份，renderer 只引声明复用符号）。`ensureTexture` 惰性建纹（RGBA8_UNORM **原样采样不做 sRGB**，与引擎粒子颜色线性直接算口径一致），像素上传推迟到 draw 里 `writeTexture`（建纹时无 cmd list；上传与 `setTextureState` 同帧同列表——0x87d 第一坑同源纪律）。无贴图绑 1x1 白 dummy（引擎空纹理槽先例），PS 双路径 branch 保形（`g_params.y` 开关：0 = 软圆盘逐字 M-PT0 原表达式）。
- sizeY 非方形 ✅：**Y/X 比走 CB 标量 `g_params.z`**（单发射器子集所有粒子同比例；ratio=1.0 时 `corner.y*1.0f` 逐位恒等，基线零漂移，实例流 32B 布局不变）。⚠️ 实测教训：VS 里 if-else 会被 dxc select 化（两路径混合评估）致基线 ±1 LSB 漂移 15 像素；PS lerp-0「恒等」同样被 FMA 展开漂移——**shader 保形铁律：凡回归基线依赖的浮点表达式，禁 branch 化/lerp 化改写，只能逐字保留或用精确恒等变换（×1.0）**。
- 贴图资产 ✅：Effekseer 1.80.8 官方工具包 `Sample/00_Basic`（CC-0）Particle01/Flame01 入库 `assets/efx/textures/`（gh-proxy 直链下载 96MB 工具包，LFS 指针问题绕过）。正例 .nfx 手工构造（全包 0 个子集内特效为预期——官方样例均教学复合特效；转换器 texture 提取已由 M-PT1 selftest 往返断言覆盖）。解析约定：`.nfx` texture 字段取 basename → `assets/efx/textures/<basename>`。
- 验收 ✅：贴图模式双卡（tex_particle01 alpha 光斑 / tex_flame01_rect additive 火焰 2:1 扁形）双后端可见 + alive=101 稳态 + deviceErrors=0；贴图模式 DX12 同参两跑 PNG 逐字节同；**双后端贴图采样区域逐位同**（diff 仅 HUD adapter 文字）；pt0 6/6 + pt1 10/10 回归 ALL GREEN 全逐字节；负向人工实证（texture 缺失 → soft-disc fallback 不炸帧）。交代：`example/particles-pt2/` 4 基线 + run.bat verify 全绿 + cases.json 卡 2 用例。
- 已知边界（M-PT3 候选）：rate 区间驱动、easing/渐变颜色仍留候选；sizeY 当前 per-emitter 定值比例（per-particle 随机 Y 需实例流扩字段，v1 子集内无此需求）；贴图无 mip 链（远处粒子闪烁，需时再加 mip）；sRGB 转换未做（引擎全链无 gamma 口径下一致，若未来引入 HDR/离屏 RT 需重议）。

## M-PT3 · 贴图 mip 链 + flipbook 序列帧（✅ 收官 2026-11-10，tag `particles-pt3`）

- 贴图 mip 链（R2）✅：`ensureTexture` CPU box-filter 逐层 2×2 均值下采样（纯整数/加法 = 跨次运行逐位确定；奇数尺寸层 clamp 边缘取样），`TextureDesc::setMipLevels` 全链建纹 + draw 首帧 `writeTexture` 按层循环上传（texturePixels_ 平铺布局：层 n = (W>>n)×(H>>n)，行距 = 层宽×4 无 padding）+ sampler `setMipFilter(true)`。dummy 1x1 无 mip 链零影响。**基线影响如实落档**：tex-flame（0.25m 高粒子采样进高层 mip）双后端重拍（1652px maxDelta 28），tex-hud 逐字节未漂（粒子屏幕尺寸 ≥ 贴图尺寸采样全落 layer 0）。
- flipbook 序列帧（E6+R7）✅：转换器 `UVs[0]` Animation 子集提取（StartFrame 恒 0 / LERP 帧间插值 / FrameLength≤0 或极大值 → exit 2；LoopType once/loop/reverse 全支持——三者皆 Effekseer `UVFunctions::GetUV` 无分支数学）；`.nfx` **v1 可选字段向后兼容**（magic 不变，`render.flipbook` 缺省 = 无，老文件零改动；selftest 加 flipbook 往返 + 老格式兼容双断言）；帧号折算**全在 CPU**（`elapsed×60+0.5f` 消浮点截断抖动，once 末帧驻留 / loop 取模 / reverse 来回，纯整数运算确定性无碍）；实例流 32→48B 扩 `progressMisc`（已有成员偏移不变 float4 对齐）；**VS 统一无 branch 公式** `flipUv = uvRect.xy + uvRect.zw*(frameXY + localUv)`——非 flipbook 时 `uvRect=(0,0,1,1)`、`frameXY=(0,0)`，IEEE 精确恒等于 M-PT2 的 texUv（texUv 从 PS 移到 VS 属恒等移动，软圆盘分支不读 flipUv 零影响）；CB 112→128B 加 `g_uvRect`。
- 贴图资产：`flipbook_4x2.png`（PIL 生成 4×2 帧编号色相环网格，CC-0）+ 手工 `tex_flipbook.nfx` 正例（散开初速让不同年龄粒子落不同位置 → 多帧共存画面可验帧序）。
- 验收 ✅：flipbook 画面多帧共存（红0/橙/绿/青/蓝/紫数字清晰正立——V 轴口径与 cell 对齐画面实证）+ alive=101 稳态 + deviceErrors=0；flipbook DX12 同参两跑 PNG 逐字节同；双后端粒子采样区域逐位同（diff 仅 HUD 文字区）；pt0 6/6 + pt1 10/10 全逐字节零回归 + pt2 4/4（重拍后）；ctest DX12/VULKAN 100%（until-pass:3，竞态卡重试即过）；run.bat verify exit 0；server --check OK。交代：`example/particles-pt3/` 2 基线 + run.bat + cases.json 卡 1 用例 + pt2 flame 卡 note 补重拍记录。
- 已知边界（登记表见 EFFECTS-STATUS.md）：flipbook LERP 帧间插值（双采样混合）未支持；StartFrame 随机未支持；flipbook 测试贴图无真实美术素材（PIL 网格足够验口径）。

## M-PT4 · E1 rate 区间 + E2 easing/渐变颜色（✅ 收官 2026-11-11，tag `particles-pt4`）

### 拍板记录（2026-11-11 用户「按照推荐来」）
- **Q1=b**：拆 M-PT4（E1+E2，零基线波及先收口）+ M-PT5（E5+R1，扩字段+全量重拍隔离）。
- **Q2=a**：E5 旋转走 per-particle 实例流字段（M-PT5）。
- **Q3=b**：实例流 48→64B float4（M-PT5，spare 留 E3/E4/E8）。

### 内容
- **E1 rate 区间驱动（小）**：vfx-tools 收 Effekseer `EmitSpeed` min/max → `.nfx` `emit.rateMin/rateMax`（v1 向后兼容，缺省 = 现行定值 `rate`）；模拟 step 的「每帧固定发射数」改为**整数部分 + 小数部分概率**随机化——**新占 pcg 流位**（确定性纪律：禁复用初速/寿命位）；rate 定值路径逐字保留零回归。
- **E2 easing/渐变颜色（中）**：`.nfx` 可选 `render.colorCurve` 关键帧数组 `{t,rgba}`（照 M-PT3 flipbook 可选字段向后兼容模式，v1 「出生→消亡线性」= 两点曲线特例，magic 不变老文件零改动）；采样**全在 CPU**（每帧填实例流 colorAlpha 时按曲线插值），**PS 不动**——零 shader 保形风险。转换器收 Effekseer ColorAll 的 Easing/Gradient 子集；FCurve 等子集外显式 exit 2（负例清单式报错）。

### 验收判据（拍板后按此放行）——✅ 已全部达成（2026-11-11 收口）
- E1 正例 e1_rate_range.nfx（rate [60,120]/s）喷泉可见，alive=146 稳态（理论 E[rate]×life=150，区间 [100,200] 命中）；E2 正例 e2_gradient.nfx 三关键帧红→绿→蓝同屏三色相共存，alive=101（60/s×1.667s 理论命中）✅
- 转换器负例与畸形：selftest 往返 + 老格式兼容 + 4 类畸形拒绝全 PASS；e_bad_colorcurve.nfx 运行时 `'--nfx: .nfx malformed colorCurve object (fallback to legacy fountain)'` 不炸帧 ✅
- rate 定值 / v1 线性色老路径逐位零回归：pt0 6/6 + pt1 10/10 + pt2 4/4 + pt3 2/2 全 bytes ALL GREEN（存量 7 份 .nfx 全 rate=[60,60] 实测，span=0 IEEE 恒等坐实零漂移）✅
- 确定性：e2 DX12 同参两跑 PNG 逐字节同（MD5 `b5b747b8bc75febbd53d67e5dac688b4`）；双后端 alive 同值 146/101 ✅
- 交代：`example/particles-pt4/` 4 基线 + run.bat verify exit 0（pair×2 + 确定性 + 锚点 + 双负向）+ cases.json 卡 2 用例（tag 字段齐）+ VFX-TOOLS-GUIDE schema/映射表增补 + A1 抽查（仓内 6 样例：3 OK 参数不变 / 3 结构性 exit 2，命中面不变）✅
- ctest `p-mpt` 双后端 2/2（until-pass:3）；`server.py --check` exit 0 ✅

### 边界
- 不动实例流布局（48B 保持）、不动 shader；StartFrame 随机 / LERP 插值仍 exit 2；FCurve 旋转/缩放曲线仍 exit 2（归 M-PT5 及后续）。

## M-PT5 · E5 旋转 billboard（per-particle）+ R1 per-particle sizeY（✅ 收官 2026-11-11，tag `particles-pt5`）

> **验收写实（2026-11-11 收口）**：实例流 64B 落地（progressMisc.z=angle / .w=每粒子 Y/X 比 /
> misc2 spare float4）；VS 无 branch 2D 旋转 + per-instance sizeY（CB g_params.z 退役，布局不动）；
> 转换器收 Rotation None/Fixed/PVA 的 Z 分量 + Scaling PVA y 区间；selftest 增旋转/sizeYRange
> 往返 + 5 类畸形拒绝；新随机槽 `s*7u+0x100u+{0,1,2}` 与旧 5 槽输入空间不相交。
> **零漂移双保险兑现**：angle=0 IEEE 恒等 + w=旧 ratioY 同值 → **pt0~pt3 回归 22/22 全 bytes
> 逐字节同，「全量重拍预算」零消耗（一张没重拍）**。正例 e5_spin（ArrowNE 非对称箭头贴图 +
> 程序化生成，旋转朝向随机可见）/ r1_sizey（软圆盘压扁拉长可见），alive=51 双后端同值；
> e5 DX12 确定性 MD5 01abd003… 逐字节同；run.bat verify exit 0（含三负向）。

### 内容
- **实例流 48→64B（Q3=b）**：新 float4 = `{sizeY, angle, spare0, spare1}`；shader 换 per-instance 读替代 CB 标量 `g_params.z`；per-emitter 定值路径（ratioY 定值 / angle=0）**逐位恒等**——shader 保形铁律：统一无 branch 公式，`cos(0)=1/sin(0)=0` IEEE 恒等，禁 branch/lerp 化改写（M-PT2 实锤）。VS 2D 旋转矩阵作用于 corner。
- **E5 转换器子集（Q2=a）**：Rotation 收 PVA 随机区间（初角/角速度 min/max）；Easing/FCurve 旋转仍 exit 2。sizeY 随机区间同步收进 `.nfx`（R1）。
- **RNG**：sizeY、角度随机各自**新占 pcg 位**。
- **⚠️ 全量重拍预算（用户已列入）**：实例流扩字段 = pt0~pt3 全部基线重拍，属预期非回归；per-emitter 定值用例重拍后与旧基线 diff 应仅帧号水印级差异，逐张核对后更新版本卡。

### 验收判据（已全过）
- ~~旋转正例（贴图粒子 visibly 转动）~~ ✅ 箭头贴图朝向随机各异；~~随机 sizeY 正例~~ ✅ 软圆盘高矮胖瘦不一；双后端 alive=51 同值 + DX12 确定性逐字节。
- ~~pt0~pt3 重拍记录逐张落台账~~ ✅ **零重拍**：回归 22/22 全 bytes 逐字节同（恒等论证兑现，重拍预算未动用）；run.bat verify exit 0；cases.json particles-pt5 卡 2 用例（规范写入）。
- tag `particles-pt5`。

### 边界
- R4 软粒子 depth fade **不在本轨**：深度图暴露是主渲染管线侧改动（T0 `setSharedDepth` 后接口现成），按归属声明走主线协调，主线会话单独 claim；本轨一行不碰。粒子直画 swapchain 保持。

## 风险登记

- 半透明粒子与场景 blend 次序：起步单发射器无排序问题；多发射器/大粒子后的排序策略到时另议。
- 本机 exe 随机秒退（ENV-FLAKE 台账）：验收失败先重跑 2~3 次再定性。
- HUD 任何格式变动走 kStage bump 闸（`reshot-hud-stage.py`），超 527px 签名必须追因。
