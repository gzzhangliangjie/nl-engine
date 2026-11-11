#pragma once
// embed_window.hpp — 引擎侧嵌入辅助（M-E2 框架；M-E1 仅暴露 API，待 viewer 接线）
// 职责：① 把引擎原生窗口的 HWND 经 RPC 上报编辑器（engine.announce）；
//       ② 收到编辑器父 HWND 后，用 Win32 SetParent 把引擎窗口挂入编辑器视口子窗口。
// 不依赖 glfw / 任何图形栈——HWND 由调用方（viewer）用 glfwGetWin32Window 取出后传入，
// 本文件只吃裸 void*，避免在公共头强引 windows.h。
#include "rpc_bridge.hpp"
#include <cstdint>

namespace nl::editor {

class EmbedWindow {
public:
    // 上报自身窗口给编辑器 host（caps 可带分辨率/后端等元信息）。
    static void announce(RpcBridge& bridge, void* engineHwnd, int pid, int proto,
                         const RpcValue& caps = RpcValue());

    // 把引擎窗口挂入编辑器父窗口（去标题栏/边框，铺满父窗口）。
    static void attach(void* engineHwnd, void* parentHwnd);
};

} // namespace nl::editor
