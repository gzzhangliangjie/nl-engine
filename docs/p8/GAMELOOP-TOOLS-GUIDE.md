# GAMELOOP-TOOLS-GUIDE — gl-tools 使用与验收归档（P8 游戏循环轨道）

> 轨道：A 类并行轨道 `feat/gameloop`（wt `nl-engine-gameloop`），计划 `docs/p8/P8-GAMELOOP-PLAN.md`。
> 工具 exe：`build/{DX12,VULKAN}/gl-tools.exe`（只链 nl_core，零图形 API）。
> 退出码契约：**0=PASS / 1=断言失败 / 2=usage error**（对齐 phys-tools / 展示台 expectExit）。

## 1. M-GL0 命令面

### selftest
```
gl-tools selftest [--verbose]
```
30 个机械锚点，全部手算可验（部分经 python 同参数预演核对）：

| 组 | 锚点 | 期望 |
|---|---|---|
| 非法输入 | 零 / 负 / NaN realDt | 0 tick、simTime 不动（防御性钳 0） |
| 单帧累积 | 16ms | <1/60 → 0 tick，余量进 accumulator |
| 单帧累积 | 16.7ms | 恰 1 tick（index 0、dt==kStep） |
| 长跑节奏 | 100×16.7ms | 恰 100 tick（floor(100.2)）；simTime==tickCount*kStep |
| maxDelta 截断 | 5000ms 单帧 | 钳到 250ms → **恰 15 tick**（IEEE 下 15×kStep==0.25 精确成立，python 预演） |
| 暂停 | paused 帧 | accumulator/simTime 逐位冻结、frameDt=0；恢复后 tick 索引连续、无追帧爆发 |
| 倍速 | ×2 / ×0.5 | 60×16.7ms → 120 / 30 tick |
| 追帧上限 | scale10×100ms | 缩放后 1s 被钳到 250ms → 15 tick（死亡螺旋防护） |
| scale 边界 | 负/NaN→0；1e9→1024；0=冻结 | 钳制语义 |
| 确定性 | 同 dt 序列双跑 | tick 索引序列与 simTime **逐位一致** |
| reset | 时间线归零 | simTime/accumulator/tickCount/frameDt 清零；timeScale/paused 保留 |

### trace
```
gl-tools trace [--out <tsv>] [--frames N] [--dt MS] [--dts a,b,c]
               [--scale S] [--pause-at N] [--resume-at N]
```
- `--frames N --dt MS`：常值 dt 跑 N 帧（默认 240×16.7ms）；`--dts` 给逐帧序列（与 `--frames` 互斥，同给 exit 2）。
- 输出 TSV 列：`frame  realDtMs  ticks  tickCount  simTime  alpha  paused`；stdout 摘要行 `gl-m0 trace: PASS (frames=… ticks=… simTime=… alpha=…)`。
- 用法锚点：usage 退出码 2；`--dt abc` / `--frames -1` / `--scale -1` / `--dts` 非法项 / `--resume-at <= --pause-at` 均 exit 2。

## 2. 独立复算（防假绿）

`example/gl0/verify.py` —— **独立 python 孪生实现**（同参数重写 60Hz/250ms/暂停契约，不引 C++ 代码）逐行对拍 trace TSV：
```
python example/gl0/verify.py example/gl0/trace-const.tsv --dt 16.7
python example/gl0/verify.py example/gl0/trace-burst.tsv --dts 5000,16.7,16.7,8,16.7,33.4,16.7,0,16.7 --scale 2 --pause-at 4 --resume-at 7
```
M-GL0 实测：240 行 + 9 行全等（ticks/tickCount 精确；simTime/alpha 1e-9）。C++ 与 python 双实现一致 = 契约写对了，不是实现自证。

## 3. 验收链（example/gl0/run.bat）

`selftest → trace 恒定节奏 → trace burst/pause/scale 混合 → verify.py ×2 → usage(2) → negative(2) → make-gl-evidence.py`，全绿打 `GL-M0 ACCEPTANCE: ALL GREEN`，exit 0。证据图 `acceptance.png` 由真实日志/TSV 生成（含 140 帧 tick 节奏条），禁手绘。

## 4. ctest

`gl0-selftest` / `gl0-trace` / `gl0-negative` 三条，DX12 + VULKAN 双后端构建树各跑一遍（M-GL0 实测 3/3 × 2）。

## 5. 消费约定（给 M-GL1/M-GL2）

- 渲染帧开头 `clock.tick(realDtMs, logicTick)`；渲染侧状态直接读 `simTime()` 等（起步不插值，`alpha()` 暂只作输出）。
- 逻辑帧 tick 顺序（PLAN §3）：input → script/action → physics → ECS transform sync → audio sync。
- 暂停语义 = `setPaused(true)` 后逻辑时钟全冻结；渲染帧照常 present/HUD/console。
- 确定性：GameClock 纯 double 同序运算；回放/对拍场景喂固定 realDt 序列即可逐位重现。
