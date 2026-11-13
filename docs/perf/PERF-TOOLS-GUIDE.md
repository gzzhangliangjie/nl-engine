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

（待落码后补章。）
