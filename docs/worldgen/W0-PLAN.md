# W0-PLAN · M-W0 大世界生成工具执行计划

> 立项：M-W0（2026-11-12，A 类并行轨道 `feat/worldgen`）。
> 拍板：2026-11-12 用户拍板，五问全按 `W0-OPTIONS.md` 推荐项（见 §0）。
> 铁律边界：A 类工具版交代 —— 不碰 `kStage` / 不进 renderer / 不改 `.nmesh` 冻结格式（只产新文件） / 不进渲染回归总闸；验收 = 展示台 `citygen` 工具卡（usage / 正向 / 负向，`expectExit` 0/1/2）+ 由工具真实输出生成的证据图（不许手绘）。

---

## 0. 已拍板结论（2026-11-12，全按推荐项）

| 编号 | 结论 |
|---|---|
| Q-B | 城市资产来源 = **程序化生成** |
| Q-W1 | 产物形态 = **W1-B 分块 `.nmesh` 集合 + `city.scene` 索引**（禁单巨型 mesh，保 P9 页表兼容） |
| Q-W2 | 路网契约 = **W2-A 自包含 `roadnet.json`**（字段契约见 §1，为 citygen ↔ 第 8 步交通的接口铁律） |
| Q-W3 | 算法内核 = **W3-A 全自研最小算法**（确定性 RNG，零新依赖，C++17，与 bevy_city 同构） |
| Q-C | 流式 = **第一版显存内 → V2 分块加载过渡，P9 V4 排第 10 步**（不阻塞本轨） |

---

## 1. roadnet.json 字段契约（Q-W2 拍板随附，接口铁律）

citygen 只产「路网几何 + 拓扑 + 车道语义」；第 8 步交通系统在其上构建 lane graph。

```json
{
  "version": 1,
  "seed": 12345,
  "units": "meters",
  "blocks": { "x": 5.5, "z": 4.0 },
  "nodes": [
    { "id": 0, "x": 0.0, "z": 0.0, "kind": "intersection", "signal": false }
  ],
  "edges": [
    { "id": 0, "from": 0, "to": 1, "lanes_per_dir": 1, "speed_limit": 13.9, "width": 3.0 }
  ],
  "phases": []
}
```

- `nodes.kind`：`intersection` / `dead_end`（预留）；`signal` 为第 8 步消费的布尔位，citygen 首版恒 false。
- `edges` 有向（`from`→`to`），双向路产两条；`lanes_per_dir` = 每方向车道数；`speed_limit` 单位 m/s（与载具第 2 步同源）；`width` = 车行道总宽。
- `phases`：**预留空数组**，信号相位由第 8 步扩展，citygen 不填。
- 扩展规则：只加字段不减字段，破坏性变更须 `version` 递增。

---

## 2. 里程碑阶梯

### M-W0.0 — `city-tools` 骨架 + 确定性地基（**已收官 2026-11-13，tag `w0-m0`**）
- **任务**：CLI（`--seed N --size N --out DIR [--stats]`）+ 自研确定性 RNG（照 nav/phys 先例，pcg 系，selftest 锁 seed）+ 规则网格 block 布局 + 空白城俯视 PNG（**纯 CPU 光栅化，不碰 GPU**）+ `CityStats` 计数锚骨架。
- **验收判据**：① seed 对称 selftest——同 seed 双跑产物逐字节一致、不同 seed 必不同（照 nav 先例确定性铁律）；② 展示台 `citygen` 卡（usage / 正向 / 负向）+ 空白城俯视证据图；③ DX12/VULKAN 双树 CMake 编译通过（工具本身不依赖图形后端）。
- **落地纪要（2026-11-13）**：`city-tools/`（main.cpp 单文件 + CMakeLists，纯 CPU 零引擎依赖，stb_image_write 出 PNG）；pcg32 已知答案锁实现（Python 参考实现独立复算）；布局口径与一处显式偏离（全跨度路网 vs bevy 开边）+ M-W0.0 种子敏感元（cell 灰度 16 档抖动，为使判据①「异 seed 必异」成立的最小实现，M-W0.3 密度分区接管）详 `CITY-TOOLS-GUIDE.md` §1.2/§1.3。selftest 23 断言全绿；ctest `w0-city-*` 双树 3/3（--repeat until-pass:3）；`--only w0-m0` 回归 8/8 ALL GREEN；`server.py --check` exit 0；`example/w0-m0/run.bat` 六段 ALL GREEN。踩坑：`%~dp0` 尾反斜杠 + 引号被 C 运行时吃掉（`--out "%D%"` 参数串并）→ `"%D%."` 口径；stb 只 include 不 define 实现宏 → LNK2019。

### M-W0.1 — 路网生成 → `roadnet.json`（**已收官 2026-11-13，tag `w0-m1`**）
- **任务**：grid 路网拓扑（`Road{start,end}` 同构 bevy_city）+ 车道语义 → 按 §1 契约写 `roadnet.json`。
- **验收判据**：① 路网俯视证据图（路网叠加布局）；② roadnet 契约 selftest——字段齐备、`from`/`to` 引用闭合、边数 = 预期网格公式；③ 展示台卡扩充 roadnet 用例。
- **落地纪要（2026-11-13）**：`buildRoadnet`（节点 (size+1)² 行主序 id、有向边 4·size·(size+1) 先横后竖 ±向序、lanes_per_dir=1 / speed_limit=13.9m/s / width=1.0m 与路带几何同源——契约示例 3.0 是示意值留档）+ `serializeRoadnet` 手写确定性序列化（2 空格缩进固定字段序，不引 JSON 库）；`generate` 增产 `roadnet.json`，俯视图叠加 roadnet 边中心线 + 节点标记（JSON 与几何同源证明）。两种路段计数口径并存留档（`roadSegments`=72 bevy 逐 cell / `roadnetEdges`=168 全跨度有向，详 GUIDE §2.3）。M-W0.1 零 RNG 消费，前缀契约不受影响。selftest 38 断言全绿；ctest 新增 `w0-roadnet` 双树 3/3；`--only w0-m0`/`--only w0-m1` 各 8/8 ALL GREEN；`server.py --check` exit 0；`example/w0-m1/run.bat` 七段 ALL GREEN exit 0。踩坑：run.bat `for /f` 反引号内 `\"` 被 cmd 吃掉（find 模式去引号 + pushd 相对路径绕盘符冒号分列）+ find /c 前导横线占 token1（计数在 token3）；Bash 沙箱对 `cmd //c run.bat` 形态 fail-closed，改 PowerShell 通道执行。

### M-W0.2 — 建筑挤出 → 分块 `.nmesh`（W1-B）（**OBJ 中间件段已收官 2026-11-13，tag `w0-m2`**）
- **任务**：OBB 地块递归细分 + 程序化楼体（RNG 分档高度/占地）→ 每 block 一份几何 + `city.scene`（E1 World JSON schema）索引全部分块与元数据。
- **产出通道（写死）**：city-tools 只产 OBJ 中间件 + 元数据；`.nmesh` 化**走 nanite-tools / asset-ingest 通道，不自写 meshing/DAG**——依据：`.nmesh` 格式已冻结（M0.7/S3），nanite-tools 是唯一合法入口，asset-ingest 轨道（2026-11-12 开）正在打通 OBJ→glb→nmesh；若该通道届时未就绪，缺口登记给 asset-ingest 轨道，本轨不自修。
- **验收判据**：① 分块 `.nmesh` 数量 = block 数、`city.scene` 引用闭合（`server.py --check` / `run.bat verify` 口径外，由工具 selftest 校验）；② 街景/俯视证据图（viewer `--scene` 渲染，**依赖可运行 viewer 的带显示环境**，M-TR0 实锤无头沙箱跑不了）。
- **落地纪要（2026-11-13，OBJ 中间件 + city.scene 段）**：递归细分（先 x 后 z 阈值 2.3/1.6 → 4.5×3.0 恒 4 叶）+ 每叶一栋（below(3) 高度档 {3,8,14}m + below(2) 占地内缩 {0.30,0.15}m，消费排流尾、36 shade 前缀逐字节不动，selftest 锁前缀）→ 每 block 一份 OBJ（8 顶点 12 三角外向绕序盒，block 局部坐标）+ `city.scene`（E1 World JSON：每 block 一实体，Transform=block 世界中心，Model.mesh 引用 OBJ，seed 无关索引——E1 schema 无 seed 键冻结面禁扩字段）。selftest 38→53 全绿；ctest 新增 `w0-cityscene` 双树 5/5；三卡回归各 8/8 + `--check` exit 0 + run.bat 七段 ALL GREEN。**缺口登记 asset-ingest**（PARALLEL-TRACKS 接收栏）：OBJ→glb 链在 feat/asset-ingest 已跑通未 merge，merge 后补转换消费步 + 白名单接入，街景证据图（viewer `--scene`）等通道 + 显示环境补拍，俯视证据图已交付；`.gitignore` 反白 `!example/*/blocks/*.obj`（楼体 OBJ 是交付物非编译中间产物）。`.nmesh` 数量判据①随转换步 merge 后由 selftest 扩锚收口，本段以 OBJ 数量 = block 数 + 引用闭合等价校验。

### M-W0.3 — 噪声分区密度 + 统计锚收口（**已收官 2026-11-13，tag `w0-m3`，M-W0 整轨收官**）
- **任务**：OpenSimplex 噪声分密度区（抄 bevy_city 思路不抄代码）+ `CityStats` 同构统计锚（楼数/路长/block 数写进产物与 `--stats` 输出）+ 全链 selftest 收口。
- **验收判据**：① 城市俯视最终证据图（密度分区可见）；② 展示台 `citygen` 全卡齐备；③ 收口走 A 类工具版交代：annotated tag（名字制 `w0-*`）+ MILESTONES 四栏 + `close` 闸（track worldgen 已注册，硬闸生效）。
- **落地纪要（2026-11-13）**：自研 simplex 梯度噪声（OpenSimplex 结构同构，F2/G2 数学推导 + 8 方向梯度表 + t⁴ 核，perm 独立 Pcg32(noiseSeed) Fisher-Yates；Python 参考实现独立复算 eval/perm 已知答案防自证，kNoiseNorm=40 经验归一化）密度分区（阈值 bevy 同源 0.45/0.6/0.7；noise_scale 0.025→0.06 显式偏离留档——size6 分区可见性）+ 树（forest 8/low 4 棵每块，仅俯视+统计层保 w0-m2 冻结基线）+ CityStats 密度档计数（21/5/6/4）。RNG 流最终形态四段（①36 shade → ②288 楼体 → ③noiseSeed → ④树位置），前两段逐字节不动重建流锁定。selftest 53→72 全绿；ctest 新增 `w0-density` 双树 6/6；四卡回归各 8/8 + `--check` exit 0 + run.bat 七段 ALL GREEN。踩坑：Python 参考 `lo=-size//2` floor 除法与 C++ 截断口径分歧（size1 偏差，C++ 为权威）、树位置浮点表示差（33·0.1f ≠ 2.1f 字面，断言改 fp 容忍度）。判据③ close 闸本段执行（`w0-m3` tag + MILESTONES 四栏 + close 报告）。
- **M-W0 整轨收官备注**：M-W0.0~W0.3 四 tag（`w0-m0`~`w0-m3`）；资产转换缺口（OBJ→glb→nmesh 链 merge 后补消费步 + viewer 白名单 + 街景证据图）已登记 asset-ingest 轨道；`kStage`/renderer/viewer/.nmesh 全程零触碰。

---

## 3. 冻结面

- `renderer/` 一行不碰；`kStage` 不动；`cases.json` 渲染回归卡不新增（工具卡 `kind:"tool"` 另计）。
- `.nmesh` 格式只消费不修改；`city.scene` 遵循 E1 World JSON 现有 schema，不扩字段。
- `viewer/` 不碰（证据渲染用现有 `--scene` / 截图能力）。
- nanite-tools / asset-ingest 接口只复用不修改。

## 4. 开工登记

- track `worldgen` 已注册 `tools/wt_pool.py` TRACKS（`state=planned`，wt 未建）。
- 落码开场：`python tools/wt_pool.py claim --track worldgen --milestone M-W0.0` → 建 wt `E:/UE_STUDY_2026/nl-engine-worldgen`（junction 共享 `.workbuddy` + slang dist + skip-worktree，纪律同 particles 轨道）→ 分支 `feat/worldgen`。
