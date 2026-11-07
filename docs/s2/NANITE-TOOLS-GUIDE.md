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
