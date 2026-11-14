# W3-PLAN — M-W3 树/车/街具资产通道（立项调研，2026-11-14）

> 状态：**整轨收官**（2026-11-14，tag `w3-m0`/`w3-m1`；§2 资产侦察实录 + §3 零改动验证已兑现，落地明细见 CITY-TOOLS-GUIDE §8/§9 与 MILESTONES W3 节）。
> 上游：M-W0 布局（roadnet/trees RNG 数据）→ M-W1 贴图化 + city2engine 链 → M-W2 楼型池（FNV-1a 选型惯例）。

## 1. 现状盘点（实读证据）

- **树**：`CityTree{cx,cz}` 已有位置数据（RNG 流尾消费，forest 8/块 low 4/块，seed12345=188 棵），
  但 **只进俯视图 PNG + 统计层，不进 OBJ/city.scene**（main.cpp M-W0.3 注释原话：
  「M-W1 若需树资产走 asset-ingest 通道」——本轨即该通道的兑现）。
- **车**：`stats.cars` 为挂零锚（M-W0 未消费，无生成逻辑、无位置数据）。
- **街具**（红绿灯/路灯/长椅）：无任何数据与渲染。
- **桥**：城市纯平地、无河流/高程，桥没有存在前提 → **不进本轨**（待地形轨 M-W4+）。
- **路面纹理**（车道线/斑马线）：W0 道路是程序化几何 + 纯色，属贴图档扩展，本轨顺带评估（§4 Q-W3C）。

## 2. 资产选型实测（本轮已拉样张解剖，kenney 前科教训执行）

| 包 | 内容 | 许可证 | 获取路径 | 样张解剖 |
|---|---|---|---|---|
| **Kenney Nature Kit 2.1** | 329 glb：61 种树（blocks/cone/default/detailed × normal/dark/fall）+ 15 种桥件 + cliff/rock/bush/fence | CC0（License.txt 随包） | GitHub `ANAMASGARD/ClanCode` 镜像（gh-proxy sparse clone；kenney.nl 挡 curl 403） | `tree_default.glb`：1 mesh 2 prim 384 顶点，材质 `woodBark`/`leafsGreen`，**零贴图**，9~31KB |
| **Kenney Car Kit** | 25 辆（sedan/suv/taxi/police/van/truck/firetruck/tractor…），已下 20 辆 | CC0（kenney.nl 官方，经 cdn.cinevva.com 获取） | `cdn.cinevva.com/assets/packs/kenney/car-kit/<name>.glb` 直链 | `sedan.glb`：5 节点（body+4 轮），colormap 图集 1 贴图，3184 顶点，172KB |
| **街灯**（poly.pizza） | Street Light by Quaternius | CC0 | `static.poly.pizza/451a3b57-….glb`（无需登录） | 1 mesh 2 prim 350 顶点，2 材质 + 1 atlas 贴图，23KB |
| poly.pizza 街具（红绿灯/长椅等） | 单件下载 | 多为 **CC-BY**（需署名） | 同上，逐件筛 CC0 | 探针 3 件均 CC-BY → 后备源 |

否决/缓议：
- kenney.nl 官网直下：curl 全站 403（nginx 统一拦截页），Edge 直链下载实测未落盘（URL 已失效）；
- Quaternius Ultimate Nature Pack（opengameart）：OBJ 格式可走 obj2glb，但下载 ~3KB/s 中断两次，且
  kenney 镜像已覆盖同类需求 → 放弃；opengameart 直链留档备用。
- poly.pizza CC-BY 件：许可闸上弱于 CC0，首轮不引入；若后续街具缺口大再单独拍板（署名进 VENDOR.md）。

## 3. 消费链书面验证（结论：**引擎零改动**）

| 环节 | 证据 | 结论 |
|---|---|---|
| 白名单 | `renderer.cpp:22 validAssetFileName`（扁平可打印 ASCII、禁路径分隔/`..`）+ viewer `runScene` 同套 | kenney 文件名全部合法（`tree_default.glb` 等）✓ |
| 无贴图材质 | `scene.cpp:294` 1x1 白占位 + `model.slang:237` `baseColor = factor.rgb * baseTex.rgb` | kenney 树纯材质色直渲，零适配 ✓ |
| 多节点层级 | `mesh_io.cpp:117~180`（TRS/matrix）+ 339~439（场景树展开） | 车 body/4 轮定位，零适配 ✓ |
| 嵌入图集贴图 | M-W1 facade 贴图已上墙实证 | 车 colormap / 街灯 atlas，零适配 ✓ |
| 摆放数据 | `city.scene` 实体 meshFile 引用 + drive 已有 scene-out 装配层 | 树/车/街具 = 新实体行，drive 现有机制 ✓ |

## 4. 待拍板项

### Q-W3A 资产接入层
- **A1（拍板）drive 层装配**：city-tools 保持纯生成器；city2engine drive 负责把 vendored glb
  拷进 `assets/models/`（扁平白名单名）+ 按 city.scene 实体引用装配。与 M-W1「drive 是资产
  装配层」定位一致；city-tools 零资产依赖（不联网、不知路径）。
- A2 city-tools 层：city-tools 直接写外部 glb 文件名进产物。破坏「city-tools 纯 CPU 生成器
  零资产依赖」定位（selftest/回归卡 city-tools 都要拖资产），不推荐。

### Q-W3B 摆放数据进入 city.scene 的方式（含车的新增数据）
- **B1（拍板）city-tools 产出**：树沿用已有 RNG 位置（零新增消费，流契约 ①②③④ 不动）；
  车新增生成逻辑但走 **FNV-1a 纯函数**（`hash(seed‖roadEdge‖k)` 定有无/位置/车型，零 RNG），
  沿 roadnet edge 采样停靠；树/车实体写进 city.scene（serializeLayout/serializeCityScene 扩展 +
  selftest 硬闸），drive 只搬运。**优点：canonical 数据源唯一、RNG 流契约零改动、w0 俯视图锚
  兼容（树位置不变）。**
- B2 drive 反推：drive 从 roadnet.json 自行采样摆放。数据源分裂（scene 不含摆放 → 换 drive
  参数城市就变），且 drive 要读 roadnet 语义，违反「drive 只搬运」定位，不推荐。

### Q-W3C 首轮范围
- **C1（拍板）树 + 车 + 路灯试点**：树（61 种池 FNV-1a 选型）+ 车（20 种池 + 沿路停靠）+
  路灯 ×1（验证街具通道机制，沿路口摆放）。街具后续加 = 纯数据扩展。
- C2 首轮即全街具（红绿灯/长椅/消防栓）：需引入 CC-BY 件或另寻 CC0 镜像，扩范围 + 拖验收。

### Q-W3D 路面纹理档（顺带项）
- D1（推荐）本轨不做，留 M-W4+；D2 顺带做（路面车道线贴图，ambientCG/kenney road tiles 另选）。
  首轮聚焦资产通道机制，避免范围膨胀。

## 5. 拍板后执行拆分（草案，Q 定后细化）

| 步 | 内容 |
|---|---|
| M-W3.0 | 资产 vendored 入库（thirdparty/kenney-nature-kit + kenney-car-kit + poly-streetlight，VENDOR.md 溯源）+ city-tools 树/车/街灯实体进 city.scene（Q-W3B 拍板方案）+ selftest 扩展 + w0 基线重拍 |
| M-W3.1 | drive 装配外部 glb + viewer 消费验收（街景双机位：树/车/路灯可见）+ cases.json 卡 + 回归收口 |
