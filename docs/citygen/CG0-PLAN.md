# CG0-PLAN — M-CG0 citygen 库 + OSM 输入源(feat/citygen 并行轨道)

> 立项:2026-11-16,用户三项拍板(Q-GZ1 区域试水 / Q-GZ2 Overpass+nlohmann / Q-GZ3 新轨 citygen),
> 详见 `docs/game3/S-PLAN.md` §1。本文件是 M-CG0 详案。
> 前置实读:三处渲染参考(Carrot / bevy_pbr / UE Renderer)经查均无 OSM / 真实地图导入同类模块
> (W2-PLAN 立项时已实证 Carrot 无 world 模块;bevy_city 是程序化生成;UE 树内无 OSM,World
> Partition 只管流送)→ 本模块属「无成熟参考」域,**对照锚 = ①GuangzhouBusViewer 实测资产
> (用户指定参考,详见 S-PLAN §2)②city-tools 既有输出契约(M-W0~W3/M-T4)③bevy_city 消费形态**
> (M-W0 立项时已读)。

## 1. 目标

`modules/citygen/` 自研库(M-CG0 只落 OSM 输入源,程序化规格网输入后续里程碑迁入):
Overpass `[out:json]` 数据 → 区域(珠江新城一带 2~3km)真实路网 + 建筑块 → 与 city-tools 同构的
三件产物(`roadnet.json` / `city.scene` / 俯视 PNG)+ CLI 壳。纯 CPU 离线,运行时零联网,
引擎侧零改动(不 bump `kStage`)。

## 2. 范围与输入

- **范围**:Q-GZ1 区域试水。开发样本 bbox 取珠江新城(约 23.108~23.130N,113.316~113.342E,
  ≈2.4×2.9km),CLI 支持 `--bbox south,west,north,east` 任意指定。
- **道路输入**:已有离线素材 `E:/GuangzhouBusViewer/assets/_pull-roadnet/tile-3-2.json`(全广州
  highway 分片,2026-10-29,Overpass `[out:json]`)按 bbox 过滤;CLI 另留 `--overpass` 在线入口
  (镜像清单照抄 BusViewer 五镜像竞速,失败落缓存兜底)。**分片只含 `way["highway"]`,不含建筑**。
- **建筑输入**:需单独拉一次 `way["building"]`(bbox)(区域小,单请求可过);拉取脚本落
  `assets/osm/` 缓存(不进 git,样本进 example 时裁剪)。M-CG0.2 消费。
- **解析库**:nlohmann json 单头 vendored `thirdparty/nlohmann/`(源 = 仓内 tinygltf 内嵌同款
  原样复制 + LICENSE + VENDOR.md,MIT,版本如实)。选型闸合规:MIT / header-only / 零构建期
  新依赖 / 无使用风险(MIT 无显式专利授权条款,如实写明)。

## 3. 输出契约(对齐既有铁律,只加不减)

| 产物 | 契约源 | OSM 版差异 |
|---|---|---|
| `roadnet.json` | `docs/worldgen/W0-PLAN.md` §1(Q-W2 接口铁律)+ M-T4 phases 填充规则(进入边≥2 置位) | 新增字段:`osm_name`(路名)、`cls`(highway 类别映射);`lanes_per_dir` 按 OSM `lanes` 标签缺省 1;`speed_limit` 按 highway 类别缺省表(motorway 33.3 / trunk 22.2 / primary 16.7 / secondary 13.9 / residential 8.3 m/s,OSM `maxspeed` 标签优先);`signal` 按 M-T4 规则填充。只加字段,version 保持 1 |
| `city.scene` | E1 World JSON schema(冻结面,不扩字段);city-tools 同构 | 每 building footprint 一实体(OBJ 中间件同 city-tools 口径),bbox 外剔除 |
| 俯视 PNG | city-tools 俯视图口径(确定性软件绘制) | 底图 = 真实路网(按 highway 类别分宽分色)+ 建筑块;附署名注记(ODbL) |
| CLI 壳 | `modules/citygen/` 下 `citygen` exe | `--input <overpass.json> --bbox ... --out <dir>` / `--overpass --bbox ...`(在线)/ `--selftest` |

- 坐标:WGS-84 → 局部米,中心锚定 bbox 中心,`dx=Δlng·cos(23.1°)·111320, dy=Δlat·110540`
  (BusViewer route-planner.js 同公式);锚点与偏移写进产物元信息,保证可复现。
- 路网拓扑:OSM way 端点/交叉点合并为路口节点(参照 routing-graph.json「51 万路口/71 万边」
  的紧凑形态,区域版同法缩配);**way 中间形状点不进节点表**,作边 shape 留给俯视 PNG 绘制。
- ODbL 署名注记随产物落盘(S-PLAN §3)。

## 4. 步骤

| 步 | 内容 | 交代物 |
|---|---|---|
| M-CG0.0 | nlohmann vendored + 库骨架(include/src)+ Overpass JSON 解析器(tile 分片解析:bbox 过滤、node 索引、way 拓扑)+ CLI 壳 `--input`/`--bbox`/`--selftest` | selftest 解析锚(样本 way/节点计数、bbox 剔除率)+ ctest |
| M-CG0.1 | highway → roadnet.json 转换:路口合并 / 类别→车道速度映射 / phases(M-T4 规则)/ 确定性序列化 | roadnet 契约 selftest(字段齐备 / from-to 引用闭合)+ 样本数值锚 |
| M-CG0.2 | 建筑数据拉取(一次性 bbox)+ building footprint → 块 / city.scene / 俯视 PNG | example/cg0-*/ 卡(cases.json,kind:"tool",preview = 俯视 PNG)+ 回归 |

## 5. 验收判据

1. `citygen --selftest` 全断言绿:解析计数锚 / bbox 过滤 / 路口合并幂等 / roadnet 契约 / PNG 确定性。
2. `roadnet.json` 契约校验:字段齐备、`from`/`to` 引用闭合、边有向成对、phases 按规则。
3. **肉眼可辨**:俯视 PNG 珠江新城路网(珠江大道/花城大道/临江大道走向、猎德桥位)对照真实地图
   可指认;建筑块 footprint 与路网不重叠越界。
4. 展示台 `kind:"tool"` 卡(usage / 正向 / 负向,expectExit 0/2)+ `CG0-TOOLS-GUIDE.md` 归档。
5. `regression.py --only cg0-*` 双后端 ALL GREEN(bytes/语义锚分档);ctest 双树带 `--repeat until-pass:3`。
6. close 闸 + `release --done`(track citygen,state 转 active 后)。

## 6. 风险与既知坑

- **Overpass 大陆直连不稳**(BusViewer 实测):开发期主用离线分片,在线入口仅兜底;建筑数据
  拉取若连败,降级方案 = 先交路网版(M-CG0.1 产物),建筑步顺延,不阻塞收官。
- 68MB 级 JSON 解析内存:区域 bbox 先过滤后建索引,分片整读一文件 ≈ 18MB 峰值可接受;
  全城合并不进 M-CG0。
- `way` 引用缺失 node(Overpass 分片可能截断):解析器按「缺引用 way 丢弃 + 计数锚」处理,
  不静默。
- city-tools 与 citygen 的俯视 PNG 绘制逻辑重复:第一版允许各自实现(数据源形态不同);
  M-CG1 迁程序化输入时再评估抽公共绘制层,不过早抽象。

## 7. 后续挂账 — M-CG1 地标混合架构(2026-11-16 用户拍板「写吧」)

**方案:citygen 程序化打底 + max 场景只抠地标单体**,替代原「真轮廓挤出」条目。
用户确认形态:「部分标志建筑用 max 的,其他自己生成」。

| 项 | 内容 |
|---|---|
| 架构 | citygen 全量 box 打底(CG0 产物不变) + 地标单体 glb 增量叠加;`landmark_whitelist.json`(osm id → glb 路径)命中 → 该 footprint 跳过 box 实体、产真模型实体(label 契约照 A 档扩 `mesh` 字段,只加不减) |
| 源资产 | `E:/canton_work/`(canton_tower_scene.max 1.19GB / canton_cleaned.max / canton_fbx/canton_scene.fbx 570MB / tex 806 张 / MAXScript 全套);**3ds Max 2027 本机在装** |
| 抠模 | MAXScript 按 bbox/名字选节点逐地标导出 FBX(改 `export_fbx.ms`);场景 20808 几何体中 18804 是 Missing_GeomObject 死代理,有效网格约 2000,只取目标地标 |
| 转换 | FBX→glb(FBX2glTF 或 Blender);**inch→米、Z-up→Y-up 修正**;按 M-W1 白名单路线裁剪,不整场景进引擎 |
| 首个地标 | **广州塔**(23.1066N,113.3246E,在 CG0 bbox 南界外)——**零抑制逻辑直接加**;bbox 内地标(西塔/东塔)才需 whitelist 抑制 |
| 对位 | 场景内定真实坐标锚点(塔底)→ inch/Z-up → citygen 局部米制(`dx=Δlng·cos(23.1°)·111320`);每地标独立锚 footprint 中心,错只错一个 |
| 验收 | 广州塔 glb 挂进 city.scene + 俯视 PNG 地标位金色标记 + 引擎侧 `--traffic-city` 场景可见(消费端接线时) |

贴图注记(2026-11-16 排查):OSM 数据无图像(436 栋仅 2 条 `building:material`);引擎材质入口
现成;程序化 facade 纹理(窗格/楼层条纹)归 M-CG1 一并评估;真实照片贴图无源且版权不可用。
