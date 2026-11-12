# S12 工具与验收指南

> 权威计划：`docs/s12/S12-PLAN.md`（§0 拍板 / §0.6 任务书）；槽位表：`docs/SHADER-CONVENTIONS.md` §2.19；台账：`docs/MILESTONES.md`。
> 本文只讲「怎么跑、为什么这么验收」，不重复计划里的架构决策。适用里程碑：M12.0（tag `s12-temporal`）§1~§7、M12.1（tag `s12-spatial`）§8。

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
| `--spatial` | **M12.1 5×5 空间联合双边**（挂在时间层之后） | **关** | 默认关是保形硬要求：关层时 pass 仍 dispatch 但 shader 内 `g_flags.z` 门控退化成 pass-through，M12.0 的 105 张卡与 `run.bat verify` 尺子逐字沿用、零重拍 |
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
| `regression.py` 报「exe 过期门禁」 | 只重编 `.slang` 产物**没有重链 exe** → `viewer.exe` mtime < shader mtime。必须删 `build/{DX12,VULKAN}/viewer.exe` 后再 `build-m0.2.bat` 重链，见 §8.6 |
| 空间层 ON 的 MAE 比 OFF 还大 | 联合双边是**有偏估计**（邻域平均换方差下降），不是亮度漂移；门限按实测标定，见 §8.4 |

## 8. M12.1 空间联合双边（tag `s12-spatial`，Q12-C = A）

### 8.1 串联顺序与数据通路

```
csGather → csFilterTemporal(M12.0) → csFilterSpatial(M12.1) → csGatherCompose(一字不动)
                 │ 双写                       │ SRV 5x5 邻域读
          history[write] + gatherLinear_   history[write] → 写回 gatherLinear_ 自身 texel
```

单次串联，**不做时间↔空间交替迭代**（三处实读判定「交替迭代」无参考佐证，见 `S12-PLAN.md §0.3/§0.4 第 8 条`）。零新增 RT / CB / GBuffer：b0 复用 `filterCB_`、b1 复用 `gatherCB_` 前 80B（`invViewProj` + `eye`）、t1 复用 `probeDepth_`。每线程只写自己 `tid.xy`、读的是**另一张纹理**，无跨线程 hazard（M12.0 时间层能原地读写是因为它不读邻域）。

### 8.2 三个画面（注意 OFF 语义与 M12.0 不同）

| 画面 | 命令 | 含义 |
|---|---|---|
| REF | `--mode probe --filter-gi-dump` | 无抖动、确定性直出 GI |
| **OFF** | `--mode filter --filter-gi-dump` | **时间层收敛后**的结果（等价 M12.0 的 ON），空间层默认关 |
| ON | `--mode filter --spatial --filter-gi-dump` | 时间层 + 5×5 空间联合双边 |

⚠️ M12.0 的 OFF 是 `--temporal-off`（单帧噪声）；**M12.1 的 OFF 是收敛后的时间累积结果**。两把尺子不要混用。三图一律 `--dist 4 --frames 16 --size 960x540 --yaw 12 --pitch 9`（§2 原因不变）。

复现：

```bat
build\DX12\viewer.exe --mode filter --spatial --filter-gi-dump ^
  --model Suzanne-materials.nmesh --no-hud --size 960x540 --yaw 12 --pitch 9 ^
  --dist 4 --frames 16 --screenshot on.png

python tools\check-s12-spatial.py onoffref ref.png off.png on.png 0.90 1.15 1000
example\s12-spatial\run.bat verify
```

一键验收 `example\s12-spatial\run.bat verify`，退出 0 = `[ALL GREEN] s12-spatial verify`（双后端 × 三图 + 负向控制 + 工具 selftest）。`HFR=0.90` / `MAER=1.15` / `MINDIFF=1000` 是脚本顶部常量。

### 8.3 `tools/check-s12-spatial.py`

纯标准库（`zlib` + `struct` 解 8-bit 非交错 PNG），**不依赖 PIL**，与 `check-s12-temporal.py` 同口径。

| 子命令 / 断言 | 判据 |
|---|---|
| `onoff <off> <on> [hfRatio] [minDiff]` | `ON != OFF`（`|diff|>2` 像素 ≥ 1000）+ `hf(ON) ≤ 0.90·hf(OFF)` + `edgeGradRatio ≥ 0.30` |
| `onoffref <ref> <off> <on> [hfRatio] [maeRatio] [minDiff]` | 上述全部 + `MAE(ON,ref) ≤ 1.15·MAE(OFF,ref)` |
| `selftest` | 合成棋盘噪声 / 平图 / 糊平图的正向与三类负向自检，不读文件 |

`edgeGradRatio` = OFF 高梯度子集（固定 cut 4.0）上 `mean(grad ON) / mean(grad OFF)`，是**硬断言**，对应 `S12-PLAN` M12.1 自检「边缘不被糊穿」。GPU 数值图按 Q11-1 拍板 A 走统计不变量，禁 `bytes` 断言。

### 8.4 为什么 MAE 门限是 1.15 而不是继续收紧

首轮按 M12.0 的直觉预置 `MAER=1.05`，双后端实测 ratio 1.120 / 1.121 → FAIL。追因链：

1. **分区诊断**：与 REF 的误差在边缘区 `off 8.995 → on 11.209`（ratio 1.246）、平坦区 `2.323 → 2.541`（1.094）；ON 的**均值反而更接近 REF** → 不是亮度漂移/绑定错误，是邻域平均把边缘拉平。
2. **shader 正确性加固**：① 中心法线重建失败（物体/天空边界）时原来放行 `wN=1`，改为 `deviceZ` 一致性代理；② 去掉 Carrot `depthDivisor` 按 `gradZ` 放大分母的写法，改固定 `sigmaZ = 0.002f`（那份实现里 `weightDepth` 本就被 TODO 注释掉，照抄会让边缘处权重反而更宽松）。重编后数值几乎不变（`hf` 0.475、MAE 3.012）。
3. **临时深度可视化**实锤：`probeDepth_` 的 `deviceZ` 在 8-bit 下只有 **184~186 三档** —— 单凸体 + 纯天空场景几乎没有真实几何边缘，法线/深度权重退化，联合双边在本场景近似高斯。debug 分支已还原、临时 PNG / `.bak` 已清理、双后端从最终源码重编并重链。
4. **结论**：联合双边是**有偏估计**（方差↓换偏差↑），MAE 小幅上升属预期；按 M12.0「门限按实测标定」先例把 `MAER` 定 **1.15**，同时把 `edgeGradRatio ≥ 0.30` 从观察项升为硬断言，杜绝「把画面糊平来骗降噪指标」。

⚠️ 本场景**无真实几何边缘**，法线/深度权重的保边能力需在 M12.2 / 多物体场景再充分验证（挂后续项）。

### 8.5 实测数据（2026-11-12，最终源码重编 + 重链后）

| 指标 | DX12 OFF | DX12 ON | VULKAN OFF | VULKAN ON | 门限 |
|---|---|---|---|---|---|
| 高频能量 `hf` | 0.677 | **0.475** | 0.677 | **0.475** | ratio ≤ 0.90（实测 **0.702 / 0.702**） |
| `diffPixels(ON,OFF)` | — | **20666** | — | **20656** | ≥ 1000 |
| `edgeGradRatio` | — | **0.493** | — | **0.493** | ≥ 0.30（edgePx 5606 / 5597） |
| 与 REF 的 MAE | 2.687 | **3.012** | 2.686 | **3.012** | ratio ≤ 1.15（实测 **1.121 / 1.121**） |

**保形（`bytes` 级实证）**：冻结的 `ref-dx12/vulkan.png` 与 M12.0 的 `example/s12-temporal/ref-*.png` **逐字节相同**，`spatial-off-dx12/vulkan.png` 与 M12.0 的 `temporal-on-*.png` **逐字节相同**（4/4 MD5 一致）；`example/s12-temporal/run.bat verify` 复跑 ALL GREEN 且数值与冻结记录逐项相同（`hf` ratio 0.556 / 0.557、MAE 0.688、diffPixels 135409 / 135413）→ 空间层默认关不是「统计等价」，而是输出完全等价 M12.0。两后端 `spatial-on-*.png` MD5 **不同**（Q11-1：GPU 数值非 bit-exact），但统计不变量一致。

### 8.6 收口前必做

- shader 产物三处登记逐字同口径：CMake `_ENTRIES_FILTER_SPATIAL` + `renderer.cpp` `kArtifacts` + `scene.cpp` `loadShader("filter_spatial.csfilterspatial")`；selftest 报 `[108/108 ok]`。
- **删 exe 强制重链**：只改 `.slang` 时 `cmake --build` 只重编字节码、不重链 `viewer.exe`，`regression.py` 的 exe 过期门禁会直接拒绝出报告（实测报「落后于 1 个源文件（最新 renderer\shaders\filter_spatial.slang）」）。删两棵 `build/{DX12,VULKAN}/viewer.exe` 再 `build-m0.2.bat` 即可。
- `run.bat` 保持 ASCII + CRLF：`python tools/check-bat-encoding.py --fix example/s12-spatial`。
- 冻结证据图 `ref-*.png` / `spatial-on-*.png` 只作展示；断言用**同次重拍的 REF/OFF/ON**（Q11-1 禁逐字节依赖）。
## 8.7 全量总闸与 regression 工具可观测性修复（2026-11-13 收口）

**首轮全量静默挂死 25 分钟**：stdout 零新增、回归进程无子进程、报告未生成，是用户先问「还没跑完么」才被发现。根因不在用例多（812 条本来 5~10 分钟），而在 `tools/regression.py` 的 `run_viewer`：`subprocess.run(capture_output=True, timeout=120)` 下，`material-graph-tools` 的 check / compilepar 会派生系统 shell 再拉起 `slangc.exe` 孙进程并继承 stdout 管道写端；CPython 超时只 kill 直接子进程，随后那次**无 timeout** 的 `communicate()` 等 EOF → 孙进程不退就永久阻塞。4 个 worker 线程全被占住 → 整个线程池停摆（Python 线程不能强杀）。

**修复（工具层，用例一个不删、纪律不动）**：
- `run_viewer` 改 `Popen` + `tempfile.TemporaryFile` 重定向日志 + `stdin=DEVNULL` + `wait(timeout)`；读日志不再依赖管道 EOF，超时 60s 内真 kill；超时 marker 文本保持不变（「超时+零输出」自动重试判据依赖它）。
- `RUN_TIMEOUT` 120→**60s**：依据 M12.0 全量 810 条实测 p50 141ms / p99 7.6s / max 15.8s / >30s 0 条，约 4 倍余量。
- 可观测性三件套：每条完成 `flush=True` 打印；增量落盘 `<report>.progress.jsonl`（整体卡死也能从尾部定位停在哪条）；每 50 条打 `[regression][progress] N/812 elapsed=..s` 心跳；超时单独打 `[STALL]` 告警。

**验证（按 `AGENTS.md §4`：改 regression 工具本身禁止起全量，只用小子集）**：① Python 模拟孙进程 sleep 30s → 主用例 2.49s 返回且 `CHILD_DONE` 日志完整；② 挂起命令 → 2.01s 被 kill、返回 `timeout after 2s`；③ 3 条真实用例 `s12-spatial/gpu-smoke` / `m-j5a-mg/check` / `m-sc3/selftest` 全绿、增量 JSONL 正常落盘。随后 tag 收口唯一一次全量：**812 条 / 286 s / 810 通过**，零 `[STALL]`、零超时记录。

**收口实测**：810/812（bytes 809 / near 1 / FAIL 2）。2 个 FAIL = `s11-hash/cacheviz` 双后端既有基线漂移（M11.2 删 `wcMakeKey` 法线分量，71981 px / maxDelta 255），M12.0 收口同款；M12.0 那 4 个 `m-sc0/m-sc1` dx12 环境间歇族本轮全绿 → 本轮零新增回归，按用户拍板不动冻结基线、照常收口。报告：`build/DX12/s12-spatial-full-regression.md` / `.json` / `.progress.jsonl`。

⚠️ **已知余量风险**：本轮最慢 `m-j5a-mg/compilepar` vulkan **61634 ms** 略超 60s 阈值但 verdict=bytes 通过、无 timeout marker → 阈值偏紧，后续建议调到 90s。另：**多进程分片 worker 隔离**（主进程可单独 kill 某个 worker 进程树、其余片继续）挂独立后续项单独拍板，它只缩小爆炸半径，不替代 60s 超时 + `[STALL]` + 增量 JSONL + 短检查点主动报进度这套发现机制。

## 9. M12.2 历史拒绝 + D-B 时域参考（tag `s12-done`，Q12-B 续拍 B1 / Q12-D D-B）

### 9.1 新增开关与工具

| 开关 / 工具 | 作用 | 默认 |
|---|---|---|
| `--filter-reject` | 打开历史拒绝层（三层独立开关的第三层） | **关**（保形：M12.0/M12.1 数值逐字沿用） |
| `--reject-threshold <f>` | 深度/切平面阈值，写入 `g_flags.w`（0 = 层关）；CPU clamp 0.0001..1 | 0.01 |
| HUD filter 面板 | 时间 checkbox + `temporal alpha`（滤波强度）+ `history max frames`（历史长度）+ 空间/拒绝 checkbox + `reject threshold` | `filterPanelShow` 仍默认关，既有 `--hud` 卡零布局变化 |
| `tools/check-s12-done.py` | 纯标准库（无 PIL）子命令 `avg` / `refcheck` / `ghost` / `selftest` | — |
| `example/s12-done/run.bat verify` | 双后端一键：14 个 shot + 参考图 + 静态断言 + 负向控制 + 运动鬼影 + selftest | — |

### 9.2 D-B 参考图形态与硬约束

`--mode filter --temporal-off --filter-gi-dump --frames 32 --screenshot-seq <dir>` 出 32 帧**单帧 GI 直出**（抖动仍开，时间层关 → 每帧是独立噪声样本），`check-s12-done.py avg` 逐像素 RGB 平均 → **S12 自立、与被测对象同采样结构**（同 `csGather` 权重偏置）的收敛参考图。抖动帧号 `s12Frame_ % 64` → **N ≤ 64 封顶**，工具对 N>64 直接拒绝并在 selftest 里验证；本轮取 32（收敛质量与序列成本折中）。**运动序列绝不平均**（会糊），改以「同帧无历史单帧」逐帧序列作鬼影基准。`--screenshot-seq` 与 `--filter-gi-dump` 组合已先跑 3 帧冒烟（`f00001..f00003.png` 全落盘、exit 0）才纳入验收。

⚠️ 序列命令**必须同时给 `--screenshot <dummy>`**：`o.screenshot` 控制「N 帧后退出 + 确定性回放 + 关 HUD」，只给 `--screenshot-seq` 窗口循环不会按 frames 退出。序列产物放 `%TEMP%\nl-s12-done`（避免上百张 PNG 入仓），只有双后端参考图 `ref-avg-*.png`、静态图与 log 落 `example/s12-done/`。

### 9.3 拒绝层判据（B1 零落盘）

`filter_temporal.slang` 内 `rejectHistory()` 五条：① 越界（M12.0 已有）；② 重投影 `deviceZ` 差 > thr；③ 当前帧法线的一阶代理 —— 右/下邻域 `deviceZ` 梯度外推 `prevUv` 处局部切平面深度，残差 > 2·thr（bevy `pixel_dissimilar` 的零 previous 属性退化版）；④ 3×3 **深度**邻域 min/max clamp（bevy TAA 邻域 clamp 的零资源版：读只读深度纹理，避开 M12.1 实锤的 RWTexture 原地读邻域跨线程 hazard）；⑤ 帧数置信度 —— 拒绝时 `len=0`，下一帧靠 `alpha=max(userAlpha,1/(len+1))` 自动短历史。

**零新增 previous normal / previous_gbuffer / RT / CB / 纹理**：`filterLayout_` 仅增 t2 = 已有 `probeDepth_` SRV，`filterSpatialLayout_` 一字不动；`g_flags.w` 复用为「开关 + 阈值」→ FilterConstants 仍 **32B**（`dxc -dumpbin` 实锤 `Size: 32`、反射 `g_depth_0 → T2`），64B 过量分配不变（SSC §2.21 / M11.0 CB 越界铁律）。静态 `velocity=0` → `prevUv==uv`，②③④ 残差全 0 → 拒绝层**结构性不触发**，ON/OFF 逐值相同（与 §0.8.2「本场景无真实几何边缘、拒绝层测不出量化收益」一致）。

### 9.4 验收断言与门限标定（2026-11-13 双后端实测，**不猜**）

| 断言 | DX12 | VULKAN | 门限 |
|---|---|---|---|
| `refcheck` MAE(OFF,D-B) / MAE(ON,D-B) | 0.779 / **0.779** | 0.780 / **0.780** | `MAER=1.05`、`MAXMAE=1.00`（按 0.779 留 28% 余量） |
| `refcheck` hf(OFF) / hf(ON) | 0.677 / 0.677 | 0.677 / 0.677 | `HFR=1.05` |
| 静态 diffPixels(ON,OFF) | 0 | 0 | 打印不断言（静态不触发是预期） |
| `ghost` meanMAE(ON,single) / meanMAE(OFF,single) | 3.456 / 4.086 | 3.456 / 4.086 | ratio **0.846** ≤ `GHOSTR=0.95` |
| 层响应 meanDiff(ON,OFF) → meanDiff(REJALL,OFF) | 6000.4 → 20012.4 | 6000.4 → 20011.1 | 后者必须更大（阈值极小 = 拒绝更多历史） |
| 负向控制：单帧噪声图冒充 ON | 被拒（MAE 3.175、hf 1.216，三条断言全触发） | 被拒 | 非零退出 |
| 工具 `selftest` | OK | OK | avg / refcheck / ghost 正负路径全覆盖 |

门限全部按实测标定（M12.1 先例：`MAER=1.05` 猜成 1.05 曾 FAIL，实测 1.121 后改 1.15）：`GHOSTR=0.95` 相对 0.846 留约 12% 余量容纳跨后端方差。`refcheck` 负向控制用参考序列的 `f00016.png`（与 16 帧静态图同抖动相位）冒充 ON，在标定门限下三条断言全部触发。

### 9.5 ⚠️ 鬼影证据的口径（不许回填成「已有主观鬼影实测」）

M12.0/M12.1 两个 `run.bat verify` 均无 `--orbit-speed`、台账与日志无 S12 鬼影数值；S11 的 1.5°/帧×64 帧是 cell 涌现感，不是时域鬼影。本轮补的是**运动跟手性代理指标**：`--orbit-speed 1.5` 16 帧序列里，`ON`（拒绝层开）相对「同帧无历史单帧」基准的 MAE 比 `OFF` 低（ratio 0.846），说明历史污染更少、拖影更轻；**这不是主观鬼影判据**。本场景单凸体+纯天空、`probeDepth_` 仅 184~186 三档、无真实几何边缘 → 主观鬼影与拒绝层保边能力仍需多物体场景复评（挂后续项）。双后端这组数值几乎相同（GI dump 与每帧 rebuild 的 AS 内部布局无关），但按 Q11-1 仍走统计不变量、禁 `bytes`。

### 9.6 收口状态（2026-11-13 实际执行，逐项已过）

| 纪律项 | 执行结果 |
|---|---|
| 删两棵 `viewer.exe` 再 `build-m0.2.bat` 强制重链 | 🟢 双后端 BUILD OK + selftest 16/16 + shader 108/108（无新增 shader 文件 → CMake 登记一字不动）；`dxc -dumpbin` 实锤 cbuffer `Size: 32`、`g_depth_0 → T2` |
| `run.bat` ASCII + CRLF | 🟢 `python tools/check-bat-encoding.py --fix example/s12-done` → 0 violations；命令用 `cmd //c` 展开写避免 `%%B` 被 POSIX shell 破坏 |
| `cases.json` 追加卡后跑新卡冒烟 | 🟢 python json 库 indent=1 无损 roundtrip（LF + 尾换行），107 张卡；`regression.py --only s12-done --jobs 1` → 2/2 bytes 全绿 |
| 保形 | 🟢 `s12-temporal/run.bat verify` + `s12-spatial/run.bat verify` 数值逐项与冻结记录相同；3 条 `--only` 双后端 bytes（`s10-gather/gather` 逐字节相同）；`server.py --check` 退出 0（stage `S8.2` 未回归） |
| 全量总闸（tag 收口唯一一次） | 🟢 **810/814**，299 s，零 `[STALL]` / 零超时。4 FAIL = 2 个 `m-sc0` dx12 环境间歇族（重跑 5/5 全绿）+ 2 个 `s11-hash/cacheviz` 既有基线漂移（M12.0/M12.1 同款）→ **零新增回归**。报告 `build/DX12/s12-done-full-regression.md` |
| 证据图肉眼核验 | 🟢 `ref-avg-dx12.png`（32 帧平均，明显更平滑）、`static-on-dx12.png`（猴头轮廓与天空分离清楚），均无全黑/全白/糊平伪影 |
| 文档固化 | 🟢 `S12-PLAN.md` / 本指南 §9 / `SHADER-CONVENTIONS.md §2.19`（t2 槽位勘误）/ `UE-DIFFS.md D20` / `ROADMAP.md` / `MILESTONES.md` 四栏 / `.workbuddy/memory/{MEMORY.md,2026-11-13.md}` |

⚠️ 保留判据口径：本轮运动证据是**跟手性代理**，不是主观鬼影判据；多物体场景复评仍挂后续项（见 §9.5 / `UE-DIFFS.md D20` 触发条件）。
