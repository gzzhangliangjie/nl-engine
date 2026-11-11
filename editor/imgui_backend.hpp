#pragma once
// imgui_backend.hpp — 编辑器专用极简 ImGui 后端（GLFW + OpenGL 2.0 固定管线）
// 不依赖 NVRHI / DX12 / Vulkan：编辑器是独立轻量进程，复用引擎的 HUD 管线会拖入整套图形栈。
// Windows SDK 自带 GL/gl.h + opengl32.lib，零新增 vendored 依赖、零联网。
#include <GLFW/glfw3.h>

namespace nl::editor::ui {

// 初始化：创建字体纹理、绑定裁剪板回调。window 必须已创建 GL 上下文且 current。
bool imguiInit(GLFWwindow* window);
void imguiShutdown();

// 每帧：轮询 glfw 输入 + ImGui::NewFrame()
void imguiNewFrame(GLFWwindow* window);

// 渲染当前 ImGui::GetDrawData()（正交投影 + 顶点/UV/色彩 client state）
void imguiRenderDrawData();

} // namespace nl::editor::ui
