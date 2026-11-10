#include "backend.hpp"
#include "scene.hpp"
#include "hud.hpp"
#include "gpu_timer.hpp"

#if defined(NL_BACKEND_DX12)

#include "nl/core/log.hpp"
#include "nl/core/meshlet_io.hpp"   // M3.2：探测第二粗层 dagGroups 组数

#include <nvrhi/nvrhi.h>
#include <nvrhi/d3d12.h>
#include <nvrhi/validation.h>

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace nl::renderer::detail {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kBufferCount = 2;
constexpr DXGI_FORMAT kSwapFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

std::string hrMessage(const char* what, HRESULT hr) {
    std::ostringstream os;
    os << what << " failed (0x" << std::hex << static_cast<unsigned>(hr) << ")";
    return os.str();
}

std::string narrow(const wchar_t* wide) {
    if (!wide) return {};
    char buf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    return buf;
}

// NVRHI 的消息回调接进 core 日志：错误信息独立计数，验收要求为 0。
class NvrhiMessageCallback final : public nvrhi::IMessageCallback {
public:
    void message(nvrhi::MessageSeverity severity, const char* messageText) override {
        const char* text = messageText ? messageText : "<null>";
        switch (severity) {
        case nvrhi::MessageSeverity::Error:
        case nvrhi::MessageSeverity::Fatal:
            nl::core::log::deviceError("NVRHI", text);
            break;
        case nvrhi::MessageSeverity::Warning:
            nl::core::log::warn("NVRHI: ", text);
            break;
        default:
            nl::core::log::debug("NVRHI: ", text);
            break;
        }
    }
};

class Dx12Backend final : public Backend {
public:
    ~Dx12Backend() override { shutdown(); }

    bool init(void* nativeWindow, Extent extent, bool enableDebugLayer,
              const char* modelFile, bool clusterMode, bool cullMode,
              bool frustumCull, bool dagMode, bool skinMode, bool hizMode,
              bool sceneMode, bool visbufMode, bool pullingMode, bool rtMode,
              std::string& outError) override {
        if (!nativeWindow || extent.width == 0 || extent.height == 0) {
            outError = "invalid window handle or extent";
            return false;
        }
        hwnd_ = static_cast<HWND>(nativeWindow);
        extent_ = extent;

        // 调试层：Windows SDK 自带，不需要任何厂商 SDK。
        // 照 Donut DeviceManager_DX12 的写法，controller 是局部对象：EnableDebugLayer()
        // 是进程级开关，调用后即可 Release。长期持有它会让 Debug Layer 在
        // swapchain backbuffer 销毁 / ResizeBuffers 时抛 0x87d CORRUPTION（退出码 2173）。
        if (enableDebugLayer) {
            ComPtr<ID3D12Debug> debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
                debugController->EnableDebugLayer();
                info_.debugLayerActive = true;
            }
        }
        info_.debugLayer = info_.debugLayerActive
            ? "D3D12 Debug Layer (Windows SDK) + NVRHI validation"
            : "D3D12 Debug Layer unavailable + NVRHI validation";

        // DXGI_CREATE_FACTORY_DEBUG 会打开 DXGI 侧调试层；本机实测它在 swapchain
        // resize / 销毁时抛 0x87d。D3D12 Debug Layer 本身仍然启用，错误通过
        // ID3D12InfoQueue 统计，不需要 DXGI 那层。
        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) { outError = hrMessage("CreateDXGIFactory2", hr); return false; }

        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(factory_->EnumAdapters1(0, &adapter))) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) info_.adapterName = narrow(desc.Description);
        }

        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12_));
        if (FAILED(hr)) { outError = hrMessage("D3D12CreateDevice", hr); return false; }

        if (info_.debugLayerActive) {
            device12_->QueryInterface(IID_PPV_ARGS(&infoQueue_));
            if (infoQueue_) {
                // Debug Layer 默认遇到 CORRUPTION 会 RaiseException(0x87d)，没有调试器时
                // 表现为退出崩溃。关掉 break，消息仍然进队列，由 drainMessages 机械统计。
                infoQueue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
                infoQueue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
                infoQueue_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
            }
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        hr = device12_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_));
        if (FAILED(hr)) { outError = hrMessage("CreateCommandQueue", hr); return false; }

        nvrhi::d3d12::DeviceDesc desc;
        desc.errorCB = &messageCallback_;
        desc.pDevice = device12_.Get();
        desc.pGraphicsCommandQueue = queue_.Get();
        desc.enableDxrClusters = false;   // 不依赖预览版 Agility SDK
        desc.enableNvapiClusters = false;
        rawDevice_ = nvrhi::d3d12::createDevice(desc);
        if (!rawDevice_) { outError = "nvrhi::d3d12::createDevice returned null"; return false; }
        device_ = nvrhi::validation::createValidationLayer(rawDevice_);

        // M8.1：帧命令列表常驻（创建一次，每帧 open/close/execute 复用）。
        frameCmd_ = device_->createCommandList();
        if (!frameCmd_) { outError = "createCommandList for frame recording failed"; return false; }

        // frame-in-flight=2 的 fence：每帧提交后给当前 swapchain slot 记一个值，
        // 下一帧轮到同 slot 前先等它。删掉 renderFrame 里的等待 = CPU 在 GPU 仍持有该
        // backbuffer 时又画 → 同步报错（S0-PLAN 的校验点）。
        if (FAILED(device12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
            nl::core::log::warn("DX12: CreateFence failed; frame-in-flight gating unavailable");
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        reportFeatures();

        hr = createSwapChain();
        if (FAILED(hr)) { outError = hrMessage("CreateSwapChainForHwnd", hr); return false; }
        if (!createTargets(outError)) return false;
        // M4.0：vertex pulling 走 initCull（三 SSBO + indirect draw）。DX12 入口名被
        // NVRHI 忽略（真正入口是编译期 -entry 写进 blob 的），这里传真实入口名仅为可读性；
        // Vulkan 侧恒传 "main"（见 docs/SHADER-CONVENTIONS.md §2.4）。
        bool sceneOk = false;
        if (sceneMode) {
            // M-E1b.0：ECS 场景不在此初始化 Scene（swapchain/depth/framebuffer/
            // present 照常）；场景资源由 loadEcsScene 建立后才可绘制（ready() 为
            // false 时 draw 段跳过，与历史空场景行为一致）。
            sceneOk = true;
        }
        else if (visbufMode)      // M7.0：优先级最高，走独立 vis buffer 三段式
            sceneOk = scene_.initVisbuf(device_, "dxil", "vsCull", "psCull", modelFile,
                                         pullingMode, outError);
        else if (skinMode)
            sceneOk = scene_.initSkin(device_, "dxil", "vsMain", "psMain", modelFile, outError);
        else if (hizMode)
            sceneOk = scene_.initHiz(device_, "dxil", "vsCull", "psCull", modelFile, outError);
        // M8.2：rt 模式走 initDag 资产链（BLAS/TLAS 挂在 initDag → initBlas →
        // initTlas 上），渲染只做 RayQuery compute + 调试图呈现。
        else if (rtMode)
            sceneOk = scene_.initRt(device_, "dxil", "vsCull", "psCull", modelFile, outError);
        else if (dagMode)
            sceneOk = scene_.initDag(device_, "dxil", "vsCull", "psCull", modelFile, outError);
        else if (clusterMode)
            sceneOk = scene_.initCluster(device_, "dxil", "vsMain", "psMain", modelFile, outError);
        else if (cullMode)
            sceneOk = scene_.initCull(device_, "dxil", "vsCull", "psCull", modelFile,
                                      frustumCull, outError);
        else
            sceneOk = scene_.init(device_, "dxil", "vsMain", "psMain", modelFile, outError);
        if (!sceneOk) return false;
        if (!hud_.init(device_, "dxil", "vsMain", "psMain", outError)) return false;
        info_.modelName = scene_.modelName();
        info_.skinRadius = scene_.skinRadius();
        info_.triangleCount = scene_.triangleCount();
        info_.visibleClusters = scene_.visibleClusters();     // M4.1：-1 = 暂无数据
        info_.totalClusters = scene_.clusterCount();
        info_.visibleTriangles = scene_.visibleTriangles();   // M5.2：-1 = 暂无数据
        // M5.0：屏幕误差投影球数据（cluster/dag 路径有效，normal 恒 0）。
        for (int i = 0; i < 4; ++i) info_.assetSphere[i] = scene_.assetSphere()[i];
        // M2.3：探测 assets/meshlets/<stem>.lodN.nmesh 的连续层数（0 = 没有分层资产）。
        // 探测失败不是错误：cluster / normal 模式照样跑，只是 HUD 不画滑条。
        // cull 模式不依赖分层资产，跳过探测。
        if (clusterMode) {
            lodStem_ = detail::meshletStem(modelFile);
            lodLevelCount_ = detail::probeLodLevelCount(lodStem_);
            info_.lodLevelCount = lodLevelCount_;
            info_.lodLevel = scene_.lodLevel();
            info_.lodMaxError = scene_.lodMaxError();
            nl::core::log::info("lod layers: ", lodLevelCount_, " | stem: ", lodStem_);
            // M3.2：可下钻组数 = 链第二粗层（lodCount-2）的 dagGroups。读它的
            // meta 只为了拿组数；加载失败不是错误（dag-top 纯顶层照样画，
            // 只是 HUD 不出下钻滑条）。
            if (lodLevelCount_ >= 2) {
                const std::string childFile = detail::lodMeshFileName(lodStem_, lodLevelCount_ - 2);
                nl::core::NMeshData probe;
                std::string probeError;
                if (nl::core::loadNMesh((std::string(NL_MESHLET_DIR "/") + childFile).c_str(),
                                        probe, probeError) &&
                    probe.lodMeta.dagPresent) {
                    dagGroupCount_ = static_cast<int>(probe.lodMeta.dagGroups.size());
                }
            }
            info_.dagGroupCount = dagGroupCount_;
        }

        nl::core::log::info("device ready: ", info_.adapterName,
                            " | ", info_.debugLayer,
                            " | swapchain ", extent_.width, "x", extent_.height);
        return true;
    }

    void resize(Extent extent) override {
        if (extent.width == 0 || extent.height == 0) return;
        if (extent.width == extent_.width && extent.height == extent_.height) return;
        if (!swapchain_) return;

        flushGpu();
        // 顺序照 Donut DeviceManager_DX12::ReleaseRenderTargets：
        // 先 GC 放掉 NVRHI 内部（command list / state tracker）对 backbuffer 的在途引用，
        // 再释放 RHI handle 和原生 resource，最后才允许 ResizeBuffers。
        // 反序会让 D3D12 Debug Layer 判定旧 backbuffer 仍被引用，抛 0x87d CORRUPTION。
        device_->waitForIdle();
        device_->runGarbageCollection();
        scene_.invalidatePipelines();
        hud_.invalidatePipeline();
        for (auto& fb : framebuffers_) fb = nullptr;
        depth_ = nullptr;
        rhiTargets_.clear();
        nativeTargets_.clear();
        nl::core::log::debug("resize: old targets released");

        const HRESULT hr = swapchain_->ResizeBuffers(kBufferCount, extent.width, extent.height,
                                                     kSwapFormat, 0);
        nl::core::log::debug("resize: ResizeBuffers returned");
        if (FAILED(hr)) {
            nl::core::log::deviceError("DX12", hrMessage("ResizeBuffers", hr));
            return;
        }
        extent_ = extent;
        std::string unused;
        createTargets(unused);
        nl::core::log::info("resize -> ", extent_.width, "x", extent_.height);
    }

    // M2.3：切 LOD 层——只重传 VB/IB（scene_.setClusterLevel），pipeline / binding 不动。
    // 越界或文件缺失时返回 false 且保持原层；调用方（viewer）负责提示与退出码。
    bool setLodLevel(int level, std::string& outError) override {
        if (lodLevelCount_ <= 0) {
            outError = "no layered .nmesh assets (run: nanite-tools lod <base>.nmesh -o <dir>/<stem>)";
            return false;
        }
        if (level < 0 || level >= lodLevelCount_) {
            outError = "lod level " + std::to_string(level) + " out of range 0.." +
                       std::to_string(lodLevelCount_ - 1);
            return false;
        }
        const std::string file = detail::lodMeshFileName(lodStem_, level);
        if (!scene_.setClusterLevel(device_, file.c_str(), outError)) return false;
        info_.modelName = scene_.modelName();
        info_.skinRadius = scene_.skinRadius();
        info_.triangleCount = scene_.triangleCount();
        info_.lodLevel = scene_.lodLevel();
        info_.lodMaxError = scene_.lodMaxError();
        info_.dagGroup = scene_.dagGroup();
        info_.dagGroupError = scene_.dagGroupError();
        for (int i = 0; i < 4; ++i) info_.assetSphere[i] = scene_.assetSphere()[i];
        nl::core::log::info("lod -> ", file, " | triangles=", info_.triangleCount,
                            " maxError=", info_.lodMaxError);
        return true;
    }

    // M3.2：dag-top 组原子下钻。group < 0 = 纯顶层（渲染最粗层全部簇）；
    // group >= 0 = 顶层挖掉该组生成的父簇区间、换回该组在第二粗层的成员簇。
    // 越界 / 无血缘 / 层数不足返回 false 且保持原画面。
    bool setDagCut(int group, std::string& outError) override {
        if (lodLevelCount_ <= 0) {
            outError = "no layered .nmesh assets (run: nanite-tools lod <base>.nmesh -o <dir>/<stem>)";
            return false;
        }
        const int top = lodLevelCount_ - 1;
        if (group < 0) {
            // 纯顶层：走与 lod 模式同一条 uploadClusterMesh 路径。
            const bool ok = setLodLevel(top, outError);
            if (ok) info_.dagGroup = -1;
            return ok;
        }
        if (lodLevelCount_ < 2) {
            outError = "dag drill needs >= 2 layers, got " + std::to_string(lodLevelCount_);
            return false;
        }
        if (group >= dagGroupCount_) {
            outError = "dag drill group " + std::to_string(group) + " out of range 0.." +
                       std::to_string(dagGroupCount_ - 1);
            return false;
        }
        const std::string topFile = detail::lodMeshFileName(lodStem_, top);
        const std::string childFile = detail::lodMeshFileName(lodStem_, top - 1);
        if (!scene_.setDagCut(device_, topFile.c_str(), childFile.c_str(), group, outError)) {
            return false;
        }
        info_.modelName = scene_.modelName();
        info_.skinRadius = scene_.skinRadius();
        info_.triangleCount = scene_.triangleCount();
        info_.lodLevel = scene_.lodLevel();
        info_.lodMaxError = scene_.lodMaxError();
        info_.dagGroup = scene_.dagGroup();
        info_.dagGroupError = scene_.dagGroupError();
        // M5.0：下钻后球数据两套都换（viewer 按 dagGroup 选组球 / 资产球）。
        for (int i = 0; i < 4; ++i) info_.assetSphere[i] = scene_.assetSphere()[i];
        for (int i = 0; i < 4; ++i) info_.dagGroupSphere[i] = scene_.dagGroupSphere()[i];
        return true;
    }

    // M-E1b.0：ECS 场景加载（sceneMode=true 的 init 之后由 Renderer 转发调用）。
    // DX12 运行时入口名被 NVRHI 忽略（真正入口是编译期 -entry vsScene/psScene 写进
    // blob 的），传真实名仅为可读性；Vulkan 侧恒传 "main"（§2.4 两阶段规则）。
    bool loadEcsScene(const SceneObjectDesc* objects, std::uint32_t objectCount,
                      const SceneLightDesc* lights, std::uint32_t lightCount,
                      float ambient, const char* sceneName,
                      std::string& outError) override {
        if (!scene_.initScene(device_, "dxil", "vsScene", "psScene", objects, objectCount,
                              lights, lightCount, ambient, sceneName, outError)) return false;
        info_.modelName = scene_.modelName();
        info_.triangleCount = scene_.triangleCount();
        info_.instanceCount = scene_.instanceCount();
        for (int i = 0; i < 4; ++i) info_.assetSphere[i] = scene_.assetSphere()[i];
        return true;
    }

    // M-E1b.5 渲染用例：转发 scene_.updateObjects（每帧实例数据刷新）。
    bool updateEcsObjects(const SceneObjectDesc* objects, std::uint32_t objectCount,
                          std::string& outError) override {
        return scene_.updateObjects(device_, objects, objectCount, outError);
    }

    // M-PT1：.nfx v1 加载端（转发 scene_.loadParticlesNfx；运行时零 Effekseer）。
    bool loadParticlesNfx(const char* path, std::string& outError) override {
        return scene_.loadParticlesNfx(path, outError);
    }

    bool renderFrame(ClearColor color, const FrameParams& frame,
                     const char* screenshotPath, HudFrame* hud,
                     std::string& outError) override {
        if (!swapchain_ || !device_) { outError = "renderer not initialized"; return false; }
        const UINT index = swapchain_->GetCurrentBackBufferIndex();
        if (index >= rhiTargets_.size() || !rhiTargets_[index]) {
            outError = "swapchain target missing";
            return false;
        }

        // M8.2 t0（S8-PLAN）：renderFrame 三段拆分——beginFrame（上一帧 GPU 结果
        // 的消费 + 本帧渲染准备）/ recordFrame（唯一 open/close/execute，frameCmd_
        // 唯一落点）/ endFrame（fence signal + 截图回读 + present）。纯搬移零逻辑
        // 变化；逻辑 tick / 插值 alpha 消费留在 viewer game loop（renderFrame 调用
        // 之前），beginFrame 不吞逻辑 tick（M-GL3 口径）。
        beginFrame(index, hud);
        nvrhi::StagingTextureHandle staging =
            recordFrame(index, color, frame, screenshotPath, hud);
        return endFrame(index, staging, screenshotPath, outError);
    }

private:
    // ---- M8.2 t0 段一：beginFrame = 回读消费只能发生在这里 ----
    // slot fence 等待、pipeline ensure、全部 timer poll/回读消费（GPU 整帧、cull
    // stats、BLAS/TLAS 耗时、TLAS 探针）、hud build（console/audio/inputDebug 三
    // 窗口，M-I2）、info_ 写回。「开列表前消费」从注释升级为结构防线。
    void beginFrame(UINT index, HudFrame* hud) {
        // ---- frame in flight = 2 同步安全网 ----
        // 先等 GPU 用完这个 swapchain slot 上一帧的内容，再往里记录新一帧。
        // 删掉这段 = CPU 在 GPU 仍持有该 backbuffer 时又 clear/draw → 同步报错（S0-PLAN 校验点）。
        if (fence_ && fenceEvent_ && fence_->GetCompletedValue() < frameFence_[index]) {
            fence_->SetEventOnCompletion(frameFence_[index], fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }

        ensurePipeline(index);
        // HUD 全链路都不带 hud 参数时完全不碰：M0.4 及更早的回放基线保持不变。
        // M6.2：GPU timer 段**移出 HUD 块**无条件 ensure+poll——真实整帧 GPU 时间
        // 的机器出口（DeviceInfo.lastGpuMs，--no-hud 也要能出帧时间）；timer query
        // 不写任何像素目标，画面基线不受影响（M6.2 收口跑回归验证 bytes 不变）。
        if (gpuTimer_.ensure(device_)) {
            float gpuMs = 0.0f;
            if (gpuTimer_.poll(gpuMs)) gpuLastMs_ = gpuMs;
            if (hud && hud->displayWidth > 0.0f) {
                // 截图回归模式：viewer 已填固定 GPU 样本，不写回真实计时，保基线 MD5 稳定。
                if (!hud->deterministic) hud->gpuMs = gpuLastMs_;
                hud->gpuTimingAvailable = true;
            }
        }
        if (hud && hud->displayWidth > 0.0f) {
            if (!framebuffers_.empty() && framebuffers_[0]) {
                std::string err;
                if (!hud_.ensurePipeline(device_, framebuffers_[0]->getFramebufferInfo(), err))
                    nl::core::log::deviceError("DX12", err);
            }
            // M4.2：cull pass 计时同理，必须在 hud_.build() 之前写回标志。
            scene_.pollCullTimer(device_, hud);
            // M8.0：BLAS 构建耗时（init 一次性；resolve 后恒定值）。
            scene_.pollBlasTimer(device_, hud);
            // M8.1：TLAS 每帧 rebuild 耗时（每帧 begin/end，非确定性模式显示最近值）。
            scene_.pollTlasTimer(device_, hud);
            // CPU 侧：ImGui 面板 + draw data，控件值写回 HudFrame。
            hud_.build(*hud);
        }
        // M4.1：开命令列表前消费两帧前的剔除计数回读（mapBuffer 零等待窗口）。
        scene_.pollCullStats(device_);
        // M8.1：TLAS 探针 readback（--tlas-probe 才有数据；同样开列表前消费）。
        scene_.pollTlasProbe(device_);
        info_.visibleClusters = scene_.visibleClusters();
        info_.visibleTriangles = scene_.visibleTriangles();   // M5.2
        info_.hizMipCount = scene_.hizMipCount();             // M6.0：hiz 模式非零
        info_.hizTopFar = scene_.hizTopFar();                 // M6.1：金字塔最高 mip（最远表面）
        info_.hizOccluded = scene_.hizOccluded();             // M6.2：被 Hi-Z 剔除簇节点数
        info_.lastGpuMs = gpuLastMs_;                         // M6.2：真实整帧 GPU 时间（机器出口）
        info_.instanceCount = scene_.instanceCount();         // M-E1b.0：ECS 场景实例数
        info_.particleAlive = scene_.particlesAlive();        // M-PT0：活粒子数（关闭恒 0）
    }

    // ---- M8.2 t0 段二：recordFrame = 帧列表生命周期唯一落点 ----
    // 唯一 open/close/execute 落点，frameCmd_ 只在这里取；timer begin、
    // buildTlasFrame、clear、scene/debugLines/hud draw、screenshot copy、timer end。
    // 其他地方拿不到裸列表。返回截图 staging 句柄（endFrame 回读消费）。
    nvrhi::StagingTextureHandle recordFrame(UINT index, ClearColor color,
                                            const FrameParams& frame,
                                            const char* screenshotPath,
                                            HudFrame* hud) {
        // M8.1：帧列表常驻复用（frameCmd_ 成员；每帧列表句柄析构会连带销毁 DXR
        // scratch manager 的 chunk——AS build ECL 在飞行中就被 final-release，
        // debug layer CORRUPTION #921 → 后续帧 fail-fast 0x87D，见成员声明注释）。
        nvrhi::ICommandList* cmd = frameCmd_;
        cmd->open();
        // M0.6：timer query 包住整帧 GPU 命令（clear + scene + hud）；
        // 找不到空闲 slot 时 begin 返回 false，本帧不计时、不报错。
        const bool gpuTiming = gpuTimer_.begin(cmd);
        // M8.1：TLAS 每帧全量 rebuild + 探针 dispatch（进帧命令列表——§2.8 帧内
        // 禁第二 cmd list，正好满足 0x87d 同列表纪律；BLAS 未就位时内部直接返回）。
        scene_.buildTlasFrame(cmd, frame);
        cmd->clearTextureFloat(rhiTargets_[index], nvrhi::TextureSubresourceSet(),
                               nvrhi::Color(color.r, color.g, color.b, color.a));

        // ---- M0.4：清深度 + 画带贴图的 Suzanne（MVP / 剔除开关来自 FrameParams）----
        if (depth_) {
            cmd->clearDepthStencilTexture(depth_, nvrhi::TextureSubresourceSet(),
                                          true, 1.0f, false, 0);
        }
        if (index < framebuffers_.size() && framebuffers_[index] && scene_.ready() && depth_) {
            scene_.draw(cmd, framebuffers_[index],
                        nvrhi::Viewport(float(extent_.width), float(extent_.height)), frame);
        }
        // M-P3：物理调试线框叠加层（LINE_LIST）。仅当 frame.debugLines 且数据非空才绘制，
        // 缺省零开销；画在主场景之后、HUD 之前（尊重已有深度，不写深度）。
        if (index < framebuffers_.size() && framebuffers_[index] && depth_) {
            scene_.drawDebugLines(cmd, framebuffers_[index],
                                 nvrhi::Viewport(float(extent_.width), float(extent_.height)), frame);
        }
        // M-PT0：粒子叠加层（instanced billboard，alpha blend、深度测试开写入关）。
        // 画在调试线框之后、HUD 之前；仅 frame.particles 开启时步进 + 绘制（缺省零开销）。
        if (index < framebuffers_.size() && framebuffers_[index] && depth_) {
            scene_.drawParticles(cmd, framebuffers_[index],
                                 nvrhi::Viewport(float(extent_.width), float(extent_.height)), frame);
        }
        // M0.5：HUD 叠在模型之后、截图回读之前——PNG 里能看到的就是用户看到的画面。
        if (hud && index < framebuffers_.size() && framebuffers_[index]) {
            hud_.draw(cmd, framebuffers_[index],
                      nvrhi::Viewport(float(extent_.width), float(extent_.height)));
        }

        nvrhi::StagingTextureHandle staging;
        if (screenshotPath) {
            const auto stagingDesc = nvrhi::TextureDesc()
                .setWidth(extent_.width)
                .setHeight(extent_.height)
                .setFormat(nvrhi::Format::RGBA8_UNORM)
                .setDebugName("ScreenshotReadback");
            staging = device_->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
            if (staging) {
                cmd->copyTexture(staging, nvrhi::TextureSlice(),
                                 rhiTargets_[index], nvrhi::TextureSlice());
            }
        }
        // M0.6：结束整帧计时（clear + scene + hud 都已记录）。
        if (gpuTiming) gpuTimer_.end(cmd);
        cmd->close();
        device_->executeCommandList(frameCmd_);
        return staging;
    }

    // ---- M8.2 t0 段三：endFrame = fence signal + 截图 staging 回读 + present ----
    bool endFrame(UINT index, nvrhi::StagingTextureHandle staging,
                  const char* screenshotPath, std::string& outError) {
        // ---- 给这个 slot 记一个 fence 值（提交之后）----
        if (fence_) {
            const UINT64 v = ++fenceValue_;
            queue_->Signal(fence_.Get(), v);
            frameFence_[index] = v;
        }

        bool ok = true;
        if (screenshotPath) {
            if (!staging) {
                outError = "staging texture for screenshot could not be created";
                ok = false;
            } else {
                std::size_t rowPitch = 0;
                void* pixels = device_->mapStagingTexture(
                    staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
                if (!pixels) {
                    outError = "mapStagingTexture returned null";
                    ok = false;
                } else {
                    ok = writePngRGBA8(screenshotPath, pixels, extent_.width, extent_.height,
                                       static_cast<std::uint32_t>(rowPitch));
                    if (!ok) outError = std::string("writePngRGBA8 failed: ") + screenshotPath;
                    device_->unmapStagingTexture(staging);
                }
            }
        }

        const HRESULT hr = swapchain_->Present(0, 0);
        if (FAILED(hr)) {
            outError = hrMessage("Present", hr);
            ok = false;
        }
        // 不再每帧全量 waitForIdle（那是 frame-in-flight=1）。等待交给上面的 slot fence，
        // GC 不阻塞。
        device_->runGarbageCollection();
        return ok;
    }

public:
    int drainMessages() override {
        int errors = 0;
        if (infoQueue_) {
            const UINT64 count = infoQueue_->GetNumStoredMessages();
            if (count) nl::core::log::debug("D3D12 info queue messages: ", count);
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T bytes = 0;
                if (FAILED(infoQueue_->GetMessage(i, nullptr, &bytes))) continue;
                std::vector<char> buffer(bytes);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                if (FAILED(infoQueue_->GetMessage(i, msg, &bytes))) continue;
                const std::string text = msg->pDescription ? msg->pDescription : "<null>";
                if (msg->Severity >= D3D12_MESSAGE_SEVERITY_ERROR) {
                    nl::core::log::deviceError("D3D12", text);
                    ++errors;
                } else {
                    nl::core::log::debug("D3D12: ", text);
                }
            }
            if (count) infoQueue_->ClearStoredMessages();
        }
        return errors;
    }

    const DeviceInfo& info() const override { return info_; }
    Extent extent() const override { return extent_; }

private:
    // D3D12 Debug Layer 在 backbuffer 仍有 Command Queue 在途工作时被释放会抛
    // 0x87d / EXECUTION ERROR #921 OBJECT_DELETED_WHILE_STILL_IN_USE。
    // NVRHI 的 waitForIdle() 只等它自己提交的 fence 序列；这里直接对原生
    // command queue 做一次 Signal + CPU 等待，保证所有 backbuffer 的工作都结束。
    void flushGpu() {
        if (!queue_ || !device12_) return;
        if (!fence_) {
            if (FAILED(device12_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                              IID_PPV_ARGS(&fence_)))) return;
            fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        if (!fence_ || !fenceEvent_) return;
        const UINT64 value = ++fenceValue_;
        if (FAILED(queue_->Signal(fence_.Get(), value))) return;
        if (fence_->GetCompletedValue() < value) {
            fence_->SetEventOnCompletion(value, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    void reportFeatures() {
        info_.rayQuery = device_->queryFeatureSupport(nvrhi::Feature::RayQuery);

        D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9{};
        if (SUCCEEDED(device12_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9,
                                                     &options9, sizeof(options9)))) {
            info_.shaderInt64Atomics =
                options9.AtomicInt64OnTypedResourceSupported == TRUE ||
                options9.AtomicInt64OnGroupSharedSupported == TRUE;
        }
        // M7.0（S7）：Options9 的两位是 typed resource（R64 纹理）与 group shared；
        // 我们走 RWStructuredBuffer<uint64_t>，对应 Options11 的 descriptor heap
        // resource 位——这一位才是 vis buffer 真正的硬件门槛，必须单独查。
        D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11{};
        if (SUCCEEDED(device12_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11,
                                                     &options11, sizeof(options11)))) {
            info_.shaderInt64Atomics =
                info_.shaderInt64Atomics ||
                options11.AtomicInt64OnDescriptorHeapResourceSupported == TRUE;
        }
        info_.indirectDraw = true;  // ExecuteIndirect / DrawIndirect 是 DX12 核心能力

        nl::core::log::info("features: rayQuery=", info_.rayQuery ? 1 : 0,
                            " shaderInt64Atomics=", info_.shaderInt64Atomics ? 1 : 0,
                            " indirectDraw=", info_.indirectDraw ? 1 : 0);
        if (!info_.rayQuery || !info_.shaderInt64Atomics) {
            nl::core::log::warn("feature report has unsupported items; S7/S8 需按此能力调整");
        }
    }

    HRESULT createSwapChain() {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = extent_.width;
        desc.Height = extent_.height;
        desc.Format = kSwapFormat;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBufferCount;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        ComPtr<IDXGISwapChain1> chain1;
        const HRESULT hr = factory_->CreateSwapChainForHwnd(
            queue_.Get(), hwnd_, &desc, nullptr, nullptr, &chain1);
        if (FAILED(hr)) return hr;
        return chain1->QueryInterface(IID_PPV_ARGS(&swapchain_));
    }

    bool createTargets(std::string& outError) {
        rhiTargets_.clear();
        nativeTargets_.clear();
        rhiTargets_.reserve(kBufferCount);
        nativeTargets_.reserve(kBufferCount);

        for (UINT i = 0; i < kBufferCount; ++i) {
            ComPtr<ID3D12Resource> resource;
            const HRESULT hr = swapchain_->GetBuffer(i, IID_PPV_ARGS(&resource));
            if (FAILED(hr)) { outError = hrMessage("GetBuffer", hr); return false; }

            nvrhi::TextureDesc textureDesc;
            textureDesc.width = extent_.width;
            textureDesc.height = extent_.height;
            textureDesc.format = nvrhi::Format::RGBA8_UNORM;
            textureDesc.debugName = "SwapChainBuffer";
            textureDesc.isRenderTarget = true;
            textureDesc.isUAV = false;
            textureDesc.initialState = nvrhi::ResourceStates::Present;
            textureDesc.keepInitialState = true;

            auto handle = device_->createHandleForNativeTexture(
                nvrhi::ObjectTypes::D3D12_Resource,
                nvrhi::Object(resource.Get()), textureDesc);
            if (!handle) { outError = "createHandleForNativeTexture failed"; return false; }
            rhiTargets_.push_back(handle);
            nativeTargets_.push_back(resource);
        }
        // frame-in-flight 的 slot fence 表必须在这里初始化：renderFrame 在调用
        // ensurePipeline() 之前就要按 slot 读它，放在懒建流程里会首帧越界。
        framebuffers_.assign(rhiTargets_.size(), nullptr);
        frameFence_.assign(rhiTargets_.size(), 0);

        // M0.4 深度缓冲：随 swapchain 尺寸一起重建，为每个 framebuffer 提供 depth attachment。
        nvrhi::TextureDesc depthDesc;
        depthDesc.width = extent_.width;
        depthDesc.height = extent_.height;
        depthDesc.format = nvrhi::Format::D32;
        depthDesc.debugName = "SceneDepth";
        depthDesc.isRenderTarget = true;
        depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
        depthDesc.keepInitialState = true;
        depthDesc.setClearValue(nvrhi::Color(1.0f));
        depth_ = device_->createTexture(depthDesc);
        if (!depth_) { outError = "createTexture failed for depth buffer"; return false; }
        return true;
    }

    void shutdown() {
        flushGpu();
        if (device_) {
            device_->waitForIdle();
            device_->runGarbageCollection();
        }
        // 场景 / HUD 资源（含 binding set / pipeline）必须在 device 仍存活时释放。
        scene_.shutdown();
        hud_.shutdown();
        gpuTimer_.shutdown();   // M0.6：device 仍存活时释放 timer query
        for (auto& fb : framebuffers_) fb = nullptr;
        depth_ = nullptr;
        rhiTargets_.clear();
        nativeTargets_.clear();
        device_.Reset();
        rawDevice_.Reset();

        // 严格按创建的逆序销毁，swapchain 必须在 device / queue / factory 之前释放，
        // debug controller 留到最后（它挂的是进程级 hook）。
        swapchain_.Reset();
        infoQueue_.Reset();
        queue_.Reset();
        device12_.Reset();
        factory_.Reset();
        fence_.Reset();
        if (fenceEvent_) { CloseHandle(fenceEvent_); fenceEvent_ = nullptr; }
    }

    // M0.4：懒建 framebuffer（color + depth）与两套 pipeline（开/关背面剔除）。
    void ensurePipeline(UINT index);

    HWND hwnd_ = nullptr;
    Extent extent_{};
    DeviceInfo info_{};
    std::string lodStem_;              // M2.3：<stem>.lodN.nmesh 的 stem
    int lodLevelCount_ = 0;            // 0 = 没有分层资产
    int dagGroupCount_ = 0;            // M3.2：可下钻组数（第二粗层 dagGroups）
    NvrhiMessageCallback messageCallback_;

    ComPtr<IDXGIFactory4>            factory_;
    ComPtr<ID3D12Device>             device12_;
    ComPtr<ID3D12CommandQueue>       queue_;
    ComPtr<IDXGISwapChain3>          swapchain_;
    ComPtr<ID3D12InfoQueue>          infoQueue_;
    ComPtr<ID3D12Fence>              fence_;
    HANDLE                           fenceEvent_ = nullptr;
    UINT64                           fenceValue_ = 0;
    std::vector<ComPtr<ID3D12Resource>> nativeTargets_;

    nvrhi::DeviceHandle rawDevice_;
    nvrhi::DeviceHandle device_;
    std::vector<nvrhi::TextureHandle> rhiTargets_;

    // M8.1：帧命令列表常驻复用（NVRHI 官方用法：一个提交线程一个列表跟踪器）。
    // ⚠️ 不能每帧新建——局部列表句柄析构时其 DXR scratch manager 随之销毁，AS build
    // 的 scratch chunk（DEFAULT 堆）在 ECL 飞行中被 final-release → debug layer
    // CORRUPTION #921（仅 OutputDebugString 可见）→ 后续帧 scratch 复用被释放页 →
    // 执行期 fail-fast 0x87D（M8.1 帧三必炸的根因，DBWIN 抓的现场）。
    nvrhi::CommandListHandle frameCmd_;

    // ---- M0.4 场景资源：glTF 模型 / 贴图 / pipeline 由共享 Scene 管理 ----
    Scene                                 scene_;
    Hud                                   hud_;
    GpuTimer                              gpuTimer_;
    float                                 gpuLastMs_ = 0.0f;  // M6.2：最近一次 resolve 的整帧 GPU 时间
    nvrhi::TextureHandle                  depth_;
    std::vector<nvrhi::FramebufferHandle> framebuffers_;   // 每个 swapchain slot 一个
    std::vector<UINT64>                   frameFence_;     // frame-in-flight=2 同步安全网
};

    void Dx12Backend::ensurePipeline(UINT index) {
        if (framebuffers_.size() != rhiTargets_.size())
            framebuffers_.assign(rhiTargets_.size(), nullptr);
        if (frameFence_.size() != rhiTargets_.size())
            frameFence_.assign(rhiTargets_.size(), 0);

        if (index < framebuffers_.size() && !framebuffers_[index] && depth_) {
            nvrhi::FramebufferDesc fbDesc;
            fbDesc.addColorAttachment(rhiTargets_[index]);
            fbDesc.setDepthAttachment(depth_);
            framebuffers_[index] = device_->createFramebuffer(fbDesc);
        }
        // pipeline 绑定 framebuffer 格式（color + depth），建一次即可；resize 后由
        // scene_.invalidatePipelines() 触发重建。两套分别对应开/关背面剔除。
        if (!scene_.ready() && !framebuffers_.empty() && framebuffers_[0]) {
            std::string err;
            if (!scene_.ensurePipelines(device_, framebuffers_[0]->getFramebufferInfo(), err))
                nl::core::log::deviceError("DX12", err);
        }
    }

} // namespace (anonymous)

std::unique_ptr<Backend> createBackend() {
    return std::make_unique<Dx12Backend>();
}

} // namespace nl::renderer::detail

#endif // NL_BACKEND_DX12
