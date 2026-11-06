#include "hud.hpp"
#include "backend.hpp"       // readFileBytes（两后端共用的字节码读取）

#include "nl/core/log.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace nl::renderer::detail {
namespace {

// 一个进程只跑一个 viewer / 一个后端，故 ImGui context 用进程级单例。
ImGuiContext* g_context = nullptr;

float channel(ImU32 col, int shift) {
    return static_cast<float>((col >> shift) & 0xFFu) / 255.0f;
}

} // namespace

HudConstants hudProjection(float displayWidth, float displayHeight,
                           float displayPosX, float displayPosY) {
    HudConstants out{};
    if (displayWidth <= 0.0f || displayHeight <= 0.0f) {
        out.projection[0] = out.projection[1] = 0.0f;
        out.projection[2] = -1.0f;
        out.projection[3] = 1.0f;
        return out;
    }
    // ImGui 的 y 向下、NDC 的 y 向上，故 Y 尺度必须取负（官方后端的正交投影同理）：
    //   ndc.x = (x - L) * 2/W - 1,  ndc.y = -(y - T) * 2/H + 1
    // 写成 y * scale + offset，就是下面这四项。少一个负号整个面板会上下颠倒。
    const float sx = 2.0f / displayWidth;
    const float sy = -2.0f / displayHeight;
    out.projection[0] = sx;
    out.projection[1] = sy;
    out.projection[2] = -1.0f - displayPosX * sx;
    out.projection[3] = 1.0f - displayPosY * sy;
    return out;
}

bool Hud::init(nvrhi::IDevice* device, const char* shaderSuffix,
               const char* vsEntry, const char* psEntry, std::string& outError) {
    device_ = device;

    auto shaderPath = [shaderSuffix](const char* stage) {
        return std::string(NL_SHADER_DIR) + "/hud." + stage + "." + shaderSuffix;
    };
    std::vector<std::uint8_t> vsBlob, psBlob;
    if (!readFileBytes(shaderPath("vs").c_str(), vsBlob) ||
        !readFileBytes(shaderPath("ps").c_str(), psBlob)) {
        outError = std::string("failed to load hud shader (") + shaderSuffix +
                   ") from " NL_SHADER_DIR;
        return false;
    }
    nvrhi::ShaderDesc vsDesc;
    vsDesc.setShaderType(nvrhi::ShaderType::Vertex).setEntryName(vsEntry);
    nvrhi::ShaderDesc psDesc;
    psDesc.setShaderType(nvrhi::ShaderType::Pixel).setEntryName(psEntry);
    vs_ = device->createShader(vsDesc, vsBlob.data(), vsBlob.size());
    ps_ = device->createShader(psDesc, psBlob.data(), psBlob.size());
    if (!vs_ || !ps_) { outError = "createShader failed for hud shaders"; return false; }

    // ---- ImGui context + 字体 atlas ----
    IMGUI_CHECKVERSION();
    if (!g_context) g_context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // 不写 imgui.ini：截图回归要确定性，也不污染工作区
    io.LogFilename = nullptr;
    io.Fonts->SetTexID((ImTextureID)(intptr_t)1);  // 单纹理，TexID 只作占位，不参与绑定

    unsigned char* pixels = nullptr;
    int fontW = 0, fontH = 0, bpp = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fontW, &fontH, &bpp);
    if (!pixels || fontW <= 0 || fontH <= 0 || bpp != 4) {
        outError = "ImGui font atlas is empty";
        return false;
    }

    nvrhi::TextureDesc fontDesc;
    fontDesc.width = static_cast<std::uint32_t>(fontW);
    fontDesc.height = static_cast<std::uint32_t>(fontH);
    fontDesc.format = nvrhi::Format::RGBA8_UNORM;
    fontDesc.debugName = "ImGuiFontAtlas";
    fontDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    fontDesc.keepInitialState = true;
    fontTexture_ = device->createTexture(fontDesc);
    if (!fontTexture_) { outError = "createTexture failed for ImGui font atlas"; return false; }

    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
    sampler_ = device->createSampler(samplerDesc);
    if (!sampler_) { outError = "createSampler failed for HUD"; return false; }

    // hud.slang 的 HudConstants（volatile CB，每帧写一次投影）。
    nvrhi::BufferDesc cbDesc;
    cbDesc.byteSize = sizeof(HudConstants);
    cbDesc.isConstantBuffer = true;
    cbDesc.isVolatile = true;
    cbDesc.maxVersions = 16;
    cbDesc.debugName = "HudConstants";
    constants_ = device->createBuffer(cbDesc);
    if (!constants_) { outError = "createBuffer failed for HUD constants"; return false; }

    // 槽位与 hud.slang 的 register(b0/t0/s0) 一致；Vulkan 侧由 slangc 的
    // -fvk-b-shift 256 / -fvk-s-shift 128 平移到 NVRHI 默认 binding。
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.setVisibility(nvrhi::ShaderType::All)
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0))
        .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0));
    bindingLayout_ = device->createBindingLayout(layoutDesc);
    if (!bindingLayout_) { outError = "createBindingLayout failed for HUD"; return false; }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, fontTexture_))
           .addItem(nvrhi::BindingSetItem::Sampler(0, sampler_))
           .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_));
    bindingSet_ = device->createBindingSet(setDesc, bindingLayout_);
    if (!bindingSet_) { outError = "createBindingSet failed for HUD"; return false; }

    nvrhi::VertexAttributeDesc attrs[3] = {
        nvrhi::VertexAttributeDesc().setName("POSITION")
            .setFormat(nvrhi::Format::RG32_FLOAT).setOffset(0)
            .setBufferIndex(0).setElementStride(sizeof(HudVertex)),
        nvrhi::VertexAttributeDesc().setName("TEXCOORD")
            .setFormat(nvrhi::Format::RG32_FLOAT).setOffset(8)
            .setBufferIndex(0).setElementStride(sizeof(HudVertex)),
        nvrhi::VertexAttributeDesc().setName("COLOR")
            .setFormat(nvrhi::Format::RGBA32_FLOAT).setOffset(16)
            .setBufferIndex(0).setElementStride(sizeof(HudVertex)),
    };
    inputLayout_ = device->createInputLayout(attrs, 3, vs_);
    if (!inputLayout_) { outError = "createInputLayout failed for HUD"; return false; }

    // 字体 atlas 一次性上传（staging + 自动 barrier，与 Scene 的贴图上传同路）。
    nvrhi::CommandListHandle upload = device->createCommandList();
    upload->open();
    upload->writeTexture(fontTexture_, 0, 0, pixels,
                         static_cast<std::size_t>(fontW) * 4u);
    upload->close();
    device->executeCommandList(upload);

    nl::core::log::info("HUD ready: ImGui ", ImGui::GetVersion(),
                        " | font atlas ", fontW, "x", fontH,
                        " | shader=", shaderSuffix);
    return true;
}

bool Hud::ensureCapacity(std::size_t vertexCount, std::size_t indexCount,
                         std::string& outError) {
    if (vertexCapacity_ >= vertexCount && indexCapacity_ >= indexCount) return true;
    auto grow = [](std::size_t need, std::size_t old) {
        return std::max(need, std::max<std::size_t>(4096, old * 2));
    };
    vertexCapacity_ = grow(vertexCount, vertexCapacity_);
    indexCapacity_  = grow(indexCount, indexCapacity_);

    // keepInitialState：NVRHI 在每条 command list 开头自动从 CopyDest 开始跟踪，
    // 结束时再转回 CopyDest。否则下一帧的 writeBuffer 不知道上一帧留下的
    // VertexBuffer 状态，validation 会报 "Unknown prior state of buffer" 并连累设备。
    nvrhi::BufferDesc vbDesc;
    vbDesc.byteSize = vertexCapacity_ * sizeof(HudVertex);
    vbDesc.isVertexBuffer = true;
    vbDesc.debugName = "ImGuiVB";
    vbDesc.initialState = nvrhi::ResourceStates::CopyDest;
    vbDesc.keepInitialState = true;
    vertexBuffer_ = device_->createBuffer(vbDesc);
    nvrhi::BufferDesc ibDesc;
    ibDesc.byteSize = indexCapacity_ * sizeof(std::uint32_t);
    ibDesc.isIndexBuffer = true;
    ibDesc.debugName = "ImGuiIB";
    ibDesc.initialState = nvrhi::ResourceStates::CopyDest;
    ibDesc.keepInitialState = true;
    indexBuffer_ = device_->createBuffer(ibDesc);
    if (!vertexBuffer_ || !indexBuffer_) {
        outError = "createBuffer failed for ImGui VB/IB";
        return false;
    }
    return true;
}

bool Hud::ensurePipeline(nvrhi::IDevice* device, const nvrhi::FramebufferInfo& info,
                         std::string& outError) {
    if (pipeline_) return true;

    nvrhi::GraphicsPipelineDesc desc;
    desc.setPrimType(nvrhi::PrimitiveType::TriangleList)
        .setInputLayout(inputLayout_)
        .setVertexShader(vs_)
        .setFragmentShader(ps_)
        .addBindingLayout(bindingLayout_);

    desc.renderState.rasterState
        .setCullNone()
        .setFrontCounterClockwise(true)
        .enableScissor();                       // 每个 ImDrawCmd 有自己的 ClipRect
    desc.renderState.depthStencilState
        .disableDepthTest().disableDepthWrite();  // HUD 永远压在 3D 画面之上

    auto& rt = desc.renderState.blendState.targets[0];
    rt.enableBlend()
      .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
      .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
      .setBlendOp(nvrhi::BlendOp::Add)
      .setSrcBlendAlpha(nvrhi::BlendFactor::One)
      .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
      .setBlendOpAlpha(nvrhi::BlendOp::Add);

    pipeline_ = device->createGraphicsPipeline(desc, info);
    if (!pipeline_) { outError = "createGraphicsPipeline failed for HUD"; return false; }
    return true;
}

void Hud::invalidatePipeline() { pipeline_ = nullptr; }

void Hud::build(HudFrame& frame) {
    buildOk_ = false;
    commands_.clear();
    vertices_.clear();
    indices_.clear();
    if (!g_context || !fontTexture_ || frame.displayWidth <= 0.0f ||
        frame.displayHeight <= 0.0f) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(frame.displayWidth, frame.displayHeight);
    io.DeltaTime = std::max(frame.deltaSeconds, 1.0f / 1000.0f);
    io.MousePos = ImVec2(frame.mouseX, frame.mouseY);
    io.MouseDown[0] = frame.mouseLeftDown;
    io.MouseWheel = frame.mouseWheel;

    ImGui::NewFrame();
    // 固定位置 + 固定尺寸：长的 adapter / debugLayer 字符串不会把窗口撑到半个屏幕，
    // 截图回归也不依赖 imgui.ini 或窗口历史状态。长文本一律 TextWrapped。
    ImGui::SetNextWindowPos(ImVec2(14.0f, 14.0f), ImGuiCond_Always);
    // 小画布上把面板收在可视范围内（内容多时出滚动条，不越界）；大画布给足高度。
    const float panelH = std::min(400.0f, frame.displayHeight - 28.0f);
    ImGui::SetNextWindowSize(ImVec2(330.0f, std::max(180.0f, panelH)), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);   // 半透明：HUD 与背后的模型同时看得见
    ImGui::Begin("nl-engine HUD", nullptr, ImGuiWindowFlags_NoResize);
    ImGui::TextWrapped("nl-engine  %s", frame.stage);
    ImGui::Separator();
    ImGui::Text("backend  : %s", frame.backend);
    ImGui::TextWrapped("adapter  : %s", frame.adapter);
    ImGui::TextWrapped("debug    : %s", frame.debugLayer);
    ImGui::Text("mode     : %s", frame.mode);
    ImGui::Separator();
    ImGui::Text("FPS %.1f   frame %.2f ms", frame.fps, frame.frameMs);
    ImGui::PlotLines("frame ms", frame.frameMsHistory, frame.frameMsCount,
                     0, nullptr, 0.0f, 40.0f, ImVec2(260.0f, 52.0f));
    ImGui::Text("yaw %.1f  pitch %.1f  dist %.2f",
                frame.yawDeg, frame.pitchDeg, frame.distance);
    ImGui::Text("timing: %s", frame.deterministic
                ? "capture/replay (fixed samples)" : "live sampling");
    ImGui::Separator();
    ImGui::Checkbox("back-face culling", &frame.cullBack);
    ImGui::SliderInt("clear color", &frame.colorIndex, 0, 3);
    if (ImGui::Button("reset camera")) frame.resetCamera = true;
    ImGui::TextWrapped("drag=orbit  wheel=zoom  C=cull  R=reset  ESC=quit");
    ImGui::End();
    ImGui::Render();

    frame.wantCaptureMouse = io.WantCaptureMouse;

    ImDrawData* data = ImGui::GetDrawData();
    if (!data || !data->Valid || data->CmdListsCount <= 0 ||
        data->DisplaySize.x <= 0.0f || data->DisplaySize.y <= 0.0f) return;

    projection_ = hudProjection(data->DisplaySize.x, data->DisplaySize.y,
                                data->DisplayPos.x, data->DisplayPos.y);

    const int maxX = static_cast<int>(std::lround(data->DisplaySize.x));
    const int maxY = static_cast<int>(std::lround(data->DisplaySize.y));
    for (int n = 0; n < data->CmdListsCount; ++n) {
        const ImDrawList* list = data->CmdLists[n];
        const std::uint32_t vtxBase = static_cast<std::uint32_t>(vertices_.size());
        const std::uint32_t idxBase = static_cast<std::uint32_t>(indices_.size());
        vertices_.reserve(vertices_.size() + static_cast<std::size_t>(list->VtxBuffer.Size));
        for (int i = 0; i < list->VtxBuffer.Size; ++i) {
            const ImDrawVert& s = list->VtxBuffer[i];
            HudVertex v{};
            v.x = s.pos.x; v.y = s.pos.y;
            v.u = s.uv.x;  v.v = s.uv.y;
            v.r = channel(s.col, IM_COL32_R_SHIFT);
            v.g = channel(s.col, IM_COL32_G_SHIFT);
            v.b = channel(s.col, IM_COL32_B_SHIFT);
            v.a = channel(s.col, IM_COL32_A_SHIFT);
            vertices_.push_back(v);
        }
        indices_.reserve(indices_.size() + static_cast<std::size_t>(list->IdxBuffer.Size));
        for (int i = 0; i < list->IdxBuffer.Size; ++i)
            indices_.push_back(static_cast<std::uint32_t>(list->IdxBuffer[i]) + vtxBase);

        for (int c = 0; c < list->CmdBuffer.Size; ++c) {
            const ImDrawCmd& d = list->CmdBuffer[c];
            if (d.ElemCount == 0 || d.UserCallback) continue;
            // ClipRect 是 display-space，减 DisplayPos 后 clamp 到 viewport。
            const int x0 = std::clamp(static_cast<int>(std::floor(d.ClipRect.x - data->DisplayPos.x)), 0, maxX);
            const int y0 = std::clamp(static_cast<int>(std::floor(d.ClipRect.y - data->DisplayPos.y)), 0, maxY);
            const int x1 = std::clamp(static_cast<int>(std::ceil (d.ClipRect.z - data->DisplayPos.x)), 0, maxX);
            const int y1 = std::clamp(static_cast<int>(std::ceil (d.ClipRect.w - data->DisplayPos.y)), 0, maxY);
            if (x1 <= x0 || y1 <= y0) continue;
            DrawCmd out{};
            out.indexOffset = idxBase + d.IdxOffset;
            out.indexCount  = d.ElemCount;
            out.x0 = x0; out.y0 = y0; out.x1 = x1; out.y1 = y1;
            commands_.push_back(out);
        }
    }

    std::string err;
    if (!ensureCapacity(vertices_.size(), indices_.size(), err)) {
        nl::core::log::error("HUD: ", err);
        commands_.clear();
        return;
    }
    buildOk_ = true;
}

void Hud::draw(nvrhi::ICommandList* cmd, nvrhi::IFramebuffer* framebuffer,
               const nvrhi::Viewport& viewport) {
    if (!buildOk_ || !ready() || !framebuffer || commands_.empty()) return;

    // VB/IB 是普通（非 volatile）buffer：Vulkan 后端禁止 volatile 非 CB buffer，
    // 故每帧用 writeBuffer 走 upload manager + 自动 barrier（CopyDest -> VB/IB）。
    cmd->writeBuffer(constants_, &projection_, sizeof(projection_));
    cmd->writeBuffer(vertexBuffer_, vertices_.data(),
                     vertices_.size() * sizeof(HudVertex));
    cmd->writeBuffer(indexBuffer_, indices_.data(),
                     indices_.size() * sizeof(std::uint32_t));

    for (const DrawCmd& dc : commands_) {
        nvrhi::GraphicsState state;
        state.setPipeline(pipeline_)
             .setFramebuffer(framebuffer)
             .addBindingSet(bindingSet_)
             .addVertexBuffer(nvrhi::VertexBufferBinding()
                 .setBuffer(vertexBuffer_).setSlot(0).setOffset(0))
             .setIndexBuffer(nvrhi::IndexBufferBinding()
                 .setBuffer(indexBuffer_).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
        // Rect 的构造参数顺序是 (minX, maxX, minY, maxY)，别按常规 xyxy 写错。
        state.viewport.addViewport(viewport)
                      .addScissorRect(nvrhi::Rect(dc.x0, dc.x1, dc.y0, dc.y1));
        cmd->setGraphicsState(state);
        nvrhi::DrawArguments args;
        args.setVertexCount(dc.indexCount)
            .setStartIndexLocation(dc.indexOffset);
        cmd->drawIndexed(args);
    }
}

void Hud::shutdown() {
    pipeline_ = nullptr;
    bindingSet_ = nullptr;
    bindingLayout_ = nullptr;
    inputLayout_ = nullptr;
    indexBuffer_ = nullptr;
    vertexBuffer_ = nullptr;
    constants_ = nullptr;
    sampler_ = nullptr;
    fontTexture_ = nullptr;
    ps_ = nullptr;
    vs_ = nullptr;
    vertexCapacity_ = indexCapacity_ = 0;
    vertices_.clear(); indices_.clear(); commands_.clear();
    buildOk_ = false;
    device_ = nullptr;
    if (g_context) {
        ImGui::DestroyContext(g_context);
        g_context = nullptr;
    }
}

} // namespace nl::renderer::detail

namespace nl::renderer {

bool hudSelfTest() {
    using namespace nl::renderer::detail;
    // volatile 让 /W4 不把 sizeof 比较折叠成 C4127「条件表达式是常量」警告；
    // 编译期还有 hud.hpp 里的 static_assert 兜底，这里验的是运行期可见的值。
    volatile std::size_t vtxSize = sizeof(HudVertex);
    volatile std::size_t cbSize = sizeof(HudConstants);
    if (vtxSize != 32 || cbSize != 16) return false;
    const HudConstants p = hudProjection(640.0f, 360.0f);
    if (std::fabs(p.projection[0] - 2.0f / 640.0f) > 1e-6f) return false;
    if (std::fabs(p.projection[1] + 2.0f / 360.0f) > 1e-6f) return false;  // Y 必须取负
    if (std::fabs(p.projection[2] + 1.0f) > 1e-6f) return false;
    if (std::fabs(p.projection[3] - 1.0f) > 1e-6f) return false;
    const HudConstants q = hudProjection(640.0f, 360.0f, 10.0f, 20.0f);
    if (std::fabs(q.projection[2] - (-1.0f - 10.0f * 2.0f / 640.0f)) > 1e-6f) return false;
    if (std::fabs(q.projection[3] - (1.0f + 20.0f * 2.0f / 360.0f)) > 1e-6f) return false;
    if (hudProjection(0.0f, 360.0f).projection[0] != 0.0f) return false;
    HudFrame f;
    if (!f.cullBack || f.frameMsCount != 0 || f.colorIndex != 0 ||
        f.resetCamera || f.wantCaptureMouse || f.deterministic) return false;
    return true;
}

} // namespace nl::renderer
