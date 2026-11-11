# 夜景灯光计划草案（NIGHTLIGHTING-PLAN-DRAFT，2026-11-12 凌晨，纯文档轮）

> 与 `docs/WATER-PLAN-DRAFT.md`、`docs/WEATHER-PLAN-DRAFT.md` 同轮：开放调研、未分配里程碑编号、未 claim/release、零落码 / 零 shader / 零 RT·UAV·槽位 / 零 tag / 零 `kStage` / 零 example / 零回归。
> 工作区并行会话在途改动（当日日志、MEMORY.md、S11-PLAN、gi-tools/main.cpp、s10/s11 verify 产物）一行未碰。
> 方向口径（已给、非待拍板）：夜景灯光 **本质不是单一个新技术，而是「自发光材质 + bloom 辉光 + 体积光/光柱 + 夜间大气雾 + 夜空 + 点/面光源夜间 GI」的组合场景**；其中只有「体积光柱（God Rays）」和「自发光贡献 GI（emissive-as-light）」是真正新做的东西，其余全是已有候选/基建的拼接。验收应当作为**串联场景**而非独立技术里程碑。
> **铁律**：自研、只读 UE、bevy 作轻量阶梯、Carrot 只给节奏；开源项目只作算法参照，UE 只读不抄；凡引入第三方须逐项核 LICENSE 原文 + C++17 + 零构建期新依赖并写 `VENDOR.md`。

## 0. 为什么单列（且性质不同于水体/天气）

- **水体**是独立工程链（波形/折射/吸收/透明排序），有自己独有的「反射必须先于水体」依赖链。
- **天气**是 Lumen 的时变输入层（参数挂光照/介质/表面），验收含 GI 侧证据。
- **夜景灯光 = 上面两者的「验收场景」**：它不产生新 primitive，而是把「自发光 + bloom + 体积光 + 夜间雾 + 夜空 + 点面光源 GI」串起来，证明这些能力在夜间组合后成立。
- 之所以也单列草案：用户明确要，且它暴露了两个**真正的空白点**——① 体积光柱（God Rays）的实现，② **自发光当作光源喂进 world cache / 探针（emissive-as-light）**。这两件是夜景里唯一不是「已有东西拼起来」的硬骨头。

## 1. 三处定向实读（夜景/灯光/自发光/God Rays 真实现状）

### 1.1 本项目（nl-engine，自研栈）
- **bloom / tonemap / 辉光**：归 T0 post-chain，离屏 RT 已齐（`hdrSceneColor_` 等），纯增量。
- **自发光材质**：PBR direct 路径有 emissive 通道（待实读 `direct.slang` 确认通道名与是否进 GI）；当前自发光**只进最终着色**，是「发光表面」而非「光源」——即它不会照亮邻居。
- **体积光 / God Rays**：**无**。候选 B 组的「体积雾/God Rays」尚未立项。
- **点/面光源 GI**：M10 探针 + M11 world cache 只消费 `traceCB_` 里的直光（灯向/灯色/强度），**不知道场景里有哪些自发光体** → emissive-as-light 是新增信息通路。
- 结论：夜景里除了自发光着色与 bloom 基建，其余（体积光柱、自发光-as-light、夜空、夜间雾）全是待建或未规划项。

### 1.2 bevy（轻量阶梯，本地 `E:/UE_STUDY_2026/bevy`，已读）
- `bevy_pbr/src/volumetric_fog/volumetric_fog.wesl`：**已含 God Rays**（AABB raymarch + shadow map 采样 + HG 相函数 + 抖动），是体积光柱的现成可读阶梯。
- `bevy_pbr/src/atmosphere/*` + `bevy_solari/src/realtime/atmosphere.wesl`：大气透射率乘进 GI 直光（见 WEATHER-PLAN-DRAFT §2）——证明「夜空/大气参与 GI」的可行路径。
- bevy **无 emissive-as-light**（自发光不进 bevy 的 lightmap/探针），所以这一点 bevy 也给不了参照，须自研或抄 UE。

### 1.3 UE 5.8.4（工业终态，只读不抄）
- `LightRendering.cpp`：`r.LightShafts`（radial-blur 屏幕空间 God Rays，光源 occludee 预渲 + 径向模糊叠加）+ `r.LightShafts` 预算。
- `VolumetricFog.cpp`：体积雾 raymarch 内直接累积 **emissive / LightShaft 贡献**；光源可标 `bAffectsVolumetricFog`（自发光进体积介质）。
- `Lumen/LumenRadianceCache.cpp` / `LumenMeshCards.cpp`：自发光网格卡（emissive mesh cards）**作为间接光来源**喂进 Lumen——这是「emissive-as-light」的工业级答案，但依赖完整 mesh card 体系（我们无此基建）。
- 结论：UE 把 God Rays、自发光进体积雾、自发光进 Lumen 三者都做全了；算法口径唯一权威，但每一样都比我们现况重一个量级。

### 1.4 Carrot（零夜景参照，节奏反面）
- 全目录 grep 夜景相关（light shaft / volumetric / emissive-as-light）无命中；只有静态 cubemap 天空 + 线性距离雾。印证「自研路线默认无夜景能力」。⚠️ 本轮未单独重 grep Carrot 灯光路径，沿用 WEATHER 草案结论。

## 2. 夜景灯光架构分解（组合哪些能力）

| 层 | 在夜景里做什么 | 现状 | 是否新做 |
|---|---|---|---|
| ① 自发光材质 | 霓虹/灯箱/车灯自身发亮 | PBR direct 有 emissive 着色 | 否（但通道进 GI 要拍板） |
| ② bloom 辉光 | 自发光溢出成光晕 | T0 post-chain 已规划 | 否（纯增量） |
| ③ 体积光柱 God Rays | 点/面光源经遮挡后的光束 | **无** | **是（新增）** |
| ④ 夜间大气雾 | 远处灯光被雾吸收/散射成体积感 | 天气线 B 组候选 | 否（复用天气线） |
| ⑤ 夜空 | 无日照的暗天穹 + 星点 | `sky.slangh` 当前是日天穹 | 部分（夜空参数化归天气线） |
| ⑥ 点/面光源夜间 GI | 路灯/窗光反弹照亮邻居 | M10 探针只吃直光 | **是（emissive-as-light，最难）** |

**真正新做的只有 ③ 和 ⑥**；①② 是已有基建增量；④⑤ 归天气线。

## 3. 与现有基建对接

| 需求 | 现有基建 | 缺口 |
|---|---|---|
| bloom 辉光 | T0 `hdrSceneColor_` + post-chain 离屏 RT 齐 | 纯增量，双后端风险最低 |
| 自发光着色 | PBR direct emissive 通道（待核通道名） | 确认是否进最终色 + 是否进 GI |
| 体积光柱（屏幕空间） | `hdrSceneColor_` + 遮挡深度/模板 + post-chain | 径向模糊 3-pass（遮挡预渲→场景→叠加），bevy/UE 都有口径 |
| 体积光柱（体积） | 体积雾 raymarch（候选 B 组） | 复用 B 组 froxel 基建（若天气 B 方案先落地） |
| 自发光-as-light | M10 探针 `traceCB_` + M11 world cache | **当前信息通路缺失**：需把「自发光体列表/SH」注入探针/world cache；依赖 S11 世界缓存，是硬骨头 |
| 夜空/夜间雾 | 天气线 A/B（WEATHER-PLAN-DRAFT §5） | 复用天气线参数化天穹 + 高度雾 |

## 4. 开源方案初筛（许可证来自搜索摘要，引入前须逐项核 LICENSE 原文）

**整场景夜景 demo（看「好夜景长什么样」）**：
- `city-of-wires`（MIT，C++20/Vulkan/GLSL→SPIR-V）：程序化赛博城市，有**独立的 neon lighting system**（脉动霓虹 + bloom）+ 远处天光**体积光** + 重雾。→ 最贴「夜景」语义；但 C++20 + Vulkan 直写，算法可读、栈不对齐。
- `cg25-group25 Cyberpunk City Demo`（Vulkan RTX）：emissive neon + bloom + distance fog + RT 反射/阴影多 pass 组合。→ 参考「夜景=自发光+辉光+雾+反射」的拼法；依赖 RTX 硬件光追，我们无此基建。

**体积光/光柱（God Rays，真正新做的 ③）**：
- `math-araujo/screen-space-godrays`（MIT，C++20/OpenGL 4.5）：经典论文 *Volumetric Light Scattering as a Post-Process*（Kenny Mitchell, GPU Gems 3）实现，3 pass（遮挡预渲→正常场景→径向模糊叠加），支持多光源 + ImGui 调参。→ **屏幕空间 God Rays 最佳参照**。
- `Erkaman/glsl-godrays`（MIT，~203★）：同一论文的 GLSL 极简版，`godrays(density,weight,decay,exposure,...)` 单函数。→ **最易抄的极简版**。
- **bevy `volumetric_fog` 已含 God Rays**（见 §1.2），是体积路线现成阶梯。

**自发光-as-light（真正新做的 ⑥）**：
- 无轻量开源参照（bevy 无、Carrot 无）；UE `LumenMeshCards` 是工业答案但依赖 mesh card 体系。→ 自研，先走「简化版：把少量自发光体当作额外点光源喂 `traceCB_`」而不是完整 mesh card。

选型硬闸结论：以上全部**只作算法/参数/拼法参照**，零 vendored、零构建期新依赖；UE 源码一行不进 repo。God Rays 的 C++20 实现与本项目 C++17/MSVC 14.38 冲突，只抄 shader 思路。

## 5. 方案 A/B/C + 代价矩阵

| 方案 | 体积光柱 | 自发光-as-light | 夜空/夜间雾 | 代价 | 与冻结基线关系 |
|---|---|---|---|---|---|
| **A 屏幕空间夜景（推荐）** | **屏幕空间 God Rays**（radial blur 3-pass，math-araujo/Erkaman 参照） | 首版**不做**（自发光只着色 + bloom 辉光） | 复用天气线 A（参数化夜空 + 高度雾） | 低：仅加后处理 pass + 遮挡预渲 | 不碰 GI 冻结基线；occlusion 预渲复用深度 |
| **B 体积 + emissive-as-light** | 体积 raymarch（复用天气 B froxel） | **简化版**：少量自发光体当额外点光源喂 `traceCB_` | 同 A | 中高：新信息通路 + 体积 pass | 改探针/world cache 输入 = 碰 S11 统计口径，须重拍授权 |
| **C 贴皮夜景（否决）** | 无（暗背景 + 自发光贴图 + 假 bloom） | 无 | 无 | 最低 | 验收必挂（关掉 bloom 几乎不变） |

**建议路径**：A（屏幕空间 God Rays + bloom + 天气线夜空/雾，自发光不做 GI）→ 日后要「霓虹照亮街道」再升 B（emissive-as-light 简化版）→ 体积光柱升级走天气 B 组 froxel。

## 6. 依赖顺序铁律

**夜景由已有能力拼成，依赖它们的落地顺序**：
1. **bloom（T0 post-chain）**——已规划，最优先，零风险。
2. **自发光着色确认**（PBR direct emissive 通道名 + 是否进最终色）——纯确认，无新增。
3. **屏幕空间 God Rays（A 方案 ③）**——独立后处理，可与 bloom 同链，不依赖体积雾。
4. **天气线 A（夜空 + 高度雾）**——夜景的「夜间介质」来自天气线，须天气线先拍板（见 WEATHER-PLAN-DRAFT）。
5. **emissive-as-light（B 方案 ⑥）**——**最难、最后**：依赖 S11 世界缓存输入结构，须单独拍板 + 重拍授权；首版不做。

## 7. 硬约束核对（不新增偏离）

1. 自研边界闸：只能自研，UE 只读不抄，bevy/Carrot 作阶梯。
2. `sky.slangh` 是 M10.1/M11.1 探针 miss 与 `--sky-bg` 公共函数，M11.1 已实锤「冻结基线上的代码形态也是基线」——夜空/夜间天穹参数化若改 `sky.slangh` 采样路径须单独授权重拍（同 WEATHER Q-WE5）。
3. Q4 冻结不新增独立 GBuffer 纹理/MRT/落盘 UAV；God Rays 走 T0 同款离屏 RT（occlusion 预渲复用深度），不进 GBuffer。
4. M11.1 Q11-1 统计口径：God Rays 含时间抖动 + 时域重投影，bytes 基线大概率不保，须固定步长/固定帧数或统计断言；体积路线更甚。emissive-as-light 改探针/world cache 输入 → 走统计。
5. 主线在 S11 收尾、`kStage` 仍 S8.2 → 夜景只能作穿插项（不占 m0.* 编号、tag 用名字、不挡主线）或扩 S14。

## 8. 待拍板（未替用户定）

- `Q-NL1` 立项归属：穿插项 / S14 第七分支 / **串联验收场景**（建议）/ A 类轨道。
- `Q-NL2` 夜景定位：独立技术里程碑 / **串联验收场景（建议，串起 bloom+体积光+自发光+夜空）** / 扩天气线。
- `Q-NL3` 体积光柱：屏幕空间 God Rays（A，推荐，轻量）/ 体积 raymarch（B，复用天气 froxel）。
- `Q-NL4` 自发光-as-light：首版不做（A）/ 简化版额外点光源喂 `traceCB_`（B，依赖 S11、须重拍授权）。
- `Q-NL5` 冻结基线重拍授权（夜空/夜间天穹改 `sky.slangh`；emissive 进 GI 改探针/world cache）。
- `Q-NL6` 是否引入第三方（建议零 vendored，只读算法；city-of-wires 的 C++20 / God Rays 的 C++20 实现均不入库）。

拍板后须写回 ROADMAP/S14-PLAN + 新建任务书（如 `docs/n*/NL*-PLAN.md`）+ 全仓口径同步 + SHADER-CONVENTIONS 新节，才允许落第一行码。编号用 `Q-NL*` 与同日 `Q-WE*`（天气）/`Q-WA*`（水体）/`Q-W*`（worldgen）区分，勿混。

## 9. 与水体/天气/体积云的关系

- **水体**：夜景下水体 = 反射霓虹/bloom + 夜间吸收，是夜景验收的反射面；两者立项独立，但夜景验收可借用水体反射证据。
- **天气**：夜景的「夜空 + 夜间雾 + 体积光介质」直接消费天气线 A/B 的参数化产出；夜景不重复造天穹，等天气线拍板。
- **体积云**：独立里程碑，只在 UE 有；夜景里云层遮挡灯光可成额外氛围，但非必须。
- **体积雾/God Rays（B 组候选）**：与夜景 ③ 体积光柱同源；若先落 B 组 froxel，夜景体积光柱直接复用，省一遍基建。
- 三者（水体/天气/夜景）+ B 组在「反射/体积 RT 交叉处理」上可能共用基建，但立项各自独立、待拍板项各自编号。
