# M-PQ0 — 通用物理查询 API（波 1 / A 类轨道）

> 轨道 `phys-query` / wt `E:/UE_STUDY_2026/nl-engine-phys-query` / 分支 `feat/phys-query`
> 立项 2026-11-15，用户拍板 `A A ①`：**物理查询 = A 最小面**、2D 精灵簇 = A 严格切分、轨道 = 两轨。
> 本文件是冻结口径，无待定架构判定。权威纪律见根 `AGENTS.md`。

## 1. 目标与边界

给游戏层一个**不暴露 Jolt 类型**的通用物理查询面：raycast / sweep / overlap 三族，
覆盖游戏①压线与定点判定、游戏③交互检测、NPC 交互距离判定。

**本里程碑只交付「查询 API + 确定性夹具」**，不接引擎主场景的 `PhysicsSystem`
（`DebugScene::Impl` 持有），也不碰绑定面。原因：接线触碰现有 DebugScene 生命周期，
属于独立里程碑，且绑定面被波 0 闸挡住（见 §5）。

### 明确不做（范围外，不是待定项）

- UE 式 Channel / Profile / ObjectType / 复杂查询参数 / face index / 物理材质
- 异步查询、`std::function` 过滤回调（Carrot 厚封装形态，违反 PIMPL 纪律）
- `exports.api` Physics 查询域 / `Bindings.cpp` / `Wiring.g.cs` / `API.md`（波 2）
- 与 `DebugScene` 主物理世界共享实例（M-PQ1 单独立项）
- 渲染、`kStage`、截图基线、viewer 任何改动

## 2. 三处证据（本轮定向实读，2026-11-15）

| 来源 | 实读文件 | 结论 |
|---|---|---|
| Carrot | `engine/engine/physics/PhysicsSystem.cpp:142-248` | 只有 raycast 一种；公开头直接暴露 `JPH::` 类型与 `std::function` 回调 → **厚封装反例**，本项目不取 |
| bevy | `crates/bevy_pbr/src/meshlet/`、`crates/bevy_solari/src/realtime/` | 本体无官方物理 crate，无查询口径可抄 |
| UE 5.8.4（只读） | `Engine/Classes/Engine/World.h` | `LineTraceSingleByChannel` / `SweepSingleByChannel` / `OverlapMultiByChannel` 三族 × Single/Multi 全公开面；`FCollisionQueryParams` 承载 ignore/层过滤 |

**综合口径**：三族划分取 UE（工业界通用最小完备集），**过滤参数收敛为一个 `QueryFilter`**
（layerMask + ignoreBodyIds），公开头保持 Jolt-free。不取 UE 的 channel/profile 膨胀面
（当前三游戏无消费方，抽象会空转），也不取 Carrot 的厚封装。

## 3. 冻结公开面（`core/include/nl/physics/query.hpp`，纯 POD + PIMPL 宿主）

```cpp
struct QueryFilter { uint32_t layerMask; vector<uint64_t> ignoreBodyIds; };
struct RayQuery    { Vec3 origin, direction; float maxT; QueryFilter filter; };
struct ShapeDesc   { enum Kind { Sphere, Box, Capsule }; float dims[3]; };
struct SweepQuery  { ShapeDesc shape; Vec3 from, to; Quat rot; QueryFilter filter; };
struct OverlapQuery{ ShapeDesc shape; Vec3 pos; Quat rot; QueryFilter filter; };
struct QueryHit    { bool hit; float fraction, distance; Vec3 point, normal;
                     uint32_t bodyId, subShapeId, layer; uint64_t owner; };

class QueryScene {                       // PIMPL，唯一 Jolt 符号在 .cpp
    bool build(std::string* err);        // 固定确定性夹具：floor + 3 盒 + 1 sensor
    uint32_t bodyCount() const;
    uint32_t bodyIdBySlot(int) const;    // selftest 取盒 id（ignore 用例必需）
    QueryHit              raycast(const RayQuery&) const;
    QueryHit              sweep  (const SweepQuery&) const;
    vector<QueryHit>      overlap(const OverlapQuery&, size_t maxHits) const;
};
```

`ShapeDesc::dims` 语义：Sphere = `[radius]`；Box = `[hx,hy,hz]`；Capsule = `[halfHeight,radius]`。
`QueryFilter::layerMask` 复用 `layers::k*` 四位（0 Floor / 1 Default / 2 Debris / 3 Ghost）。
`ignoreBodyIds` 存 `JPH::BodyID::GetIndexAndSequenceNumber()` 原值。

## 4. 实现映射（`core/src/physics/Query.cpp`）

- `JPH::TempAllocatorImpl` + `JobSystemSingleThreaded` + 1:1 的 `BPLayerInterfaceImpl` /
  `ObjectVsBroadPhaseLayerFilter` / `ObjectLayerPairFilter`（全通），`PhysicsSystem::Init`。
- **查询专用过滤器**（这是与仿真侧最大的差别）：`NarrowPhaseQuery` 的默认过滤参数是全通，
  **不会自动复用 `LayerMatrix`**；必须显式传 `QueryLayerFilter`（按 layerMask）与
  `QueryBodyFilter`（按 ignoreBodyIds），否则会打到 `kGhost` 等被禁层。
- `raycast` → `CastRay(RRayCast, RayCastResult)` 单命中版；`direction * maxT` 作为 Jolt 的
  `mDirection`（完整位移向量）。法线用 `BodyLockRead` + `Body::GetWorldSpaceSurfaceNormal`。
- `sweep` → `RShapeCast::sFromWorldTransform` + `CastShape` +
  `ClosestHitCollisionCollector<CastShapeCollector>`；法线取 `-mPenetrationAxis.Normalized()`。
- `overlap` → `CollideShape` + `AllHitCollisionCollector<CollideShapeCollector>`，
  结果**按 bodyId 排序**保证确定性（Jolt collector 的 Sort 对 CollideShape 不稳定）。
- 每个 body 的 `userData` 直接存 owner；`QueryHit.owner` 由 `BodyInterface::GetUserData` 回读。

## 5. 冻结面（波 0 硬闸）

`input` 轨（`feat/input`，M-I3~I5 已 RELEASED 待用户 merge 指令）活跃占用：
`exports.api`、`ApiTable.g.hpp/.inc`、`Bindings.cpp`、`Wiring.g.cs`、`API.md`、`viewer/main.cpp`。
**本轨一行不碰上述文件**。M-PQ0 的 ctest / 工具卡全部走 `phys-tools`，零渲染改动。

## 6. 验收判据（A 类工具版，纯 CPU）

`phys-tools query-trace --out <tsv>` 输出六条确定性数值锚，`query-check <tsv>` 回读断言：

| # | 锚 | 期望 |
|---|---|---|
| 1 | 射线命中盒 A | `hit=1` / `distance=14` / `point.z=4` / `normal=(0,0,-1)` / `owner=boxA` |
| 2 | 射线 miss（x=50 平行地板） | `hit=0` |
| 3 | `layerMask=1<<kDebris` | 跳过盒 A，命中盒 B（`point.z=9`） |
| 4 | `ignoreBodyIds={boxA}` | 跳过盒 A，命中盒 B |
| 5 | 球 sweep 撞盒 A | TOI `fraction≈3.5/30`，`normal=(0,0,-1)` |
| 6 | 大盒 overlap | 返回 4 个 body，按 bodyId 升序 |

- `phys-tools selftest` 追加同样六条断言，log 锚 `phys-pq0 selftest: PASS`。
- `phys-tools/CMakeLists.txt` 加 `pq0-query-trace` / `pq0-query-check`（DEPENDS）/
  `pq0-query-negative`（WILL_FAIL TRUE）三例；ctest 统一 `--repeat until-pass:3`。
- `example/phys-query/` 展示台 tool 卡三条：usage（exit 2）/ 正向（exit 0）/ 负向（exit 1）。
- 不 bump `kStage`，不进渲染回归总闸。

## 7. 交付物清单

`core/include/nl/physics/query.hpp`、`core/src/physics/Query.cpp`、根 `CMakeLists.txt`（1 行源登记）、
`phys-tools/main.cpp`（2 子命令 + selftest 段）、`phys-tools/CMakeLists.txt`、
`example/phys-query/`（run.bat + cases.json 卡 + 真实 TSV 生成的证据图）、
`docs/p8/PHYQUERY-PLAN.md` + `PHYQUERY-TOOLS-GUIDE.md`、`docs/MILESTONES.md` 四栏。
