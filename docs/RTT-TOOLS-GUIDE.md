# RTT 后视镜工具指南（M-RT0 / S8.3）

> 配套 `docs/RTT-PLAN.md`。本文件记录 `--mirror` 的运行口径、验收命令与已知边界。

## 1. 开关与命令

`--mirror` 缺省关。开时仅 `--mode scene`（ECS 城市场景）生效——Scene 在
sceneMode_ 主 pass 之后，用后向相机（现役相机 yaw+180）把 `sceneBatches_`
再画一遍到半分辨率 RT，present 阶段把 RT blit 到屏幕右下角（1/4 宽 × 1/4 高，
2% 边距）。

```
viewer.exe --mode scene --scene <city.scene.engine> --mirror \
    --screenshot <out.png> --frames 8 --no-hud --size 960x540 --yaw 40 --pitch 11 --dist 12
```

首帧 log 锚：`mirror pass ready: WxH batches=N`（`infoCh("mirror", ...)`，只打一次）。
旗标关时该 log 不出现、mirror RT 整组不建、既有截图逐字节不变。

## 2. 实现形态（落码勘误）

| Q | 拍板默认 | 落码实做 | 差异 |
|---|---|---|---|
| Q1 后向相机 | 独立 yaw+180 | 临时 `OrbitCamera` yaw+180，fov/aspect/dist 同源 | 一致 |
| Q2 写入 pass | 复用主 draw 换相机+RT | `Scene::drawMirror` 复用 `sceneBatches_` + `pipelineSceneBack_` | 一致 |
| Q3 RT 格式 | 半分辨率 RGBA8 | **半分辨率 RGBA32F**（scene PSO 锁 RGBA32F+D32，换格式要整套 PSO/set 重建；半分辨率仍省 3/4 像素） | 勘误 |
| Q4 显示 | quad blit 角落 | `rt.psmirror` + 右下 1/4 viewport + `vsRtPresent` | 一致 |
| Q5 kStage | bump | `S8.3`；旗标关零影响，重拍面按 `reshot-hud-stage.py` 实测 | 一致 |

## 3. 状态纪律（踩坑预防）

- `mirrorTex_` 不设 permanent，逐帧 `beginTrackingTextureState(RenderTarget→SRV)`，
  与 `hdrSceneColor_` 同款（§2.15/§2.19）。创建态 SRV + keepInitialState 与帧首声明对齐。
- 深度用独立 `mirrorDepth_`（D32，自带 clear），不碰后端永驻 `depth_`。
- `constants_`（volatile 160B）在主 pass 后写 mirrorMvp 第二版——同一 cmd list
  内两版（maxVersions=16），主 pass 已完成不再读，无冲突。
- 所有 mirror 句柄进 `Scene::shutdown()`/`shutdownMirror()` 清单（S15 UAF 铁律：
  新 GPU 资源漏登会拖到 device 销毁后析构 → Vulkan 退出段错误）。
- resize 时 mirror RT/fb 懒重建；`mirrorPresentSet_` 引用旧 tex 句柄，重建后首次
  present 检测到 createBindingSet 失败会自动重建 set（兜底路径）。

## 4. 验收

- 双后端 BUILD OK；selftest 16/16；ctest 双后端全绿。
- `example/s8.3/run.bat verify` 全绿；双后端 PNG 基线各自落盘、逐字节可复现。
- `regression --only s8.3` 双后端 ALL GREEN；`showcase --check` exit 0。
- 旗标关闭回归：旧 `--mode scene` 用例 bytes 逐字节不变（新旧 exe 对拍）。

## 5. 边界 / 后置

- 平面镜（无凸面曲率畸变）；曲率 UV 变形留后续里程碑。
- 只服务 sceneMode_（ECS 城市）；normal/cluster/cull/rt 等模式不接。
- 脚本绑定面（exports）不暴露 mirror，游戏层要调另立里程碑。
- 单 RT；bevy `ViewTarget` 双缓冲形态未做。
