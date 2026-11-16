# CG0-TOOLS-GUIDE — citygen 工具使用指南(M-CG0 收口归档)

> 工具轨归档口径,对应展示台卡 `example/cases.json` 的 `cg0-m0` 版本(2026-11-16)。

## 定位

`citygen` = OSM 真实地图 → 引擎城市资产的**离线 CLI**(纯 CPU,运行时零联网,引擎侧零改动,
不 bump kStage)。与 `city-tools`(程序化城市)同构输出、数据源互补:citygen 吃 Overpass
`[out:json]`,city-tools 吃 seed。

## 命令总览

```
citygen selftest                     # 8 组断言(解析/bbox/坐标锚/roadnet/建筑/PNG 内容锚)
citygen stats    --input <json> [--bbox S,W,N,E]
citygen buildings --input <json> [--bbox S,W,N,E]   # 建筑统计锚
citygen convert  --input <json> --bbox S,W,N,E --out <roadnet.json>
citygen export   --input <osm.json> --buildings <bld.json> --bbox S,W,N,E --out <dir>
```

负向契约:非法输入/缺参数显式退出 2,不静默。

## 输入数据形态(实测)

- **形态 = Overpass `out geom`**:way 自带 `geometry` 数组,无独立 node 元素,解析器按
  way-only 处理。
- 路网素材:`E:/GuangzhouBusViewer/assets/_pull-roadnet/` 64 分片(全广州 highway,
  2026-10-29,ODbL);建筑需另拉 `way["building"]`。
- 在线拉取实测(2026-11-16):主站 overpass-api.de 对代理出口 IP 反滥用(406);
  **可用配方 = mail.ru 镜像 + GET + 浏览器 UA + 本地代理 + 重试循环**(POST 一律 504,
  约半数失败需重试 2~3 次)。kumi/private.coffee/openstreetmap.ru 当日不可用。

## 输出契约(对齐 city-tools / W0-PLAN §1,只加不减)

| 产物 | 差异 |
|---|---|
| `roadnet.json` | version 1 不变;新增 `osm_name`/`cls`;`lanes_per_dir` 缺省 1(OSM `lanes` 覆盖);`speed_limit` 类别缺省表(OSM `maxspeed` 优先);`signal` 按 M-T4 规则(进入边≥2) |
| `city.scene` | 每 building 一实体(共享 unit-box.obj);**实体级 `label` 字段 = OSM name**(A 档拍板,只加不减;消费端 M-CG1/W2 接线时同契约实现) |
| `top.png` | 确定性栅格:建筑高度灰度 0x58→0xe8(9~120m),路网按类别分色分宽;内容锚 bldPx/roadPx>0 |
| `ODbL-attribution.txt` | 署名随产物落盘(S-PLAN §3;OSM 衍生数据分发须署名,引擎代码不受传染) |

坐标:`dx=Δlng·cos(23.1°)·111320, dy=Δlat·110540`,锚 = bbox 中心(BusViewer 同公式)。

## fixture 与复现

- `test/zjx-sample.json`(300 ways,224KB)/ `test/zjx-buildings-sample.json`(150 栋):
  从真实数据按 id 排序确定性裁剪,进 git 作 golden。
- `test/out-cg0/` = 真实产物 golden 锚(roadnet 375 节点/417 边;scene 150 栋/80 label;
  PNG 2048×1875,bldPx=263136),逐字节回归基准。
- fixture 生成脚本:`test/gen_fixture.py`(记录裁剪口径,可复现)。

## 既知坑(工具轨口径)

1. 新 wt 首次构建前 touch 5 个 scriptapi 生成文件(checkout 同秒 mtime 使 exports.api 新于
   生成产物,freshness 闸真红;wt 内跑 `python tools/scriptapi/gen.py` 亦可)。
2. `assets/osm/` 不进 git(ODbL 数据 + 体量);example 只收确定性裁剪的 fixture 与产物。
3. Overpass 在线入口是兜底,不是主路径——开发期用 BusViewer 离线分片。

## M-CG1 地标混合(2026-11-16,`cg1-m0`)

- **新增 CLI**：`citygen export ... --landmarks <whitelist.json> [--out <dir>]`;
  白名单原文随产物落盘为 `landmark_whitelist.json`。不带 `--landmarks` 的
  三参路径与 CG0 逐字节同构(`cg0-m0` 五文件 sha256 实测全同)。
- **白名单 schema**：`{"version":1,"landmarks":[{osmId?,name,lat,lon,mesh,scale,heightM?}]}`。
  `mesh` 必须是扁平 ASCII `.glb` 名(W1 资产契约),`scale>0`;`osmId=0`(或缺省)
  = bbox 外地标直接追加;非 0 且命中 OSM building → 抑制该 box、产真模型实体。
  `lat/lon/name/heightM` 命中时可省略(从 building 回填),广州塔用真实塔底
  23.1066N/113.3246E 显式锚定。
- **产物差异**：`city.scene` 在 box 实体后追加 `lm_<i>` 实体,组件
  `Model.mesh=<glb>` + `Transform.position=[x,0,z]`(本地米,底座落地)、
  `scale=[s,s,s]`、单位 rotation;实体级 `label` 沿用 CG0 契约。
  `top.png` 顶层加金色方块 `#ffd700`(半宽 7px),画布自动扩到 bbox 外地标,
  `TopViewStats.landmarkPixels` 为内容锚(防止地标被裁掉仍报成功)。
- **资产**：`assets/models/canton_tower.glb` = max 场景抠出的 620.33m 真尺度塔,
  25169 顶点/44178 面/871396B,Z-up→Y-up 烘焙、塔底原点归零、零 node transform,
  glTF bounds Y=[0,620.33] X/Z=±36m。UE 2023 工程版只有 244m(Actor scale=1,
  模型自身艺术缩比且材质槽全空),本轮按用户「用max」拍板弃用。
- **复现**：`build/DX12/citygen.exe export --input modules/citygen/test/zjx-sample.json
  --buildings modules/citygen/test/zjx-buildings-sample.json --bbox
  23.108,113.316,23.130,113.342 --landmarks assets/citygen/landmark_whitelist.json
  --out example/cg1-m0` → 375n/417e/150box + `lm_0`,PNG 2048×1991
  `bldPx=263673 lmPx=256`,scene 151 实体。
- **已知坑**：① max FBX 的 HERO 节点含 4402 个 loose 分量(约 13.5k 顶点散布
  0.6~2.8km 外),必须空间窗裁剪,否则导出一堆别处道具;② glTF 高度轴是 Y,
  `export_yup=False` 会让塔沿 Z 躺倒;③ 俯视画布不扩则 bbox 外地标被裁,
  `landmarkPixels=0` 是假绿;④ 真实照片贴图无源(FBX 贴图路径丢失、UE 塔槽全空),
  facade 贴图链已由 W1.1 ambientCG CC0 三档覆盖,M-CG2 把 citygen 真实数据首次接上该链。

## M-CG2 引擎交付（2026-11-16,`cg2-m0`）

- **脚本**：`scripts/citygen2engine.py`（stdlib,Python 3.13;复用 `obj2glb.py` 的
  `--maps` 嵌贴图链）。`drive --src <citygen 产物目录> --out <交付目录>`
  只读消费 `city.scene`/`roadnet.json`/`buildings/box.obj`/白名单/署名/PNG。
- **scene 派生**：`city.scene.engine` 正则只改 `"mesh"` 值 + `nextID` + 追加
  `car_*` 实体;其余字节(含 UTF-8 label 与 `lm_*`)逐字节保真。150 box 按
  `Transform.scale[1]` 分三档共享 glb（<30m tier1=112 / 30~80m tier2=6 /
  ≥80m tier3=32），各档内嵌 ambientCG CC0 `tierN.jpg`;塔 `lm_0` mesh 保留
  `canton_tower.glb`。8 辆 `car_00..07` 轮换 sedan/taxi/van/police,
  nextID 152→160。
- **派生路网适配(Q-CG2C 用户拍板=双向化最大分量)**：真实 OSM 375 节点/
  417 边裂 44 个弱分量,而 `TrafficCarSystem::init` 用 fnv1a64 从全 lane 池
  随机选起终点,孤立分量必报 `goal lane not reachable`（8 个 seed 全挂实锤）。
  派生层保留最大弱分量 199 节点/260 边,补反向边=452 有向边;源 roadnet
  冻结零改,单向语义只在注记 `ROADNET-ADAPT.txt` 说明。坐标取节点中位数
  归一化到世界原点（viewer 相机固定在原点,km 级坐标会出画）。
- **消费验收**：`viewer --traffic-city example/cg2-m0` 双后端 rc=0,
  `8 cars registered | lanes=500 | signal lamps=401`,600 tick odometer
  496.27m;街景 `cg2-street.png` 可见建筑窗格贴图与车灯,俯视 `cg2-top.png`
  可见楼群分布。双后端 PNG 各自存档(`-VULKAN` 后缀)。
- **坑**：① viewer rc=0 ≠ 画面有内容——首轮街景纯背景色,根因是坐标 km 级
  而相机盯原点,必须派生层归一化 + 肉眼核验截图;② selftest 含 e2e 双跑
  >120s 被默认超时杀,双跑确定性移到 run.bat seg2;③ `%~dp0` 已含尾 `\`,
  拼子路径用 `%ROOT%\assets` 而非 `%ROOT%assets`;④ **证据图 O(像素×N) 性能陷阱**：
  初版每像素遍历 159 实体（2048² 像素=6.6 亿次 Python 循环）致 selftest 90s+ 被
  regression 超时杀,改网格预索引 O(1) 查询 + 画布降到 1024² 后 59 断言 1.9s;
  ⑤ **python 工具卡 args 必须前置脚本路径**（W1 GUIDE §4 铁律）：`exe={python}`
  时漏写 = python 把 `usage` 当脚本名跑裸 REPL,regression 90s 超时。
