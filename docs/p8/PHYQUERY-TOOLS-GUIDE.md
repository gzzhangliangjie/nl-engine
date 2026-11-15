# phys-tools M-PQ0 查询工具说明

M-PQ0 给游戏层交付的是一条 **Jolt-free 的通用物理查询面**：
`core/include/nl/physics/query.hpp` 只暴露 POD + PIMPL 宿主，
所有 `JPH::` 符号封在 `core/src/physics/Query.cpp`。本工具卡负责
在无窗口、无渲染的前提下把三族查询（raycast / sweep / overlap）跑成
可复现的数值锚。

## 退出码

沿用 phys-tools 既有口径：`0` PASS / `1` 断言失败或读写失败 / `2` 用法错误。

## 命令

```bat
build\DX12\phys-tools.exe selftest
build\DX12\phys-tools.exe query-trace --out phys-pq0-query.tsv
build\DX12\phys-tools.exe query-check phys-pq0-query.tsv
```

`query-trace` 的 `--out` 可省略，缺省写 `phys-pq0-query.tsv`（当前目录）。
`query-trace` 与 `selftest` 共用同一个 `buildQueryRows()` 行构造器，
锚值全部由夹具几何常量重算，**不从上一次运行的输出复制**。

## 确定性夹具

`QueryScene::build()` 建一个固定的物理世界：floor + 3 盒 + 1 sensor。
`QueryScene::bodyIdBySlot()` 供 `ignoreBodyIds` 用例取盒 id。
层沿用 `layers::k*`：0 Floor / 1 Default / 2 Debris / 3 Ghost。

## TSV 格式

第一行是注释锚（含 Jolt 版本与夹具描述），第二行表头，之后每行一条查询：

```
# jolt=<version> query=M-PQ0 fixture=floor+3box+sensor
name  hit  fraction  distance  px  py  pz  nx  ny  nz  bodyId  layer  owner
```

列以 TAB 分隔，浮点 `std::fixed` + `setprecision(6)`，binary 写入（`\n` 行尾，
避免 CRLF 漂移）。`query-check` 读回后用 `checkQueryRows()` 断言。

## 六条验收锚

| # | 行名 | 期望 |
|---|---|---|
| 1 | `ray_hitA` | `hit=1` / `distance=14` / `point.z=4` / `normal=(0,0,-1)` / `owner=boxA` |
| 2 | `ray_miss` | `hit=0`（x=50 平行地板） |
| 3 | `ray_layerDebris` | `layerMask=1<<kDebris` 跳过盒 A，命中盒 B（`point.z=9`） |
| 4 | `ray_ignoreA` | `ignoreBodyIds={boxA}` 跳过盒 A，命中盒 B |
| 5 | `sweep_wall` | 球 sweep 撞盒 A，TOI `fraction≈3.5/30`，`normal=(0,0,-1)` |
| 6 | `overlap_*` | 大盒 overlap 返回 4 个 body，按 `bodyId` 升序；`overlap_count` 记录总数 |

`layerMask` 与 `ignoreBodyIds` 两条是**查询专用过滤器**的实证：
`NarrowPhaseQuery` 默认过滤参数全通，不会自动复用 `LayerMatrix`。
实现里必须显式传 `QueryLayerFilter` / `QueryBodyFilter`，否则会打到
`kGhost` 等被禁层——这两条锚就是防这个回归的。

## ctest 登记

`phys-tools/CMakeLists.txt`：

| 用例 | 作用 |
|---|---|
| `pq0-query-trace` | 产出 TSV 到 `${CMAKE_CURRENT_BINARY_DIR}` |
| `pq0-query-check` | 回读断言（依赖上一条先跑） |
| `pq0-query-negative` | `query-trace --out` 缺路径 → exit 2（WILL_FAIL TRUE） |

统一走 `scripts\wt.bat phys-query ctest DX12|VULKAN`（内部带
`--repeat until-pass:3`，规避便携 ctest 快进程随机丢 stdout）。

## 实测结果（2026-11-15）

- DX12：`BUILD OK` + `SELFTEST PASS (16/16)` + artifacts `170/170`；ctest **167/167**
  （`pq0-query-trace` / `pq0-query-check` / `pq0-query-negative` 三条全 Passed）。
- VULKAN：`BUILD OK` + `SELFTEST PASS (16/16)` + artifacts `170/170`；ctest **167/167**。
- 首次构建 `rc=1` 是 `scriptapi freshness` 的 mtime 误判（见下），非查询代码缺陷。

## 边界与踩坑

- **公开头零 Jolt 符号**：`query.hpp` 只含 `<cstdint>/<vector>/<string>` 与
  `nl::math` POD；新增任何 `JPH::` 到公开头即视为破界。
- **Jolt 头路径**：`BroadPhaseLayer.h` 在
  `Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h`（不是 `Collision/` 根），
  另需 `Jolt/Physics/Body/BodyType.h`。
- **单精度 `fromJolt`**：本仓未开 `JPH_DOUBLE_PRECISION`，`RVec3 == Vec3`；
  两个 overload 会重复定义，改成 `template <class VecT>`。
- **overlap 结果排序**：Jolt collector 的 Sort 对 `CollideShape` 不稳定，
  必须按 `bodyId` 自行排序才能保证双后端与跨次运行的确定性。
- **merge 后 freshness 闸**：checkout 同秒 mtime 会让
  `IS_NEWER_THAN` 误判 `exports.api` 比生成物新 → 构建在 84% 报
  `scriptapi freshness`。先 `python tools/scriptapi/gen.py --check` 验内容，
  内容绿即 mtime 症状，`gen.py` 重写生成物（内容不变、无新 diff）即解。
- 不 bump `kStage`，不碰绑定面（`exports.api` / `Bindings.cpp` / `Wiring.g.cs`），
  不进渲染回归总闸。

## 展示台交代物（工具版）

`example/cases.json` 新增版本卡 `phys-query`（4 条 `kind:tool` 用例：usage /
query-trace / query-check / negative，退出码 2 / 0 / 0 / 2），`exe` 双后端指向
`build/{dx12,vulkan}/phys-tools.exe`，每条卡带 `preview` 证据图与 `script` 复现入口。

```bat
example\phys-query\run.bat
```

该脚本实测 `ALL GREEN`：query-trace 写 10 行真实 TSV → query-check 独立 oracle
`PASS (10 rows)` → 两次 TSV 用 `fc /b` 逐字节一致 → usage / 两条负例退出码 →
调 `tools/make-pq0-evidence.py` 从真实日志与 TSV 渲染 4 张证据图。
`server.py --check` 退出 0（只覆盖 `S8.2` 旧卡）；`regression.py --only phys-query`
实测 **8/8 ALL GREEN**（4 卡 × 双后端，bytes 8 / FAIL 0，2s），不手绘示意、
不复用其他版本截图。

## M-PQ1：查询接 live DebugScene（2026-11-16 归档）

冻结口径 `docs/p8/PQ1-PLAN.md`（方案 A：引用 + 动态体）。核心面：

- **QueryHost 内部桥**（`core/src/physics/QueryHost.hpp`，非公开）：最终只有
  `void* system()` 一个方法——落码时确认 narrowphase 查询过滤与仿真 layer
  matrix 正交，冻结草案里的 matrix filter 传递不需要。`DebugScene::Impl`
  实现之（scenario 1 CharacterHarness 独立 system 不暴露，挂账 M-PQ2）。
- **QueryScene 双模式**：`attach(host)` 后查询与注册全走 `activeSystem()`
  （live 优先、fixture 兜底）；`detach()` 回未初始化态可 re-attach。fixture
  六数值锚逐位保形（ctest 176/176 ×2 实证）。slot API（`bodyIdBySlot` /
  `bodyCount`）语义仅 fixture——attach 模式恒 0。
- **动态体注册**：`registerBody`（Static 体 + userData=owner，attached 模式写
  共享 live system，**live 世界绝不 OptimizeBroadPhase**）+ `unregisterBody`
  （tombstone 句柄不复用，双重注销返回 false）。
- **viewer 注入**：runNormal `--physics-debug --physics-scenario 2` + `--script`
  时 `QueryScene::attach(physicsScene.queryHost())` → `querySetScene(&qs)`；
  `scriptQueryLive` 声明序先于 `physicsScene` 析构（S15 纪律），收尾解绑点在
  `driveSetScene(nullptr)` 旁。绑定面 `querySetScene` 签名零改动。
- **验收口径 = 结构断言**（`phys-tools query-live [--frames N]`，缺省 90）：
  ray down hit kFloor / ray up miss / overlap ≥1 且 bodyId 升序含
  floor+course / registerBody by owner 命中、非法层 -1、双重注销 false、
  注销后透落地板 / detach 后 inert / re-attach 可用，共 21 条。**fraction 与
  坐标不进锚**——live 世界动态体状态不是字节稳定锚。
- **展示台**：`example/phys-query-pq1/run.bat`（live=0 / usage=2）+
  cases.json `phys-query-pq1` 卡 4 用例（`--only` 8/8 ALL GREEN）；
  selftest 内嵌 PQ1 live 节（与 CLI 同一 `checkQueryLive(90)`）。
- **踩坑**：① cmake generate 阶段挂死新形态（"Configuring done" 后空转、
  无 nmake/cl 进程、CPU 4s 不动）——杀 cmake 进程后 bat 链内 nmake 自动恢复，
  两树各中一次；② viewer 注入块作用域：QueryScene 须与 physicsScene 同级
  声明，收尾解绑在更外层作用域时局部声明会撞析构序。
