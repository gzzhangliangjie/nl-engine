# nanite-tools 使用说明与结果归档 — S2 篇（M2.0 聚组）

> **交代载体（规范见 `docs/ROADMAP.md`「里程碑交代规范」节）**：首选展示台网页
> `tools/showcase/start.bat` → http://127.0.0.1:8787/ —— s2-group 版本卡上
> `kind:"tool"` 用例点「运行工具」现场实跑；本文是同一批输出的文本归档，两者同源、
> 退出码语义一致（0 成功 / 1 用法或运行错误 / 2 已声明未实现）。
> S1 各子命令的归档见 `docs/s1/NANITE-TOOLS-GUIDE.md`。

## M2.0 交代（tag `s2-group`，2026-11-07）

### 新增：METIS vendored

- `thirdparty/metis/`：METIS 5 + GKlib（KarypisLab 官方 main，浅克隆 2026-11-07），
  自写 `CMakeLists.txt` 编静态库 `nl_metis`（不用上游构建系统——其顶层还挂着
  悬空的 `build/xinclude` 子目录与外部 GKLIB_PATH 依赖）。
- vendored 改动仅两处（见 `thirdparty/metis/VENDORED.md`）：metis.h 宽度宏固定 32 位、
  INT32_MIN 等宏加 `#ifndef` guard。
- MSVC 编译宏：`__thread=__declspec(thread)`（上游靠 HAVE_TLS 检测注入）、
  Windows 下 `USE_GKREGEX`（无 `<regex.h>` 时 GKlib 内置兜底）、`NDEBUG`。
- 选型依据：bevy 0.15/0.16 同款（JMS55：「DAG 质量是 virtual geometry 最重要也最难的部分」；
  调参 `seed` 固定、分组只看顶点位置忽略 UV 缝）；UE 用自研 GraphPartitioner
  （流送分页需求，S3 前不需要）；Carrot 本地仓库无离线简化实现可参考。

### 新增：`simplify` 子命令（M2.0 = 聚组）

```
nanite-tools simplify <model.nmesh> [-v]
```

流程：读 .nmesh → 位置 weld（位级相等，UV 缝忽略）→ 簇邻接图
（节点=簇、边=共享几何点、边权=共享点数）→ `METIS_PartGraphKway`
（vwgt=簇三角数、固定 seed、CONTIG 仅整图连通时开）→ 不连通分区按连通分量
fixup 拆分 → 打印统计。组大小目标 8 簇/组（`core/grouping.hpp`
`kTargetMeshletsPerGroup`，bevy 0.15 实证值；S2-PLAN 旧稿 50000-tri 口径是
UE page 尺寸而非 group，已修订）。

### usage 实跑（M2.0 后）

```
nanite-tools - nl-engine offline mesh pipeline (S1)
usage:
  nanite-tools stat <model>                 summary (.glb/.gltf 或 .nmesh)
  nanite-tools cluster <model> [-v] [-o out.nmesh]   meshlet build + save
  nanite-tools selftest cluster|nmesh|group deterministic unit tests
  nanite-tools simplify <model.nmesh> [-v]  cluster grouping (S2 M2.0)
  nanite-tools dag <model> [options]        error DAG (S3, not yet)
```

### simplify 实跑输出（DX12 构建，2026-11-07）

Suzanne（237 簇，目标 8 簇/组 → ceil(237/8)=30，1 个 METIS 分区不连通被
fixup 拆成 2 组 → 31）：

```
file        : assets/meshlets/Suzanne.nmesh
clusters    : 237 (3936 triangles)
target      : 8 meshlets/group
groups      : 31
group size  : min 3 avg 7.6 max 10
split fixups: 1
connectivity: PASS (0 disconnected group(s))
```

IcoSphereDense（4929 簇 → 617 组，零 fixup）：

```
clusters    : 4929 (327680 triangles)
target      : 8 meshlets/group
groups      : 617
group size  : min 5 avg 8.0 max 12
split fixups: 0
connectivity: PASS (0 disconnected group(s))
```

### selftest group 实跑输出（全部 ok）

```
[single quad (2 tris)]
  ok   coverage (every cluster grouped, ids contiguous)
  ok   single quad -> exactly 1 group
  ok   every group connected on shared-vertex graph
  groups=1 max=1 fixups=0
[2 disconnected 32x32 blocks]
  ok   coverage (every cluster grouped, ids contiguous)
  ok   max group size sane
  ok   every group connected on shared-vertex graph
  ok   no group spans disconnected blocks
  groups=10 max=9 fixups=0
[connected 64x64 grid]
  ok   coverage (every cluster grouped, ids contiguous)
  ok   max group size sane
  ok   every group connected on shared-vertex graph
  groups=22 max=12 fixups=0
  ok   empty mesh rejected
group selftest: PASS
```

要点：64x64 连续网格约 165 簇（64 顶点上限比 128 tri 先爆，每簇 ~48 tri）→
22 组；组内连通在 selftest 侧独立重算（与 grouping.cpp 的 fixup 交叉验证，防同错）。

### 负向路径与退出码（实跑）

```
> nanite-tools simplify                     → "simplify: missing <model.nmesh>"，exit 1
> nanite-tools simplify __missing__.nmesh   → "simplify: cannot open ..."，exit 1
> nanite-tools simplify x -bad              → "simplify: unknown option -bad"，exit 1
> nanite-tools dag                          → "dag: not implemented yet (S2 / S3)"，exit 2
```

### 实踩坑（M2.0）

1. **METIS `CONTIG=1` 要求输入图整体连通**：断连合成网格直接
   `rc=-4 (METIS_ERROR_INPUT)`。处理：先 BFS 检测连通性，连通才设 CONTIG；
   断连时靠 fixup 按连通分量拆分兜底（组不跨断连块的验收由 selftest 锁死）。
2. **GKlib `__thread`**：MSVC 无该关键字，缺宏定义时报 60+ 语法错。处理：
   固化上游 CMake 的 TLS 检测结果 `__thread=__declspec(thread)`（vendored 版
   直接写死，见 `thirdparty/metis/CMakeLists.txt`）。
3. **UCRT limits.h 已定义 INT32_MIN**：metis.h 重复定义 C4005。处理：vendored
   metis.h 加 `#ifndef` guard。

## M2.1 交代（tag `s2-qem`，2026-11-07）

### 新增：组内 QEM 简化 + 边界锁定（`core/simplify`）

- **QEM**（Garland & Heckbert）：每点累积邻域三角形的面积加权平面二次型
  （退化平面按面积 clamp，零面积不贡献）；边折叠队列 = 有序集按误差升序，
  代价平局按点编号决胜 → 完全确定（同输入同输出，selftest 重跑断言锁死）。
  折叠位置取二次型最优点（3x3 Cramer，奇异回退端点/中点取最小误差）。
- **边界锁定（灵魂机制，没有第二个魔法）**：边界判定看**组归属**——canonical
  点被 ≥2 个组的簇引用即边界（组内两簇共享点不是边界，锁了简化率上不去）。
  锁定点永不移动、永不折叠；双锁定端点的边（接缝本体）不折叠；被摧毁三角形
  不得是某条双锁定边（≠本边）的唯一三角形——否则那条接缝边在本组一侧没三角
  形了，跨组裂缝由此产生。锁定点集合进 `SimplifyResult.lockedPoints`
  （S3 DAG 合并沿用，M2.2 落分层 .nmesh 时持久化）。
- **坏折叠拒绝**：link condition（N(a)∩N(b)==edge(ab) 对顶点）+ 法线翻转检查
  （非归一化点积 ≤0 拒）+ 非流形边（>2 三角）拒 + 不许清空整组。
- **目标**：每组折叠到 round(输入×0.5) 即停；误差=世界空间长度（sqrt 二次型值）。
- meshoptimizer vendored（1.3，`thirdparty/meshoptimizer/`）作**对照 oracle**：
  只链进 nanite-tools（`meshopt_SimplifyLockBorder` 同口径），core 不依赖——
  正式实现是自己的 QEM。注意口径差：oracle 锁子网格全部拓扑边界（含模型外缘），
  我们只锁组间边界，oracle 更保守、简化率略低是预期。

### simplify 实跑输出（M2.1 后，DX12 构建）

Suzanne（31 组，622 边界点锁定）：

```
file        : assets/meshlets/Suzanne.nmesh
clusters    : 237 (3936 triangles)
groups      : 31
connectivity: PASS (0 disconnected group(s))
qem ratio   : 50%
boundary pts: 622 locked (S3 DAG 沿用)
simplify    : 3936 -> 1976 tris (50.2%)  collapses 992  maxErr 0.0250 (world)
oracle      : meshopt LockBorder 3936 -> 1948 tris (49.5%)  rel err avg 0.0051 max 0.0134
```

IcoSphereDense（617 组，24,406 边界点，617 组全跑 7.6 s）：

```
clusters    : 4929 (327680 triangles)
groups      : 617
connectivity: PASS (0 disconnected group(s))
qem ratio   : 50%
boundary pts: 24406 locked (S3 DAG 沿用)
simplify    : 327680 -> 178308 tris (54.4%)  collapses 74686  maxErr 0.0005 (world)
oracle      : meshopt LockBorder 327680 -> 163362 tris (49.9%)  rel err avg 0.0001 max 0.0003
```

球模型 54.4% 略高于 50%：部分组在 50% 附近只剩 2-tri 折叠（一次删 2 个会越过
目标太多或被 link condition 拒），提前停住——「约减半」口径内，M2.2 重切不受影响。

### selftest qem 实跑输出（全部 ok）

```
[connected 64x64 grid, group QEM 50%]
  ok   locked set == independent boundary set
  ok   boundary vertices bit-identical          <- 灵魂断言：边界逐点位级不变
  ok   seam edges alive on both sides           <- 接缝边双侧存活（无裂缝拓扑前提）
  ok   output ratio in [30%,60%]
  ok   every group keeps triangles
  ok   indices in range
  ok   deterministic (rerun identical)
  info groups=22 boundary=807 tris 7938 -> 3966 (50.0%)
[single quad, no boundary]
  ok   single quad -> 1 group
  ok   no boundary points
  ok   quad collapses 2 -> 1
[2 disconnected blocks]
  ok   locked points have per-block positions   <- 锁定点不跨块
  ok   output accounting consistent
  ok   output ratio in [30%,60%]
[input validation]
  ok   ratio 0 rejected / ratio >1 rejected / grouping size mismatch rejected / empty mesh rejected
qem selftest: PASS
```

要点：边界集合与接缝边存活在 selftest 侧**独立重算**（与 simplify.cpp 交叉
验证防同错）；64x64 网格 22 组相邻、807 边界点，简化后边界逐点位级一致——
这就是「相邻组拼接无裂缝」的自动证明（S2.3 再做视觉验收）。

## M2.1 补充交代：可视化证据图（同日补齐，tag `s2-qem`）

M2.1 原交付只有文本统计与 selftest 断言。用户追问「有图么」后补上**真实几何**
的对比图：不是示意图、不是别的版本截图，而是 `simplify --obj` 导出的同一份
canonical 三角 soup，经离线渲染得到的左右对照。

### 导出：`--obj <prefix>`

```
nanite-tools simplify assets/meshlets/Suzanne.nmesh --obj build/qem/suzanne
obj dump   : build/qem/suzanne.before.obj (2012 pts / 3936 tris),
             build/qem/suzanne.after.obj  (2012 pts / 1976 tris),
             build/qem/suzanne.locked     (622 pts)
```

- `.before.obj` / `.after.obj`：同一 canonical 点编号体系（`weldPositions` 口径），
  点数量相同、边界点位置位级不变，所以两图能用同一机位同一尺度叠红点。
- `.locked`：组间边界点编号（0-based，逐行一个），即 S3 DAG 要持久化的集合。
- 该导出也是 M2.2 分层落盘的几何口径参考（重切在此 soup 上做）。

### 出图：`tools/qem_preview.py`（Pillow，纯本地，无 GPU / 无网络）

```
python tools/qem_preview.py build/qem/suzanne example/s2-qem/qem-suzanne.png
# 可选 --yaw 12 --pitch 9（默认）
```

正交投影 + 画家算法实心填充 + 三角线框 + 红点高亮，2x 超采样。左右两栏
**共用 center/scale/相机**（从两网格并集包围盒算），因此红点位置若不一致会
一眼看出——这是 selftest「边界逐点位级不变」的视觉对应物。

成品：`example/s2-qem/qem-suzanne.png`（1260x640）
——左 `BEFORE 3936 tris` / 右 `AFTER 1976 tris (50.2%)`，
622 个红点（组间锁定边界点）在两栏逐像素同位。
一键复现：`example/s2-qem/run.bat`。

### 展示台挂图

`example/cases.json` 的 `s2-qem/simplify` 卡带 `"preview": "example/s2-qem/qem-suzanne.png"`；
`tools/showcase/server.py` 的 `declared_shots()` 收集 `preview` 进 `/shots/` 只读白名单，
`app.js` 在工具卡正文下方渲染静态图。另加 `s2-qem/dump` 工具用例现场实跑 `--obj`
（`expectExit 0` + `expectLog "obj dump   :"`）。静态证据图不进总闸 PNG 比对，
回归仍只校验文本输出：96/96 ALL GREEN，ctest 10/10。

## M2.2 交代（tag `s2-loop`，2026-11-07）

### `core/lod` 逐层循环：严格对齐标准做法，不自造机制

```
L0 → groupClusters 重新分组 → 按当前分组重算边界锁 → 每组硬目标 50% 简化
   → buildMeshlets 重切 → L1 → 重新分组 → …
```

源码依据（本机三处实读，2026-11-07）：

| 出处 | 结论 |
|---|---|
| bevy `from_mesh.rs:87-195` | 每轮 `group_meshlets` 重新分组；`:114-120` `lock_group_borders` 在轮首调用，`:520-523` 对 `vertex_locks` **全量覆写**（含置 false）→ 上轮锁不继承；`:556` 每组硬目标 `group_indices.len()/2`；`:154` 重切 `compute_meshlets`；`:190-192` 未达标组进 stuck 后重新入队、换一组边界再试 |
| UE `ClusterDAG.cpp:1067+` | 逐层 `ReduceMesh`；`:1470` `TargetNumTris = NumParents * TargetClusterSize` 是硬目标；`:1441` `ParentLODError = Max(..., LODError)` 误差单调 |
| UE `Cluster.cpp:1023-1039` | `LockPosition` 只对**当前 group 的 `ExternalEdges`** 端点生效；`:1080-1091` 简化后按位置对重建外部边标记 |
| meshoptimizer `simplifier.cpp:2492` | `while (result_count > target_index_count)` 反复折叠到硬目标；`:526-529` `SimplifyLockBorder` 把 border/fringe 改判 `Kind_Locked`（锁点不能作折叠源、只能作目标，双锁边不折叠） |

据此：

- **边界锁每轮由当前分组重算，不跨层累积**。`simplifyGroups` 只接收本次
  `groupClusters` 的组归属（点被 ≥2 组引用即锁），M2.1 的边界锁实现一行未改
  ——它本来就是标准 `LockBorder` 语义。跨层无裂缝由 DAG cut 以 group 为原子
  单位保证（S3），不靠把历史接缝点一直背下去。
- **每组硬目标 50%**（`round(inputTris * ratio)`），不是全局比例、不是误差阈值
  自然收敛。简化不足的组不特殊处理：重切已改变簇划分，下一轮重新分组会给出
  全新边界（bevy stuck 重新入队同义）。
- **终止条件只用**三角数 < `minTriangles`(128) / 组数 < `minGroups` / 达到
  `maxLevels` / 三角数不再下降。没有「简化率不足就提前停」的自造门槛。
- **误差单调**：`parentError(k+1) = max(parentError(k), 本次简化误差)`，S3 cut
  判据才成立；L0 为 0。

### `.nmesh` version 2（只加尾部，v1 逐字节兼容）

```
NMeshHeader(version=2) + name + records + vertices + triangles
  + NMeshMetaHeader(24 B: magic 'NLOD' / payloadBytes / version / level / maxError / lockedCount)
  + float[3*lockedCount]   lockedPositions（世界空间位置）
```

`lockedPositions` = **本层按当前分组重算的组间边界点**（供更粗层拼接用），
顶层为空；`maxError` = parent error（单调不减）。`lodMeta.present=true` 才写 v2；
`loadNMesh` 两种都读，v1 严格检查无尾随字节的老逻辑不变。

### `lod` 实跑输出（Suzanne，DX12 构建）

```
file        : assets/meshlets/Suzanne.nmesh
qem ratio   : 50% (per-group hard target)  maxLevels 8  minTris 128
levels      : 6 (L0..L5)
  L0  clusters=237   tris=3936    groups=31   locked=622    collapses=992    maxErr=0.0000 (parent, world)
  L1  clusters=33    tris=1976    groups=7    locked=117    collapses=502    maxErr=0.0250 (parent, world)
  L2  clusters=17    tris=985     groups=6    locked=84     collapses=252    maxErr=0.0250 (parent, world)
  L3  clusters=13    tris=490     groups=4    locked=35     collapses=128    maxErr=0.0250 (parent, world)
  L4  clusters=5     tris=246     groups=3    locked=0      collapses=62     maxErr=0.0250 (parent, world)
  L5  clusters=3     tris=124     groups=3    locked=0      collapses=0      maxErr=0.0260 (parent, world)
lod files   : 6 written (...lod0.nmesh .. ...lod5.nmesh, .nmesh v2)
```

每层严格约 50%，锁点 622→117→84→35→0→0 **递减**（每层重算，不是累积），
parent error 单调不减。写入的 6 个文件均可 `stat` / `viewer --model` 读回。

### `selftest loop` 实跑输出（全部 ok）

```
[connected 64x64 grid, LOD loop]
  ok   at least 3 LOD levels
  ok   triangle count strictly decreases
  ok   each level ~50% of previous (30~70%)
  ok   meshlet limits respected
  ok   per-level lodMeta present with correct level
  ok   locks == independent per-level boundary recompute   <- 灵魂断言
  ok   top level has no outgoing seams
  ok   parent error monotonic across levels
  info L0 tris=7938   clusters=176  locked=807   maxErr=0.0000
  info L1 tris=3966   clusters=81   locked=331   maxErr=0.1518
  info L2 tris=1981   clusters=41   locked=136   maxErr=0.2316
  info L3 tris=990    clusters=19   locked=54    maxErr=0.2784
  info L4 tris=495    clusters=11   locked=24    maxErr=0.4608
  info L5 tris=246    clusters=6    locked=0     maxErr=0.5156
  info L6 tris=123    clusters=3    locked=0     maxErr=0.6163
  ok   deterministic (rerun identical)
  ok   v2 .nmesh round-trip keeps lodMeta
  ok   v1 .nmesh still readable
[input validation]
  ok   ratio / maxLevels / minGroups / empty mesh rejected
loop selftest: PASS
```

灵魂断言在 selftest 侧**独立重算**每层的组间边界（重新 `groupClusters` +
按组归属找被 ≥2 组引用的 canonical 点），要求与资产里写的 `lockedPositions`
逐点相等——只要还有跨层继承，就会多出历史锁点而立刻不等。

### 可视化证据图

`example/s2-loop/lod-suzanne.png`（1340x920）：直接解析 `lod` 落盘的
`.nmesh` v2 二进制，L0..L5 六栏**同机位同尺度**渲染，红点 = 该层组间边界点
（逐层递减、顶层无红点）；线框密度逐层降低即每组 50% 硬目标的结果。
`tools/lod_preview.py` + `example/s2-loop/run.bat` 一键复现（不进总闸比对）。

## M2.3 交代（tag `s2-done`，2026-11-07）

### 新增：`viewer --mode lod N` + HUD 滑条切层（S2 收官验收）

```
viewer.exe --mode lod --lod 3 --model Suzanne.nmesh        # 直接渲染第 3 层
viewer.exe --mode lod                                      # 缺省 Suzanne + L0，HUD 滑条切层
```

机制（全部在 `renderer/`，两后端共享）：

- **分层资产探测**：backend init 后扫 `NL_MESHLET_DIR/<stem>.lodN.nmesh`
  （从 0 连续计数，跳号宁少报），层数写进 `DeviceInfo`，HUD 只在层数 >1 时画滑条。
- **切层路径唯一**：`scene.uploadClusterMesh` = 读盘 → 簇窗口展开 → 新建 VB/IB
  上传 → 替换句柄（旧 buffer 由 NVRHI 引用计数延迟释放）。首载与滑条切层走
  同一条路；shader / pipeline / binding set 一律复用，切层只动 VB/IB。
- **层信息回传**：层号与 parent error 从 `.nmesh` v2 尾部 section 读回，
  HUD 显示 `lod N/M tri <n> maxErr <e>`（滑条值走 HudFrame 双向 POD）。
- **缺省着色 normal**（几何不连续在逐面法线色上最刺眼）；显式 `--shading` 则听用户的。
- **负向全拒**：`--lod` 非数字（parseArgs 直接 exit 1，不建窗口）、N 越界、
  资产无分层文件（提示 `nanite-tools lod` 命令）——绝不静默回退到最细层，
  否则「以为在看 L3」是假证据。

### 分层资产入仓

`nanite-tools lod Suzanne.nmesh -o assets/meshlets/Suzanne` 产出的
`Suzanne.lod0..lod5.nmesh`（共 ~0.5 MB）已提交进仓，干净 clone 直接跑 lod 模式；
IcoSphereDense（8 MB × 层数）不入仓，需要时本地生成。

### 无缝验收（S2 收官判据）

- 六层同机位画廊 `example/s2-done/lod-layers.png`（`tools/lod_gallery.py` 拼接
  viewer 真实截图 + 从 .nmesh 读回的层号/tri/maxErr 标注）：
  3936 → 1976 → 985 → 490 → 246 → 124 tri，逐层简化、轮廓连续、**无裂缝无洞**。
- L4/L5 眼窝处的黑色面是 M2.2 已知口径的翻转三角形（法线朝内），`--cull none`
  复检该区域为完整内壁几何，不是裂缝/洞——边界锁定生效。
- 滑条在相邻层间来回切（交互），层切换零闪烁、三角数/HUD 数字同步变化。
- 展示台 `s2.0` 版本卡（8 用例：L0/L3/L5 + HUD pair 双后端基线、selftest、
  3 个负向 tool 卡）+ `example/s2-done/run.bat`；kStage 升 `S2.0`，
  m0.9 gate-hud 基线随 HUD stage 文本重拍（预期动作）。

### 回归口径

ctest 12/12（双后端）、回归总闸 118/118 ALL GREEN、`server.py --check` OK。
