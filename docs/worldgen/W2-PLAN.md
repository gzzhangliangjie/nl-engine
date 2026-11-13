# W2-PLAN — M-W2 楼体几何变体（feat/worldgen 并行轨道）

> 立项：2026-11-13，用户拍板「按推荐」（Q-W2A=A1 程序化楼型池 / Q-W2B=B1 FNV-1a 纯函数选型 / Q-W2C 形状集本轮定死，见 §2）。
> 前置实读：bevy `examples/large_scenes/bevy_city/src/assets.rs`（楼型池 = 每密度档
> `Buildings{meshes,materials}` + kenney glb 外部资产 + `rng.random_range` 消费选型）；
> Carrot `engine/render/` 无城市/楼体同类模块；UE Engine Source 无程序化城市模块
> （City Sample 在示例工程不在本机树）。历史约定：W0-PLAN Q-W3（W3-A 全自研）、
> CITY-TOOLS-GUIDE §3.1（RNG 流契约）、M-W1.1 先例（FNV-1a 纯函数材质分配）、
> kenney city-kit 已实测否掉（色板图集无窗户）。

## 1. 目标

city-tools 楼体从「每叶单一盒体」扩成**程序化楼型池**：每密度档多种盒体组合
楼型（退台 / 裙楼塔 / L 形），零新资产零新依赖，贴图沿用 M-W1.1 ambientCG
三档 facade 平铺。消费链（city2engine drive → nanite cluster/lod/dag → viewer）
零改动，几何升级直接吃满。

## 2. 拍板记录

| 问 | 结论 |
|---|---|
| Q-W2A 楼型池来源 | **A1 程序化楼型池**：盒体组合全自研（A2 外部 CC0 glb 池被否——kenney 前科 + 与 ambientCG 立面风格匹配风险） |
| Q-W2B 变体选取确定性 | **B1 FNV-1a 纯函数**：`fnv1a64(seed ‖ blockIndex ‖ buildingIndex) % 池容量`，RNG 流零消费，前缀契约零扰动（M-W1.1 材质分配同构） |
| Q-W2C 形状集 | 4 种形状原语 × 分档池（下表），池容量恒 3，按档子集映射 |

### 2.1 形状集（本轮定死）

| id | 形状 | 几何（footprint 局部：x0..x1 × z0..z1，高 h） |
|---|---|---|
| V0 | 单盒 | 现状盒体（y 0..h），全档兜底 |
| V1 | 退台 | 下段 y 0..0.55h 全 footprint；上段 y 0.55h..h，缩放 0.62 居中 |
| V2 | 裙楼塔 | 裙楼 y 0..0.30h 全 footprint；塔楼 y 0.30h..h，缩放 0.55 居中；**h < 6m 自动降级为 V1**（3m 楼配 0.9m 裙楼观感崩，降级是纯函数非 RNG） |
| V3 | L 形 | 主体 = 全宽 × 0.62 深（z0 起步）；翼 = 0.55 宽 × 全深，**x0 内缩 0.06m**（消共面 z-fighting），同高 h |

### 2.2 分档池映射（密度档 → 池，容量均 3）

| 材质档（=密度档） | 池 | 依据 |
|---|---|---|
| tier1 低层住宅（forest/low） | {V0, V1, V3} | 低层配退台小楼 + L 形住宅，无裙楼 |
| tier2 中层办公（medium） | {V0, V1, V2} | 退台 + 裙楼塔 |
| tier3 高层塔楼（high） | {V0, V1, V2} | 同 tier2，楼型差异由 14m 高度档 + 亮窗立面呈现 |

## 3. 实现切面（city-tools/main.cpp 单文件）

- `CityBuilding` 加 `std::uint32_t variant`；`generateCity` 第二遍叶循环内纯函数填充（RNG 流消费序 ①②③④ 逐字节不动）。
- `buildingParts(b)` = variant → `vector<BoxPart{cx,cz,hx,hz,y0,y1}>`（纯函数，楼型几何唯一真相源）。
- `serializeBlockObj` 改按 parts 展开：每 part 8 顶点 + 6 面 quad，UV 规则沿 W1.1（立面 kWindowUnit[档] 平铺、v 跨度 = part 实际净高/unit、顶/底面收缩角点微区）。
- `serializeLayout` 每楼追加 `variant` u32（规范字节变化 → 确定性锚全部动态比对，无硬编码 hash 波及）。
- OBJ 头注释 bump `M-W2.0`；`city.scene` / roadnet.json / 俯视 PNG 零改动。
- selftest 82 → ~90：variant 池界 / 跨档覆盖 / seed 敏感 / OBJ part 展开（v 行数 = Σparts×8、f 行数 = Σparts×12）/ part 落原 footprint 内（保「块内两两不重叠」不变量）/ 裙楼降级统计。

## 4. 步骤

| 步 | 内容 | 交代物 |
|---|---|---|
| M-W2.0 | city-tools 楼型池落码（本文件 §3 全部）+ w0-m2/m3、w1-m0/m1 基线重拍 | example/w2-m0/ + cases.json 卡 + 回归 |
| M-W2.1 | 消费验收：city2engine drive 全城 → viewer 双机位证据图 + GUIDE §7 | example/w2-m1/ + cases.json 卡 + 回归 |

## 5. 验收判据

1. `selftest` 全绿（双后端），新增断言全过；
2. `ctest --repeat until-pass:3` 双后端全绿；
3. w0/w1 基线重拍 ALL GREEN（run.bat 锚兼容）；
4. w2 证据图：俯视 + 街景双机位双后端，楼型变体肉眼可辨（单盒/退台/裙楼/L 同城可见）；
5. 回归 `--only` 抽 2~3 卡；收官 tag 时跑全量总闸一次。
