# GAME3 S-PLAN — 游戏③ 广州地图 + 复杂交通 总计划

> 立项:2026-11-16,用户指令「广州地图项目开工」。游戏③ = 广州正式地图 + 复杂交通 + 开车漫游
> (2026-11-15 三游戏方向拍板③)。本文件是游戏③总计划骨架;各里程碑详案落 `docs/citygen/` 等
> 对应目录,排期基准 = `docs/GAME-DEV-ORDER.md` 12 步台账(第 6 步程序化城市线 M-W0~W3 已收官)。

## 1. 拍板记录(2026-11-16,三项全拍)

| 问 | 结论 |
|---|---|
| Q-GZ1 第一版地图范围 | **区域试水**:2~3km 见方(首选珠江新城/天河一带),显存内(对齐 Q-C 既有拍板);全城挂 P9 V4 流送(GAME-DEV-ORDER 第 10 步)后再拉 |
| Q-GZ2 OSM 数据获取与解析 | **Overpass + nlohmann**:curl 按 bbox 拉 `[out:json]` 落盘缓存(离线解析,运行时零联网);解析用 nlohmann json 单头(仓内 tinygltf 内嵌同款,原样复制独立 vendored + VENDOR.md,MIT,零新外部依赖) |
| Q-GZ3 轨道归属 | **新轨 `citygen`(feat/citygen)**,编号 M-CG0 起;citygen 是自研库(`modules/citygen/`,2026-11-15 拍板,非 thirdparty),与 worldgen 的 city-tools 是演化关系非延续 |
| Q-GZ0 三游戏顺序 | 用户点名游戏③开工 = ③ 先行(①科目二、②Brotato 顺延);MEMORY 旧「未拍板顺序」就此清账 |

## 2. 参考资产(GuangzhouBusViewer,用户 2026-11-16 指定)

`E:\GuangzhouBusViewer`(Electron + Leaflet 公交/地铁查看器,OSM 数据生态已趟通):

| 资产 | 内容 | 对 citygen 的价值 |
|---|---|---|
| `assets/_pull-roadnet/tile-*.json`(64 片,~230MB) | 2026-10-29 全广州 `way["highway"]` Overpass `[out:json]` 原始分片(8×8 分片,0.199°×0.135°/片) | **M-CG0 离线测试素材直接可用**;珠江新城 ≈ tile-3-2(lat 22.946~23.145 / lon 113.22~113.355) |
| `data/routing-graph.json`(68.4MB) | 合并产物:222 万原始节点 → 51.3 万路口 / 70.9 万有向边 / 453 万形状点 / 2 万路名 | 节点合并(路口判定)与紧凑路由图形态的实测参照 |
| Overpass 镜像策略 | 5 镜像并发竞速 + 重试:`overpass-api.de` / `kumi.systems` / `private.coffee` / `maps.mail.ru` / `openstreetmap.ru`;大陆直连不稳定是实测结论 | CLI `--overpass` 入口复用同一镜像清单 |
| 坐标换算 | WGS-84 → 局部米:`dx=Δlng·cos(lat)·111320, dy=Δlat·110540`(route-planner.js) | citygen 局部坐标系同公式;GCJ-02 纠偏仅高德底图需要,引擎渲染一律 WGS-84,不做纠偏 |

## 3. 数据许可注记(如实留档)

- OSM 数据 © OpenStreetMap contributors,**ODbL** 许可:citygen 产物(roadnet/PNG/城市数据)属
  衍生数据库,对外分发须按 ODbL 署名;**引擎代码(MIT)不因消费数据被传染**(数据库许可不作用于
  代码)。城市数据文件随包分发时附署名注记(沿用 BusViewer「代码 MIT / 地图数据 ODbL」双轨口径)。
- nlohmann json(MIT)单头 vendored 进 `thirdparty/nlohmann/`,VENDOR.md 写明版本 / 来源(tinygltf
  内嵌同款)。

## 4. 里程碑骨架(逐步细化,每个开工时单独过闸)

| 编号 | 内容 | 详案 |
|---|---|---|
| **M-CG0** | citygen 库骨架 + OSM 输入源(Overpass JSON 解析 → 区域路网 roadnet.json + 建筑块 city.scene + 俯视 PNG + CLI 壳),区域试水范围 | `docs/citygen/CG0-PLAN.md` |
| M-CG1+ | 程序化规格网输入源迁入库(与 city-tools 对齐)、车道数 >1(多车道前提)、行人路网(人行道 navmesh,归属届时拍板)、地块密度/POI | 待 M-CG0 收官后立项 |
| 下游 | OSM 城市接 city2engine → viewer 驾驶(GAME-DEV-ORDER 第 7 步口径) | 届时出计划 |

## 5. 冻结面

- `.nmesh` / `city.scene`(E1 World JSON schema)/ `roadnet.json`(W0-PLAN §1 接口铁律)只加不改;
  OSM 版新增字段(如 `osm_name` / `highway` 类别)属「只加字段」,version 不递增。
- 引擎侧零改动:citygen 是纯 CPU 离线库 + CLI,不碰 renderer / core / kStage(A 类工具轨)。
