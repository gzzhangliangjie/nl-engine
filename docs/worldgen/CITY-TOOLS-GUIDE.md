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
