# PERF-TOOLS-GUIDE — shader uniformity 门禁与 per-pass 计时

> M-PERF0 / M-PERF1 交代归档。拍板与设计权威落点 `docs/perf/PERF-PLAN.md`。

## M-PERF0：check-shader-uniformity（静态 uniformity 门禁）

### 是什么
对 `CMakeLists.txt` 定义的**每个 shader artifact**（2026-11-13 实测 59 个：file.entry）× **dxil + spirv 双 target**，以与 CMake shader 构建命令**严格同源**的参数跑 `slangc -validate-uniformity`。唯一数据源 = CMakeLists.txt（`_ENTRIES_*` 定义 + `if/elseif` 文件映射链 + `_SM66_ARTIFACTS` + `renderer/shaders/*.slang` GLOB），无手写清单——CMake 漂移时工具自动跟随。

### 门禁口径（Q-P3 拍板）
| 类别 | 判定 | 退出码 |
|---|---|---|
| slangc `error[...]`（编译失败） | 违规（无法验证 uniformity 即不过） | 1 |
| warning 行含 `uniform`（大小写无关） | uniformity 违规 | 1 |
| 其余 warning（E30081 隐式转换 / E39001 binding overlap 等） | informational，只报告不挡 | — |

干净基线实锤（2026-11-13）：59 artifacts × 2 targets 全量 **零 uniformity 违规、零编译错误**、304 条 informational，`--jobs 4` 约 13s。新增 uniformity 违规即 FAIL——防退化闸。

### 用法
```
python tools/check-shader-uniformity.py                 # 全量（exit 0=PASS / 1=违规）
python tools/check-shader-uniformity.py --list          # 只列 artifact 清单
python tools/check-shader-uniformity.py --only direct.csrefgi    # 单 artifact
python tools/check-shader-uniformity.py --target dxil   # dxil|spirv|both
python tools/check-shader-uniformity.py --jobs 8
python tools/check-shader-uniformity.py selftest        # 分类器+解析器自测
```
展示台/回归卡 exe = `tools/check-shader-uniformity.bat`（薄包装，python 从 PATH）。

### 与 CMake 的参数同源性
- dxil：`-dxc-path <SDK dxcompiler 目录> -target dxil -entry E -stage S` + SM66 白名单 artifact 加 `-profile sm_6_6`
- spirv：`-target spirv` + `-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-u-shift 384 0`
- 追加 `-validate-uniformity`（唯一差异）

### 验收（example/perf-m0/run.bat verify）
四段 ALL GREEN：selftest / 全量门禁（锚 `check-shader-uniformity: PASS` + `uniformity=0` + `compileErrors=0`）/ 负向 `--only bogus`（exit 2 + 锚）/ usage `--target bogus`（exit 2 + 文档头锚）。回归卡 `perf-m0/*` 8/8（regression 2026-11-13）。

### 已知边界
- 该 vendored slang 版本对「wave/梯度操作进发散分支」的经典模式**不告警**（实测构造样本零输出）——本闸当前为「防退化 + 接入上游未来分析增强」性质，不承诺检出所有 uniformity 问题。
- selftest 的 uniformity 违规分类是**关键词匹配**（不依赖具体警告码），slang 未来改警告文本不影响。

## M-PERF1：per-pass GPU 计时（--pass-timing，默认关）

### 是什么
`gpu_timer.hpp` 的 `PassTimers`（Q-P4）：13 个命名 pass 各一对 `begin/end`，每 pass 4-slot `ITimerQuery` 环——懒建 / poll 不阻塞 / 无空闲槽本帧跳过，语义与既有 `GpuTimer` 完全同源（Carrot `RenderGraph.h` per-pass timestamp pool 同构）。开关 `--pass-timing` 默认关（Q-P5）：开关走 `FrameParams.passTiming` 单向锁存，query 由 `Scene::pollPassTimers` 首次轮询时懒建——**关 = 零 query 零开销零日志**（OFF shot 0 行实锤，冻结基线 bytes 不动）。

### pass 清单（Q-P4 按实码对位，13 行）
| pass | 覆盖段 | 模式 |
|---|---|---|
| csCull | 剔除 dispatch + 回读拷贝（与 M4.2 `cullTimer_`「剔除+回读」合并口径并存、独立计时） | cull/dag/hiz |
| hizBuild | Hi-Z 深度预通道 + 金字塔逐级 + top（M4.2 合并段拆出的 hiz 部分） | hiz |
| visRaster | vis buffer 簇光栅 indirect draw | visbuf 系 |
| matResolve | material 反解全屏三角形 | visbuf pulling 系 |
| csDirect | 直光 compute dispatch | shadow |
| csRefGI | 参考图累积 dispatch（reference 与 reference-gi 共槽） | reference |
| csPlace / csTraceProbe | 探针布点 / 追踪 dispatch | probe 系 |
| csGather / csFilterTemporal / csFilterSpatial / csGatherCompose | gather→时域→空间→合成链 | filter/probe-gather |
| mainDraw | 非 visbuf 主绘制 indirect draw | cull/dag/hiz |

按实码与 Q-P4 原表的差异：原表 13 项（12 pass + 主绘制）全数落地；`--filter-gi-dump` 验收分支替换 csGatherCompose、不计时；visbuf pulling 路径无独立 mainDraw（matResolve 即主绘制）；dag-only（无 hiz 资产）hizBuild 不跑，行显示 `--`。

### 输出形态（Q-P5）
- **log 锚**：每 pass 首次 resolve 落 `[nl:info|pass-timing] <名> first resolve: <ms> ms`（ms 机器相关，不进锚匹配段）。`pollPassTimers` 在 backend beginFrame 无条件轮询（移出 HUD 块）——`--no-hud` 也出锚。
- **HUD 面板**：default-off，13 行 ASCII（`%-16s %8.3f ms`），本帧未跑/未 resolve 的 pass 显示 `--`；deterministic 截图模式 valid 照置、ms 不写真实值（cullMs 固定样本同款纪律，保 MD5 稳定）。
- **检查工具**：`tools/check-perf-m1.py`（纯标准库）——`anchors <log> <hiz|filter|shadow>`（preset→该模式全部 pass 锚 + deviceErrors=0）/ `off <log>`（0 pass-timing 行 + deviceErrors=0）/ `selftest`（5 断言）。

### 用法
```
viewer.exe --mode hiz --pass-timing --no-hud ...     # 日志锚（--no-hud 也出）
viewer.exe --mode filter --pass-timing --hud ...     # HUD 逐 pass 面板
python tools/check-perf-m1.py anchors <log> hiz      # 锚断言
python tools/check-perf-m1.py off <log>              # OFF 静默断言
python tools/check-perf-m1.py selftest
```

### 验收（example/perf-m1/run.bat verify）
双后端 8 段 ALL GREEN：checker selftest + hiz ON（csCull/hizBuild/mainDraw）+ filter ON（8 pass 全链）+ shadow ON（visRaster/csDirect）+ hiz OFF 静默。实测 ms（DX12）：csCull 0.302 / hizBuild 0.052 / mainDraw 0.015 / filter 链 0.005~0.062 / csDirect 0.040；csRefGI 1.612（reference 卡实证）。回归卡 `perf-m1/*` 6/6；保形 `--only s6.0`（含 --hud）与 `--only s12-spatial` 双后端 bytes 全绿。

### 已知边界
- csRefGI 无 run.bat 段（reference 模式帧耗时高），由 cases.json `--mode reference` 手工实证；preset 未含 reference 档。
- 各 pass 数值异步 1~2 帧延迟（与整帧计时同款）；无空闲槽本帧跳过（4 slot 下实测未见）。
