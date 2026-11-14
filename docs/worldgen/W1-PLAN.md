# W1-PLAN · M-W1 城市资产进引擎执行计划

> 立项：M-W1（2026-11-13，worldgen 轨道接 M-W0）。四项拍板全按 `W1-OPTIONS.md` §5（Q-W1A 双轨 / Q-W1B 扁平 / Q-W1C 等 merge / Q-W1D 终端补拍）。
> 铁律边界（承 W0-PLAN）：A 类工具版交代——不碰 `kStage` / `renderer/` / `viewer/` / shader / `.nmesh` 格式（只新增资产文件）/ 不代 asset-ingest merge；city-tools C++ 本体与 w0 冻结产物**一字不动**。
> **落码开工闸：feat/asset-ingest 的 OBJ→glb 链（`scripts/obj2glb.py`）merge 进 master。** 闸未开只做不依赖项（W1.0 的驱动脚本骨架 + 锚设计），不许拿 asset-ingest wt 里的未 merge 文件跑链。

---

## 1. 产物契约（双轨，Q-W1A）

city-tools 现有产物（M-W0 冻结基线，零改动）：

```
example/w0-m3/blocks/*.obj          # 36 块楼体 OBJ（block 局部坐标）
example/w0-m3/city.scene            # E1 World JSON，meshFile 引用 blocks/*.obj
example/w0-m3/roadnet.json          # 路网契约
```

M-W1 新增引擎交付产物（转换驱动步机械派生，全部新文件）：

```
assets/models/city_block_XX.glb     # 扁平命名（Q-W1B），每 block 一份（禁合并巨型 mesh）
assets/meshlets/city_block_XX*.nmesh  # nanite-tools cluster/lod/dag 套件（W1-B 产物形态）
example/w1-m0/city.scene.engine     # E1 World JSON，meshFile = 扁平 .glb 文件名（白名单合规）
```

- `city.scene.engine` 与 `city.scene` 同源：同一份 seed + 同一份 block 几何；差异仅 meshFile 值（`blocks/block_xx.obj` → `city_block_XX.glb`）。E1 schema **零扩字段**。
- glb 文件名带 seed/size 维度编码（如 `city_block_<seed>_<size>_NN.glb`），避免不同参数集撞车。
- 转换确定性：同 seed 双跑驱动产物逐字节一致（obj2glb.py stdlib 确定性 + nanite-tools 确定性 selftest 已内置）。

## 2. 里程碑阶梯

### M-W1.0 — 引擎交付驱动步（tag `w1-m0`）
- **任务**：Python stdlib 驱动脚本（照 obj2glb.py 先例）：① 读 city-tools 产物目录（blocks/*.obj + city.scene）；② 逐块调 obj2glb.py → assets/models 扁平 glb；③ 逐块调 nanite-tools `cluster → lod → dag` → assets/meshlets 套件；④ 产 `city.scene.engine`（meshFile 换 glb 文件名，Transform/其余字段原样）；⑤ selftest 扩锚。
- **验收判据**：① `.nmesh` 套数 = block 数（W1-B 判据收口）；② `city.scene.engine` 引用闭合 + 全部 meshFile 过白名单（扁平 ASCII .glb）；③ 同 seed 双跑逐字节一致；④ 展示台 `w1-m0` 卡（usage / 正向 / 负向，`expectExit` 0/1/2）+ 真实输出证据图；⑤ ctest 双树。
- **闸**：obj2glb.py 已 merge master（Q-W1C）。

### M-W1.1 — 引擎消费验收（tag `w1-m1`，M-W1 收官）
- **任务**：viewer `--scene city.scene.engine` 渲染城市；街景 + 俯视证据图补拍；场景日志锚（batches / triangles）。
- **验收判据**：① 街景证据图（**你终端首跑 run.bat 补拍**，Q-W1D；沙箱只出指引与产物）；② `--only w1-m0` / `w1-m1` 回归绿 + 旧卡小子集 spot；③ `server.py --check` exit 0；④ 四项自查 + `close` 闸（track worldgen）+ `release --done`。

## 3. 冻结面（承 OPTIONS §4）

- city-tools C++ 本体、w0-m0~m3 产物与回归卡零改动。
- `renderer/` / `viewer/` / shader / `.nmesh` 格式 / `kStage` 零触碰。
- asset-ingest 接口只复用不修改；不碰其 wt、不代 merge。
- `cases.json` 仅追加 `kind:"tool"` 卡（第 122 张起）。

## 4. 开工登记

- track `worldgen` 已注册（wt `nl-engine-worldgen` 在位，feat/worldgen 分支）。
- 落码开场：`python tools/wt_pool.py claim --track worldgen --milestone M-W1.0` → 闸检查（obj2glb merge 状态）→ 落码。
- tag 名字制 `w1-*`，不占 m0.* 编号、不 bump `kStage`。
