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
