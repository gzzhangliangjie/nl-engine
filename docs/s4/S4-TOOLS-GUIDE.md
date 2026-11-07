# frustum_check.py 使用说明与结果归档（M4.1 交代）

> **交代载体**：展示台网页 `tools/showcase/start.bat` → http://127.0.0.1:8787/ ——
> s4.1-tools 版本卡上有 `kind:"tool"` 用例现场实跑；本文是同一批输出的文本归档。
> 渲染侧证据 = `example/s4-cull/` 截图基线（s4.1 版本卡）。

## 定位

M4.1 GPU 视锥剔除的**离线参考实现**：与 `renderer/shaders/cull.slang` 的 csCull
逐语义对照——同一相机公式、同一 5 平面包围球测试、同一份 `.nmesh` 簇包围球。
GPU 回读的 `instanceCount` 与本工具算出的可见数相等，即剔除语义正确。

纯 CPU、零第三方依赖（标准库 only），不链图形后端。

## 相机公式复刻口径

`frustum_check.py` 逐行复刻 `renderer/src/camera.cpp`：

- eye：yaw/pitch 球坐标（弧度转换、pitch 夹紧）+ lookAt(原点)；
- 投影：`perspectiveZO(fovY, aspect, near, far)`，**ZO 深度**（0..1）；
- mvp = proj × view，列主序存储；
- 平面提取：Gribb-Hartmann——Slang/HLSL 里 `M[i]` 即第 i 行，取
  `row3±row0`（左右）、`row3±row1`（上下）、`row2`（近平面）共 5 面
  （远平面在 ZO 下恒可省，与 shader 一致）；
- 球测试：平面归一化后 `dot(c,n)+d < -r` 判外部。

**注意宽高比进视锥**：`--width/--height` 必须与 GPU 跑的 `--size` 一致，
否则 aspect 不同、可见数不同（640x360 下 Suzanne 进模 dist0.45 是 64，
640x720 下是 30——竖构图水平视锥更窄，不是 bug）。

## 用法

```
python tools/frustum_check.py <mesh.nmesh> [--yaw D] [--pitch D] [--dist F]
                              [--width W] [--height H] [--expect N] [--list]
```

| 参数 | 默认 | 说明 |
|---|---|---|
| `--yaw/--pitch/--dist` | 12 / 9.2 / 4.2 | 与 viewer 默认机位一致 |
| `--width/--height` | 640 / 360 | 决定 aspect，须与 GPU `--size` 同步 |
| `--expect N` | 无 | 可见数不符 → exit 1（回归锁数字） |
| `--list` | 无 | 打印可见簇号列表（GPU visibleList 同一 tid 空间） |

退出码：`0` 成功（含 `--expect` 命中）、`1` 文件不存在 / expect 不符。

## M4.1 交代（tag `s4-cull`）

### 真剔除姿态：相机进模型内部（真值 30）

```
> python tools/frustum_check.py assets/meshlets/Suzanne.nmesh --yaw 12 --pitch 9.2 --dist 0.45 --width 640 --height 720 --expect 30
frustum_check: assets/meshlets/Suzanne.nmesh
  pose      : yaw=12.0 pitch=9.2 dist=0.45 size=640x720
  clusters  : 237
  visible   : 30
  culled    : 207
  expect    : 30 -> MATCH
```

GPU 对照（双后端同机位，日志与 HUD frustum 行同数）：

```
> build\dx12\viewer.exe --mode cull --frames 4 --yaw 12 --pitch 9.2 --dist 0.45 --size 640x720
[nl:info] cull stats: visible 30 / 237 clusters (frustum on)
> build\vulkan\viewer.exe --mode cull --frames 4 --yaw 12 --pitch 9.2 --dist 0.45 --size 640x720
[nl:info] cull stats: visible 30 / 237 clusters (frustum on)
```

### 正脸姿态：全可见 237（默认 640x360）

```
> python tools/frustum_check.py assets/meshlets/Suzanne.nmesh --yaw 12 --pitch 9.2 --dist 4.2
  visible   : 237
  culled    : 0
```

GPU：`cull stats: visible 237 / 237 clusters (frustum on)`（双后端一致）。

### 一次重要的如实修正：S4-PLAN「背对≈0」前提不成立

轨道相机永远 look at 原点，背对时**整个模型仍在视锥内**（dist 4.2 时视锥
半高 ≈1.74 > 模型半径 ≈1.3），三姿态里的「背对」剔除数依然是 237/237。
真剔除维度是**近平面**：把相机推进模型内部（dist 0.45），外围簇的包围球
被近平面切掉，才出现 237→30 的真剔除。MILESTONES 与 S4-PLAN 已同步修正。

### 回归挂接

- `example/cases.json` s4.1-tools 卡：3 个 `kind:"tool"` 用例
  （offline-inside / offline-front / negative-missing），经
  `tools/frustum_check.bat` 包装启动（regression 无 shell 启动 .py 需可执行
  shim，退出码直通）；
- `example/s4-cull/run.bat` 一键复现：pass-through 字节比对 + GPU 统计行
  grep + 离线 --expect 三段验收。
