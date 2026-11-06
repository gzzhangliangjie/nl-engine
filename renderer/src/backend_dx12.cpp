#include "backend.hpp"

#if defined(NL_BACKEND_DX12)

#include "nl/core/log.hpp"

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

        reportFeatures();

        hr = createSwapChain();
        if (FAILED(hr)) { outError = hrMessage("CreateSwapChainForHwnd", hr); return false; }
        if (!createTargets(outError)) return false;

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

    bool renderFrame(ClearColor color, const char* screenshotPath,
                     std::string& outError) override {
        if (!swapchain_ || !device_) { outError = "renderer not initialized"; return false; }
        const UINT index = swapchain_->GetCurrentBackBufferIndex();
        if (index >= rhiTargets_.size() || !rhiTargets_[index]) {
            outError = "swapchain target missing";
            return false;
        }

        nvrhi::CommandListHandle cmd = device_->createCommandList();
        cmd->open();
        cmd->clearTextureFloat(rhiTargets_[index], nvrhi::TextureSubresourceSet(),
                               nvrhi::Color(color.r, color.g, color.b, color.a));

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
        cmd->close();
        device_->executeCommandList(cmd);
        device_->waitForIdle();

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

        // M0.2 用全量 flush 做保守同步；M0.3 再换 frame in flight = 2 的 fence/semaphore 模型。
        const HRESULT hr = swapchain_->Present(0, 0);
        if (FAILED(hr)) {
            outError = hrMessage("Present", hr);
            ok = false;
        }
        device_->waitForIdle();
        flushGpu();  // 必须等 Command Queue 上所有 backbuffer 的工作真的结束
        device_->runGarbageCollection();
        return ok;
    }

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
        return true;
    }

    void shutdown() {
        flushGpu();
        if (device_) {
            device_->waitForIdle();
            device_->runGarbageCollection();
        }
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

    HWND hwnd_ = nullptr;
    Extent extent_{};
    DeviceInfo info_{};
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
};

} // namespace (anonymous)

std::unique_ptr<Backend> createBackend() {
    return std::make_unique<Dx12Backend>();
}

} // namespace nl::renderer::detail

#endif // NL_BACKEND_DX12
