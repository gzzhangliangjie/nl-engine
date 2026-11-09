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

## viewer 运行时控制（M-GL1 起）

| 入口 | 语义 |
|---|---|
| `game.pause [on\|off]` | 逻辑时钟停摆/恢复；无参=翻转。暂停=bevy 语义：simTime/accumulator 全冻结，渲染帧照常（HUD 活、相机活） |
| `game.timeScale <f>` | 逻辑时间缩放 [0,1024]（NaN/负→0 冻结，越界钳制）；无参=查询 |
| `game.status` | 打印 `simTime/ticks/paused/scale/alpha`（截图日志锚） |
| `--fixed-dt <ms>` | 钉渲染帧 delta（喂时钟+帧统计同源）；0=关。暂停/倍速截图对拍必开 |
| `--anim-time <s>` | （M-A5 原有）直接钉 skinTime，绕过时钟——既有基线兼容路径，优先级高于时钟 |

- 双帧消费现状：skin 模式动画时间 = simTime（量化到 tick，起步不插值）；物理叠加层与 ECS scene tick 仍逐渲染帧步进，M-GL2 统一迁入。
- 消费约定：`--console-exec "game.pause"` 在主循环前生效（命令注册先于预执行行）；截图终帧日志行 `game clock: simTime=... ticks=... paused=... scale=...` 与 PNG 同帧（口径见 PLAN §6 M-GL1 留痕）。
- 验收复现：`example/gl1/run.bat all`（8 张 PNG + check.py 10 项 MD5 交叉对拍，exit 0 全绿）。

## 场景模式双帧 + M-S3 音频集成（M-GL2 起）

| 入口 | 语义 |
|---|---|
| `audio.play <wav> <entity>` | （scene 模式）createSource + 绑定 ECS 实体 + looping 播放；错误走 out-param 返回（createSource 不写 `error()`） |
| `game.pause` | 逻辑时钟停摆 **且音频 mix 冻结**（复用 audio.freeze / DebugController，M-GL2 拍板 B）；恢复时解冻。runScene 亦注册（--console-exec 可用） |
| `--fixed-dt <ms>` | runScene 同样生效（clockDtMs 钉点在 `++frame` 前）；迁移用例统一 `16.7`（floor(N×16.7/16.667)=N 保 tick 数） |

- **双帧消费全景**（M-GL2 起）：逻辑帧 60Hz = ECS world.tick（serial/parallel）+ Jolt step（--physics-debug）+ 音频同步（声源位置 ← 实体 `getWorldPosition`）；渲染帧 = present / 相机 / HUD / listener（随相机，漫游时暂停听筒仍活）。
- **gl2-audio.scene 设计**：orb-a/orb-b 是自转实体 spin-a/spin-b 的**轨道子实体**（E1a 层级）——SpinSystem 只改 rotation（原地自转），直接挂 spin 实体世界位置不变，证不了「随物体移动」；子实体偏移随父旋转形成轨道（半径 1.6/1.4 不变量 = verify.py 几何断言）。
- **AudioWorld 生命周期**（scene 模式）：realtime 优先，设备初始化失败降级 offline（位置读回同步不依赖设备，无扬声器环境可跑全套验收）；listener 位置渲染帧同步，声源位置逻辑帧同步。
- **验收复现**：`example/gl2/run.bat`（动/停两组 × 双后端 + verify.py 孪生全绿，exit 0）。

## 渲染帧插值（M-GL3 起）

- **开关**：CLI `--interp`（初值）+ console `game.interp`（无参翻转 / `on|off`）。
- **口径**：`alpha = GameClock::alpha()`（= accumulator/kStep，与 bevy `Time<Fixed>::overstep()/timestep()` 同构）。每渲染帧 tick 前后快照 prev/curr **世界 TRS**（列主序矩阵分解：三列长度 = scale、归一化列 = 旋转、Shepperd 法矩阵→四元数），交互态渲染 `mat4TRS(lerp(pos), slerpQuat(rot), lerp(scale))`（`nl::math::slerpQuat` 最短路径 + 近平行 nlerp 退化）。
- **纪律（拍板）**：插值**渲染专用、绝不回写 sim state**——组件态只在逻辑帧内变化（社区 crate bevy_transform_interpolation 的公开纪律；bevy core 全树无内建 transform 插值器，v0.16.1 实读 0 命中）。Carrot/UE 不插值。
- **截图 = 确定性产物**：截图模式恒走不插值原路径（`getWorldMatrix()`），PNG 与 `--interp` 缺省运行**逐字节同**。首版曾试「截图钉 alpha=0 走分解重构」——分解→重构 roundtrip 非逐位（normalize/sqrt 引入 1ulp 级差异），PNG MD5 不等，否决（留痕 viewer/main.cpp M-GL3 注释）。
- **数值证据**：捕获帧 probe 日志（`interp probe: k=... alpha=... prev/curr/interp pq/cq/iq`）——verify.py 独立孪生复算三重：① lerp/slerp 自洽；② GameClock accumulator 复推（`--fixed-dt 25` → 每帧 tick [1,2,1]、捕获帧 alpha=0.5）；③ SpinSystem 公式孪生（`w_i = 0.9×(1+0.5·idx)` rad/s，idx = Transform 载体实体注册序）算轨道世界位置。
- **验收复现**：`example/gl3/run.bat`（equal/mid/pause/smoke 四组 × 双后端，exit 0）。
