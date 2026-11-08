#pragma once

// P8 M-C1：开发者控制台 ImGui 前端（A 类轨道）。
// ImGui 符号只允许出现在 renderer/ ——本文件与 hud.cpp 是仅有的两个 ImGui include 点。
// 纯逻辑（注册表 / 解析 / 执行 / 日志）在 core/console（M-C0），这里只做：
//   窗口布局、滚动日志（级别配色）、输入行（历史 ↑↓ = ImGui CallbackHistory）、
//   Tab 补全（前缀匹配 + 唯一候选直接补全）、GLFW->ImGuiKey 输入映射。
// 参考口径：Carrot console/renderToImGui 结构（但其 AutocompleteField 依赖 ImSearch，
// 不引第三方——补全按 P8-CONSOLE-PLAN 判据自己写）；UE UConsole 只读（打字流交互）。

#include "nl/renderer/renderer.hpp"

namespace nl::renderer::detail {

// Hud::build() 内（NewFrame 与 Render 之间）调用：consoleOpen 为真时画控制台窗口。
// 顺带消费 frame.keyEvents / textChars 喂 ImGuiIO（即使窗口未开也喂——输入状态
// 对 ImGui 是全局的），并写回 wantCaptureKeyboard / wantCaptureMouse 由 viewer 消费。
void drawConsoleWindow(HudFrame& frame);

} // namespace nl::renderer::detail
