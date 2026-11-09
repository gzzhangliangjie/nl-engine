# feat/console 轨道证据截图索引

> 台账（动作/提交版本/完成情况/踩坑）以 `docs/MILESTONES.md` P8 M-C* 节为准，本文件只做图目索引。
> 纪律修正：M-C0~C2 期间的证据图原本拍在 `build/out/`（gitignore 内，不可追），2026-11-09 补归档进本目录；后续证据一律直接落 `docs/screenshots/`。

## M-C1 控制台 ImGui 前端（commit b4787da + 修复包 a4cc4eb）

| 文件 | 内容 |
|---|---|
| `mc1-console-evidence-dx12.png` | DX12 控制台首拍：滚动日志配色 + 输入行 + HUD 同框 |
| `console-mc1-dbg.png` | Debug layer 开启态同上（验证期对照） |

（M-C1 修复包的 SendInput 自动化四连拍 `hand-1-tab`~`hand-4-grave` 当时只在 `build/out/`，未归档，已随截图管道脚本一并失效弃用；交互通路的最终证据以用户真手验 + 上述 DX12 首拍为准。）

## M-C2 运行时调参 + 内建命令集（commit 0cb89a3）

| 文件 | 内容 |
|---|---|
| `mc2-registry-dx12.png` | DX12：`help` 内建命令列表 + `cvar.list` 三条真参数 CVar（cam.dist/cam.fov/hud.show）+ 输入行 Tab 补全产物 `cam.fov \|`，HUD 面板同框 |
| `mc2-fov100-dx12.png` | DX12：`cam.fov 100` 生效（模型透视明显变广 vs 45°）+ `hud.show false` 只藏 HUD 面板、控制台窗口存活（panelShow 解耦直拍证据） |
| `mc2-registry-vulkan.png` | Vulkan 对称证据：HUD `backend : vulkan` 同框 `help`/`cvar.list` 完整输出 + Suzanne.glb 贴图模型正常（通道层验证，画面回归豁免） |

拍法：`viewer --console on --console-exec "help" --console-exec "cvar.list"`（Vulkan 图）+ SendInput 注入链（DX12 图）；截图 = PrintWindow + PW_RENDERFULLCONTENT（flip-model swapchain 下 BitBlt 会拿过期帧）。
