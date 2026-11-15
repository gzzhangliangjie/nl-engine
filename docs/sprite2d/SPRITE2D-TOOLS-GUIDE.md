# sprite-tools 使用说明（M-SP0）

`sprite-tools` 是 2D 精灵簇 **core 侧** 的无头验收工具：只链 `nl_core`，
不链接 `nl_renderer`、不创建窗口、不接触任何图形 API。它验证的是
`core/include/nl/sprite/` 下三组纯 POD/CPU 逻辑：

| 模块 | 验证内容 |
|---|---|
| `atlas.hpp` | `AtlasGrid::uvRect()` 的均匀网格 UV、越界 `Throw/Repeat/Clamp` 三模式 |
| `flipbook.hpp` | `Clip`/`FlipbookPlayer` 的 loop、reverse、pause、stop、finished latch、时间轴回绕 |
| `sprite_desc.hpp` | `SpriteBatch::sortByTexture()` 的 (layer, textureId, sequence) 稳定排序与桶区间覆盖 |

GPU 侧（`Sprite2DPass` / `sprite2d.slang`）不在这个工具的验收范围内，
由双后端 BUILD + `shaderArtifactsSelfTest()` 覆盖。

## 退出码

| rc | 含义 |
|---|---|
| 0 | PASS |
| 1 | 断言失败 / 文件读写失败 |
| 2 | 用法错误（工具卡负例依赖此码） |

## 命令

```bat
build\DX12\sprite-tools.exe selftest
build\DX12\sprite-tools.exe atlas-trace    --out sp0-atlas.tsv
build\DX12\sprite-tools.exe atlas-check    sp0-atlas.tsv
build\DX12\sprite-tools.exe flipbook-trace --out sp0-flipbook.tsv
build\DX12\sprite-tools.exe flipbook-check sp0-flipbook.tsv
build\DX12\sprite-tools.exe batch-trace    --out sp0-batch.tsv
build\DX12\sprite-tools.exe batch-check    sp0-batch.tsv
```

`trace` 写确定性 TSV，`check` 从 TSV 读回并用**独立 oracle**（不调用被测函数）
重算期望值 + 校验几何/时间/排序不变式。两步分开是可复现证据的前提：
TSV 可以留档、可以 diff、可以用别的实现复算。

`selftest` 等价于一次性跑完三组 build + check，实测输出锚：

```
  [PASS] atlas grid UV rect + addressing (6 cases)
  [PASS] flipbook timeline (loop/reverse/pause/stop/finished)
  [PASS] sprite batch stable sort + texture buckets
  [PASS] POD sizes (UvRect=16B)
SELFTEST PASS (4/4)
```

## TSV 格式

所有浮点固定 `%.6f`，列以 TAB 分隔，行尾 `\n`（binary 写，避免 CRLF 漂移）。

- `atlas`：表头 `name dimX dimY addr index ok u0 v0 u1 v1`；
  `addr` = 0 Throw / 1 Repeat / 2 Clamp；`ok=0` 的行四个 UV 必须为 0。
- `flipbook`：表头 `name step state finished frame time`；
  `state` = 0 Stopped / 1 Playing / 2 Paused；`frame` 只能落在 clip 的帧集合内。
- `batch`：表头 `#type pos begin count layer textureId sequence`；
  `S` 行是排序后的精灵，`B` 行是桶区间（后四列对 `S` 行有效位不同）。

## ctest 登记

`sprite-tools/CMakeLists.txt` 共 9 条：

| 用例 | 作用 |
|---|---|
| `sp0-sprite-selftest` | 三组一次全跑 |
| `sp0-selftest-negative` | `selftest bogus` → exit 2（WILL_FAIL） |
| `sp0-atlas-trace` / `sp0-atlas-check` | atlas TSV 产出 + 回读 |
| `sp0-flipbook-trace` / `sp0-flipbook-check` | 时间线 TSV 产出 + 回读 |
| `sp0-batch-trace` / `sp0-batch-check` | 分桶 TSV 产出 + 回读 |
| `sp0-trace-negative` | `atlas-trace --out` 缺路径 → exit 2（WILL_FAIL） |

`trace` 必须先于对应 `check` 注册；ctest 默认串行，按注册顺序执行。

```bat
scripts\wt.bat sprite2d ctest DX12
scripts\wt.bat sprite2d ctest VULKAN
```

## 展示台交代物（工具版）

`example/cases.json` 新增版本卡 `sprite2d`（4 条 `kind:tool` 用例：usage /
selftest / batch-trace / negative，退出码 2 / 0 / 0 / 2），`exe` 双后端指向
`build/{dx12,vulkan}/sprite-tools.exe`，每条卡带 `preview` 证据图与
`script` 复现入口。

```bat
example\sprite2d\run.bat
```

该脚本实测 `ALL GREEN`：selftest 4/4 → 三组 TSV 的 trace+check → 两次
`batch-trace` 用 `fc /b` 逐字节一致 → usage / 两条负例退出码 → 调
`tools/make-sp0-evidence.py` 从上述真实日志渲染 4 张 PNG。
`server.py --check` 退出 0（它只覆盖 `S8.2` 旧卡，不覆盖本卡）；本卡的机械验收由
`regression.py --only sprite2d` 承担，实测 **8/8 ALL GREEN**（bytes 8 / near 0 / FAIL 0，
4 卡 × 双后端，2s）。证据图全部来自工具真实 stdout/stderr 与 TSV，不手绘示意、
不复用其他版本截图；8 张 PNG（两轨各 4 张）均已逐张解码校验。`run.bat` 行尾经
`check-bat-encoding.py --fix` 转 CRLF 后门禁 0 violations。

## 边界

- 本工具**不**验证画面，工具本体也不产出 PNG；证据图由 `run.bat` 调渲染器
  从真实输出生成。M-SP0 没有 `viewer/main.cpp` 的 demo 旗标与**渲染截图基线**
  （冻结口径后置 M-SP1）。
- 分桶的 GPU 侧「每桶换 binding set 一次 draw」同样后置 M-SP1；本轮 CPU 侧
  分桶完整保留并由此工具验收，GPU 提交为整批单次 draw。理由见
  `SPRITE2D-PLAN.md` §4 / §8。
- 新增 `.slang` 产物登记在 `renderer/src/renderer.cpp::kArtifacts[]`，
  与工具卡无关，勿漏（M8.1 教训）。
