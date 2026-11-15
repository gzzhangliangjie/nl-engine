# M-PQ1 PLAN — 物理查询接 live DebugScene（方案 A：引用 + 动态体）

> 状态：**冻结口径**（2026-11-15 用户拍板方案 A「PIMPL 改持 DebugScene 的 system 引用
> + body 动态注册/移除 API 一次做齐；查 DebugScene matrix filter；TSV 锚改结构断言」）。
> 前置：M-PQ0（`phys-query-pq0`）fixture 形态与六数值锚**不破**；绑定面
> `querySetScene(QueryScene*)` 签名**零改动**（attached 的 QueryScene 走同一注入点）。

## 1. 目标与边界

把 M-PQ0 的确定性夹具查询世界升级为**可选挂接 live `DebugScene` 主物理世界**：
游戏①压线判定、游戏③交互检测最终要查的是现役世界（角色/车/落体），不是静态夹具。

**做**：attach（读 live 全部 body，narrowphase 天然可见动态体）+ 动态体注册/移除
API（fixture 模式写自建 system；attached 模式写共享 live system）。
**不做**：导出面改动（`exports.api` 5 条 query 语义不变）、`nl/anim`、渲染、kStage、
PNG 基线、CharacterVirtual 角色胶囊查询（scenario 1 的 CharacterHarness 是独立
PhysicsSystem，本里程碑只接主 system——挂账 M-PQ2）。

## 2. 设计

### 2.1 QueryHost 内部桥（Jolt 符号不出 core/src/physics）

新内部头 `core/src/physics/QueryHost.hpp`（非公开，两 cpp 共享）：

```cpp
struct QueryHost {                       // DebugScene::Impl 实现
    virtual ~QueryHost() = default;
    virtual void* system() = 0;          // JPH::PhysicsSystem*（Query.cpp 内转型）
    virtual unsigned layerFromMask(std::uint32_t layerBit) const = 0;  // 引擎层号 -> Jolt ObjectLayer
    virtual const void* broadPhaseLayerFilter() const = 0;  // matrix 驱动版
    virtual const void* objectLayerPairFilter() const = 0;
};
```

- `World.cpp`：`DebugScene::Impl` 实现 QueryHost（matrix filter = 既有
  `BPLayerInterfaceImpl` / `ObjectLayerPairFilterImpl`，World.cpp L1290-1292）；
  `DebugScene::queryHost()` 公开方法返回 `QueryHost*`（build 前返回 nullptr）。
- `World.hpp`：`struct QueryHost;` 前向 + `QueryHost* queryHost();`——公开头零 Jolt。
- `query.hpp`：`bool attach(QueryHost* host, std::string* err);`（QueryHost 前向声明；
  attach 后本对象**不拥有** host，生命周期由调用方保证先 detach/后析构 DebugScene）。

### 2.2 QueryScene 双模式

- `build()`（fixture 自建 system）：M-PQ0 语义逐位保形——六数值锚用例零改动。
- `attach(host)`：Impl 持 host 指针（不持 JPH system），查询走
  `host->system()` 的 `GetNarrowPhaseQuery()` + **host 的 matrix filter**
  （不是 Query.cpp 的全通 filter——scenario 2 需要 floor/floor 碰撞语义）。
- `detach()`：回未初始化态（可再 build/attach）。
- `ready()` = fixture built || attached。

### 2.3 动态体注册 API（尾部追加，只增不改）

```cpp
// 返回句柄（>=0），失败 -1（err 置因）。attached 模式写入共享 live system
//（Static 刚体，owner 存 userData——语义 = 往现役世界放查询用静态标记体，
// 与动态体真实碰撞，调用方自行选层）；fixture 模式写自建 system。
int  registerBody(const ShapeDesc& shape, const float pos[3], const float quat[4],
                  std::uint32_t layer, std::uint64_t owner, std::string* err);
bool unregisterBody(int handle);   // tombstone 语义（句柄不复用）
```

注册体的 bodyId 进 `ignoreBodyIds` 过滤的同一 ID 空间（Jolt BodyID 原值）。

### 2.4 viewer 注入与绑定面

`viewer/main.cpp` runNormal physics 训练场段（driveSetScene 旁）：`--script` 加载且
`o.physicsDebug && scenario==2` 时 `QueryScene qs; qs.attach(physicsScene.queryHost())`
→ `querySetScene(&qs)`；收尾先 `querySetScene(nullptr)` 再析构（S15 UAF 纪律，
attach 的 QueryScene 析构**必须先于** DebugScene）。runScene 的 M-W2A 静态夹具注入
保留不动（无 live 世界可挂）。绑定面 `querySetScene` 签名/错误码零改动。

## 3. 验收判据（结构断言为主）

- **M-PQ0 保形**：fixture 六数值锚 ctest 全绿（raycast/miss/layerMask/ignoreBodyIds/
  sweep TOI/overlap 四 body 升序）。
- **live 锚（phys-tools 新用例）**：build scenario 2（固定步确定性）→ step 90 帧
  → 结构断言：① raycast 向下打地板 `hit==true` 且 `layer==kFloor`；② raycast 向上
  miss；③ overlap 车体位置 `count>=1` 且 owner 序稳定；④ registerBody 后
  `bodyCount` 增 1、unregister 后回退；⑤ attach 后 fixture 专用 API
  `bodyIdBySlot` 返回 0（slot 语义仅 fixture）。fraction 数值**不进锚**。
- 双后端 BUILD OK；`gen.py --check` 不适用（未碰绑定面）；kStage 不 bump。
- cases.json tool 卡（phys-tools exe）+ run.bat + 回归冒烟。

## 4. 交付物

`core/src/physics/QueryHost.hpp`、`core/include/nl/physics/query.hpp`（attach/
detach/registerBody/unregisterBody）、`core/include/nl/physics/World.hpp`
（queryHost()）、`core/src/physics/Query.cpp` + `World.cpp`、
`phys-tools/`（live 用例 + selftest 段）、`viewer/main.cpp` 注入点、
`docs/p8/PHYQUERY-PLAN.md` §M-PQ1 + TOOLS-GUIDE、`example/cases.json`、
MILESTONES 四栏、tag `phys-query-pq1`。

## 5. 后置挂账（M-PQ2 候选）

CharacterHarness（scenario 1 独立 PhysicsSystem）的查询挂接、动态体注册的
Kinematic 变体、querySetScene 与 driveSetScene 的联合注入旗标化（当前隐含
scenario 2 + script 同时启用才注入 live）。
