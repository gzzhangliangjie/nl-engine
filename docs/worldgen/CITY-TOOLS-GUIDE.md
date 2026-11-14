# CITY-TOOLS-GUIDE · 大世界生成工具归档（worldgen 轨道）

> 轨道：A 类并行 `feat/worldgen`（M-W0）；计划权威落点 `docs/worldgen/W0-PLAN.md`。
> 工具 exe = `build/{dx12,vulkan}/city-tools.exe`（纯 CPU、零图形 API、零引擎依赖，
> 仅 vendored stb_image_write.h 出 PNG）。退出码契约：0 = PASS / 1 = 断言失败 /
> 2 = 用法错误（对齐 phys-tools 与展示台 `expectExit`）。本文档按里程碑写实追加。

---

## 1. M-W0.0 — city-tools 骨架 + 确定性地基（2026-11-13，tag `w0-m0`）

### 1.1 子命令与用法

```
city-tools selftest [--verbose]            # 确定性/锚点自测，--verbose 逐条 ok 行
city-tools generate --seed N --size N --out DIR [--stats]
```

- `--seed`：任意 u64（默认 12345），进 pcg32 initstate。
- `--size`：每边 block 数 1..64（默认 6）；越界/缺值/未知选项一律退出 2，
  错误行前缀 `invalid arguments: ...`（负向用例锚）。
- `--out DIR`：必填；产物 `<DIR>/city_top.png`。
- `--stats`：stdout 追加 CityStats 计数块（锚：`stats: blocks=... groundTiles=...`）。
- `generate` 成功输出锚：`nl-citygen M-W0.0 blank city top view` / `wrote ... city_top.png (...)`。

### 1.2 确定性契约（M-W0.0 核心验收，nav/phys 确定性铁律同源）

- **RNG**：自研 pcg32（O'Neill 规范算法，public domain 口径抄算法不抄文件）；
  `seed()` = 规范 srandom(initstate=seed, 流常量 `0x14057B7EF767814F`)。
  已知答案锁实现版本：seed 12345 前三输出 = `1321476956 / 17539747 / 3348728241`。
- **消费序（跨里程碑契约，只许追加不许重排）**：单流，cell 行主序（gx 外层、
  gz 内层），每 cell 一次 `below(16)` 地面灰度档。M-W0.1 车道语义 / M-W0.2 楼体
  分档 / M-W0.3 噪声种子一律排在此流之后。
- **seed 对称 selftest**：同 seed 双跑布局规范序列化（`NLCT01` magic + 固定宽字段
  LE + cell 表）FNV-1a 逐字节一致；异 seed / 异 size 必异。PNG 双编码逐字节一致
  （stb zlib 确定性），异 seed PNG 必异。
- **为什么 M-W0.0 有种子敏感元**：空白城（无楼无车）本身无随机内容，而验收判据①
  要求「不同 seed 必不同」——取最小实现：每 cell 地面灰度 16 档抖动（233−2×shade）。
  M-W0.3 OpenSimplex 密度分区接管视觉分区后，该档位被密度分档替代（流前缀序保留）。

### 1.3 布局口径（bevy_city 同构 + 一处显式偏离）

- block 5.5m × 4.0m（= roadnet.json 契约 `blocks:{x:5.5,z:4.0}`）；`size` 居中
  （lo = −size/2）；block 内区 4.5m × 3.0m（= bevy ground tile 尺寸）。
- **偏离留档**：路网画 (size+1) 条横线 + (size+1) 条竖线全跨度（1.0m 宽），而
  bevy_city 是逐 cell 线段 + crossroad 补丁（+x/+z 两侧开边）。理由：无开边、
  无死路，M-W0.1 roadnet 节点直接取 (size+1)² 交点；拓扑信息量等价于
  「线段 ∪ 交点补丁」再补上边界两圈。
- CityStats 计数锚（size 6）：blocks=36 / groundTiles=36 / roadSegments=72 /
  crossroads=49；buildings/trees/cars/fences/paths 是 M-W0.2/0.3 挂零骨架。

### 1.4 M-W0.0 验收纪要

- selftest 19 条断言全绿（pcg32 流 64 输出一致 + 3 已知答案 + 异 seed 异首输出；
  布局双跑逐字节 / 异 seed 异 / 异 size 异；36/72/49 计数锚；契约锚 5.5/4.0 与
  4.5×3.0；extent 33×24m；shade 界；PNG 双编码逐字节 / 异 seed 异 / 1152×864 画布 /
  PNG 签名）。
- ctest：`w0-city-selftest` / `w0-city-generate` / `w0-city-negative`（双后端）。
- 交代物：`example/w0-m0/`（run.bat 六段 + selftest/generate/usage/negative 日志 +
  `city_top.png` 真实工具输出证据图）+ 展示台 `w0-m0` 卡四用例
  （usage exit2 / selftest / generate --stats / negative exit2）。
- 冻结面自查：`renderer/`、`kStage`、`.nmesh` 格式、`viewer/`、cases.json 渲染卡
  全部未触碰；`example/cases.json` 仅追加 `kind:"tool"` 版本卡。

### 1.5 踩坑

- （M-W0.0 无新增；RNG 已知答案常量由 Python 侧 pcg32 参考实现独立复算后回填，
  非工具自身输出自证。）

## 2. M-W0.1 — 路网拓扑 + 车道语义 → roadnet.json（2026-11-13，tag `w0-m1`）

### 2.1 契约落点（W0-PLAN §1，Q-W2 接口铁律）

- `generate` 在 `city_top.png` 之外新增 `roadnet.json`，字段与形态完全照
  W0-PLAN §1 原文：`version:1` / `seed` / `units:"meters"` /
  `blocks:{x:5.5,z:4.0}` / `nodes[]`（id/x/z/kind/signal）/
  `edges[]`（id/from/to/lanes_per_dir/speed_limit/width）/
  `phases:[]`（预留，citygen 恒不填，由第 8 步扩展）。
- 序列化手写确定性（2 空格缩进、固定字段序、`%.1f` 米、C locale）——
  不引 JSON 库（M-W0.0 零依赖口径延续），selftest 以 in-memory 闭包校验 +
  文本锚 + 双跑逐字节覆盖正确性。

### 2.2 拓扑口径

- 节点 = 全跨度网格线交点，`(size+1)^2` 个，`id = row*(size+1)+col`（行主序），
  坐标 `x=(lo+col)*5.5` / `z=(lo+row)*4.0`；kind 恒 `intersection`
  （所有节点度 >= 2，无 dead_end；signal 恒 false，第 8 步消费）。
- 无向路段 = `2*size*(size+1)`（横、竖各 `size*(size+1)`）；有向边 ×2 =
  `4*size*(size+1)`（size 6 → 168），每条有向边必有同属性反向边。
- 边序：先横后竖、行主序，每路段先 `+向` 后 `-向`，id 连续递增——
  消费方不要依赖此序做语义（只依赖 id 唯一与 from/to 闭合）。
- 车道语义与 bevy_city 对应：`Road{start,end}` 每方向 1 车道 →
  `lanes_per_dir=1`；`speed_limit=13.9` m/s（契约示例同源，约 50 km/h）；
  `width` = 路带几何同源 1.0m（契约示例 3.0 是示意值，以实际布局为准）。
  lane graph、信号相位由第 8 步交通系统在 edges 之上构建，citygen 只供
  几何 + 拓扑 + 车道语义。

### 2.3 两种路段计数口径（并存，各有对应物）

| 字段 | size 6 值 | 口径 | 对应物 |
|---|---|---|---|
| `roadSegments` | 72 | bevy 逐 cell（每 cell 一横一竖） | bevy `CityStats.roads` / M-W0.0 冻结锚 |
| `roadnetEdges` | 168 | 全跨度有向边（契约 edges 数组长） | `roadnet.json` edges |
| `roadnetNodes` | 49 | 全跨度交点 = `(size+1)^2` | nodes 数组 = crossroads |

### 2.4 俯视图叠加（验收判据①）

`renderTopView` 在路网带之上从 `buildRoadnet` 结果反画：每条有向边 1px 暗红
中心线（150,40,40）+ 每节点 3×3 深红标记（120,30,30）——证明 roadnet.json
与俯视几何同源（同数据源两种消费），非布局代码直接画。

### 2.5 M-W0.1 验收纪要

- selftest 23 → 38 条断言全绿（新增 15：节点/边数公式锚、from/to/id 闭包、
  双向对称、几何恰跨一个 block 间距、横竖 84+84、角点坐标、车道属性齐同、
  JSON 双跑逐字节、契约头字段、49 交点/168 边文本锚、异 seed 异 JSON、
  size 1 边界 4 节点 8 边、stats 锚）。
- ctest：新增 `w0-roadnet`（`wrote ...roadnet.json (nodes=49 edges=168 directed)`
  锚），共 4 条 × 双后端。
- 交代物：`example/w0-m1/`（run.bat 七段 ALL GREEN + 证据图 + 4 份日志）+
  展示台 `w0-m1` 卡四用例 + 本 §2。
- RNG：M-W0.1 零消费（空白城车道语义全同质），消费序前缀契约不受影响；
  M-W0.2 楼体分档消费排流尾。

## 3. M-W0.2 — OBB 地块递归细分 + 程序化楼体 → OBJ 中间件 + city.scene（2026-11-13，tag `w0-m2`）

### 3.1 楼体口径（W3-A 全自研；bevy_city 低密度同构）

- 内区 4.5x3.0（block 局部 [-2.25,2.25]x[-1.5,1.5]）递归细分：宽 > 2.3 对半分 x，
  否则深 > 1.6 对半分 z，叶 = 楼地块（4.5x3.0 恒产 4 叶 2.25x1.5；左/前半先入列）。
- 每叶一栋：RNG 消费序 = 先 below(3) 高度档 {3, 8, 14}m，后 below(2) 占地档
  （内缩 0.30 / 0.15）。**消费排流尾**：36 个 shade（M-W0.0 前缀）先全部消费完，
  第二遍 cell 行主序消费楼体档位——selftest 逐个比对锁前缀不变。
- 每 block 4 栋（size 6 → 144 栋）；footprint = 叶矩形内缩，块内两两不重叠
  （selftest 全块校验）。

### 3.2 产物通道（W0-PLAN 写死，本轨不自修 meshing）

- `generate` 产出：`city_top.png` + `roadnet.json` + `blocks/block_NN.obj`
  （每 block 一份 OBJ 中间件：楼 = 8 顶点 12 三角外向绕序盒，block 局部坐标，
  原点 = cell 中心，y-up 米）+ `city.scene`。
- `city.scene` = **E1 World JSON**（M-E2 `World::serialise` 形态）：
  `version:1 / nextID:N+1 / entities[{id,name:"block_XX",flags:0,parent:0,
  components{Model{mesh:"blocks/block_XX.obj",tint},Transform{position=block
  世界中心,scale,rotation}}}] / systems:[]`。手写固定序确定性文本（语义同构，
  非 ECS roundtrip dump，键序不锁字母序）。
- **seed 无关性**：E1 schema 无 seed 键、冻结面禁扩字段——city.scene 是
  seed 无关的索引（同 size 异 seed 产物逐字节相同），种子差异只存在于 OBJ
  楼体档位与 PNG 灰度（selftest 明文锚定此性质）。

### 3.3 资产转换缺口（登记给 asset-ingest 轨道，2026-11-13）

- 实测 master：nanite-tools 无 OBJ 导入、viewer scene 模式资产白名单只认
  `.glb`——city.scene 引用 `.obj` 中间件今天还不能被 viewer 渲染。
- asset-ingest wt（feat/asset-ingest，M-P9V1）已跑通外部 OBJ→glb→
  nanite-tools→`.nmesh` 链（未 merge 进 master）。
- **缺口**：等 OBJ→glb 链 merge 后，补「city-tools 产物→glb/nmesh 转换消费步
  + viewer scene 资产白名单接入」，再补拍 street-view 街景证据图（viewer
  `--scene` 渲染，需带显示环境）；俯视证据图已由纯 CPU 光栅化交付。
- `.gitignore` 加根锚定反白 `!example/*/blocks/*.obj`（楼体 OBJ 是工具交付物，
  不是编译中间产物——全局 `*.obj` 兜底规则会静默挡掉它们）。

### 3.4 M-W0.2 验收纪要

- selftest 38 → 53 断言全绿（新增 15：RNG 前缀契约 / 4 栋每块 / 高度恰三档 /
  占地两档 / 足迹界与两两不重叠 / 高度混合种子敏感 / OBJ 32v-48f-4o 双跑逐字节
  异 seed 必异 / scene E1 锚 version-nextID37-systems / 36 实体 36 mesh 引用闭合
  / scene seed 无关性）。
- ctest 新增 `w0-cityscene`，共 5 条 x 双后端各 3/3（`--repeat until-pass:3`）。
- 回归 `--only w0-m0` / `w0-m1` / `w0-m2` 各 8/8 ALL GREEN；`server.py --check`
  exit 0；`example/w0-m2/run.bat` 七段 ALL GREEN exit 0（含 36 OBJ 文件计数与
  引用闭合锚）。
- RNG：M-W0.0 前缀 36 shade 逐字节不动；M-W0.2 消费 288 次（36 块 x 4 楼 x 2 档）
  全部排流尾。

## 4. M-W0.3 — simplex 噪声密度分区 + 树 + 统计锚收口（2026-11-13，tag `w0-m3`，M-W0 收官）

### 4.1 噪声口径（W3-A 全自研；OpenSimplex 结构同构）

- 自研 simplex 梯度噪声：F2=(√3−1)/2、G2=(3−√3)/6（数学推导），梯度表 8 方向
  (±1,±2)/(±2,±1)（非归一化），径向核 t=0.5−d²、贡献 t⁴·dot；内部纯 double。
- perm 表 256 项由独立 Pcg32(noiseSeed) Fisher-Yates 洗牌（below 拒绝采样），
  复制一倍到 512 回绕；梯度索引双重查表 `perm[perm[ii]+jj]&7`。
- **kNoiseNorm=40 经验归一化**：129² 采样实测 max|raw|=0.0218 → 归一后 max≈0.87，
  |v|≤1 断言余量 13%。Q11-1 拍板 A 口径：噪声进断言 = 统计不变量（值域/均值）
  + 已知答案（eval 四点 + perm 头，Python 参考实现独立复算回填防自证）。
- **noise_scale 0.025 → 0.06 显式偏离**：bevy 值在 33m 跨度 size6 下噪声域仅
  0.83 周期、36 块全落同一波峰（实测全 low 档），判据①「密度分区可见」不成立；
  0.06 → 约 2 周期，四档全可见。密度阈值 0.45/0.6/0.7 与 bevy 同源不变。

### 4.2 RNG 流最终形态（跨里程碑契约收官）

| 段 | 消费 | 归属 |
|---|---|---|
| ① | 36 × below(16) shade（行主序） | M-W0.0 前缀，逐字节不动 |
| ② | 288 × (below(3)+below(2)) 楼体档位 | M-W0.2，值不动 |
| ③ | 1 × next() u32 = noiseSeed | M-W0.3 |
| ④ | 树位置（次数由密度档决定，流尾最后） | M-W0.3 |

selftest 以「重建流 next() == noiseSeed」锁定 ③ 段位置；①② 段由既有断言保持。

### 4.3 密度消费与树

- 密度档：density = noise(cell 中心 × scale)·0.5+0.5 → forest(<0.45)/low(<0.6)/
  medium(<0.7)/high。俯视基调四档（forest 绿 150,184,150 / low 222,226,214 /
  medium 202,208,200 / high 182,188,184）+ shade 微抖保留。
- 树：forest 每块 8 棵 / low 每块 4 棵，每棵消费 below(45)/below(31) 各一次
  （dx∈[−2.2,2.2]、dz∈[−1.5,1.5] 米）；俯视 3×3 深绿块。**仅俯视 + 统计层，
  不进 OBJ/city.scene**——保 w0-m2 冻结基线产物（楼数/顶点/引用闭合锚不破）；
  M-W1 若需树资产走 asset-ingest 通道。
- CityStats 扩：forestCells/lowCells/mediumCells/highCells（和恒 = blocks）+
  trees 实数；serializeLayout 同步扩展（noiseSeed + 4 计数 + cell tier + 树位）。

### 4.4 M-W0.3 验收纪要

- selftest 53 → 72 断言全绿（新增 19：密度档 21/5/6/4、噪声统计锚、eval 四点 +
  perm 头已知答案、noiseSeed 流段位置、树数公式 + 首树位置、异 seed 分布必异
  19/4/4/9 + trees 168、size1 边界含 lo 截断口径锚）。
- ctest 新增 `w0-density`，共 6 条 × 双树各 3/3（`--repeat until-pass:3`）。
- 回归 `--only w0-m0`~`w0-m3` 各 8/8 ALL GREEN；`server.py --check` exit 0；
  `example/w0-m3/run.bat` 七段 ALL GREEN exit 0（stats 密度档 + noiseSeed 锚、
  异 seed 密度分布必异段）。
- RNG：①② 段逐字节不动（重建流断言）；③④ 段纯追加。

## 5. M-W1.1 — 建筑贴图化：ambientCG CC0 立面（2026-11-13，tag `w1-m1`）

- 贴图三档（`thirdparty/ambientcg-facades/`，VENDOR.md 溯源）：tier1=Facade018A
  砖墙住宅 / tier2=Facade001 幕墙办公 / tier3=Facade002 亮窗塔楼（512px JPG）。
  `generate` 增产 `city.mtl`（3 newmtl，map_Kd basename）+ `textures/`（入库
  JPG 逐字节拷贝）；OBJ 加 6 面 quad vt（立面 kWindowUnit[12,8,10] 米/tile
  平铺、顶/底面收缩到纹理角点微区）+ usemtl 档分组。
- 材质分配 = 密度档确定函数（forest 归 tier1），**零 RNG 消费**——M-W0 流
  契约 ①②③④ 逐字节不动。

## 6. M-W2.0 — 程序化楼型池：4 形状原语 × 分档池（2026-11-13，tag `w2-m0`）

- 拍板（W2-PLAN §2）：A1 全自研程序化楼型池（A2 外部 CC0 glb 池否决——kenney
  色板图集前科）+ B1 FNV-1a 纯函数选型（M-W1.1 材质档同构）。
- 形状原语：V0 单盒 / V1 退台（0.55h 分段、上段 0.62 缩放）/ V2 裙楼塔
  （0.30h 裙楼 + 0.55 缩放塔楼，**h<6m 纯函数降级退台**）/ V3 L 形（主体
  全宽×0.62 深 + 翼 0.55 宽×全深，**翼 x0 内缩 0.06m 消共面 z-fighting**）。
- 分档池（容量均 3）：tier1={V0,V1,V3} tier2/3={V0,V1,V2}；选取 =
  `fnv1a64(seed‖block‖building) % 3`，**零 RNG 消费**（流契约头注释冻结）。
- 实现切面：`CityBuilding.variant`（进规范序列化）；`buildingParts` = 楼型
  几何唯一真相源（part = 8 顶点盒 y0..y1，全部落在原 footprint 内——保
  「块内两两不重叠」不变量）；`serializeBlockObj` 按 part 展开（UV 立面 v
  跨度 = 段净高/unit）；`generate --stats` 增 `variants:` 分布行（纯追加锚）。
- 分布锚：seed12345 = single48/setback54/podium8/lshape34；
  seed12346 = 49/54/10/31（裙楼降级把 3m 楼层裙楼挪进退台 → setback > 单盒）。
- selftest 82→88：池界 / 四形状同城出现 / 异 seed 分配必异 / 裙楼降级 /
  全城 36 block OBJ v/vt/f 行数守恒 / part footprint 包含。
- bevy_city 对应（实读留档）：其池 = 每密度档 `Buildings{meshes,materials}` +
  kenney `building-type-N.glb` 外部资产 + `rng.random_range` 消费选型
  （examples/large_scenes/bevy_city/src/assets.rs）；本轨几何自研。

## 7. M-W2.1 — 变体消费验收 + 两处基建加固（2026-11-13，tag `w2-m1`）

- 消费链零适配：city2engine drive / obj2glb / nanite 套件 / viewer 白名单
  直接吃多段楼型几何——「消费链对变体几何零适配成本」是本轨核心判据。
- **run_checked 加固**（city2engine.py）：M-W1.1 的 retries=2 + sleep 0.2s
  实测不够（drive 108 连发下 transient 空输出 3 连挂、失败点漂移、死亡率
  ~11/12；python 单项直拉同命令 20/20 全过）→ retries=4 + 指数退避
  0.5s→1s→2s→4s。确定性产物不受影响，只影响瞬时干扰恢复力。
- **w1_check.py hash 集合修复**（W1.0 潜伏 bug 基线重拍炸出）：文档口径 =
  glb + meshlets + scene.engine + evidence png；旧 `HERE.iterdir()` 全量
  收录把 h1/h2 重定向文件卷进集合——h1 计算时 0B、h2 计算时已有内容，集合
  永不对称 → run.bat seg5 确定性对拍**按原写法永远不通过**（M-W1 验收是
  复刻工具在集合外对拍的等效口径）。修复后 seg5 真跑 MATCH。
- **w1-m1/w2-m1 image 卡补 baselines 登记**（m0.2 老口径：per-case
  `baselines:{dx12,vulkan}` 指向产物 PNG）——旧卡无 baselines，回归
  judge_image 基线缺失必 FAIL（W1 收官「回归 8/8」实为口径含糊，本轨勘误）。

## 8. M-W3.0 — 树/车/路灯资产通道：摆放数据 canonical 化（2026-11-14，tag `w3-m0`）

- 拍板（W3-PLAN §4）：**A1** drive 层装配（city-tools 零资产依赖，只写
  vendored 扁平名）+ **B1** city-tools 产摆放数据（树沿用 M-W0.3 RNG 位置，
  树型/车型/街灯全部 fnv1a64 纯函数）+ **C1** 树+车+路灯试点（红绿灯等
  CC-BY 件不进，poly.pizza 探针三件均 CC-BY）+ **D1** 路面纹理不做。
- 池：`kTreeVariantNames` 12 种（kenney nature kit 61 树中选形状代表）×
  `kCarVariantNames` 8 种（kenney car kit）× 路灯 1 种（Quaternius，
  poly.pizza CC0）。名字表 ↔ `thirdparty/{kenney-nature-kit,kenney-car-kit,
  poly-streetlight}/` 一一对应（selftest 白名单形态闸）。
- 生成逻辑（`generateStreetAssets`，generateCity 尾部调用、内部重建 roadnet）：
  车 = 每有向边 `hash(seed‖'car'‖edgeId) % 4 == 0` 停 1 辆（168 边期望 42），
  t ∈ [0.2,0.8] 避路口 + 垂边 0.3m 路缘停靠 + `hash%8` 车型 + edgeId 溯源
  （M-T0 交通消费接口预埋）；路灯 = 每路口 1 盏 `hash%4` 四角选位。
- scene 实体 36→315（36 楼块 + 188 树 + 42 车 + 49 路灯，nextID=316 锚）；
  车 rotation = 绕 Y 四元数 `[0, sin(yaw/2), 0, cos(yaw/2)]`；`--stats` 增
  `street:` 分布行（seed12345 = 42 车/49 灯/树 12 种 14~18 棵；seed12346 =
  42 车/168 棵全均匀 14——**42 恰是 p=1/4 二项分布众数，两 seed 同值非异常**）。
- selftest 88→97；W0 挂零锚 `cars=0` 改写为 `cars=42 props=49`（W0「骨架
  计数挂零」语义已由本轨兑现，fences/paths 仍挂零）。
- 基线波及：w0-m2（nextID 37→316 锚）、w0-m3、w2-m0 全重拍；OBJ/PNG 产物
  字节不变（树不进 OBJ、俯视图不画车/灯）。

## 9. M-W3.1 — drive vendored 装配 + 两处资产端变形（2026-11-14，tag `w3-m1`）

- `copy_vendored_assets`：scene mesh 引用解析 → thirdparty 三目录查找 →
  `assets/models/` 同字节去重拷贝（21 件 = 12 树 + 8 车 + 路灯全池被引用）；
  blocks/block_NN.obj 与 city_block_*.glb 两类中间引用跳过。GLB_NAME_RE 扩
  连字符（`sedan-sports.glb`；viewer `validAssetFileName` 允许全部可打印
  ASCII，drive 侧保持安全子集）。w1_check hash 集合按「引用闭合」扩入
  vendored glb。
- viewer 渲染 objects=315 / batches=215，车/路灯/树全链零引擎改动直渲。
- **变形一（车贴图外部 uri）**：cinevva 车包 glb 的 image 是外部 uri 引用
  （`Textures/colormap.png` 分离残留），tinygltf 不加载外部文件 → 渲染报
  「not 8-bit decoded RGBA-capable data」。`tools/embed_glb_textures.py`
  把官方 CC0 colormap.png（sz-games 镜像获取，512x512 RGBA）追加进 BIN
  chunk 转嵌入式 bufferView（8 车全处理）。教训：**单文件 glb 获取渠道可能
  携带外部 uri 残留，样张解剖必须查 image.uri 有无**。
- **变形二（截图脚本文件名）**：for 循环复刻拍图时两后端写同一输出文件名
  → VULKAN 覆盖 DX12 主图且 -VULKAN 后缀图从未落盘 → 回归基线混用两代产物
  大面积 FAIL。教训：**多后端批量拍图必须逐后端显式输出文件名，循环变量
  只许进后端段不许进输出路径**。
- 特写机位两次探针实测 dist 21/26 < 城市半径（6 块 × 5.5m ≈ 33m）必钻楼内
  （M-W2.1 教训复现+量化：**orbit 相机 dist 必须 ≥ 城市对角半径**），证据
  机位沿用 top/street 验证位。
