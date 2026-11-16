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
