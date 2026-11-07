# nl-engine 全量回归报告（M0.9 总闸）

- 时间：2026-11-07 12:01:26　用时 39 s
- 结果：**ALL GREEN** — 58/58 通过（bytes 58 / near 0 / FAIL 0）
- 判据：PNG 与基线 MD5 相同 = `bytes`；像素差 ≤1/255 且 ≤2 像素 = `near`（GPU 取整噪声档）；selftest 校验日志；deviceErrors=0。

## 明细

| 版本 | 用例 | 后端 | 判定 | 说明 |
|---|---|---|---|---|
| m0.1 | selftest | dx12 | bytes | selftest 通过（SELFTEST PASS） |
| m0.1 | selftest | vulkan | bytes | selftest 通过（SELFTEST PASS） |
| m0.2 | clear | dx12 | bytes | 与基线逐字节相同 |
| m0.2 | clear | vulkan | bytes | 与基线逐字节相同 |
| m0.2 | resize | dx12 | bytes | 与基线逐字节相同 |
| m0.2 | resize | vulkan | bytes | 与基线逐字节相同 |
| m0.2 | clear-color | dx12 | bytes | 与基线逐字节相同 |
| m0.2 | clear-color | vulkan | bytes | 与基线逐字节相同 |
| m0.3 | triangle | dx12 | bytes | 与基线逐字节相同 |
| m0.3 | triangle | vulkan | bytes | 与基线逐字节相同 |
| m0.3 | selftest | dx12 | bytes | selftest 通过（SELFTEST PASS） |
| m0.3 | selftest | vulkan | bytes | selftest 通过（SELFTEST PASS） |
| m0.4 | suzanne | dx12 | bytes | 与基线逐字节相同 |
| m0.4 | suzanne | vulkan | bytes | 与基线逐字节相同 |
| m0.4 | inner | dx12 | bytes | 与基线逐字节相同 |
| m0.4 | inner | vulkan | bytes | 与基线逐字节相同 |
| m0.4 | inner-cullback | dx12 | bytes | 与基线逐字节相同 |
| m0.4 | inner-cullback | vulkan | bytes | 与基线逐字节相同 |
| m0.4 | resize | dx12 | bytes | 与基线逐字节相同 |
| m0.4 | resize | vulkan | bytes | 与基线逐字节相同 |
| m0.4 | selftest | dx12 | bytes | selftest 通过（SELFTEST PASS） |
| m0.4 | selftest | vulkan | bytes | selftest 通过（SELFTEST PASS） |
| m0.5 | hud | dx12 | bytes | 与基线逐字节相同 |
| m0.5 | hud | vulkan | bytes | 与基线逐字节相同 |
| m0.5 | controls | dx12 | bytes | 与基线逐字节相同 |
| m0.5 | controls | vulkan | bytes | 与基线逐字节相同 |
| m0.5 | nohud | dx12 | bytes | 与基线逐字节相同 |
| m0.5 | nohud | vulkan | bytes | 与基线逐字节相同 |
| m0.5 | resize | dx12 | bytes | 与基线逐字节相同 |
| m0.5 | resize | vulkan | bytes | 与基线逐字节相同 |
| m0.5 | selftest | dx12 | bytes | selftest 通过（SELFTEST PASS (14/14)） |
| m0.5 | selftest | vulkan | bytes | selftest 通过（SELFTEST PASS (14/14)） |
| m0.6 | hud | dx12 | bytes | 与基线逐字节相同 |
| m0.6 | hud | vulkan | bytes | 与基线逐字节相同 |
| m0.6 | nohud | dx12 | bytes | 与基线逐字节相同 |
| m0.6 | nohud | vulkan | bytes | 与基线逐字节相同 |
| m0.6 | selftest | dx12 | bytes | selftest 通过（SELFTEST PASS (14/14)） |
| m0.6 | selftest | vulkan | bytes | selftest 通过（SELFTEST PASS (14/14)） |
| m0.7 | hud | dx12 | bytes | 与基线逐字节相同 |
| m0.7 | hud | vulkan | bytes | 与基线逐字节相同 |
| m0.7 | icosphere | dx12 | bytes | 与基线逐字节相同 |
| m0.7 | icosphere | vulkan | bytes | 与基线逐字节相同 |
| m0.7 | selftest | dx12 | bytes | selftest 通过（SELFTEST PASS (14/14)） |
| m0.7 | selftest | vulkan | bytes | selftest 通过（SELFTEST PASS (14/14)） |
| m0.8 | hud | dx12 | bytes | 与基线逐字节相同 |
| m0.8 | hud | vulkan | bytes | 与基线逐字节相同 |
| m0.8 | wire | dx12 | bytes | 与基线逐字节相同 |
| m0.8 | wire | vulkan | bytes | 与基线逐字节相同 |
| m0.8 | uv | dx12 | bytes | 与基线逐字节相同 |
| m0.8 | uv | vulkan | bytes | 与基线逐字节相同 |
| m0.8 | tris | dx12 | bytes | 与基线逐字节相同 |
| m0.8 | tris | vulkan | bytes | 与基线逐字节相同 |
| m0.8 | selftest | dx12 | bytes | selftest 通过（SELFTEST PASS (15/15)） |
| m0.8 | selftest | vulkan | bytes | selftest 通过（SELFTEST PASS (15/15)） |
| m0.9 | gate-hud | dx12 | bytes | 与基线逐字节相同 |
| m0.9 | gate-hud | vulkan | bytes | 与基线逐字节相同 |
| m0.9 | selftest | dx12 | bytes | selftest 通过（SELFTEST PASS (15/15)） |
| m0.9 | selftest | vulkan | bytes | selftest 通过（SELFTEST PASS (15/15)） |

## 双后端对照（信息档）

- m0.2/clear: 逐字节相同
- m0.3/triangle: 逐字节相同
- m0.4/suzanne: 逐字节相同
- m0.4/inner: 不同（像素差 2，maxDelta 1/255，HUD 文本差异属预期）
- m0.4/inner-cullback: 逐字节相同
- m0.4/resize: 不同（像素差 1，maxDelta 1/255，HUD 文本差异属预期）
- m0.5/hud: 不同（像素差 895，maxDelta 240/255，HUD 文本差异属预期）
- m0.5/controls: 不同（像素差 895，maxDelta 238/255，HUD 文本差异属预期）
- m0.5/nohud: 逐字节相同
- m0.5/resize: 不同（像素差 896，maxDelta 240/255，HUD 文本差异属预期）
- m0.6/hud: 不同（像素差 895，maxDelta 240/255，HUD 文本差异属预期）
- m0.6/nohud: 逐字节相同
- m0.7/hud: 不同（像素差 895，maxDelta 240/255，HUD 文本差异属预期）
- m0.7/icosphere: 逐字节相同
- m0.8/hud: 不同（像素差 895，maxDelta 240/255，HUD 文本差异属预期）
- m0.8/wire: 不同（像素差 14096，maxDelta 242/255，HUD 文本差异属预期）
- m0.8/uv: 逐字节相同
- m0.8/tris: 逐字节相同
- m0.9/gate-hud: 不同（像素差 4215，maxDelta 254/255，HUD 文本差异属预期）
