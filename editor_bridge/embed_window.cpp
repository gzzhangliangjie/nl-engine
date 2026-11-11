// embed_window.cpp — 引擎侧嵌入辅助实现（Win32 SetParent + RPC announce）
#include "embed_window.hpp"
#include <windows.h>

namespace nl::editor {

void EmbedWindow::announce(RpcBridge& bridge, void* engineHwnd, int pid, int proto,
                           const RpcValue& caps, const std::string& viewId) {
    msgpack::Map a;
    a["hwnd"]  = msgpack::Value(int64_t(reinterpret_cast<uintptr_t>(engineHwnd)));
    a["pid"]   = msgpack::Value(int64_t(pid));
    a["proto"] = msgpack::Value(int64_t(proto));
    a["caps"]  = caps.isMap() ? caps : msgpack::Map{};
    a["viewId"] = msgpack::Value(viewId);   // M-E6 多视口路由键（单视口恒 "main"）
    bridge.notify("engine.announce", msgpack::Value(a));
}

void EmbedWindow::attach(void* engineHwnd, void* parentHwnd) {
    HWND child = reinterpret_cast<HWND>(engineHwnd);
    HWND parent = reinterpret_cast<HWND>(parentHwnd);
    ::SetParent(child, parent);
    // 去边框/标题栏，让引擎窗口铺满编辑器视口子窗口
    LONG style = ::GetWindowLongPtr(child, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME);
    ::SetWindowLongPtr(child, GWL_STYLE, style);
    // 触发一次样式重算（SWP_FRAMECHANGED），使去边框立即生效
    ::SetWindowPos(child, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

} // namespace nl::editor
