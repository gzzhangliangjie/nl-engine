# EFFECTS-STATUS — 特效系统固化文档与扩展点登记

> 单一入口：特效（particles 轨道）当前全貌 + 所有可深化功能的登记表。
> 本文档是扩展点的**唯一维护处**——PLAN/GUIDE 不再各留「已知边界」段落，只指回这里。
> 台账溯源：里程碑细节见 `PARTICLES-PLAN.md`（M-PT0~PT2）与 `../../MILESTONES.md`；
> 工具用法与 NFX1 schema 见 `VFX-TOOLS-GUIDE.md`。

## 1. 系统总览（固化现状）

```
.efkefc（Effekseer 官方格式）                ← 上游资产（样例入库 assets/efx/）
     │  vfx-tools convert（离线，链 Effekseer core 解析层）
     ▼
.nfx NFX1（JSON，自定义格式）                ← canonical 格式，运行时唯一入口
     │  Particles::loadNfx（运行时，零 Effekseer 依赖，nlohmann 解析）
     ▼
CPU 固定步长模拟（1/60s，pcg 确定性流） → 32B 实例流 → instanced billboard → 主帧 LDR
```

架构决策（已拍板，不重开）：**路线 B 离线转换**——运行时不链 Effekseer（UE Niagara
「authored → 编译产物」/ Carrot「Blueprint 预编译」同构口径）；转换器对子集外模块
**exit 2 显式失败**，禁静默降级。

当前支持面（NFX1 v1 子集，M-PT0~PT2 全落地）：

| 环节 | 能力 | 状态 |
|---|---|---|
| 发射 | 定点发射、rate/life 定值、box 初速 min/max 逐轴、常加速度 | ✅ |
| 渲染 | instanced billboard（X/Y 定值尺寸）、alpha/additive 双管线（懒建变体）、出生→消亡线性色、fadeIn/fadeOut | ✅ |
| 贴图 | texture 字段消费、basename → `assets/efx/textures/`、RGBA8_UNORM 原样采样、缺失回退软圆盘 | ✅ |
| 混合 | alpha blend / additive（One/One） | ✅ |
| 确定性 | 固定步长 + pcg 流，run-to-run PNG 逐字节同；legacy 喷泉路径逐位冻结 | ✅ |

## 2. 扩展点登记表

> 状态口径：**❌ 未支持**（转换器 exit 2 拦下）／**⚠️ 降级支持**（收下但折算/简化）。
> 改动面按「小＝单文件局部 / 中＝跨 shader+schema / 大＝渲染管线级」三档。
> 优先级是**建议**，开工前按触发场景与用户拍板重估；序号即登记顺序，不隐含先后承诺。

### 2.1 格式/转换器侧（扩展 = 放宽 vfx-tools 子集 + .nfx schema）

| # | 功能点 | 状态 | 差什么 | 改动面 | 触发场景 |
|---|---|---|---|---|---|
| E1 | rate 区间驱动（min≠max 随机） | ⚠️ 取均值 | step 里 per-frame 发射概率随机化（RNG 序列要新占位防串流） | 小 | 吃到真实样例后发射密度失真明显时 |
| E2 | easing/渐变颜色（ColorAll Easing/Gradient） | ❌ | .nfx schema v2 加 colorCurve 字段（关键帧数组或 easing 参数对）+ CPU 采样或 PS 曲线 | 中 | 烟雾/火焰类样例颜色形态要求高 |
| E3 | PVA accel 随机范围 | ❌ | accel 折 min/max 盒进模拟（nfx 路径已有盒初速先例） | 小 | 动态感样例 |
| E4 | 缩放生命周期曲线（ScalingType PVA/Easing/FCurve） | ❌ | 实例流或 CB 加 scale 曲线；现只有硬编码收缩 | 中 | 爆炸类「先大后小」样例 |
| E5 | 旋转（2D rotation / PVA/Easing/FCurve） | ❌ | billboard 加角度（CB 标量或实例流扩字段）+ 转换器放宽 | 中 | 碎片/纸屑类样例 |
| E6 | UV 动画 / flipbook 序列帧 | ✅ M-PT3（转换器侧） | .nfx `render.flipbook` 可选字段（v1 向后兼容）；StartFrame 随机 / LERP 插值仍 exit 2 | 中 | 序列帧火焰/烟雾（最常见的贴图特效形态） |
| E7 | 发射位置扩展（sphere/circle/line/model 表面） | ❌ | spawnPoint 扩为 spawn 形状描述 + step 内按形状采样 | 小~中 | 场景型特效（水面涟漪、体积光尘） |
| E8 | 生成 timing（Burst≠1 / MaxGeneration≠1） | ❌ | spawnSeq 流加爆发段/世代循环 | 小 | 一次性爆炸 + 世代刷新 |
| E9 | 非 Billboard 朝向（rotated/directional billboard） | ❌ | VS 朝向计算重写（方向粒子需速度对齐轴） | 中 | 拖尾/速度感样例 |
| E10 | 非轴对齐 quad（SpritePosition.Default 四角） | ❌ | 实例流带四角或 CB 变换；现恒 ±0.5 轴对齐 | 中 | 斜切/梯形贴图粒子 |
| E11 | 颜色继承 / RefEq 动态参数引用 | ❌ | 节点树求值层（v1 按单节点线性参数面，无引用图） | 大 | 复合特效——建议随多发射器一起做 |
| E12 | Model/Ribbon/Ring/Track 节点 | ❌ | 各需专门渲染路径（ribbon/trail 是条带几何，非 billboard） | 大 | 需要「条带/网格类特效」时立项独立里程碑 |
| E13 | 力场 / 碰撞 / KillRules / Falloff / AlphaCutoff / 音效 | ❌ | 逐模块进模拟层（力场最先行，物理层已有 hook 先例） | 中~大 | 物理交互型特效 |
| E14 | magnification≠1 | ❌ | 单纯数值折算 | 小 | 老资产兼容 |

### 2.2 运行时/渲染侧（与转换器无关，纯引擎深化）

| # | 功能点 | 状态 | 差什么 | 改动面 | 触发场景 |
|---|---|---|---|---|---|
| R1 | per-particle 随机 sizeY | ⚠️ per-emitter 定值比例（CB 标量 g_params.z） | 实例流 32→40B 加 sizeY 字段 + shader 换 per-instance 读 | 小 | 单发射器内粒子形状要随机分布时 |
| R2 | 贴图 mip 链 | ✅ M-PT3 | CPU box-filter 全链 + setMipLevels + 按层 writeTexture + mipFilter linear；PS 不动（硬件 tri） | 小 | 粒子远离相机场景后闪烁可见 |
| R3 | sRGB/gamma 正确性 | ⚠️ 原样采样（全链无 gamma 口径下一致） | 引擎引入 HDR/离屏 RT 时统一重议（**不是粒子单侧能改**） | 大（引擎级） | post-chain / 离屏 HDR RT 建成后 |
| R4 | 软粒子（depth fade 防硬裁剪边） | ❌ | PS 读深度缓冲做边缘衰减——依赖深度图暴露给粒子 pass | 中 | 粒子贴地/穿插模型出现硬切边时 |
| R5 | 多发射器透明排序 | ❌ 单发射器无序 | 发射器级排序或 per-particle 深度排序（CPU 排序回传或 OIT） | 大 | 场景多特效叠加后可见错序 |
| R6 | compute GPU 模拟 | ❌ CPU 模拟（4096 池） | 粒子模拟迁 compute（Carrot particles_template 同款思路）；确定性口径要重立 | 大 | 粒子规模 >1e4 后 CPU 成瓶颈 |
| R7 | flipbook 运行时（配 E6） | ✅ M-PT3 | 帧号 CPU 折算 + 实例流 48B progressMisc + VS 统一无 branch UV 公式（M-PT2 保形教训的直接应用） | 中 | 同 E6 |

### 2.3 资产侧

| # | 功能点 | 状态 | 差什么 | 触发场景 |
|---|---|---|---|---|
| A1 | 官方样例特效可转换数 = 0 | ⚠️ 预期（全包均为教学复合特效，v1 窄子集拦下全部） | 上面 E1~E14 逐项放宽后复扫 `Effekseer1.80.8Win/Sample/`（gh-proxy 直链可再拉） | 每扩一个子集模块后跑一轮「样例命中率」验证 |
| A2 | 仓库样例贴图是 LFS 指针（无真体） | ⚠️ 已绕过（M-PT2 改用官方工具包 CC-0 贴图） | 无需动 | — |

## 3. 固化约定索引（改特效代码前必读）

- **shader 保形铁律**（M-PT2 实锤，`PARTICLES-PLAN.md` M-PT2 节）：凡回归基线依赖的浮点表达式，禁 branch 化/lerp 化改写，只能逐字保留或精确恒等变换（×1.0）。**任何动 particles.slang 的扩展点（E2/E4/E5/E6/E9/R1/R4/R7）开工前先读这条。**
- NVRHI 纪律：纹理上传与 setTextureState 必须同命令列表（0x87d 同源）；空纹理槽绑 dummy；§2.15 buffer 每帧写回登记。
- 确定性纪律：固定步长 1/60s + pcg 流；新增任何随机量（E1/E3/E8/R1）必须新占 RNG 序列位，禁复用既有位。
- 格式纪律：.nfx 子集外模块 exit 2 显式失败清单式报错（vfx-tools）；子集放宽 = 正例+负例+回归三件套。
- 路径约定：texture 字段取 basename → `assets/efx/textures/<basename>`（NL_ASSET_DIR 注入 assets/models，代码取父目录拼 efx/textures）。

## 4. 已收口里程碑与 tag

| tag | 内容 | commit |
|---|---|---|
| `particles-pt0` | 运行时最小闭环（legacy 喷泉 + billboard + HUD 行） | `e70b47a` |
| `particles-pt1` | vfx-tools 转换器 + .nfx v1 + 运行时加载端 | `6363180` |
| `particles-pt2` | 贴图采样端 + sizeY 非方形 | `285716b` |

feat/particles（M-PT0~PT2）merge 回 master 待拍板；M-PT3 未立项——上表任选。
