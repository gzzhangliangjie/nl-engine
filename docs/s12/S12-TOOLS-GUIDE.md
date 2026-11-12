# S12 工具与验收指南

> 权威计划：`docs/s12/S12-PLAN.md`（§0 拍板 / §0.6 任务书）；槽位表：`docs/SHADER-CONVENTIONS.md` §2.19；台账：`docs/MILESTONES.md`。
> 本文只讲「怎么跑、为什么这么验收」，不重复计划里的架构决策。适用里程碑：M12.0（tag `s12-temporal`）。

## 1. 开关一览

| 开关 | 作用 | 默认 | 备注 |
|---|---|---|---|
| `--mode filter` | 注册 S12 时域累积模式（probe 链基建复用） | 关 | 缺省资产 `Suzanne-materials.nmesh`，同 `probe-viz` 的 NFLB fallback |
| `--temporal-off` | 关时间层（= 单层当前帧 GI） | 开 | **抖动仍开**：OFF 图 = 单帧噪声，ON 图 = 收敛后，两者构成收敛自比 |
| `--temporal-alpha <f>` | 当前帧 GI 权重下限 | 0.1 | 实际 `alpha = max(userAlpha, 1/(len+1))`，首帧恒 1 |
| `--temporal-history-max <n>` | 历史长度上限 | 32 | 1..256 |
| `--filter-gi-dump` | **把累积后的 `gatherLinear_` 直出画面** | 关 | M12.0 验收主对象；复用 M11.2 `csIrradDump` / `irradDumpLayout_` / `irradDumpPSO_`，零新增 shader / PSO / RT |
| `--probe-atlas-dump` | 直出 `irradAtlas_[0]` | 关 | M11.2 遗留工具，可用来证明 Q12-E 抖动确实改写了 atlas |
| `--orbit-speed <deg>` / `--screenshot-seq <dir>` | 快速移动确定性逐帧序列 | 关 | M11.3 工具化，M12.2 鬼影验收复用 |
| `--dist <n>` | 相机距离 | — | **验收必须用 4**，原因见 §2 |

## 2. 三个画面，只有两个能当尺子

| 画面 | 怎么出 | 在本场景能否验收 M12.0 |
|---|---|---|
| 合成图（`--mode probe/filter` 默认） | `csGatherCompose` 把 GI 叠进直光 | ❌ `--dist 7` 下 GI 恒 0；`--dist 4` 下虽可见，但混着直光，噪声不好量化 |
| **GI 直出**（`--filter-gi-dump`） | 绕开 compose，直接输出累积 GI | ✅ **M12.0 主验收对象** |
| atlas 直出（`--probe-atlas-dump`） | 输出 `irradAtlas_[0]` | ⚠️ 只能证明抖动生效（atlas 变了），不能证明时间累积收敛 |

两条实测根因（都是既有链路事实，不是 M12.0 引入）：

1. **结构性 ON≡OFF**：`probe_trace.slang` 每探针 64 条**固定**八面体方向，逐帧确定性 → `lerp(hist, cur, α)` 恒等于 `cur`；且 `probe_atlas_dump.slang` 文件头（M11.2）实锤 `csGather` 每像素只采外法线方向那一个 texel，凸体 + 纯天空下该方向恒 miss → 天空渐变。用户拍板 Q12-E 给 filter 态加逐帧**随机方向置换**（不是微扰：朝外方向微扰不改变 hit/miss，实测 atlas 变了但合成图仍逐字节相同）。
2. **`--dist 7` 下 `gatherLinear_` 恒 0**：`csGather` 权重 `exp2(-10000·rel²)` 与 fallback `exp2(-1000·rel²)` 全灭 → `sum < kMin` → `result = 0`，GI 直出图**全黑**（实测 960×540 全像素 0）。s10/s11 基线卡都用 `--dist 7`，所以它们的 GI 项本来就是 0。`--dist 3/4/5` 权重存活，验收统一取 **4**。

## 3. 复现命令

```bat
REM 冻结参考（无抖动，GI 直出）
build\DX12\viewer.exe --mode probe --filter-gi-dump --model Suzanne-materials.nmesh ^
  --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4 --frames 16 --screenshot ref.png

REM OFF：单帧噪声（抖动开、时间层关）
build\DX12\viewer.exe --mode filter --temporal-off --filter-gi-dump ^
  --model Suzanne-materials.nmesh --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4 ^
  --frames 16 --screenshot off.png

REM ON：累积 16 帧
build\DX12\viewer.exe --mode filter --filter-gi-dump ^
  --model Suzanne-materials.nmesh --no-hud --size 960x540 --yaw 12 --pitch 9 --dist 4 ^
  --frames 16 --screenshot on.png

python tools\check-s12-temporal.py onoffref ref.png off.png on.png
example\s12-temporal\run.bat verify
```

一键验收（双后端 + 负向 + 工具自测）：`example\s12-temporal\run.bat verify`，退出 0 = `[ALL GREEN] s12-temporal verify`。

## 4. `tools/check-s12-temporal.py`

纯标准库实现（`zlib` + `struct` 解 8-bit 非交错 PNG），**不依赖 PIL** —— 本机 `python` 与 `pip` 指向两个不同安装，PIL 不保证可用。

| 子命令 | 断言 |
|---|---|
| `onoff <off> <on> [hfRatio=0.88] [minDiff=1000]` | `ON != OFF`（`|diff|>2` 像素数 ≥ minDiff）；`hf(ON) ≤ hfRatio · hf(OFF)` |
| `onoffref <ref> <off> <on> [hfRatio=0.88] [maeRatio=0.90] [minDiff=1000]` | 上面两条 + `MAE(ON,ref) ≤ maeRatio · MAE(OFF,ref)` |
| `selftest` | 合成棋盘噪声 / 平图的正向与负向自检，不读文件 |

`hf` = 半分辨率格点上 `mean(|dI/dx| + |dI/dy|)`（阈值即按该口径实测标定）。输出锚 `[s12-temporal] ...`，退出码 0 通过 / 1 失败 / 2 用法。

**为什么用统计断言**：Q11-1 拍板 A —— 每帧重建的 AS 内部布局 DX12/Vulkan 不保证 bit-exact，GPU 数值图天生非逐字节可复现；双后端 MD5 不同但统计不变量一致（实测两后端 `hf` / `diffPixels` 相同、MAE 差 0.001）。禁止对这类图写 `bytes` 断言。

## 5. `run.bat verify` 断言清单

- 两后端各自重拍 REF / OFF / ON 三图（`--frames 16`），每次 `exit 0` 且日志含 `deviceErrors=0`。
- `check-s12-temporal.py onoffref` 双后端全过。
- **负向控制**：把 OFF 冒充 ON 必须非零退出（防「静态 ON≡OFF」回归）。
- `check-s12-temporal.py selftest` 通过。
- 冻结证据图 `ref-dx12/vulkan.png`、`temporal-on-dx12/vulkan.png` 仅供展示；断言用**同次重拍的 REF**，避免 Q11-1 逐字节依赖。

## 6. 实测数据（2026-11-12，双后端同口径）

| 指标 | DX12 OFF | DX12 ON16 | VULKAN OFF | VULKAN ON16 | 门限 |
|---|---|---|---|---|---|
| 高频能量 `hf` | 1.216 | **0.677** | 1.215 | **0.677** | ratio ≤ 0.88（实测 0.556 / 0.557） |
| 与无抖动参考 MAE | 3.902 | **2.687** | 3.902 | **2.686** | ratio ≤ 0.90（实测 0.688） |
| `diffPixels(ON,OFF)` | 135409 | — | 135413 | — | ≥ 1000 |
| 合成图 `hf`（补充证据） | 1.873 | 1.493 | — | — | 只作观察，不作断言 |

收敛序列（GI 直出，`--frames` 递增）：`hf` 1.240 → 0.910(4f) → 0.677(16f) → 0.691(32f)；MAE 4.410 → 3.142 → 2.687 → 2.741。16 帧后进入平台期，符合 `α≈0.1` + `historyMax=32` 的预期。

## 7. 已知坑速查

| 现象 | 原因 / 处理 |
|---|---|
| 静态四图 MD5 全同 | Q12-E 前 = 输入确定性（结构性）；Q12-E 后若仍全同，先查 `--dist` 是不是 7 |
| GI 直出全黑 | `csGather` 权重全灭 → 换 `--dist 4`（或 3/5） |
| atlas 变了、合成图没变 | M11.2 盲通道 + 微扰无效 → 抖动必须是**随机方向置换**，不是微扰 |
| DX12 ECL 报 history barrier UAV vs SRV | `keepInitialState=true` 会把资源锁成 permanent SRV；ping-pong 禁用，只留 `initialState` + 每帧 `beginTracking` |
| 非 filter 基线漂移 | 抖动必须 `g_traceBounce.z` flag 门控；`traceCB_` 恒 80B（扩字段要按 §2.21 `dxc -dumpbin` 核 Size） |
| 新卡挡全量 | `cases.json` 追加卡后必跑 `python tools/regression.py --only s12-temporal/gpu-smoke` 冒烟（M11.2 卡漏 `id` 实锤） |
| bat 行尾门禁 | `python tools/check-bat-encoding.py --fix example/s12-temporal` 机械修 CRLF |
