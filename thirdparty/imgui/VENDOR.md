# Dear ImGui — vendored 溯源与风险核查

> 本文件是 `AGENTS.md`「自研边界与第三方库选型闸」要求的溯源留档。升级版本时必须同步更新本文件。

| 项 | 值 |
|---|---|
| 库 | Dear ImGui — bloat-free graphical user interface library（立即模式） |
| 版本 | **v1.91.9 主线稳定版**（`imgui.h` → `IMGUI_VERSION "1.91.9"`；特意取主线稳定版，不用 WIP 分支——M0.x 踩坑①：Carrot 本机 1.92.7 WIP 字体系统已重构、跨代风险高） |
| 上游 | https://github.com/ocornut/imgui |
| 获取方式 | tag 归档经 gh-proxy 取（台账「imgui v1.91.9 vendored」节）；只收核心 4 个 cpp + 5 个头 + LICENSE.txt |
| 许可证 | **MIT**（`LICENSE.txt` 原文随库入库） |
| 形态 | 纯源码静态库（根 CMakeLists 编入），**不用官方 D3D12 / Vulkan backend** |
| 用途与消费点 | HUD 与编辑器 UI；渲染走自写 NVRHI 共享后端 `renderer/src/hud.cpp/.hpp`（防图形 API 散出 renderer/ 铁律）；`imgui.ini` 为运行时配置不入库 |
| 引入里程碑 | M0.x HUD 阶段（台账 246/304 行）；imgui_widgets 1.91 代 API 口径（IMGUI-GUIDE §2/§3） |

## 风险核查（选型闸逐条）

1. **许可兼容**：MIT 原文在库内，无风险面。
2. **无厂商锁定**：独立开源项目，零 EULA、不联网。
3. **零构建期新依赖**：纯源码，平台层（Win32 键鼠/剪贴板）手写进 hud.cpp，不用后端库。
4. **已知风险如实记录**：① 1.92 代字体系统重构，锁 1.91.9 稳定版避免跨代 API 断裂（升级需专项评估）；② 运行时 `imgui.ini` 状态污染确定性截图（T1 踩坑③：拍摄前 rm）。
5. **可替换性**：UI 语义经 HudFrame POD 双向通道与引擎解耦；换 UI 库改 hud.cpp + editor 宿主即可，业务层不 include imgui 头（editor 除外）。
