#include "hud.hpp"
#include "backend.hpp"       // readFileBytes（两后端共用的字节码读取）
#include "console_ui.hpp"    // P8 M-C1：开发者控制台窗口（ImGui 前端）
#include "audio_debug_ui.hpp" // P8 M-S2：独立默认关的音频声源调试面板
#include "gpu_timer.hpp"     // M0.6：GpuTimer::selfTest（秒→ms 换算 / 环形游标）
#include "nl/renderer/editor.hpp"  // T1：材质图编辑器窗口（--editor 时由 HudFrame.editor 挂入）

#include "nl/core/log.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
    // M0.6 增了 GPU 行 + 曲线后内容更高，上限从 400 提到 460，960x540 下不裁底。
    // M0.7 又增 model 行，上限提到 480（640x360 小画布仍靠滚动条收纳）。
    // M0.8 增 shading 下拉框（wireframe 复选框与 cull 并行不占新行），上限提到 510。
    // M4.2 增 cull GPU 行 + cull ms 曲线（仅 cull 模式占位），510 装不下出滚动条，
    // 上限提到 560；小画布仍由 min(cap, displayHeight-28) 兜底，行为不变。
    const float panelH = std::min(560.0f, frame.displayHeight - 28.0f);
    ImGui::SetNextWindowSize(ImVec2(330.0f, std::max(180.0f, panelH)), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);   // 半透明：HUD 与背后的模型同时看得见
    // M-C2：hud.show=false 只藏面板（ImGui 帧保持，控制台窗口照画）。
    // 整块作用域包裹：面板内控件写回（resetCamera 等）在隐藏帧不发生，字段保持缺省。
    if (frame.panelShow) {
    ImGui::Begin("nl-engine HUD", nullptr, ImGuiWindowFlags_NoResize);
    ImGui::TextWrapped("nl-engine  %s", frame.stage);
    ImGui::Separator();
    ImGui::Text("backend  : %s", frame.backend);
    ImGui::TextWrapped("adapter  : %s", frame.adapter);
    ImGui::TextWrapped("debug    : %s", frame.debugLayer);
    ImGui::Text("mode     : %s", frame.mode);
    // M0.7：加载的模型名 + 三角形数，截图直接自证 --model 生效（不用翻日志）。
    if (frame.model && *frame.model) {
        char modelLine[128];
        std::snprintf(modelLine, sizeof(modelLine), "model    : %s (%d tri)",
                      frame.model, frame.triangleCount);
        ImGui::TextUnformatted(modelLine);
    }
    // M8.0（S8.0）：BLAS 预建证据行（仅 dag/hiz 模式 + v3 lod0 资产占位，其它
    // 模式 blasTriangleCount==0 一像素不动）。deterministic 截图不显示 ms（基线
    // 稳定），真实构建耗时看日志「gpu timer resolved [blas]」行。
    if (frame.blasTriangleCount > 0) {
        char blasLine[96];
        if (frame.blasTimingAvailable && !frame.deterministic && frame.blasMs > 0.0f) {
            std::snprintf(blasLine, sizeof(blasLine),
                          "BLAS     : %d tri  built %.2f ms",
                          frame.blasTriangleCount, frame.blasMs);
        } else {
            std::snprintf(blasLine, sizeof(blasLine), "BLAS     : %d tri",
                          frame.blasTriangleCount);
        }
        ImGui::TextUnformatted(blasLine);
    }
    // M8.1（S8.1）：TLAS 每帧 rebuild 证据行（跟 BLAS 行同门控：dag/hiz 模式 +
    // v3 lod0 资产才占位，其它模式 tlasInstanceCount==0 一像素不动）。
    // deterministic 截图不显示 ms（基线稳定）；真实 rebuild 耗时看
    // 「gpu timer resolved [tlas]」日志行。
    if (frame.tlasInstanceCount > 0) {
        char tlasLine[96];
        if (frame.tlasTimingAvailable && !frame.deterministic && frame.tlasMs > 0.0f) {
            std::snprintf(tlasLine, sizeof(tlasLine),
                          "TLAS     : %d inst  rebuild %.2f ms",
                          frame.tlasInstanceCount, frame.tlasMs);
        } else {
            std::snprintf(tlasLine, sizeof(tlasLine), "TLAS     : %d inst",
                          frame.tlasInstanceCount);
        }
        ImGui::TextUnformatted(tlasLine);
    }
    // M-PT0：粒子证据行（仅 --particles 模式显示——particlesEnabled 由 viewer 喂，
    // 其它模式一像素不动；粒子数 = scene_ 当帧活粒子，稳态区间由 run.bat 断言）。
    if (frame.particlesEnabled) {
        char partLine[96];
        std::snprintf(partLine, sizeof(partLine), "Particles : %d alive", frame.particleAlive);
        ImGui::TextUnformatted(partLine);
    }
    // M6.2：Hi-Z 专属区**上移**到 mode/model 行之后、FPS 行之前——960x540 画布
    // 面板高度被 min(cap, H-28)=512 钉死，原位置（frustum/cull GPU 行之后）会让
    // 开关与 occl 行掉进滚动区截断，验收证据不可见。只影响 hiz 模式布局（其它
    // 模式 hizMipCount==0 一像素不动）；kStage bump S6.2 全量重拍基线。
    if (frame.hizMipCount > 0) {
        // M6.2：遮挡剔除总开关（双向写回，viewer 传 FrameParams.hizEnable）。
        // 定位同 UE r.Nanite.Culling.HZB——验收用退化开关，关掉立即回退到纯
        // 视锥/误差遍历（S6-PLAN 底线「可整体开关的独立层」）。
        ImGui::Checkbox("hi-z occlusion", &frame.hizEnable);
        ImGui::SliderInt("pyramid mip", &frame.hizViewLevel, 0, frame.hizMipCount - 1);
        char hizLine[96];
        if (frame.hizTopFar >= 0.0f) {
            std::snprintf(hizLine, sizeof(hizLine),
                          "hiz top : %.3f (mip %d/%d, farthest depth)",
                          frame.hizTopFar, frame.hizMipCount - 1, frame.hizMipCount - 1);
        } else if (frame.hizTopFar <= -1.5f) {   // -2：背景像素主导（max 口径合法）
            std::snprintf(hizLine, sizeof(hizLine), "hiz top : bg (mip %d/%d)",
                          frame.hizMipCount - 1, frame.hizMipCount - 1);
        } else {
            std::snprintf(hizLine, sizeof(hizLine), "hiz top : -- (mips %d)",
                          frame.hizMipCount);
        }
        ImGui::TextUnformatted(hizLine);
        // M6.2：剔除前/后簇数对照（整树语义：被遮父簇的子树未访问不计入剔除前）。
        // 剔除前 = visibleClusters + hizOccluded（本轮访问的簇节点数），剔除后 =
        // visibleClusters（实际 emit）。开关关掉时 culled 归 0、两数合一，立即回退。
        if (frame.hizOccluded >= 0) {
            const int visited = frame.visibleClusters + frame.hizOccluded;
            std::snprintf(hizLine, sizeof(hizLine),
                          "occl   : culled %d / %d visited (drawn %d)",
                          frame.hizOccluded, visited, frame.visibleClusters);
        } else {
            std::snprintf(hizLine, sizeof(hizLine), "occl   : -- (delayed readback)");
        }
        ImGui::TextUnformatted(hizLine);
        ImGui::Separator();
    }
    ImGui::Separator();
    ImGui::Text("FPS %.1f   frame %.2f ms", frame.fps, frame.frameMs);
    ImGui::PlotLines("frame ms", frame.frameMsHistory, frame.frameMsCount,
                     0, nullptr, 0.0f, 40.0f, ImVec2(260.0f, 44.0f));
    // M0.6：真实 GPU 帧内耗时（NVRHI timer query，异步天然有 1~2 帧延迟）。
    // frame ms 是 CPU 帧间隔，GPU ms 才是 GPU 干活的耗时——两者并排防混淆。
    if (frame.gpuTimingAvailable) {
        ImGui::Text("GPU  %.2f ms (NVRHI timer)", frame.gpuMs);
        ImGui::PlotLines("gpu ms", frame.gpuMsHistory, frame.gpuMsCount,
                         0, nullptr, 0.0f, 40.0f, ImVec2(260.0f, 34.0f));
    }
    ImGui::Text("yaw %.1f  pitch %.1f  dist %.2f",
                frame.yawDeg, frame.pitchDeg, frame.distance);
    ImGui::Text("timing: %s", frame.deterministic
                ? "capture/replay (fixed samples)" : "live sampling");
    ImGui::Separator();
    ImGui::Checkbox("back-face culling", &frame.cullBack);
    ImGui::SameLine();
    ImGui::Checkbox("wireframe", &frame.wireframe);   // M0.8：光栅 fill mode，与 cull 正交
    // M0.8：着色模式下拉框，双向 POD 通道与 cull/color 控件同一条路。
    // M1.3：cluster 只有 cluster 场景提供了 CLUSTERCOLOR 属性（viewer 侧在
    // normal 模式把该值钳回 0..3，这里只是同一张清单）。
    // pbr-direct：追加 "pbr"（mode 5，仅 normal/skin 场景有意义；cluster 场景
    // 选它会落 mode-0 白模兜底，无崩溃——与 normal 场景选 cluster 同级别的宽松）。
    static const char* kShadingNames[] = { "textured", "normal", "uv", "tri-id", "cluster", "pbr" };
    ImGui::Combo("shading", &frame.shading, kShadingNames,
                 static_cast<int>(sizeof(kShadingNames) / sizeof(kShadingNames[0])));
    ImGui::SliderInt("clear color", &frame.colorIndex, 0, 3);
    // M2.3：LOD 层滑条——只有探测到分层资产（lodCount>1）才占位置，cluster / normal
    // 模式的 HUD 布局与历史基线一像素不动。滑条值走 HudFrame 双向通道，由 viewer
    // 检测变化后调 Renderer::setLodLevel()（HUD 不直接碰渲染资源）。
    if (frame.lodCount > 1) {
        ImGui::SliderInt("lod level", &frame.lodLevel, 0, frame.lodCount - 1);
        ImGui::Text("lod %d/%d  tri %d  maxErr %.4f", frame.lodLevel, frame.lodCount - 1,
                    frame.lodTriangles, frame.lodMaxError);
    }
    // M3.2：DAG 组下钻滑条——只有 dag-top 模式（dagGroupCount>0）才占位置，
    // 其它模式 HUD 布局一像素不动（viewer 侧 lod 模式不下发 dagGroupCount）。
    // -1 = 纯顶层；0..N-1 = 挖掉该组生成的父簇、换回组内子簇（S5 cut 预演）。
    if (frame.dagGroupCount > 0) {
        ImGui::SliderInt("dag drill", &frame.dagGroup, -1, frame.dagGroupCount - 1);
        char dagLine[96];
        if (frame.dagGroup < 0) {
            std::snprintf(dagLine, sizeof(dagLine), "dag top (no drill)  tri %d",
                          frame.lodTriangles);
        } else {
            std::snprintf(dagLine, sizeof(dagLine), "dag drill g%d  tri %d  groupErr %.4f",
                          frame.dagGroup, frame.lodTriangles, frame.dagGroupError);
        }
        ImGui::TextUnformatted(dagLine);
        // M5.0（S5.0）：屏幕误差投影 + cut 阈值（S5 组原子 cut 的选层判据）。
        // 误差由 viewer 侧 zeux/bevy 公式算好（世界误差 -> 屏幕像素），HUD 只显示；
        // 转相机 / 拉远时该值随距离单调变化 = M5.0 的可见验收用例。阈值双向写回，
        // M5.1 GPU 遍历消费同一参数。
        if (frame.screenErrorValid) {
            char s5Line[96];
            std::snprintf(s5Line, sizeof(s5Line), "s5 err %.2f px", frame.screenErrorPx);
            ImGui::TextUnformatted(s5Line);
        }
        ImGui::SliderFloat("cut thr px", &frame.cutThresholdPx, 0.25f, 4.0f, "%.2f");
    }
    // M4.1：GPU 视锥剔除统计——仅 cull 模式（totalClusters>0）占位，其它模式 HUD
    // 布局一像素不动。visibleClusters<0 = 前两帧尚无回读数据。
    if (frame.totalClusters > 0) {
        if (frame.visibleClusters >= 0) {
            const float pct = 100.0f * static_cast<float>(frame.visibleClusters) /
                              static_cast<float>(frame.totalClusters);
            char cullLine[96];
            std::snprintf(cullLine, sizeof(cullLine), "frustum : %d / %d clusters (%.1f%%)",
                          frame.visibleClusters, frame.totalClusters, pct);
            ImGui::TextUnformatted(cullLine);
        } else {
            char cullLine[96];
            std::snprintf(cullLine, sizeof(cullLine), "frustum : -- / %d clusters",
                          frame.totalClusters);
            ImGui::TextUnformatted(cullLine);
        }
        // M5.2：GPU 可见三角数（cull / dag 同一口径，compute 原子累加 + 延迟两帧
        // 回读）。-1 = 前两帧尚无数据，与 frustum 行同款占位规则。
        if (frame.visibleTriangles >= 0) {
            char triLine[96];
            std::snprintf(triLine, sizeof(triLine), "s5 tris : %d", frame.visibleTriangles);
            ImGui::TextUnformatted(triLine);
        }
        // M4.2：cull pass GPU 耗时（独立 timer 实例，只包 compute 剔除段）。
        // 与整帧 GPU ms 并排——剔除开销是否随场景规模变化一眼可见。
        if (frame.cullTimingAvailable) {
            ImGui::Text("cull GPU  %.3f ms", frame.cullMs);
            ImGui::PlotLines("cull ms", frame.cullMsHistory, frame.cullMsCount,
                             0, nullptr, 0.0f, 1.0f, ImVec2(260.0f, 30.0f));
        }
    }
    // M6.0/M6.2 的 Hi-Z 区已上移到 mode/model 行之后（见上方注释）——原位置
    // 在 960x540 画布上会掉进滚动区截断。
    if (ImGui::Button("reset camera")) frame.resetCamera = true;
    ImGui::TextWrapped("drag=orbit  wheel=zoom  C=cull  V=wire  N=shading  R=reset  ESC=quit");
    ImGui::End();
    }   // frame.panelShow（M-C2：藏面板不藏控制台）
    // T1：材质图编辑器窗口（--editor 时 viewer 挂入；null = 完全不画，既有基线不动）。
    if (frame.editor) frame.editor->drawWindow();
    // P8 M-C1：开发者控制台（` 呼出的独立窗口）。含 GLFW->ImGuiKey 输入映射与
    // wantCaptureKeyboard 回写；consoleOpen=false 时仍同步按键状态但零布局。
    drawConsoleWindow(frame);
    // P8 M-S2: audio source debug panel — SEPARATE default-off window
    // (audio.debug CVar). Does not touch the resident HUD content above, so it
    // never forces a HUD baseline re-shoot; with the CVar off it draws nothing.
    drawAudioDebugWindow();
    // P8 M-I2: input-hud debug panel — separate window, default-off gate
    // (frame.inputDebug.show set by viewer only in --mode input-hud). Same
    // additive-sibling placement as the console/audio windows; with the gate
    // off it draws nothing, so no existing HUD baseline moves.
    if (frame.inputDebug.show) drawInputDebugUi(frame.inputDebug);
    ImGui::Render();

    frame.wantCaptureMouse = frame.wantCaptureMouse || io.WantCaptureMouse;

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
    if (!f.panelShow) return false;   // M-C2：面板缺省可见（基线一像素不动的保证）
    if (f.gpuTimingAvailable || f.gpuMs != 0.0f || f.gpuMsCount != 0) return false;
    // M4.2：cull 计时通道缺省全零（非 cull 模式 HUD 布局一像素不动的保证）。
    if (f.cullTimingAvailable || f.cullMs != 0.0f || f.cullMsCount != 0) return false;
    // M5.2：可见三角数通道缺省无数据。
    if (f.visibleTriangles != -1) return false;
    // M8.0：BLAS 通道缺省全零（非 dag/hiz 模式 HUD 布局一像素不动的保证）。
    if (f.blasTriangleCount != 0 || f.blasMs != 0.0f || f.blasTimingAvailable) return false;
    // M8.1：TLAS 通道缺省全零（同上）。
    if (f.tlasInstanceCount != 0 || f.tlasMs != 0.0f || f.tlasTimingAvailable) return false;
    // M-PT0：粒子通道缺省全关（非 --particles 模式 HUD 布局一像素不动的保证）。
    if (f.particlesEnabled || f.particleAlive != 0) return false;
    if (f.shading != 0 || f.wireframe) return false;   // M0.8：调试着色 / 线框缺省 = M0.7 行为
    // M-C1：控制台通道缺省全关（HUD 关掉控制台 = 历史 HUD 基线一像素不动的保证）。
    if (f.consoleOpen || f.consoleTab || f.wantCaptureKeyboard) return false;
    if (f.keyEventCount != 0 || f.textCharCount != 0) return false;
    if (f.consolePrefill != nullptr && *f.consolePrefill != '\0') return false;
    if (!GpuTimer::selfTest()) return false;   // M0.6：秒→ms 换算 + 环形游标回绕
    return true;
}

} // namespace nl::renderer
