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

---

## M4.2：1.3M 致密资产离线对照 + cull pass GPU 计时（S4.2）

### 新资产：IcoSphereDense1M.nmesh（入仓）

```
> python scripts/gen-dense-mesh.py --shape icosphere --subdiv 8 --out assets/models/IcoSphereDense1M.glb
  vertices=655362 triangles=1310720 indices=3932160
  md5=ea0b6749d0a681c0610bd23882a072e1          (GLB 36,702,876 B)

> nanite-tools cluster assets/models/IcoSphereDense1M.glb -o assets/meshlets/IcoSphereDense1M.nmesh
  meshlets   : 19718            (.nmesh 33,548,020 B)
  tri/cluster: avg 66.5  max 102  (limit 128)
  coverage   : 1310720/1310720 = 100.00%
  limits     : PASS
```

S4-PLAN 预估 ~6554 簇，实测 19,718——meshoptimizer 按簇上限（128 tri/64 vert）
切得更细，剔除压力更足，如实记录。gen-dense-mesh.py 确定性（MD5 锁定），
GLB 可随时重生成；.nmesh 入仓保 cases.json 基线可复现。

### 两姿态离线值（宽高比进视锥，对照必须同尺寸）

| 姿态 | 尺寸 | 可见 | 剔除 | 备注 |
|---|---|---|---|---|
| 正对 yaw30/pitch20/dist2.6 | 640x360 | 19718 | 0 | 水平视锥更宽，纯 pass-through 位姿（pair 用例落这里） |
| 正对 yaw30/pitch20/dist2.6 | 640x720 | 18365 | 1353 | 剔 1353 簇，GPU 位图仍与 cluster 逐字节一致（球测试保守性实证） |
| 进模 yaw30/pitch20/dist0.45 | 640x720 | 1932 | 17786 | 剔除 90.2% > 85% 判据；GPU == 离线，双后端一致 |

```
> python tools/frustum_check.py assets/meshlets/IcoSphereDense1M.nmesh --yaw 30 --pitch 20 --dist 0.45 --width 640 --height 720 --expect 1932
  MATCH
```

### cull pass GPU 计时（M4.2 新增 slot）

第二个 GpuTimer 实例（`Scene::cullTimer_`），begin/end 只包 Scene::draw 里的
compute 剔除段（args 重写 + dispatch + 回读拷贝）。resolve 日志带标签区分：

```
[nl:info] gpu timer resolved [frame]: 1.01581 ms (first successful NVRHI timestamp query)
[nl:info] gpu timer resolved [cull]: 0.010656 ms (first successful NVRHI timestamp query)
```

实测（RTX 4060 Laptop，限时交互窗，960x540）：整帧 0.96~1.02ms（正对）/
0.17ms（进模）；cull pass 恒约 0.010ms——19,718 簇一次 dispatch 的开销在
0.01ms 量级，S5 换 DAG 遍历后此 slot 直接复用。

**踩坑**：cull 计时轮询必须在 `hud_.build()` 之前调用——viewer 每帧重建
`HudFrame{}`，晚于 build 写标志当帧即丢、永远显示不出来（整帧计时在 build
之前轮询所以没踩过）。两后端 `pollCullTimer` 与整帧计时同位同序。

### 60fps 验收口径

- CPU 侧：截图模式（无 16ms 节流 sleep）测渲染吞吐上限，30 帧
  `frame timing: avg 4.87~5.55ms`（新增性能日志，只进日志不进画面）；
  max 尖峰（21~48ms）为首个 swapchain 帧预热，不影响均值口径。
- GPU 侧：timer query resolve 日志（上节）。
- 判据 16.6ms：CPU/GPU 均余两个数量级。

### 回归挂接

- `example/cases.json` s4.2 卡（3 渲染用例）+ s4.2-tools 卡（3 工具用例，
  负向 = --expect 99999 exit 1）；
- `example/s4-done/run.bat` 一键复现：1.3M pass-through 字节比对（双后端）
  + HUD 统计行 grep + 内壁渲染 + 离线 --expect 四段验收；
- kStage bump S4.1→S4.2：m0.9/gate-hud、s2.0/lod-hud、s3.2/dag-hud 三对
  基线随 stage 文本重拍（预期维护）。
