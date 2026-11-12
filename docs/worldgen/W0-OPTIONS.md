# W0-OPTIONS · 大世界生成工具方案矩阵

> 立项：M-W0（2026-11-12，A 类并行轨道 `feat/worldgen`）。
> 上游：GAME-DEV-ORDER 第 1~5 步（游戏壳 第 5 步 gate 排期，但 citygen 作为离线工具可与之解耦并行推进）。
> 下游：第 7 步最小可玩版（吃城市 + 路网）、第 8 步交通系统（吃 `roadnet.json`）、第 10 步流式分页（P9 V4）。
> 铁律边界：A 类，不碰 `kStage` / 不进 renderer / 不重拍冻结基线；验收走工具版交代（展示台 citygen 卡 + 真实城市俯视/街景证据图），不进 `bytes/near` 渲染回归总闸。

---

## 0. 定位与边界（已定，非待拍板）

- **工具名**：`city-tools`（独立 exe，照 `nanite-tools` / `gi-tools` 纪律）。
- **输入**：`--seed N --size N [--car-density N]`（确定性 RNG，同 seed 同城）。
- **输出**：`city.scene`（World JSON，引用分块资产）+ 分块 `.nmesh` 集合 + `roadnet.json`（路网拓扑/车道语义）。
- **验收形态**：工具版交代——展示台 `citygen` 卡（usage/seed-symmetry/分块计数/roadnet 契约）+ 真实城市俯视 & 街景证据图（由工具产物真实渲染，不许手绘）。
- **三处参照（已实读，详见 MILESTONES M-W0 立项节）**：
  - Carrot `engine/render/`：**零 world/terrain/streaming/city 模块**，实例仅 transform+UUID 扁平结构 → 反向印证「流式是后期重活」，不抄。
  - bevy `examples/large_scenes/bevy_city/`：`spawn_city(seed,size,car_density,stats)` —— 规则网格（block 5.5×4.0）、`SmallRng::seed_from_u64` 确定性、OpenSimplex 噪声分密度区、单 road asset 旋转拉伸、Road{start,end}/Car 车道语义、`CityStats` 计数锚；定位「extremely large scenes 压力测试」+ VisibilityRange LOD。
  - UE5 `WorldPartition` + `PCGGraph`：工业终态把**生成**（PCG = Node/Pin/Settings DAG）与**分区流式**（runtime cell + ObjectMapping→package/path/GUID）**解耦两层**，离线产资产是一等公民；5.8 官方新增 Mesh Terrain + 城市街道 PCG 图示例。

---

## 1. Q-W1 产物形态（核心架构判定）

| 候选 | 描述 | 代价 | 推荐 |
|---|---|---|---|
| **W1-A 单巨型 `.nmesh`** | 一次性产一个城市 mesh | ❌ 直接踩 P9 INV-1/2 + 页表草案 §4.5（页分区复用 S3 DAG 组）；事后重切贵一个数量级 | 否 |
| **W1-B 按 block/page 分块集合** | 每 block 一个 `.nmesh` + 全局 `city.scene` 引用 | ✅ 原生贴合 `NPAG` 页表（128 KiB 预算 / 复用 S3 DAG 组），流式直接用 | ✅ 推荐 |
| **W1-C World JSON 引用现有资产** | 只生成布局/元数据，几何复用现成 `.nmesh` 库 | ✅ 最快出城；但城市同质、缺程序化几何，验证力弱 | 过渡可选（首版快速验证） |

→ **推荐 W1-B（分块集合）+ `city.scene` 做索引**。W1-C 可作 M-W0.0 第一版快速冒烟，但终态走 W1-B。

---

## 2. Q-W2 `roadnet` 与第 8 步 lane graph 契约

第 8 步交通系统要吃 citygen 产的路网数据；**契约不定 → 第 8 步必返工**，这是最该现在锁死的一问。

| 候选 | 描述 | 代价 | 推荐 |
|---|---|---|---|
| **W2-A `roadnet.json` 自包含** | 节点/边/车道数/限速/红绿灯相位预留字段，citygen 只产「路网几何+拓扑+车道语义」 | ✅ 两阶段解耦，第 8 步在其上构建 lane graph，迭代互不阻塞 | ✅ 推荐 |
| W2-B 直接产第 8 步 lane graph 格式 | citygen 输出即 lane graph | ⚠️ 紧耦合，第 8 步算法变动会反噬 citygen | 否 |

→ **推荐 W2-A**。字段契约（节点/边/车道方向/连接关系/相位预留位）在本文档拍板时一并写定，作为 citygen 与第 8 步的接口铁律。

---

## 3. Q-W3 算法级 vendored 取舍

项目通例：关联系统优先用成熟 vendored 库。但 citygen 的核心是**规则建模算法内核**（路网 L-system / OBB 地块递归细分 / 建筑挤出 / 噪声分区），与「系统」不同——业界无对口 vendored C++ 库（开源多为 demo 级、依赖 SFML/OpenGL/UE，按选型闸须单独评审且不建议 vendored）。

| 候选 | 描述 | 代价 | 推荐 |
|---|---|---|---|
| **W3-A 全自研最小算法** | 吃 bevy_city 结构 + Parish&Müller 思路，零新依赖，确定性 RNG | ✅ 与「零构建期新依赖 + C++17 + 无厂商 SDK」硬闸完全契合 | ✅ 推荐 |
| W3-B vendored 某开源 city gen | 引入 demo 级库 | ❌ 评审风险高、依赖不净、与 bevy_city 同构后无收益 | 否 |

→ **推荐 W3-A**。与「关联系统优先成熟库」通例的张力在于：本轨道核心是算法而非系统，无成熟库可 vendored；走自研最小算法最干净，且与 bevy_city 同构。

---

## 4. Q-C 流式时机

- 第一版城市控制在**显存内**（W1-B 分块天然为流式铺路，但流式本身不急）→ 可用 V2 分块加载过渡。
- 城市规模拉到超显存才必须上 P9 V4 流式分页（第 10 步）。
- 与 Q-W1 联动：W1-B 分块让「第一版显存内」与「后续流式」零返工衔接。

→ **结论**：Q-C 不阻塞 M-W0 开工；第一版走显存内 + V2 分块加载，V4 流式排第 10 步。

---

## 5. 拍板记录（✅ 2026-11-12 用户拍板：五问全按推荐项）

| 编号 | 问题 | 拍板结论 |
|---|---|---|
| Q-B | 城市资产来源：程序化生成 vs 外部资产 | ✅ 程序化生成 |
| Q-W1 | 产物形态 | ✅ W1-B 分块集合 + `city.scene` |
| Q-W2 | `roadnet` 契约 | ✅ W2-A 自包含 `roadnet.json`（字段契约随拍板写死，落 `W0-PLAN.md` §1） |
| Q-W3 | 算法级 vendored | ✅ W3-A 全自研最小算法 |
| Q-C | 流式时机 | ✅ 第一版显存内 → V2 分块过渡，V4 后移 |

---

## 6. 里程碑阶梯草案（拍板后细化进 `W0-PLAN.md`）

- **M-W0.0**：`city-tools` 骨架（CLI + 确定性 RNG + 规则网格 block 布局）→ 空白城俯视图（可选 W1-C 快速冒烟）。
- **M-W0.1**：路网生成（Road{start,end} + 车道语义）→ `roadnet.json` + 路网俯视证据。
- **M-W0.2**：建筑挤出（OBB 地块 + 程序化楼体）→ 分块 `.nmesh`（W1-B）+ 街景证据。
- **M-W0.3**：噪声分区密度 + 统计锚（`CityStats` 同构）→ 城市俯视证据图收口。
- **验收**：工具版交代（citygen 卡 + 证据图），不进渲染回归总闸；不动 `kStage`、不改冻结基线。

---

## 7. 与现有地基的复用

- 几何出口直接吃 `.nmesh`（M0.7 / S3 已冻结格式）+ `NPAG` 页表草案（分块即页）。
- 资产引用走 `city.scene`（E1 World JSON + `viewer --scene` 已就绪）。
- 路网数据走 `roadnet.json`，第 8 步在其上构建 lane graph（CARLA TrafficManager 看思路不抄代码）。
- 确定性铁律：同 seed 产物逐字节一致（照 nav/phys 先例，selftest 锁 seed 对称性）。

---

*本文档为 M-W0 立项后的方案矩阵；五问已于 2026-11-12 全数拍板（均按推荐项），执行计划见 `docs/worldgen/W0-PLAN.md`，track `worldgen` 已注册 wt_pool（state=planned）。*
