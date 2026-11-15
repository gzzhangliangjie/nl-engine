#pragma once

// M-UI0：RmlUi NVRHI 共享渲染后端——两个后端都只调用这一份代码（与 Hud 同构）。
// 本文件只依赖 NVRHI 公开类型；RmlUi 头只在 rmlui_backend.cpp 里 include
//（渲染接口符号面不许散到 renderer/ 之外——CMake 根注释架构纪律）。
// viewer / core 只通过 RmlUiFrame POD 与 nl::renderer::Renderer 的 rml* 转发方法
// 交换数据；nl_core（Bindings.cpp）经 nl::script::UiSink 抽象落点，依赖方向不变。
//
// 能力边界（M-UI0 拍板）：实现 RenderInterface 必选集（CompileGeometry/
// RenderTexture/Scissor）+ SystemInterface + FileInterface；可选集（filters/
// layers/shaders/clip mask）走上游默认 no-op——box-shadow/backdrop-filter 等特效
// 降级为不渲染，文档主体（布局/字体/图片/动画）不受影响。

#include "nl/renderer/renderer.hpp"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nl::renderer::detail {

// CPU 侧顶点展开（HudVertex 同款纪律：避免子集顶点格式在两后端的差异）。
// 必须与 rmlui.slang 的 VSInput 顺序一致：POSITION / TEXCOORD / COLOR。
// 颜色 = RmlUi 6.x 预乘 alpha（Vertex::colour 是 ColourbPremultiplied，RGBA 序），
// 混合模式因此与 Hud 不同：srcBlend = One（预乘标准），见 cpp 内 pipeline 描述。
struct RmlUiVertex {
    float x, y;
    float u, v;
    float r, g, b, a;
};
static_assert(sizeof(RmlUiVertex) == 32, "RmlUiVertex must be tightly packed");

// rmlui.slang 的 RmlConstants：float4 g_projection（16 B）+ float4 g_translation（16 B）。
// translation 逐 draw 变化（同一几何可在不同位置渲染），走 volatile CB 每 draw 写。
struct RmlUiConstants {
    float projection[4];
    float translation[2];
    float pad[2];
};
static_assert(sizeof(RmlUiConstants) == 32, "RmlUi cbuffer layout must match shader");

// rmlui.slang 的正交投影：屏幕像素 -> NDC（HudConstants::hudProjection 同公式复用）。
RmlUiConstants rmlProjection(float displayWidth, float displayHeight, float tx, float ty);

class RmlUiBackend {
public:
    // shaderSuffix = "dxil" / "spv"；vsEntry/psEntry 按后端传（SPIR-V 恒为 main）。
    // 成功 = Rml::Initialise + context + 字体 + demo 文档已就绪（构建失败写 outError）。
    bool init(nvrhi::IDevice* device, const char* shaderSuffix,
              const char* vsEntry, const char* psEntry, std::string& outError);

    // 句柄必须在 device 还活着时释放（两后端 shutdown 调用；Rml::Shutdown 收官）。
    void shutdown();

    // 用 framebuffer 的格式信息建 pipeline，resize 后重建（Hud 同纪律）。
    bool ensurePipeline(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                        std::string& outError);
    void invalidatePipeline();

    bool ready() const { return impl_ != nullptr; }

    // CPU 侧：输入喂数 -> 热重载检查 -> Update -> Render（RenderInterface 收集
    // draw items 并摊平顶点）。frame.enabled == false 时整段跳过（缺省零扰动）。
    void build(RmlUiFrame& frame);

    // GPU 侧：scene 之后、HUD（ImGui）之前叠加（ImGui 调试面板永远压顶）。
    void draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
              const nvrhi::Viewport& viewport);

    // ---- M-UI0 UiSink 落点实现（Renderer::rml* 转发进来）----
    int  loadDocument(const char* path);          // handle >0 / 0 = 失败
    bool closeDocument(int handle);
    bool showDocument(int handle, bool show);
    bool elementSetText(int handle, const char* elId, const char* text);
    bool elementGetText(int handle, const char* elId, const char** outText);  // 静态缓冲

private:
    struct Impl;                    // RmlUi/NVRHI 细节全部封进 cpp（头保持干净）
    Impl* impl_ = nullptr;
};

} // namespace nl::renderer::detail
