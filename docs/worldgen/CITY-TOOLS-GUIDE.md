# CITY-TOOLS-GUIDE · 大世界生成工具归档（worldgen 轨道）

> 轨道：A 类并行 `feat/worldgen`（M-W0）；计划权威落点 `docs/worldgen/W0-PLAN.md`。
> 工具 exe = `build/{dx12,vulkan}/city-tools.exe`（纯 CPU、零图形 API、零引擎依赖，
> 仅 vendored stb_image_write.h 出 PNG）。退出码契约：0 = PASS / 1 = 断言失败 /
> 2 = 用法错误（对齐 phys-tools 与展示台 `expectExit`）。本文档按里程碑写实追加。

---

## 1. M-W0.0 — city-tools 骨架 + 确定性地基（2026-11-13，tag `w0-m0`）

### 1.1 子命令与用法

```
city-tools selftest [--verbose]            # 确定性/锚点自测，--verbose 逐条 ok 行
city-tools generate --seed N --size N --out DIR [--stats]
```

- `--seed`：任意 u64（默认 12345），进 pcg32 initstate。
- `--size`：每边 block 数 1..64（默认 6）；越界/缺值/未知选项一律退出 2，
  错误行前缀 `invalid arguments: ...`（负向用例锚）。
- `--out DIR`：必填；产物 `<DIR>/city_top.png`。
- `--stats`：stdout 追加 CityStats 计数块（锚：`stats: blocks=... groundTiles=...`）。
- `generate` 成功输出锚：`nl-citygen M-W0.0 blank city top view` / `wrote ... city_top.png (...)`。

### 1.2 确定性契约（M-W0.0 核心验收，nav/phys 确定性铁律同源）

- **RNG**：自研 pcg32（O'Neill 规范算法，public domain 口径抄算法不抄文件）；
  `seed()` = 规范 srandom(initstate=seed, 流常量 `0x14057B7EF767814F`)。
  已知答案锁实现版本：seed 12345 前三输出 = `1321476956 / 17539747 / 3348728241`。
- **消费序（跨里程碑契约，只许追加不许重排）**：单流，cell 行主序（gx 外层、
  gz 内层），每 cell 一次 `below(16)` 地面灰度档。M-W0.1 车道语义 / M-W0.2 楼体
  分档 / M-W0.3 噪声种子一律排在此流之后。
- **seed 对称 selftest**：同 seed 双跑布局规范序列化（`NLCT01` magic + 固定宽字段
  LE + cell 表）FNV-1a 逐字节一致；异 seed / 异 size 必异。PNG 双编码逐字节一致
  （stb zlib 确定性），异 seed PNG 必异。
- **为什么 M-W0.0 有种子敏感元**：空白城（无楼无车）本身无随机内容，而验收判据①
  要求「不同 seed 必不同」——取最小实现：每 cell 地面灰度 16 档抖动（233−2×shade）。
  M-W0.3 OpenSimplex 密度分区接管视觉分区后，该档位被密度分档替代（流前缀序保留）。

### 1.3 布局口径（bevy_city 同构 + 一处显式偏离）

- block 5.5m × 4.0m（= roadnet.json 契约 `blocks:{x:5.5,z:4.0}`）；`size` 居中
  （lo = −size/2）；block 内区 4.5m × 3.0m（= bevy ground tile 尺寸）。
- **偏离留档**：路网画 (size+1) 条横线 + (size+1) 条竖线全跨度（1.0m 宽），而
  bevy_city 是逐 cell 线段 + crossroad 补丁（+x/+z 两侧开边）。理由：无开边、
  无死路，M-W0.1 roadnet 节点直接取 (size+1)² 交点；拓扑信息量等价于
  「线段 ∪ 交点补丁」再补上边界两圈。
- CityStats 计数锚（size 6）：blocks=36 / groundTiles=36 / roadSegments=72 /
  crossroads=49；buildings/trees/cars/fences/paths 是 M-W0.2/0.3 挂零骨架。

### 1.4 M-W0.0 验收纪要

- selftest 19 条断言全绿（pcg32 流 64 输出一致 + 3 已知答案 + 异 seed 异首输出；
  布局双跑逐字节 / 异 seed 异 / 异 size 异；36/72/49 计数锚；契约锚 5.5/4.0 与
  4.5×3.0；extent 33×24m；shade 界；PNG 双编码逐字节 / 异 seed 异 / 1152×864 画布 /
  PNG 签名）。
- ctest：`w0-city-selftest` / `w0-city-generate` / `w0-city-negative`（双后端）。
- 交代物：`example/w0-m0/`（run.bat 六段 + selftest/generate/usage/negative 日志 +
  `city_top.png` 真实工具输出证据图）+ 展示台 `w0-m0` 卡四用例
  （usage exit2 / selftest / generate --stats / negative exit2）。
- 冻结面自查：`renderer/`、`kStage`、`.nmesh` 格式、`viewer/`、cases.json 渲染卡
  全部未触碰；`example/cases.json` 仅追加 `kind:"tool"` 版本卡。

### 1.5 踩坑

- （M-W0.0 无新增；RNG 已知答案常量由 Python 侧 pcg32 参考实现独立复算后回填，
  非工具自身输出自证。）
