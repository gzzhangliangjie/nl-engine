# W1-OPTIONS · M-W1 城市资产进引擎方案矩阵

> 立项调研：M-W1（2026-11-13，worldgen 轨道接 M-W0；用户拍板范围 =「城市资产进引擎」）。
> 本轮 = 立项调研零落码：只产出本文档，不 claim / 不写码 / 不动产物。
> M-W0 登记的缺口（PARALLEL-TRACKS asset-ingest 行接收栏）：city-tools 产物→glb/nmesh 转换消费步 + viewer scene 接入 + 街景证据图。

---

## 1. 现状盘点（2026-11-13 实读，可追责）

| 模块 | 落点 | 实读结论 |
|---|---|---|
| city-tools 产物（M-W0 冻结） | `example/w0-m3/blocks/*.obj`（36 块楼体 OBJ）+ `city.scene`（E1 World JSON，meshFile 引用 `blocks/*.obj`）+ `roadnet.json` | city.scene meshFile 现指 OBJ，**引擎当前消费不了**；w0-m2/m3 回归卡含「同 seed fc 逐字节」段，改产物 = 改冻结基线 |
| viewer scene 白名单 | `viewer/main.cpp` `runScene`（validGlbAssetName）+ `renderer/src/scene.cpp`（同款复制兜底） | 只认 `assets/` 下**扁平 `.glb` 文件名**：禁 `/`、`..`、非 ASCII；违规显式报错退出 2。city.scene 现引用**子目录 + .obj** = 双违规 |
| scene 模式渲染路径 | `renderer/src/scene.cpp` ECS scene 段 | meshFile → `loadGltfScene`（tinygltf）顶点展开直渲，**不经过 .nmesh**；`.nmesh` 是 cluster/cull 模式路径（NL_MESHLET_DIR）。多 primitive 批渲染在 feat/asset-ingest 未 merge（master 现状只渲 prim[0]） |
| 转换链（asset-ingest 轨） | `scripts/obj2glb.py`（323 行，stdlib 零依赖，≤4 材质组硬闸）+ `nanite-tools cluster→lod→dag` | OBJ→glb→nmesh 全链在 feat/asset-ingest 已跑通（ParkingLot 实证），**未 merge master**（4 commits，M-P9V1 另一会话占用中） |
| nanite-tools 入口 | `build/{DX12,VULKAN}/nanite-tools.exe` | `cluster <glb> -o out.nmesh` → `lod <nmesh> -o prefix` → `dag <prefix>`，确定性 selftest 内置 |

**三处参考口径**（复用 2026-11-12 资产格式对照留痕 + 本轮 asset 链实读）：Carrot glTF + `CARROT_meshlets` 私有扩展 / bevy `.meshlet_mesh` + `from_mesh` / UE `NaniteResources.h` 私有序列化——本项目既有结论不变：`.nmesh` 格式冻结（M0.7/S3），nanite-tools 是唯一合法入口，M-W1 只复用不自修 meshing。

---

## 2. 关键决策点（未拍板不落码，AGENTS.md 开工闸）

### Q-W1A · city-tools 产物形态：双轨 or 单轨 ⭐（最重）

| 方案 | 内容 | 代价 |
|---|---|---|
| **α 双轨（推荐）** | city-tools 产物（OBJ + city.scene 引用 OBJ）**一字不动**；M-W1 新增独立「引擎交付驱动步」：OBJ×36 → obj2glb → `assets/models/city_block_*.glb` → nanite-tools → `assets/meshlets/` nmesh 套件 → 另产 `city.scene.engine`（E1 schema，meshFile 指 glb 文件名） | w0-m2/m3 冻结基线零重拍；代价 = city.scene 两份（源头同一份几何，转换步机械派生） |
| β 单轨 | city-tools 直接改产 glb 引用的 city.scene | 触「冻结基线的代码形态也是基线的一部分」铁律：w0-m2/m3 的 fc 逐字节段 + 证据图 + 回归卡全重拍（须单独拍板 + 预算） |

### Q-W1B · glb 资产布局：扁平 or 子目录

| 方案 | 内容 | 代价 |
|---|---|---|
| **α 扁平（推荐）** | `assets/models/city_block_00.glb` 等 36 个平铺（名字含 seed/size 维度，避免撞车） | viewer / renderer 白名单**零改动**（登记缺口里的「白名单接入」实测可免——扁平命名天然合规） |
| β 子目录 | `assets/models/city/*.glb` | 需改 viewer/main.cpp + renderer/scene.cpp 两处白名单复制（跨轨冲突面：asset-ingest 分支正在改 scene.cpp batching，merge 必冲突） |

### Q-W1C · obj2glb.py 依赖处置（asset-ingest 未 merge）

| 方案 | 内容 | 代价 |
|---|---|---|
| **α 等 merge（推荐）** | feat/asset-ingest 4 commits merge master 后复用 `scripts/obj2glb.py`（接口只复用不修改，W0-PLAN 冻结面原口径） | M-W1.0 落码依赖 asset-ingest 会话收官；merge 前只做不依赖项（驱动脚本骨架 / selftest 锚设计） |
| β 协调提前 merge batch1 | 请 asset-ingest 会话先 merge batch1（obj2glb + ParkingLot），batch2 续跑 | 跨会话协调；batch1/batch2 拆开 merge 的台账口径要对方同意 |

### Q-W1D · 街景证据图

viewer `--scene` 渲染城市需**带显示环境**（M-TR0 实锤：无头沙箱跑不了 GUI）。按 M-CRASH0 L2 先例：本机生成 run.bat + 指引，**由你终端首跑补拍**，证据图落 `example/w1-*/`。

---

## 3. 里程碑切分草案（均 A 类工具版交代，不 bump `kStage`）

| 里程碑 | 内容 | 验收判据 | tag |
|---|---|---|---|
| **M-W1.0** 引擎交付驱动步 | 转换驱动（py stdlib）：36 OBJ → glb（assets/models 扁平）→ nanite-tools cluster/lod/dag（assets/meshlets）→ `city.scene.engine`（E1 schema，meshFile 指 glb）+ selftest 扩锚 | ① `.nmesh` 套数 = block 数（W1-B 判据收口）；② city.scene.engine 引用闭合 + 白名单合规（扁平 .glb 文件名）；③ 同 seed 双跑驱动产物逐字节一致；④ 展示台 w1 卡（usage/正向/负向） | `w1-m0` |
| **M-W1.1** 引擎消费验收 | viewer `--scene city.scene.engine` 渲染城市（你终端补拍街景 + 俯视证据图）+ 场景锚（批数/三角数日志锚） | ① 街景证据图（显示环境实拍）；② `--only w1-m0` 回归绿 + 旧卡小子集 spot；③ 四项自查 + close 闸（track worldgen） | `w1-m1` |

> 驱动脚本形态默认 = Python stdlib（照 obj2glb.py 先例），落 `scripts/` 或 `city-tools/` 由拍板附带决定；city-tools C++ 本体零改动（保 w0 基线）。

---

## 4. 冻结面

- city-tools C++ 本体与 w0 产物**零改动**（Q-W1A α 成立时）；`renderer/` / `viewer/` / shader / `.nmesh` 格式零触碰（Q-W1B α 成立时）。
- asset-ingest 接口只复用不修改；不代 asset-ingest merge、不碰其 wt。
- `kStage` 不动；`cases.json` 仅追加 `kind:"tool"` 卡。

---

## 5. 拍板记录（2026-11-13）

| 编号 | 问题 | 结论 |
|---|---|---|
| **Q-W1A** | city-tools 产物形态 | **双轨**：city-tools 与 w0 产物一字不动；另产 `city.scene.engine`（glb 引用版），w0-m2/m3 基线零重拍 |
| **Q-W1B** | 36 个 block glb 布局 | **扁平命名** `assets/models/city_block_XX.glb`：viewer/renderer 白名单零改动，避开 asset-ingest 分支 scene.cpp 冲突面 |
| **Q-W1C** | obj2glb.py 依赖处置 | **等 merge**：feat/asset-ingest merge master 后复用 `scripts/obj2glb.py`；merge 前只做不依赖项（驱动脚本骨架 + 锚设计） |
| **Q-W1D** | 街景证据图 | 沿 M-CRASH0 L2 先例：本机生成 run.bat + 指引，**用户终端首跑补拍**（无头沙箱跑不了 GUI） |

执行计划落 `docs/worldgen/W1-PLAN.md`；落码开工闸 = asset-ingest OBJ→glb 链 merge 进 master。
