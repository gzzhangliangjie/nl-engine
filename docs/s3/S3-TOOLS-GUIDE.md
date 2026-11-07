# S3 工具指南：cluster DAG（M3.0，tag `s3-dag`）

> `nanite-tools dag`：把 S2 的分层资产连成有向无环图并机器校验。
> 所有输出均来自真实二进制资产，可一键复现。

## 背景：为什么 M3.0 必须按组重切

三家源码实读（2026-11-07）对照结论：

| 来源 | 关键证据 | 结论 |
|---|---|---|
| UE `ClusterDAG.cpp:1378-1605` | 组内合并→简化→SplitCluster 按组重切；:1441/:1587 组 ParentLODError = max(子簇, 简化误差)；:1593 全部父簇统一携带组误差；FindCut 弹堆时 `SelectedGroups2` 保证每组只下钻一次 | cut 以组为原子 |
| bevy `from_mesh.rs:94-195` | 组记成员簇 id；:146-151 parent_error = max(简化误差, 成员误差)；:154 每组独立 `compute_meshlets` | 同上 |
| Carrot → zeux `clusterlod.h:107-136` | `cluster.refined` = 生成组 id；`clusterError` = 组简化误差；terminal group | 同上 |

**硬共识**：组是 DAG 父节点；按组重切 → 每个父簇恰有一个生成组；组误差沿链单调。
M2.2 的全局一次性重切会让父簇跨组 → cut 语义不成立（只 cut 一个父簇会把整组
成员都换上来，与同组兄弟父簇重叠 → 重复绘制）。M3.0 改为按组重切，血缘随层落盘。

## `.nmesh` meta version 2（M3.0，只加不改）

```
NMeshHeader(version=2) + name + records + vertices + triangles
  + NMeshMetaHeader(24B, magic 'NLOD', metaVersion = 1 或 2)
  + float[3*lockedCount]            lockedPositions（M2.2，本层组间边界点）
  + [metaVersion 2] DAG 血缘段：
      u32 groupCount
      NMeshDagGroupRecord[groupCount]   每个 32B：
        float error          组 ParentLODError = max(simplifyError, 成员簇误差)
        float simplifyError  本次组内 QEM 简化误差
        float sphere[4]      成员簇包围球并（S5 LODBounds 基础）
        u32  genFirst/genCount  本组父簇在下一层文件的簇号区间（顶层 0）
      u32[meshletCount]                 clusterGroup（本层每簇的组号）
```

- `clusterGroup` = 子方向边（组 ← 成员簇）；`genFirst/genCount` = 父方向边
  （组 → 生成的父簇），区间连续铺满下一层，由按组重切保证。
- 语义约束：每簇恰属一组（membership）、每非顶层组恰产出 ≥1 父簇、顶层组
  genCount=0（terminal）、组 error == max(simplifyError, 全部成员簇 bounds.error)
  逐位精确、父簇 bounds.error == 生成组 error、组 error ≥ 生成组 error。
- `loadNMesh` 全版本都读；v1 与 metaVersion 1 逐字节不变。

## `dag` 子命令

```
nanite-tools dag <prefix> [--export out.json]
```

读连续 `<prefix>.lod0..N.nmesh`（探测约定与 renderer 一致），六项校验：

| 校验 | 内容 |
|---|---|
| membership | 每簇恰属一组，组号合法、每组非空 |
| generation | gen 区间连续铺满下一层，每父簇恰一生成组，顶层 genCount=0 |
| error chain | 组 error == max(simplifyError, 全部成员簇误差)，逐位精确 |
| parent error | L≥1 每簇 bounds.error == 生成组误差（UE:1593） |
| monotonic | 组 error ≥ 生成组 error（沿链单调） |
| acyclic | 组图 DFS 染色（灰上回边 = 环） |

任一失败 exit 1 并打出第一条原因；`--export` 写分层 DAG JSON（组 id/error/
成员簇/生成簇 + 逐层统计），供 `tools/dag_preview.py` 离线画图。

### 实跑输出（Suzanne，DX12 构建）

```
file        : assets/meshlets/Suzanne
levels      : 7 (L0..L6)
  L0  clusters=237   tris=3936    groups=31   parentClusters=35    topErr=0.0000
  L1  clusters=35    tris=1970    groups=7    parentClusters=19    topErr=0.0435
  L2  clusters=19    tris=1002    groups=5    parentClusters=15    topErr=0.1339
  L3  clusters=15    tris=500     groups=4    parentClusters=8     topErr=0.1339
  L4  clusters=8     tris=252     groups=3    parentClusters=7     topErr=0.2690
  L5  clusters=7     tris=142     groups=3    parentClusters=3     topErr=0.4222
  L6  clusters=3     tris=104     groups=3    parentClusters=0     topErr=1.1375
dag checks  :
  membership    : PASS (every cluster in exactly one group)
  generation    : PASS (per-parent single generating group, spans tile next level)
  error chain   : PASS (56 groups, group err == max(simplifyErr, members))
  parent error  : PASS (87 clusters carry group error, UE ClusterDAG.cpp:1593)
  monotonic     : PASS (87 edges, group err >= generating group err)
  acyclic (DFS) : PASS
dag         : OK (56 groups across 7 levels)
```

### `selftest dag` 实跑输出（全部 ok）

```
[connected 128x128 grid, DAG lineage]
  ok   synthetic .nmesh built
  ok   buildLodHierarchy
  ok   at least 3 LOD levels
  ok   full-chain DAG invariants hold
  ok   error chain exercised on >=100 groups
  ok   top-level groups are terminal (genCount=0)
  ok   every non-top group generates parents
  ok   v3 round-trip keeps DAG lineage
  ok   reloaded chain re-validates
  ok   tampered group error detected
dag selftest: PASS
```

不抽样：合成链全量断言（规模小才做得起）；「error chain ≥100 组」防止用例
退化成零覆盖；roundtrip 后重读链再校验一遍（防内存态/文件态口径分叉）。

## 可视化证据图

`example/s3-dag/dag-structure.png`（1522x1206）：`dag --export` 的真实 JSON
经 `tools/dag_preview.py` 渲染——7 列 = 7 层，每框 = 组（描边颜色蓝→红 =
ParentLODError 高低，标注成员簇数），连线 = 组的生成簇被更粗层哪个组收养
（S5 cut 的组原子邻域）。复现：`example/s3-dag/run.bat`。

## ctest 与回归口径

ctest 14/14（双后端，新增 `s3-dag-selftest` / `s3-dag-suzanne`）；
回归总闸 118/118 ALL GREEN；`server.py --check` OK（kStage = S3.0）。

## 负向行为

| 输入 | 行为 |
|---|---|
| 无分层资产（lod0 探测失败） | `dag: cannot open for read: ...` exit 1 |
| 层号不连续 / level 字段不符 | exit 1 |
| 资产无 DAG 血缘（旧 metaVersion 1） | exit 1，提示重跑 `nanite-tools lod` |
| 未知参数 | `dag: unknown option --xxx` exit 1 |
| 任一校验失败 | exit 1 + 第一条失败原因 |

## 下一步（S3 剩余）

- M3.1 fallback mesh：取顶层整体连成普通 indexed mesh，S8 BLAS 用。
- M3.2 `viewer --mode dag-top`：顶层渲染 + 任选组下钻一层，验证父子衔接无洞。
- S5 运行时 cut：优先队列按 error 弹堆，整组原子替换（同误差兄弟一起出堆）。

---

## M3.1 fallback mesh（tag `s3-fallback`）

### 定位与 UE 对照

fallback = 链最顶层整体拼成的一份**普通 indexed mesh**（vertices + u32 indices），
S8 光追 BLAS / 非 Nanite 兜底渲染直接吃，**不追 DAG**。S3-PLAN 底线：这步定稿，
S8 不许再「顺手生成一份」。

UE 对照（2026-11-07 实读 NaniteBuilder.cpp）：`BuildFallbackMesh` :769 +
`BuildCoarseRepresentation` :415 —— UE 从 ClusterDAG 独立简化到
`FallbackPercentTriangles`（默认 ~2% 输入三角数）目标。本实现取**链顶层**：

- 顶层已有 parent error 单调链背书（M3.0），误差口径由分层链给出；
- Suzanne 104/3936 = **2.6%**，与 UE 默认 ~2% 同量级；
- 不引入第二次独立简化——同一条链、同一份确定性，无第二口径可漂移。

### 格式：.nmesh 文件 version 3

```
[version 1/2 主体不变]
NMeshFallbackHeader 24 B（'NFLB' 自描述尾段，按 magic 识别、顺序不敏感）:
  magic / payloadBytes / vertexCount / indexCount(=三角数×3) / sourceLevel / maxError
MeshVertex[vertexCount] + u32[indexCount]
```

- 落点约定 = **`<prefix>.lod0.nmesh` 尾段**（`lod` 命令链写完后拼顶层写回）；
  **输入文件不动**——展示台实跑 `-o build/...` 不会碰入仓资产。
- v1/v2 逐字节兼容读回；v3 尾段按 magic 识别（NLOD meta 可选 + NFLB 可选，
  至少一段），重复段/未知段拒绝；fallback 索引范围在 loadNMesh 校验。

### 拼接算法（core/fallback.cpp）

1. `weldPositions`（grouping.hpp 唯一口径，float 位级相等）→ canonical 点；
2. canonical 点 → 输出顶点（首现持有完整 normal/uv；边界点两侧本就是同一源
   顶点的拷贝）；
3. 簇局部索引 → 全局窗口 → 输出编号；退化三角当场拒绝（出现退化 = 上游已坏）。

### 实跑输出（Suzanne，DX12 构建）

```
lod files   : 7 written (assets/meshlets/Suzanne.lod0.nmesh [v3+fallback] .. Suzanne.lod6.nmesh [v2])
fallback    : 104 tris 96 verts (from L6, maxErr 1.1375) -> assets/meshlets/Suzanne.lod0.nmesh tail
```

3936→104 = 2.6%，与 UE FallbackPercentTriangles 默认 ~2% 同量级。

### 自检（selftest fallback，不抽样）

128x128 合成网格全链顶层拼接，灵魂断言：

- **三角守恒**：拼接是恒等重索引，不许丢/加三角；
- **weld 收敛**：顶层簇间边界重复顶点必须合并（顶层顶点数 > 拼接后顶点数）；
- **边流形**：每条无向边 ≤2 次使用（>2 = T 形接缝/重复面，渲染与 BLAS 都出洞）；
- 确定性重拼逐字节一致；v3 roundtrip（fallback + DAG 血缘同时读回）；
- fallback-only 尾段（v1 语义主体 + fallback = v3，无 lodMeta）；空网格拒绝。

### 证据图

`example/s3-fallback/fallback-preview.png`：`tools/fallback_preview.py` 直接
解析 .nmesh v3 的 NFLB 段（真实入仓资产字节），平涂 + 边线离线渲染；
**背景透出 = 洞**（边流形已由 selftest 断言，图为视觉对照）。
`example/s3-fallback/run.bat` 一键复现。L5/L6 躺倒姿态与 `lod-suzanne.png`
金字塔同款——离线预览不做 GPU 侧轴向修正，风格一致。

### ctest / 回归

ctest 14→16（s3-fallback-selftest / s3-fallback-suzanne），双后端全绿；
dag 六项校验与 check-orient 全链 inward=0 无回归。

## M3.2 viewer `--mode dag-top`（tag `s3-done`）

### 定位与三家对照

M3.2 是 **S5 运行时组原子 cut 的离线预演**：`--mode dag-top` 渲染 DAG 顶层
（最粗层 = 正确粗模），`--dag-group N` 任选一组下钻一层——从顶层挖掉该组
生成的父簇（genFirst/genCount 区间）、换回该组在第二粗层的成员簇
（clusterGroup 血缘），在 GPU 画面上验证父子衔接无洞。

| 来源 | 位置 | 与 M3.2 的对应 |
|---|---|---|
| UE 5.8 | `Nanite/NaniteHierarchyTraversal.ush` + `NaniteDataDecode.ush:701-702`（节点 MinLODError/MaxParentLODError，NumChildren = 组粒度） | 粗→细遍历：误差不可感知→出屏该组簇，否则下钻。M3.2 = 遍历的**单步冻结版**（手动选组） |
| bevy 0.15 | `meshlet/cull_bvh.wesl:43-48`（parent_error + lod_error_is_imperceptible）；`cull_shared.wesl:13`（屏幕空间公式，源自 zeux nanite.cpp#L115） | 判据是屏幕空间自动选层（`error/max(d,near)*proj22*0.5*viewport.w < 1.0`）——属 S5，M3.2 手动选组，公式已存档 |
| Carrot | `draw_virtual_geometry.slang:105-106`：`render = selfError > threshold && refinedError <= threshold`（zeux clusterlod 的 refined 口径）；`:112` 组内所有簇同决策 | 组原子挖换与 Carrot 的 task shader 组判定同构，只是我们离线、逐组手动 |

### 渲染路径

- `Scene::setDagCut(device, topFile, childFile, group)`：读两份 .nmesh →
  顶层掩码挖掉 `[genFirst, genFirst+genCount)` → 子层掩码取
  `clusterGroup==group` 的成员簇 → 同一条 expandClusterData/uploadClusterData
  路径上传。`group < 0` = 纯顶层（与 lod 模式同路径）。
- 可下钻组数 = 第二粗层 `.lodN.nmesh` meta 的 dagGroups（backend init 探测）。
- HUD 下钻滑条 `dag drill`（-1 = 纯顶层），与 lod 滑条同一条双向通道，
  两套滑条不同时出现；标题栏同步 `dag top / gN`。
- 负向全拒：非数字 / 越界 / 无分层资产均 exit 1，绝不静默回退。

### M3.2 踩坑（两个，都值得记）

1. **expandClusterData 索引基址**：`meshlet.vertexOffset` 是**文件内**偏移，
   只在「全部簇按序全量展开」时恰好等于缓冲位置（M2.3 唯一路径，一直没炸）。
   dag cut 掩码跳簇后两者分叉：被跳簇**之后**的每个 kept 簇索引全部错位
   （实测 kept 簇整个消失、洞 1182px）。修为 `clusterBase = vertices.size()`
   快照（本簇顶点入缓冲时的实际位置），全量路径逐位等价。
2. **离线探针的顶点切片 stride**：dag_weld_check.py 曾按 `i*3`（12B）切片，
   MeshVertex 实为 8 float（32B）——NN/bbox/位级重合探针全是垃圾数据，
   一度误判「L6 相对 L5 轴交换」「父簇顶点只有 10/62 位级存在」。修为
   `i*8` 后一切自洽：父簇顶点全部位级存在于子簇（简化保位置）。

### 验收双证据

1. **GPU coverage**：同机位 top/g0/g1/g2 截图的前景掩码对比，三组下钻
   hole=0（g1/g2 混合集与纯顶层 PNG 逐字节一致——16→16 零折叠的父簇
   与子簇等价；g0 有真简化 110→72，fill=6278px hole=1px AA 噪声）。
2. **焊接边流形**（`tools/dag_weld_check.py --all`）：welded edge 按位级
   位置对计数，Suzanne 开壳的 **84 条开放边（眼窝+嘴）逐层守恒**（L0
   5946 边 = 5862×2x + 84×1x + 0×>2x）；判据 = mixed 集开边集合与
   full top 完全一致（零新增开边）且零 >2x 重叠边 → 三组全 PASS。

### 交代物

- `example/s3-done/`：5 对基线 PNG（top/g0/g1/g2/HUD，DX12 vs Vulkan
  非 HUD 四对逐字节一致）+ `run.bat`（gallery/weld/selftest）+ 画廊
  `dag-drills.png`（真实 GPU 截图拼版）。
- 展示台 s3.2 卡（5 pair + 3 负向 + selftest）；s2.0 exe 映射按快照规范
  改 `build/showcase/s2-done/`（rebuild.py 补建）。
- ctest 16/16 双后端；回归总闸全绿；kStage 升 S3.2。
