#pragma once

// M0.1：只报告「编译期选定了哪个后端」，不加载、不链接任何图形 API。
// 后端由顶层 CMake 的 NL_GRAPHICS_BACKEND 决定，切换后端不需要装任何厂商 SDK。
// M0.2 起：NVRHI device / swapchain 全部实现在 renderer/ 里，
//          viewer 与 core 里不出现任何图形 API 符号。

namespace nl::renderer {

struct Status {
    const char* backend;   // "dx12" / "vulkan"，由编译期配置决定
    const char* stage;
    bool        deviceReady;
};

Status status();

} // namespace nl::renderer
