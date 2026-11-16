# CG2-PLAN — M-CG2 citygen 产物进引擎（A①转换脚本轨）

> 立项：2026-11-16 用户「继续」→ 开工闸报「待拍板 2 / 已拍板 6 / 冻结面 5」→ 用户「按推荐」
> = Q-CG2A **A**（新增 `scripts/citygen2engine.py`，只读消费 citygen 产物，viewer 零改动）
> + Q-CG2B **①**（重开 `citygen` 轨 `claim --force`，M-CG2）。

## 1. 三处证据（开工闸实读，本轮 2026-11-16）

| 源 | 实读结论 |
|---|---|
| `docs/worldgen/CITY2ENGINE-GUIDE.md` + `scripts/city2engine.py` | W1 已趟通同范式：`city.scene` → `city.scene.engine`（**只重写 mesh 值、其余逐字节保真**）+ flat glb 白名单 + `obj2glb.py --maps` 嵌 ambientCG tier 贴图 + 确定性双跑 + `{python}` exe token 展示卡。但它绑 `blocks/block_NN.obj`（每块 4 楼带 UV），不适配 citygen 的**单共享 unit box（无 UV、150 实体靠 Transform scale 拉伸）**。 |
| `viewer/main.cpp` `--traffic-city`（5521-5646）+ scene gate（5694-5715） | 硬约束三条：① DIR 含 `city.scene.engine` + `roadnet.json`；② scene 内 **必须 ≥1 个 `car_*` 实体**（否则 exit 2）；③ 每个 `Model.meshFile` 必须过 `validGlbAssetName`（**无路径分隔符、flat ASCII `.glb`**）。`car_*` 只需 Model+Transform（TrafficCarComponent 运行时挂）。 |
| `docs/game3/S-PLAN.md` §4「下游」+ §5 冻结面 | §4 已挂「OSM 城市接 city2engine → viewer 驾驶」待出计划；§5 冻结面：`.nmesh`/`city.scene`/`roadnet.json` 只加不改、**引擎侧零改动**（A 方案满足）。 |

## 2. 拍板记录

| 问 | 结论 | 理由 |
|---|---|---|
| Q-CG2A 消费接线路径 | **A 新增 `scripts/citygen2engine.py`**（stdlib，Python 3.13） | B 改 viewer 破坏 glb 白名单铁律；C 改 citygen export 重写 CG0/CG1 冻结基线。A 是唯一不碰冻结面的路径。 |
| Q-CG2B 轨道 | **① 重开 `citygen` 轨**（`claim --force`，M-CG2） | 同一数据线；S-PLAN §4「下游」已有位置。 |
| 贴图源 | **沿用 W1.1 已拍板 ambientCG CC0 三档**（`thirdparty/ambientcg-facades/tier{1,2,3}.jpg`） | 程序化像素 W1.1 被否，不重问。 |
| Q-CG2C 路网适配（实现中发现新决策） | 用户「双向化最大分量」：派生 roadnet 只留最大弱分量 199 节点/260 边 + 反向边 = 452 有向边；源 roadnet 冻结零改 | 真实 OSM 375 节点裂 44 弱分量/314 SCC，全池随机选路 8 seed 全挂；另两案（裁 SCC 13 节点 / 降级不实跑）被否 |

## 3. 冻结面（A 方案全程不碰）

1. `cg0-m0`/`cg1-m0` 五文件（city.scene/top.png/roadnet.json/box.obj/ODbL 署名）sha256 只读消费，不重写；
2. viewer / renderer / core 零源码改动，kStage=S8.3 不动；
3. `roadnet.json` schema（W0-PLAN §1）只加不改——本轮**逐字节拷贝**；
4. `city.scene` E1 World JSON 只加不改——派生 `city.scene.engine` 只重写 mesh 值 + 追加 car 实体，原 scene 一字不动；
5. `assets/models/` 既有 glb 不覆写；新增 glb 用新扁平名。

## 4. 实施方案

### 4.1 `scripts/citygen2engine.py`（纯 stdlib）

```
usage:
  citygen2engine.py
  citygen2engine.py selftest [--verbose]
  citygen2engine.py drive --src DIR --out DIR [--models-out DIR] [--stats]
exit: 0 ok / 1 runtime / 2 usage
```

`drive` 步骤（输入 = `example/cg1-m0/` 四件套）：

1. **读**：`city.scene`（`newline=""` 保字节）、`roadnet.json`（二进制）、
   `buildings/box.obj`、`landmark_whitelist.json`、`ODbL-attribution.txt`、`top.png`。
2. **box → tier glb（3 个）**：citygen 的 unit box 8 顶点/无 UV，按 W1.1 口径补每面 4 顶点 UV
   + `usemtl tierN` + `mtllib`，**按建筑高度分档**（`Transform.scale[1]`：<30m=tier1、
   30~80m=tier2、≥80m=tier3；W1 的 `kWindowUnit` 密度档口径在真实 OSM 数据上无密度字段，
   改用高度档，属派生层决策，不回写 citygen 冻结产物）→ 调 `obj2glb.main([obj, glb, --maps,
   textures])` 产 `city_box_tier{1,2,3}.glb`（flat 白名单名、内嵌 ambientCG JPG）。
   每 tier 一个 glb，150 实体按高度重写 `"mesh"` 值 → 贴图按档可见；**同档共享 glb**，
   不是每楼一份（沿用 W1 同字节 image 去重精神）。
3. **地标**：`lm_*` 实体 mesh 已是 `canton_tower.glb`（flat 白名单，M-CG1 已产），
   逐字节拷 `assets/models/canton_tower.glb` 到 `--out`，零 transform 改写。
4. **car 注入**：从 `roadnet.json` 节点确定性取前 N=8 个（按 node id 升序，避免 RNG），
   mesh 轮换 `sedan.glb`/`taxi.glb`/`van.glb`/`police.glb`（W3 已入库白名单资产，
   拷到 `--out`）；实体 `car_00..07` 追加到 entities 数组尾，`nextID` 相应 +8；
   只为viewer的car_*门槛（5521-5541），TrafficCarComponent 由 viewer 运行时挂（不需写进 JSON）。
5. **rewrite_scene**：正则只改 `"mesh": "buildings/box.obj"` → tier glb，其余字节（含 `label`
   中文、`lm_*` 实体、ODbL 注记）逐字节保真；写 `city.scene.engine`（`newline=""`）。
6. **拷贝**：`roadnet.json`（逐字节）、`ODbL-attribution.txt`、`top.png`（改名 `citygen_top.png`
   作证据图底图，不覆盖）、`landmark_whitelist.json`。
7. **stats/evidence**：产 `city_delivery.png`（stdlib PNG：150 box 实体真实位置 × 高度，
   塔标金色块——从 `city.scene.engine` 真实 Transform 反推，非手绘）。

### 4.2 确定性契约

- 同 `--src` + 同脚本 → `city.scene.engine` / 3 glb / 8 car 引用 / delivery PNG 逐字节一致（双跑 sha256 门禁）；
- OBJ/MTL 内容固定序写出，无时间戳；glb 由 `obj2glb.main` 产（已验确定性）；
- car 节点选择按 `id` 数值升序，无 dict 顺序依赖；PNG 由 stdlib zlib 固定压缩。

### 4.3 展示卡 `cg2-m0`

- `kind:"tool"` 卡 + `exe:{"dx12":"{python}","vulkan":"{python}"}`（沿用 W1 口径）；
  args 首 token = `scripts/citygen2engine.py`。
- 用例：usage（exit 2）/ selftest（>=20 组）/ drive 正向（`--src example/cg1-m0 --out example/cg2-m0`）
  / negative（缺 city.scene → exit 2）。
- `preview = example/cg2-m0/city_delivery.png`。
- 附加**引擎实跑**验收（不进 tool 卡，走 run.bat 段 + 人工证据）：
  `viewer.exe --traffic-city example/cg2-m0 --traffic-steps 75 --no-hud --screenshot <out> --frames 8
  --size 960x540 --yaw 28 --pitch -10 --dist 30`，expectLog `traffic-city: 8 cars registered`；
  双后端截图 `example/cg2-m0/cg2-street.png` / `-VULKAN.png`。

## 5. 验收判据

1. `citygen2engine.py selftest` PASS（>=20 断言：白名单、mesh 重写、tier 分档、car 注入、确定性、PNG 签名）；
2. `drive` 双跑逐字节一致；`city.scene.engine` 是合法 E1 World JSON（json.load 逐级验层级，
   grep 不算数），所有 mesh 过 flat 白名单，`lm_0` mesh=`canton_tower.glb`；
3. roadnet.json 与 cg1-m0 源逐字节 sha256 同；cg0/cg1 冻结产物零改（git status 无 M）；
4. `viewer --traffic-city example/cg2-m0` 双后端 **8 cars registered** + 截图成功（非空 PNG）；
5. showcase `--check` exit 0；`regression --only cg2-m0` 双后端 bytes ALL GREEN；
6. close 闸 PASS + RELEASED；commit + annotated tag `cg2-m0` + master ff + push。

## 6. 范围边界

- 只做**工具链派生 + 引擎实跑验收**，不改 viewer/core/renderer 一行；
- **程序化 facade 贴图**已由 W1.1 ambientCG 三档覆盖，本轮是「citygen 真实数据首次接上该链」，
  不是新贴图方案；
- 地标仅广州塔单体（M-CG1 口径），多地标/全城 V4 流送属后续里程碑。
