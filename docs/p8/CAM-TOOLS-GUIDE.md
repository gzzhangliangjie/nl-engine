# CAM-TOOLS-GUIDE — 玩法相机族无头验收工具（P8 M-CAM0，track camera）

> 归档口径照 `INPUT-TOOLS-GUIDE.md`：工具契约、锚点清单、证据管线、复现步骤。
> 工具源：`cam-tools/main.cpp`；相机实现：`core/include/nl/core/{camera_rig,chase_camera,free_camera}.hpp`。
> 本指南覆盖 M-CAM0（无头核心）；M-CAM3 轨迹相机（cam-track-trace/check）与 M-CAM4
> 平滑/防抖（F/G/H 锚）落地后扩节。

## 1. 工具定位

- **只链 nl_core**（纯 CPU 相机类），零图形 API / 零 GPU / 不碰 `kStage`——A 类并行轨道验收形态，与 input-tools 同款。
- 验收对象：CameraRig 基座（正交/透视双模式）+ ChaseCamera（阻尼跟随/boom/look-ahead/模式切换/倒车回头变体）+ FreeCamera（无阻尼飞行）。
- **确定性契约**：相机 update 只吃 dt 与输入位姿；固定 dt 重放逐位一致（memcmp 断言），跨 dt 是收敛性锚（阈值），两档在用例里显式分开。

## 2. 命令契约（退出码 0=PASS / 1=断言失败 / 2=用法错误）

| 命令 | 作用 | 输出 |
|---|---|---|
| `selftest [section]` | 机械锚；缺省跑全部 6 节；未知节 exit 2 | 逐条 `ok/FAIL` + `selftest PASS (6 sections)` |
| `cam-trace` | 150 帧跟车场景（直行 f0-89 → 转弯 f90 → orbit f120-139 → 切回 f140；f10 look kick）| per-frame TSV：frame/eye_xyz/boom_off_deg/pitch_deg/mode |
| `cam-check <tsv>` | 对 dump 的 TSV 重验 10 锚 | `cam-check PASS (150 rows, 10 anchors)` |
| `free-trace` | 60 帧自由飞行脚本（boost f10-29 / yaw+90° f30-49 / 俯冲 f50-59）| TSV：frame/eye_xyz/yaw_deg/pitch_deg |
| `free-check <tsv>` | 对 dump 的 TSV 重验 6 锚 | `free-check PASS (60 rows, 6 anchors)` |

**锚全部从实测 trace log 反推**（M-I0/I1 两连犯教训：禁手写锚）。f0 阻尼位姿
`(-0.450649, 2.164500, 5.520267)`、f10 kick `12.032114°`、稳态高度 `4.0562`
等数值均来自首次实跑 dump，check 只认 dump 本身。

## 3. selftest 六节锚点

| 节 | 锚 |
|---|---|
| `init` | rig 透视缺省 / 正交 ZO 签名（m[15]=1, m[10]<0）/ 双矩阵必异；chase clamp 族（distance 1-50 / height 0-10 / stiffness 0.1-50 / leadTime 0-1 / pitch ±83° / boomYawOffset ±180°）；free clamp（speed 0.1-500 / boost=4x）；双相机缺省透视；输出矩阵有限 |
| `chase-a` | 直线跟随稳态：位置滞后 ∈ [0.9,1.6]（离散阻尼模型理论 v·Δt/α≈1.25）、高度误差 <1e-3、look kick 12° 注册且 2.3s 内回正 <1.5°、look-ahead = pos+vel·lead（无 clamp）、eye 在车尾上方 |
| `chase-b` | 急转：转弯帧残差 >2× 跳升，f92-139 残差逐帧单调衰减（容差 1e-6），1s 内衰减到 <40% |
| `chase-c` | 模式切换位姿 memcmp 逐位不动；orbit 60 帧无回正；切回 Chase 恢复衰减；倒车/回头 XOR 稳态对照（各跑 120 帧沉降） |
| `chase-d` | **逐位档**：同 dt 150 帧重放 memcmp 一致；**收敛档**：直行 2.5s 跨 dt（1/60 vs 1/30）终点差 <0.2（理论 ≈0.05） |
| `free-e` | 60 帧脚本重放逐位一致；yaw=0 前飞沿 +Z；单帧 boost 位移恰 4×；俯冲 clamp -83°（不翻转）；yaw 90° 后前飞沿 +X；dt≤0 冻结 |

## 4. 证据管线（防手绘）

```
cam-tools cam-trace  → example/cam-m0/cam-trace.tsv   ┐
cam-tools free-trace → example/cam-m0/free-trace.tsv  ┴→ tools/make-cam-m0-evidence.py
                                                        → example/cam-m0/acceptance.png（确定性：固定画布/配色/无时间戳）
run.bat: 双后端 selftest → trace/check → PNG 重生成 fc /b 逐字节比对 → 负向退出码
```

四面板：chase eye 俯视轨迹（蓝=TSV 实测，灰=脚本参考）/ boomYawOffset 曲线（kick 尖峰 + orbit 冻结带）/ free eye 俯视轨迹 / free yaw·pitch 曲线。

## 5. 复现步骤

```
scripts\build-m0.2.bat DX12
scripts\build-m0.2.bat VULKAN
example\cam-m0\run.bat          rem ALL GREEN exit 0
build\DX12\cam-tools.exe selftest [section]
ctest -R mcam0                  rem mcam0-cam-selftest + mcam0-cam-usage-negative(WILL_FAIL)
```

## 6. cvar / viewer 接线预告

M-CAM0 全部参数走类 setter（`setDistance/setStiffness/...`）；console cvar
（`cam.chase.dist` / `cam.free.speed` / `cam.chase.stiffness` 等，照 `cam.fov`
onChanged 回写先例）在 M-CAM1/M-CAM2 viewer 接线时落。正交模式 viewer 用途
（俯视/工程调试视图）M-CAM1 起按需接。
