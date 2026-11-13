# WEATHER-TOOLS-GUIDE（M-WEA0 工具版归档）

> 归属：WEA 天气轨道 `weather` / wt `nl-engine-weather` / branch `feat/weather`。
> 计划书：`docs/WEATHER-PLAN.md`（方案 A 参数层，Q-WE1~WE6 2026-11-12 拍板；M-WEA0 拍板 Q-WEA0-A/B 2026-11-13）。
> 本文归档 M-WEA0（天气状态层）的工具面与确定性契约。

## 1. 交付物

| 交付物 | 落点 | 说明 |
|---|---|---|
| 状态层模块 | `core/include/nl/core/weather.hpp` + `core/src/weather.cpp` | 纯 CPU 零图形符号，与 `game_clock` 同级同纪律（nl_core） |
| 工具子命令 | `nanite-tools weather`（`nanite-tools/main.cpp`） | TSV dump / `--verify` / preset 锁定 |
| 机械单测 | `nanite-tools selftest weather` + ctest `wea0-*` ×4 | 双后端注册（`nanite-tools/CMakeLists.txt`） |
| 孪生对拍 | `example/weather-w0/verify.py` | 独立 python 实现，防假绿（gl0 先例） |
| 证据图 | `tools/make-weather-evidence.py` → `example/weather-w0/acceptance.png` | 曲线全部来自真实 TSV |
| 展示台卡 | `example/cases.json` id `weather-w0`（5 用例） | usage / selftest / dump / verify / negative |

## 2. 模型契约（渲染侧 M-WEA1 将消费的参数面）

**离散态**：`clear` / `partly` / `overcast` / `rain`（`WeatherKind`）。状态机 = seed 驱动的纪元推进：纪元长 120s（`kEpochLength`），每纪元按持续性概率 0.55（`kPersistProb`）维持上一态，否则从其余三态由 seed 选（splitmix64，纯整数）。

**过渡**：跨纪元边界共 20s 窗口（`kBlendWindow`，前半=上一纪元尾、后半=下一纪元头），smoothstep 因子单调 0→1 走完一次参数混合；边界瞬时恰 0.5。参数轮廓（`WeatherProfile`）：

| 态 | rainIntensity | cloudCoverage | turbidity | fogDensity(1/m) | fogHeight(m) |
|---|---|---|---|---|---|
| clear | 0 | 0.05 | 2.0 | 0.0005 | 30 |
| partly | 0 | 0.45 | 3.0 | 0.001 | 40 |
| overcast | 0 | 0.85 | 4.5 | 0.002 | 50 |
| rain | 1 | 1.0 | 6.0 | 0.004 | 60 |

**派生量**：`cloudTransmittance = 1 − 0.85·coverage`（bevy 透射率口径）；`sunIntensity = daylight·transmittance`；色温 2200~6500K 随太阳仰角；天穹基色 = M10.1 `skyGradient` 常量（zenith 0.16,0.30,0.58 / horizon 0.62,0.71,0.84）经夜因子、地平线暖移、浊度去饱和、云遮灰化调制。日长 600s（`kDayLength`），midnight 起步。

**wetness（记忆状态）**：`dw/dt = rain·0.08 − (1−rain)·0.02·w`，固定步长 1s 前向欧拉（`kWetStep`，M-N6 教训：累加同源），采样时步内线性插值。雨强 1 时约 12.5s 从 0 到 1；雨停后一阶干燥。bevy_weather `GroundConditions` 口径。

## 3. 工具用法

```
nanite-tools weather --seed N [--duration SEC] [--step SEC]
                     [--preset clear|partly|overcast|rain] [--tsv FILE] [--verify]
退出码：0=PASS 1=FAIL 2=usage error
```

- TSV 21 列：`t kind blend sunAzimuth sunElevation sunIntensity sunColorTempK zenithR/G/B horizonR/G/B turbidity sunDisk cloudCoverage cloudTransmittance fogDensity fogHeight groundWetness rainIntensity`。
- `--preset X` 锁定该态（时间线全部同态），即 cvar/CLI「强制下雨」手调语义；M-WEA1 起接 console cvar。
- `--verify`：同 seed 双建时间轴 + `evaluateWeather` 无状态路径三方逐字段对拍，任一分歧 exit 1。

## 4. 确定性契约

- 纯 double + splitmix64 整数哈希；无隐藏状态、无全局量、无运行期可变常量（`weather_detail` 命名空间常量冻结，改动 = 破坏已回放时间轴）。
- 同 seed + 同时间轴 → TSV 逐字节一致（C++ `--verify` 机械对拍）。
- 独立 python 孪生（`verify.py`，不引 C++ 代码）重算全行：901 行最大偏差 4.65e-04（libm sin ulp 噪声，容差吸收 printf 舍入），跨实现防假绿。
- CPU 侧保持 bytes（Q11-1 口径）；GPU 数值进断言才走统计口径（M-WEA1 起）。

## 5. 验收记录（2026-11-13）

- `selftest weather` 10 锚点 PASS（确定性 / preset 湿度动力学 / 范围扫描 / 0.25s 平滑连续性 / 负向钳制）。
- ctest `wea0-selftest` / `wea0-verify` / `wea0-dump` / `wea0-negative` DX12+VULKAN 双后端 4/4 PASS。
- `example/weather-w0/run.bat` ALL GREEN exit 0（selftest / 双 TSV dump / 双 verify / 孪生对拍 / usage / negative / 证据图）。
- 证据图 `acceptance.png`：日周期 + 天气纪元过渡 + 湿度饱和曲线 + 4 日志锚全 OK，全部源自真实 TSV。

## 6. 已知坑（本轮实锤，后续里程碑沿用）

1. **时间轴端点语义**：`sample(t=duration)` 与 `evaluateWeather(t)` 曾分歧（端点 clamp vs 内部定位两种表示）——修正 = 边界瞬时归一（u==0 且 e>0 → 上一纪元 u=1）+ kinds 多折一纪元供尾过渡。任何「分段 + 窗口混合」模型都要回答端点表示唯一性问题。
2. **双窗过渡设计**：纪元尾/纪元头各混一次同一条过渡会在边界跳回——必须头尾拼成一条单调 smoothstep（尾 0→0.5、头 0.5→1）。
3. **阶梯网格采样**：wetness 1s 网格直接查表在细步长采样下呈阶梯跳变——采样侧步内线性插值（仍是确定性纯函数）。
4. **nmake 同秒 mtime 跳重链**：改源码后 `Built target` 却是旧 exe（本轨两次踩中：ctest 假 FAIL / selftest unknown suite）——删 exe（必要时连 `nl_core.lib`）强制重链。
5. **Bash cwd 不跨命令**：跨命令漂回主树，相对路径 `rm build/...` 误删主树产物——一律同命令内显式 `cd <wt> &&`，主树误删产物事后重跑主树构建恢复。
