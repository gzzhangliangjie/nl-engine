# anim-tools 指南（M-A 轨道工具版交代）

> A 类并行轨道（`PARALLEL-TRACKS.md` §2.4 / `docs/action-system/ACTION-PLAN.md`）。
> 不进 `kStage`、不进渲染回归总闸；交代 = 展示台 tool 卡实跑 + 真实输出生成的证据图 + 本文归档。

## 用法

```
anim-tools selftest curve|blend|statemachine|blendspace   # 确定性单测
anim-tools sample                                          # 确定性演示输出（证据图数据源）
```

退出码：`0` 成功 / `1` 断言失败 / `2` 用法错误。工具只链 `nl_core`（零图形 API），构建：

```
scripts\build-anim.bat anim-tools    # vcvars + NMake（沙箱 reg 黑名单下的 INCLUDE/LIB 手工补丁，见 M-A1 踩坑）
```

## selftest 覆盖（全部确定性数值断言，两次运行逐位一致）

| 用例 | 覆盖 |
|---|---|
| `curve`（M-A1） | lerp/slerp 数值锚点（45°/22.5° 四元数）、最短路径翻转不变、近平行 nlerp 退化、Step 保持前 key、常量/空轨道、wrapTime 负回绕、validateClip 六负向 |
| `blend`（M-A2） | lerpPose 锚点 w=0/0.5/1、blendClips 跨 clip 混合、加法 quat 相对叠加（0/45/90/180°）、骨架不一致拒绝 |
| `statemachine`（M-A3） | 10 步 loco 场景逐值断言、回退 z 连续 + UE 缩时、validate 五负向、双跑一致 |
| `blendspace`（M-A4） | 段权重锚点、域外 clamp、单 sample 退化、validate 五负向、参数域扫掠 |

## sample 输出与证据图

`anim-tools sample` 输出两段确定性数值表（ASCII）：

1. **blend space 扫掠**：param 0→2 步长 0.25（t=0.5）——`param z (seg i0->i1 w=…)`；
2. **状态机 10 步场景**：`step speed state alpha z`（dt=0.1，speed 输入驱动 idle→walk→run→回退→idle）。

`python tools/anim_evidence.py` 消费该输出画 `example/ma-track/evidence.png`（960×540，纯标准库
zlib+struct 写 PNG，5×7 位图字体，不手绘示意）；上面板 = blend space 分段线性 z(param)，
下面板 = 状态机 z(step)（红）+ transition alpha（绿点）+ 状态色带。**两次生成逐字节一致。**

## 复现

```
example\ma-track\run.bat    # 四自测 + 负向(exit 2 校验) + sample + 证据图再生成，ALL GREEN 才 exit 0
```

ctest（构建目录内）：`ma1-anim-curve-selftest` / `ma2-anim-blend-selftest` /
`ma3-anim-statemachine-selftest` / `ma4-anim-blendspace-selftest` / `ma1-anim-unknown-case`（WILL_FAIL 负向）。

## 踩坑存档（详版见 git log 与当日记忆）

1. 沙箱 reg.exe 黑名单打断 vcvars 注册表探测 → rc/mt/LIB 缺失；`build-anim.bat` 手工补 SDK bin + INCLUDE/LIB。
2. validateClip 曾把空通道判非法 → 仅 rotation 轨道的骨骼被拒 → sampleClip 空 poses → 自测 `poses[0]` 段错误（逐用例进度输出定位）。
3. 自测期望值手算错误三连（漏算 crossfade 期间 fromTime 推进、sin(11.25°) 常数笔误、断言时机在场景结束后）——实现全对、期望全错，逐值核算修正。
4. `blendPoses` 输入输出同容器混叠（先 clear 毁输入）——evaluate 改走临时缓冲。
5. `t=1.0` 撞 wrapTime 循环归零语义（t=duration → 0）——测试时点避开端点。
6. NMake 时间戳陷阱复发：obj 在 `CMakeFiles/<target>/<target>/` 子目录，删错层不重编（M2.3a 坑 3 同款）。
