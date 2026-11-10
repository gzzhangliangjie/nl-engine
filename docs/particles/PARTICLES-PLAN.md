# PARTICLES-PLAN — 粒子系统穿插项（track `particles`，B 类渲染穿插项）

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

## 风险登记

- 半透明粒子与场景 blend 次序：起步单发射器无排序问题；多发射器/大粒子后的排序策略到时另议。
- 本机 exe 随机秒退（ENV-FLAKE 台账）：验收失败先重跑 2~3 次再定性。
- HUD 任何格式变动走 kStage bump 闸（`reshot-hud-stage.py`），超 527px 签名必须追因。
