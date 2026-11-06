#pragma once

// M0.1：只有占位状态，不加载任何图形 API。
// M0.2 起：NVRHI device / swapchain 全部实现在 renderer/ 里，viewer 与 core 不出现 vk 符号。

namespace nl::renderer {

struct Status {
    const char* backend;   // "none" 直到 M0.2 接入 NVRHI
    const char* stage;
    bool        deviceReady;
};

Status status();

} // namespace nl::renderer
