#include "backend.hpp"
#include "scene.hpp"
#include "hud.hpp"
#include "gpu_timer.hpp"

#if defined(NL_BACKEND_VULKAN)

#include "nl/core/log.hpp"
#include "nl/core/meshlet_io.hpp"   // M3.2：探测第二粗层 dagGroups 组数

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>   // 拉入 vulkan.h（C 原型），提供全部 PFN_vk* 函数指针类型
#include <vulkan/vulkan.h>

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace nl::renderer::detail {
namespace {

constexpr uint32_t kBufferCount = 2;

std::string vkResultStr(VkResult r) {
    std::ostringstream os;
    os << "VkResult " << static_cast<int>(r);
    return os.str();
}

// 从 loader 动态解析一个 Vulkan 函数指针。
template <class F>
F loadProc(PFN_vkGetInstanceProcAddr getProc, VkInstance inst, const char* name) {
    return reinterpret_cast<F>(getProc(inst, name));
}

// NVRHI 的消息回调接进 core 日志；错误信息独立计数，验收要求为 0。
// Vulkan 侧没有 Khronos VVL（本机不装厂商 SDK），NVRHI 自身的状态校验仍走这里。
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

// 命名约定（本文件内一律这么区分，避免 vkDev_ / vkDevice_ 那种看串的坑）：
//   vulkan*_  → 原生 Vulkan 句柄 / 对象（VkDevice、VkInstance、VkSwapchainKHR …）
//   nvrhi*_   → NVRHI 抽象层对象（nvrhi::vulkan::IDevice*、TextureHandle …）
//   vk*_      → 从 loader 运行时解析的 Vulkan 函数指针（本机无 vulkan-1.lib，禁止静态链接）
class VkBackend final : public Backend {
public:
    ~VkBackend() override { shutdown(); }

    bool init(void* nativeWindow, Extent extent, bool enableDebugLayer,
              const char* modelFile, bool clusterMode, bool cullMode,
              bool frustumCull, bool skinMode, std::string& outError) override {
        if (!nativeWindow || extent.width == 0 || extent.height == 0) {
            outError = "invalid window handle or extent";
            return false;
        }
        hwnd_ = static_cast<HWND>(nativeWindow);
        extent_ = extent;

        // Vulkan loader（vulkan-1.dll）由显卡驱动自带，本机没有 vulkan-1.lib 导入库，
        // 故所有 Vulkan 函数都在运行时从 loader 解析。调试层：Khronos VVL 需要厂商 SDK，
        // 本架构不装，故 Vulkan 调试能力 = NVRHI 自身校验层。
        info_.debugLayer = enableDebugLayer
            // ASCII only: M0.5 的 ImGui 默认字体没有中文字形，HUD 直接显示这个字段。
            ? "NVRHI validation (Vulkan, no VVL, zero vendor SDK)"
            : "NVRHI validation only";
        info_.debugLayerActive = enableDebugLayer;

        if (!createInstance(outError)) return false;
        if (!createSurface(outError)) return false;
        if (!pickPhysicalDevice(outError)) return false;
        if (!createDevice(outError)) return false;

        reportFeatures();

        if (!createSwapchain(outError)) return false;
        if (!createTargets(outError)) return false;
        // M4.0：vertex pulling 走 initCull。Vulkan 侧 SPIR-V 入口恒为 "main"（加载期
        // setEntryName 即 SPIR-V pName，见 docs/SHADER-CONVENTIONS.md §2.4）。
        const bool sceneOk = skinMode
            ? scene_.initSkin(nvrhiDevice_, "spv", "main", "main", modelFile, outError)
            : (clusterMode
            ? scene_.initCluster(nvrhiDevice_, "spv", "main", "main", modelFile, outError)
            : (cullMode
                ? scene_.initCull(nvrhiDevice_, "spv", "main", "main", modelFile, frustumCull, outError)
                : scene_.init(nvrhiDevice_, "spv", "main", "main", modelFile, outError)));
        if (!sceneOk) return false;
        if (!hud_.init(nvrhiDevice_, "spv", "main", "main", outError)) return false;
        info_.modelName = scene_.modelName();
        info_.triangleCount = scene_.triangleCount();
        info_.visibleClusters = scene_.visibleClusters();     // M4.1：-1 = 暂无数据
        info_.totalClusters = scene_.clusterCount();
        // M2.3：分层资产探测（与 DX12 同一份公共函数，规则不会漂移）。cull 模式不依赖分层资产。
        if (clusterMode) {
            lodStem_ = meshletStem(modelFile);
            lodLevelCount_ = probeLodLevelCount(lodStem_);
            info_.lodLevelCount = lodLevelCount_;
            info_.lodLevel = scene_.lodLevel();
            info_.lodMaxError = scene_.lodMaxError();
            nl::core::log::info("lod layers: ", lodLevelCount_, " | stem: ", lodStem_);
            // M3.2：可下钻组数 = 第二粗层 dagGroups（与 DX12 同一条探测逻辑）。
            if (lodLevelCount_ >= 2) {
                const std::string childFile = lodMeshFileName(lodStem_, lodLevelCount_ - 2);
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
        if (!nvrhiDevice_ || !vulkanSwapchain_) return;

        // 顺序照 Donut / DX12：先等所有在途工作结束，再清 NVRHI 引用，
        // 最后才销毁旧 swapchain 重建。反序会让 backbuffer 在途引用未释放。
        nvrhiDevice_->waitForIdle();
        nvrhiDevice_->runGarbageCollection();
        scene_.invalidatePipelines();
        hud_.invalidatePipeline();
        for (auto& fb : framebuffers_) fb = nullptr;
        depth_ = nullptr;
        rhiTargets_.clear();
        nativeTargets_.clear();

        extent_ = extent;
        if (!recreateSwapchain()) {
            nl::core::log::deviceError("Vulkan", "resize: recreateSwapchain failed");
            return;
        }
        std::string unused;
        createTargets(unused);
        nl::core::log::info("resize -> ", extent_.width, "x", extent_.height);
    }

    // M2.3：切 LOD 层——只重传 VB/IB，pipeline / binding 不动；失败保持原层。
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
        const std::string file = lodMeshFileName(lodStem_, level);
        if (!scene_.setClusterLevel(nvrhiDevice_, file.c_str(), outError)) return false;
        info_.modelName = scene_.modelName();
        info_.triangleCount = scene_.triangleCount();
        info_.lodLevel = scene_.lodLevel();
        info_.lodMaxError = scene_.lodMaxError();
        info_.dagGroup = scene_.dagGroup();
        info_.dagGroupError = scene_.dagGroupError();
        nl::core::log::info("lod -> ", file, " | triangles=", info_.triangleCount,
                            " maxError=", info_.lodMaxError);
        return true;
    }

    // M3.2：dag-top 组原子下钻（与 DX12 同语义）。
    bool setDagCut(int group, std::string& outError) override {
        if (lodLevelCount_ <= 0) {
            outError = "no layered .nmesh assets (run: nanite-tools lod <base>.nmesh -o <dir>/<stem>)";
            return false;
        }
        const int top = lodLevelCount_ - 1;
        if (group < 0) {
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
        const std::string topFile = lodMeshFileName(lodStem_, top);
        const std::string childFile = lodMeshFileName(lodStem_, top - 1);
        if (!scene_.setDagCut(nvrhiDevice_, topFile.c_str(), childFile.c_str(), group, outError)) {
            return false;
        }
        info_.modelName = scene_.modelName();
        info_.triangleCount = scene_.triangleCount();
        info_.lodLevel = scene_.lodLevel();
        info_.lodMaxError = scene_.lodMaxError();
        info_.dagGroup = scene_.dagGroup();
        info_.dagGroupError = scene_.dagGroupError();
        return true;
    }

    bool renderFrame(ClearColor color, const FrameParams& frame,
                     const char* screenshotPath, HudFrame* hud,
                     std::string& outError) override {
        if (!nvrhiDevice_ || !vulkanSwapchain_) { outError = "renderer not initialized"; return false; }

        const VkResult acq = vkAcquireNextImageKHR_(vulkanDevice_, vulkanSwapchain_, ~0ULL,
                                                    vulkanImageAvailableSem_, VK_NULL_HANDLE, &vulkanImageIndex_);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!recreateSwapchain()) { outError = "acquire: swapchain out of date, recreate failed"; return false; }
            std::string unused; createTargets(unused);
            return true; // 下一帧再画
        }
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
            outError = std::string("vkAcquireNextImageKHR failed: ") + vkResultStr(acq);
            return false;
        }

        // ---- frame in flight 同步安全网 ----
        // acquire 信号 imageAvailableSem；NVRHI 在下一次 submit 时等它。
        // 删掉这段等待 = 往仍被 present 占用的 image 里画 → 同步报错（S0-PLAN 校验点）。
        nvrhiDevice_->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, vulkanImageAvailableSem_, 0);

        ensurePipeline(vulkanImageIndex_);
        // HUD 全链路都不带 hud 参数时完全不碰：M0.4 及更早的回放基线保持不变。
        if (hud && hud->displayWidth > 0.0f) {
            if (!framebuffers_.empty() && framebuffers_[0]) {
                std::string err;
                if (!hud_.ensurePipeline(nvrhiDevice_, framebuffers_[0]->getFramebufferInfo(), err))
                    nl::core::log::deviceError("Vulkan", err);
            }
            // M0.6：先轮询上一帧的 timer query（异步计时天然有 1~2 帧延迟），
            // 再开始本帧；建 query 延迟到第一次需要 HUD 的帧，--no-hud 完全不碰。
            if (gpuTimer_.ensure(nvrhiDevice_)) {
                float gpuMs = 0.0f;
                // 截图回归模式：viewer 已填固定 GPU 样本，不写回真实计时，保基线 MD5 稳定。
                if (!hud->deterministic && gpuTimer_.poll(gpuMs)) hud->gpuMs = gpuMs;
                hud->gpuTimingAvailable = true;
            }
            // M4.2：cull pass 计时同理，必须在 hud_.build() 之前写回标志。
            scene_.pollCullTimer(nvrhiDevice_, hud);
            hud_.build(*hud);
        }
        // M4.1：开命令列表前消费两帧前的剔除计数回读（mapBuffer 零等待窗口）。
        scene_.pollCullStats(nvrhiDevice_);
        info_.visibleClusters = scene_.visibleClusters();

        nvrhi::CommandListHandle cmd = nvrhiDevice_->createCommandList();
        cmd->open();
        // M0.6：timer query 包住整帧 GPU 命令（clear + scene + hud）；
        // 找不到空闲 slot 时 begin 返回 false，本帧不计时、不报错。
        const bool gpuTiming = gpuTimer_.begin(cmd);
        cmd->clearTextureFloat(rhiTargets_[vulkanImageIndex_], nvrhi::TextureSubresourceSet(),
                               nvrhi::Color(color.r, color.g, color.b, color.a));

        // ---- M0.4：清深度 + 画带贴图的 Suzanne（MVP / 剔除开关来自 FrameParams）----
        if (depth_) {
            cmd->clearDepthStencilTexture(depth_, nvrhi::TextureSubresourceSet(),
                                          true, 1.0f, false, 0);
        }
        if (vulkanImageIndex_ < framebuffers_.size() && framebuffers_[vulkanImageIndex_]
            && scene_.ready() && depth_) {
            scene_.draw(cmd, framebuffers_[vulkanImageIndex_],
                        nvrhi::Viewport(float(extent_.width), float(extent_.height)), frame);
        }
        // M0.5：HUD 叠在模型之后、截图回读之前——PNG 里能看到的就是用户看到的画面。
        if (hud && vulkanImageIndex_ < framebuffers_.size() && framebuffers_[vulkanImageIndex_]) {
            hud_.draw(cmd, framebuffers_[vulkanImageIndex_],
                      nvrhi::Viewport(float(extent_.width), float(extent_.height)));
        }

        nvrhi::StagingTextureHandle staging;
        if (screenshotPath) {
            const auto stagingDesc = nvrhi::TextureDesc()
                .setWidth(extent_.width)
                .setHeight(extent_.height)
                .setFormat(vulkanSwapFormat_)
                .setDebugName("ScreenshotReadback");
            staging = nvrhiDevice_->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
            if (staging) {
                cmd->copyTexture(staging, nvrhi::TextureSlice(),
                                 rhiTargets_[vulkanImageIndex_], nvrhi::TextureSlice());
            }
        }
        // M0.6：结束整帧计时（clear + scene + hud 都已记录）。
        if (gpuTiming) gpuTimer_.end(cmd);
        cmd->close();
        nvrhiDevice_->executeCommandList(cmd);

        bool ok = true;
        if (screenshotPath) {
            if (!staging) {
                outError = "staging texture for screenshot could not be created";
                ok = false;
            } else {
                std::size_t rowPitch = 0;
                void* pixels = nvrhiDevice_->mapStagingTexture(
                    staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
                if (!pixels) {
                    outError = "mapStagingTexture returned null";
                    ok = false;
                } else {
                    ok = writeReadback(pixels, static_cast<std::uint32_t>(rowPitch), screenshotPath);
                    if (!ok) outError = std::string("writePngRGBA8 failed: ") + screenshotPath;
                    nvrhiDevice_->unmapStagingTexture(staging);
                }
            }
        }

        // NVRHI 缓冲了 semaphore 信号；用一次空 submit 真正触发 renderFinishedSem 信号。
        nvrhiDevice_->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, vulkanRenderFinishedSem_, 0);
        nvrhiDevice_->executeCommandLists(nullptr, 0);

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &vulkanRenderFinishedSem_;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &vulkanSwapchain_;
        presentInfo.pImageIndices = &vulkanImageIndex_;
        const VkResult pres = vkQueuePresentKHR_(vulkanPresentQueue_, &presentInfo);
        if (pres == VK_ERROR_OUT_OF_DATE_KHR) {
            // swapchain 已失效（resize / 窗口状态变化等）。重建后下一帧重试，不计入错误。
            // 这与 acquire 阶段的 OUT_OF_DATE 处理一致，是标准 Vulkan 实践。
            recreateSwapchain();
            std::string unused; createTargets(unused);
            return true;
        }
        if (pres != VK_SUCCESS && pres != VK_SUBOPTIMAL_KHR) {
            outError = std::string("vkQueuePresentKHR failed: ") + vkResultStr(pres);
            ok = false;
        }

        // 不再每帧 waitForIdle（那是 frame-in-flight=1）。Vulkan 的 frame-in-flight=2
        // 由 acquire/present semaphore 门控：最多 swapchain 张数（2~3）帧在途，CPU 不空等。
        nvrhiDevice_->runGarbageCollection();
        return ok;
    }

    // 无 VVL（零厂商 SDK），NVRHI 错误已由回调计入 log::deviceError；这里返回 0 不重复计数。
    int drainMessages() override { return 0; }

    const DeviceInfo& info() const override { return info_; }
    Extent extent() const override { return extent_; }

private:
    bool createInstance(std::string& outError) {
        // loader 由驱动自带，运行时加载，避免依赖 vulkan-1.lib 导入库（本机没有）。
        vulkanModule_ = LoadLibraryW(L"vulkan-1.dll");
        if (!vulkanModule_) {
            outError = "failed to LoadLibrary vulkan-1.dll (Vulkan loader 缺失？)";
            return false;
        }
        getInstanceProcAddr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(vulkanModule_, "vkGetInstanceProcAddr"));
        if (!getInstanceProcAddr_) {
            outError = "vkGetInstanceProcAddr not found in vulkan-1.dll";
            return false;
        }
        // vkCreateInstance 是 loader 极少数静态导出的函数之一，可经 NULL instance 取得。
        vkCreateInstance_ = loadProc<PFN_vkCreateInstance>(getInstanceProcAddr_, nullptr, "vkCreateInstance");

        const char* instanceExts[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "nl-engine";
        appInfo.applicationVersion = 1;
        appInfo.pEngineName = "nl-engine";
        appInfo.engineVersion = 1;
        appInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo instInfo{};
        instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instInfo.pApplicationInfo = &appInfo;
        instInfo.enabledLayerCount = 0;
        instInfo.enabledExtensionCount = sizeof(instanceExts) / sizeof(instanceExts[0]);
        instInfo.ppEnabledExtensionNames = instanceExts;

        const VkResult r = vkCreateInstance_(&instInfo, nullptr, &vulkanInstance_);
        if (r != VK_SUCCESS) {
            outError = std::string("vkCreateInstance failed: ") + vkResultStr(r);
            return false;
        }
        // 有了 instance，把其余 Vulkan 函数（核心 + KHR 扩展）一次性解析出来。
        loadRemaining();
        return true;
    }

    // 用 instance 解析除 vkCreateInstance 之外的全部 Vulkan 函数指针。
    void loadRemaining() {
        vkDestroyInstance_      = loadProc<PFN_vkDestroyInstance>(getInstanceProcAddr_, vulkanInstance_, "vkDestroyInstance");
        vkEnumeratePhysicalDevices_ = loadProc<PFN_vkEnumeratePhysicalDevices>(getInstanceProcAddr_, vulkanInstance_, "vkEnumeratePhysicalDevices");
        vkGetPhysicalDeviceProperties_ = loadProc<PFN_vkGetPhysicalDeviceProperties>(getInstanceProcAddr_, vulkanInstance_, "vkGetPhysicalDeviceProperties");
        vkGetPhysicalDeviceQueueFamilyProperties_ = loadProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(getInstanceProcAddr_, vulkanInstance_, "vkGetPhysicalDeviceQueueFamilyProperties");
        vkGetPhysicalDeviceSurfaceSupportKHR_ = loadProc<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(getInstanceProcAddr_, vulkanInstance_, "vkGetPhysicalDeviceSurfaceSupportKHR");
        vkCreateWin32SurfaceKHR_ = loadProc<PFN_vkCreateWin32SurfaceKHR>(getInstanceProcAddr_, vulkanInstance_, "vkCreateWin32SurfaceKHR");
        vkCreateDevice_        = loadProc<PFN_vkCreateDevice>(getInstanceProcAddr_, vulkanInstance_, "vkCreateDevice");
        vkGetDeviceQueue_      = loadProc<PFN_vkGetDeviceQueue>(getInstanceProcAddr_, vulkanInstance_, "vkGetDeviceQueue");
        vkGetPhysicalDeviceFeatures2_ = loadProc<PFN_vkGetPhysicalDeviceFeatures2>(getInstanceProcAddr_, vulkanInstance_, "vkGetPhysicalDeviceFeatures2");
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR_ = loadProc<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(getInstanceProcAddr_, vulkanInstance_, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        vkGetPhysicalDeviceSurfaceFormatsKHR_ = loadProc<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(getInstanceProcAddr_, vulkanInstance_, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        vkCreateSwapchainKHR_  = loadProc<PFN_vkCreateSwapchainKHR>(getInstanceProcAddr_, vulkanInstance_, "vkCreateSwapchainKHR");
        vkCreateSemaphore_     = loadProc<PFN_vkCreateSemaphore>(getInstanceProcAddr_, vulkanInstance_, "vkCreateSemaphore");
        vkDestroySemaphore_    = loadProc<PFN_vkDestroySemaphore>(getInstanceProcAddr_, vulkanInstance_, "vkDestroySemaphore");
        vkDestroySwapchainKHR_ = loadProc<PFN_vkDestroySwapchainKHR>(getInstanceProcAddr_, vulkanInstance_, "vkDestroySwapchainKHR");
        vkGetSwapchainImagesKHR_ = loadProc<PFN_vkGetSwapchainImagesKHR>(getInstanceProcAddr_, vulkanInstance_, "vkGetSwapchainImagesKHR");
        vkAcquireNextImageKHR_ = loadProc<PFN_vkAcquireNextImageKHR>(getInstanceProcAddr_, vulkanInstance_, "vkAcquireNextImageKHR");
        vkQueuePresentKHR_     = loadProc<PFN_vkQueuePresentKHR>(getInstanceProcAddr_, vulkanInstance_, "vkQueuePresentKHR");
        vkDestroySurfaceKHR_   = loadProc<PFN_vkDestroySurfaceKHR>(getInstanceProcAddr_, vulkanInstance_, "vkDestroySurfaceKHR");
        vkDestroyDevice_       = loadProc<PFN_vkDestroyDevice>(getInstanceProcAddr_, vulkanInstance_, "vkDestroyDevice");
    }

    bool createSurface(std::string& outError) {
        VkWin32SurfaceCreateInfoKHR surfInfo{};
        surfInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        surfInfo.hinstance = GetModuleHandle(nullptr);
        surfInfo.hwnd = hwnd_;
        const VkResult r = vkCreateWin32SurfaceKHR_(vulkanInstance_, &surfInfo, nullptr, &vulkanSurface_);
        if (r != VK_SUCCESS) {
            outError = std::string("vkCreateWin32SurfaceKHR failed: ") + vkResultStr(r);
            return false;
        }
        return true;
    }

    bool pickPhysicalDevice(std::string& outError) {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices_(vulkanInstance_, &count, nullptr);
        if (count == 0) { outError = "no Vulkan physical devices"; return false; }
        std::vector<VkPhysicalDevice> phys(count);
        vkEnumeratePhysicalDevices_(vulkanInstance_, &count, phys.data());

        for (VkPhysicalDevice p : phys) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties_(p, &props);
            uint32_t qfCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties_(p, &qfCount, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qfCount);
            vkGetPhysicalDeviceQueueFamilyProperties_(p, &qfCount, qf.data());

            uint32_t g = UINT32_MAX;
            uint32_t pres = UINT32_MAX;
            for (uint32_t i = 0; i < qfCount; ++i) {
                if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) g = i;
                VkBool32 support = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR_(p, i, vulkanSurface_, &support);
                if (support == VK_TRUE) pres = i;
            }
            if (g != UINT32_MAX && pres != UINT32_MAX) {
                vulkanPhysicalDevice_ = p;
                vulkanGraphicsFamily_ = g;
                vulkanPresentFamily_ = pres;
                info_.adapterName = props.deviceName;
                return true;
            }
        }
        outError = "no Vulkan physical device with graphics + present support";
        return false;
    }

    bool createDevice(std::string& outError) {
        const float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> qcis;
        VkDeviceQueueCreateInfo gqci{};
        gqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        gqci.queueFamilyIndex = vulkanGraphicsFamily_;
        gqci.queueCount = 1;
        gqci.pQueuePriorities = &priority;
        qcis.push_back(gqci);
        if (vulkanPresentFamily_ != vulkanGraphicsFamily_) {
            VkDeviceQueueCreateInfo pqci{};
            pqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            pqci.queueFamilyIndex = vulkanPresentFamily_;
            pqci.queueCount = 1;
            pqci.pQueuePriorities = &priority;
            qcis.push_back(pqci);
        }

        const char* deviceExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        VkPhysicalDeviceFeatures features{}; // 默认特性足够画一个清屏三角形

        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
        dci.pQueueCreateInfos = qcis.data();
        dci.enabledExtensionCount = sizeof(deviceExts) / sizeof(deviceExts[0]);
        dci.ppEnabledExtensionNames = deviceExts;
        dci.pEnabledFeatures = &features;

        const VkResult r = vkCreateDevice_(vulkanPhysicalDevice_, &dci, nullptr, &vulkanDevice_);
        if (r != VK_SUCCESS) {
            outError = std::string("vkCreateDevice failed: ") + vkResultStr(r);
            return false;
        }
        vkGetDeviceQueue_(vulkanDevice_, vulkanGraphicsFamily_, 0, &vulkanGraphicsQueue_);
        vulkanPresentQueue_ = (vulkanPresentFamily_ == vulkanGraphicsFamily_) ? vulkanGraphicsQueue_ : VK_NULL_HANDLE;
        if (!vulkanPresentQueue_) vkGetDeviceQueue_(vulkanDevice_, vulkanPresentFamily_, 0, &vulkanPresentQueue_);

        nvrhi::vulkan::DeviceDesc desc;
        desc.errorCB = &nvrhiMessageCallback_;
        desc.instance = vulkanInstance_;
        desc.physicalDevice = vulkanPhysicalDevice_;
        desc.device = vulkanDevice_;
        desc.graphicsQueue = vulkanGraphicsQueue_;
        desc.graphicsQueueIndex = static_cast<int>(vulkanGraphicsFamily_);
        nvrhiDeviceHandle_ = nvrhi::vulkan::createDevice(desc); // 内部会初始化 Vulkan 动态 dispatcher
        if (!nvrhiDeviceHandle_) { outError = "nvrhi::vulkan::createDevice returned null"; return false; }
        // 注意：Vulkan 的 semaphore 协调方法只在 raw vulkan::IDevice 上可用，
        // NVRHI 的 validation 包裹层不暴露它们，故这里直接用 raw device（Donut 同款做法）。
        nvrhiDevice_ = nvrhiDeviceHandle_;
        return true;
    }

    void reportFeatures() {
        info_.rayQuery = nvrhiDevice_->queryFeatureSupport(nvrhi::Feature::RayQuery);
        info_.indirectDraw = true;

        // Vulkan 1.4 头文件里 64 位整型原子已从 Vulkan12Features 拆到
        // ShaderAtomicInt64Features 独立结构，需单独挂在 pNext 链上查询。
        VkPhysicalDeviceVulkan12Features vk12{};
        vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceShaderAtomicInt64Features atom64{};
        atom64.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES;
        vk12.pNext = &atom64;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &vk12;
        vkGetPhysicalDeviceFeatures2_(vulkanPhysicalDevice_, &features2);
        info_.shaderInt64Atomics = (atom64.shaderBufferInt64Atomics == VK_TRUE)
                                || (atom64.shaderSharedInt64Atomics == VK_TRUE);

        nl::core::log::info("features: rayQuery=", info_.rayQuery ? 1 : 0,
                            " shaderInt64Atomics=", info_.shaderInt64Atomics ? 1 : 0,
                            " indirectDraw=", info_.indirectDraw ? 1 : 0);
        if (!info_.rayQuery || !info_.shaderInt64Atomics) {
            nl::core::log::warn("feature report has unsupported items; S7/S8 需按此能力调整");
        }
    }

    bool createSwapchain(std::string& outError) {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR_(vulkanPhysicalDevice_, vulkanSurface_, &caps);

        uint32_t fmtCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR_(vulkanPhysicalDevice_, vulkanSurface_, &fmtCount, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR_(vulkanPhysicalDevice_, vulkanSurface_, &fmtCount, fmts.data());

        // 优先 R8G8B8A8_UNORM，和 DX12 的 RGBA 截图字节序一致；
        // 否则退回 B8G8R8A8_UNORM（spec 保证支持），截图时做 R/B 交换。
        VkFormat chosen = VK_FORMAT_UNDEFINED;
        for (const auto& f : fmts) {
            if (f.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = VK_FORMAT_R8G8B8A8_UNORM; break; }
        }
        if (chosen == VK_FORMAT_UNDEFINED) {
            for (const auto& f : fmts) {
                if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = VK_FORMAT_B8G8R8A8_UNORM; break; }
            }
        }
        if (chosen == VK_FORMAT_UNDEFINED) chosen = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0].format;

        vulkanSwapFormat_ = (chosen == VK_FORMAT_R8G8B8A8_UNORM)
            ? nvrhi::Format::RGBA8_UNORM
            : nvrhi::Format::BGRA8_UNORM;
        vulkanSwapRB_ = (vulkanSwapFormat_ == nvrhi::Format::BGRA8_UNORM);

        VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        for (const auto& f : fmts) {
            if (f.format == chosen) { colorSpace = f.colorSpace; break; }
        }

        uint32_t w = extent_.width, h = extent_.height;
        if (caps.minImageExtent.width > w) w = caps.minImageExtent.width;
        if (caps.minImageExtent.height > h) h = caps.minImageExtent.height;
        if (caps.maxImageExtent.width < w) w = caps.maxImageExtent.width;
        if (caps.maxImageExtent.height < h) h = caps.maxImageExtent.height;
        extent_.width = w; extent_.height = h;

        uint32_t minImages = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && minImages > caps.maxImageCount) minImages = caps.maxImageCount;

        VkSwapchainCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface = vulkanSurface_;
        sci.minImageCount = minImages;
        sci.imageFormat = chosen;
        sci.imageColorSpace = colorSpace;
        sci.imageExtent = { w, h };
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sci.imageSharingMode = (vulkanGraphicsFamily_ == vulkanPresentFamily_)
            ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
        if (sci.imageSharingMode == VK_SHARING_MODE_CONCURRENT) {
            uint32_t idxs[2] = { vulkanGraphicsFamily_, vulkanPresentFamily_ };
            sci.queueFamilyIndexCount = 2;
            sci.pQueueFamilyIndices = idxs;
        }
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = VK_NULL_HANDLE;

        const VkResult r = vkCreateSwapchainKHR_(vulkanDevice_, &sci, nullptr, &vulkanSwapchain_);
        if (r != VK_SUCCESS) {
            outError = std::string("vkCreateSwapchainKHR failed: ") + vkResultStr(r);
            return false;
        }

        // 二进制 semaphore：acquire 信号一个，present 等另一个。
        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore_(vulkanDevice_, &semInfo, nullptr, &vulkanImageAvailableSem_);
        vkCreateSemaphore_(vulkanDevice_, &semInfo, nullptr, &vulkanRenderFinishedSem_);
        return true;
    }

    bool recreateSwapchain() {
        vulkanSafeWait();
        // acquire / present 的 OUT_OF_DATE 也会走这里：旧的 framebuffer 与 depth
        // 必须一并失效，否则 createTargets() 之后还在引用已销毁的 swapchain image。
        scene_.invalidatePipelines();
        hud_.invalidatePipeline();
        for (auto& fb : framebuffers_) fb = nullptr;
        depth_ = nullptr;
        if (vulkanSwapchain_) {
            rhiTargets_.clear();
            nativeTargets_.clear();
            vkDestroySwapchainKHR_(vulkanDevice_, vulkanSwapchain_, nullptr);
            vulkanSwapchain_ = VK_NULL_HANDLE;
        }
        std::string err;
        if (!createSwapchain(err)) {
            nl::core::log::deviceError("Vulkan", err);
            return false;
        }
        return true;
    }

    bool createTargets(std::string& outError) {
        rhiTargets_.clear();
        nativeTargets_.clear();

        uint32_t count = 0;
        vkGetSwapchainImagesKHR_(vulkanDevice_, vulkanSwapchain_, &count, nullptr);
        std::vector<VkImage> images(count);
        vkGetSwapchainImagesKHR_(vulkanDevice_, vulkanSwapchain_, &count, images.data());

        for (VkImage img : images) {
            nvrhi::TextureDesc texDesc;
            texDesc.width = extent_.width;
            texDesc.height = extent_.height;
            texDesc.format = vulkanSwapFormat_;
            texDesc.debugName = "SwapChainBuffer";
            texDesc.isRenderTarget = true;
            texDesc.initialState = nvrhi::ResourceStates::Present;
            texDesc.keepInitialState = true;

            auto handle = nvrhiDevice_->createHandleForNativeTexture(
                nvrhi::ObjectTypes::VK_Image, nvrhi::Object(img), texDesc);
            if (!handle) { outError = "createHandleForNativeTexture failed"; return false; }
            rhiTargets_.push_back(handle);
            nativeTargets_.push_back(img);
        }

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
        depth_ = nvrhiDevice_->createTexture(depthDesc);
        if (!depth_) { outError = "createTexture failed for depth buffer"; return false; }
        return true;
    }

    // BGRA 回读时把 R/B 交换成 RGBA，保证和 DX12 截图字节序一致。
    bool writeReadback(void* pixels, std::uint32_t rowPitch, const char* path) {
        if (!vulkanSwapRB_) {
            return writePngRGBA8(path, pixels, extent_.width, extent_.height, rowPitch);
        }
        const std::uint32_t w = extent_.width, h = extent_.height;
        const std::uint8_t* src = static_cast<const std::uint8_t*>(pixels);
        std::vector<std::uint8_t> rgba(static_cast<size_t>(rowPitch) * h);
        for (std::uint32_t y = 0; y < h; ++y) {
            const std::uint8_t* s = src + static_cast<size_t>(y) * rowPitch;
            std::uint8_t* d = rgba.data() + static_cast<size_t>(y) * rowPitch;
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::uint8_t b = s[0], g = s[1], r = s[2], a = s[3];
                d[0] = r; d[1] = g; d[2] = b; d[3] = a;
                s += 4; d += 4;
            }
        }
        return writePngRGBA8(path, rgba.data(), w, h, rowPitch);
    }

    void vulkanSafeWait() {
        if (vulkanDevice_) nvrhiDevice_->waitForIdle();
    }

    void shutdown() {
        if (vulkanDevice_) nvrhiDevice_->waitForIdle();
        // 场景 / HUD 资源（含 binding set / pipeline）必须在 device 仍存活时释放。
        scene_.shutdown();
        hud_.shutdown();
        gpuTimer_.shutdown();   // M0.6：device 仍存活时释放 timer query
        for (auto& fb : framebuffers_) fb = nullptr;
        depth_ = nullptr;
        rhiTargets_.clear();
        nativeTargets_.clear();

        // 关键析构顺序：NVRHI 只持有 VkDevice 的「非拥有」包装（vk::Device），
        // 其 Device::~Device() 会用该 device 销毁内部 query pool / pipeline cache /
        // descriptor set layout。必须在销毁原生 VkDevice 之前先把 NVRHI 句柄 Reset，
        // 否则 NVRHI 析构作用于已销毁的 device → use-after-free → 退出段错误（139）。
        // 注意 NVRHI 自身从不 vkDestroyDevice（全源码无此调用），故 VkDevice 只由本
        // 后端销毁一次，不存在双重销毁。
        nvrhiDeviceHandle_.Reset();
        nvrhiDevice_ = nullptr;

        if (vulkanImageAvailableSem_) vkDestroySemaphore_(vulkanDevice_, vulkanImageAvailableSem_, nullptr);
        if (vulkanRenderFinishedSem_) vkDestroySemaphore_(vulkanDevice_, vulkanRenderFinishedSem_, nullptr);
        if (vulkanSwapchain_) vkDestroySwapchainKHR_(vulkanDevice_, vulkanSwapchain_, nullptr);
        if (vulkanSurface_) vkDestroySurfaceKHR_(vulkanInstance_, vulkanSurface_, nullptr);
        if (vulkanDevice_) vkDestroyDevice_(vulkanDevice_, nullptr);
        if (vulkanInstance_) vkDestroyInstance_(vulkanInstance_, nullptr);

        vulkanSwapchain_ = VK_NULL_HANDLE;
        vulkanSurface_ = VK_NULL_HANDLE;
        vulkanDevice_ = VK_NULL_HANDLE;
        vulkanInstance_ = VK_NULL_HANDLE;
        // 注意：vulkan-1.dll（图形 loader）不在 shutdown 卸载——NVRHI 内部仍持有其引用，
        // 进程退出时的静态析构还会用到；卸载会导致退出段错误。让它随进程生命周期存在即可。
    }

    // M0.4：懒建 framebuffer（color + depth）与两套 pipeline（开/关背面剔除）。
    void ensurePipeline(uint32_t index);

    HWND hwnd_ = nullptr;
    Extent extent_{};
    DeviceInfo info_{};
    std::string lodStem_;              // M2.3：<stem>.lodN.nmesh 的 stem
    int lodLevelCount_ = 0;            // 0 = 没有分层资产
    int dagGroupCount_ = 0;            // M3.2：可下钻组数（第二粗层 dagGroups）
    NvrhiMessageCallback nvrhiMessageCallback_;

    // 原生 Vulkan 对象（一律 vulkan 前缀）
    VkInstance vulkanInstance_ = VK_NULL_HANDLE;
    VkPhysicalDevice vulkanPhysicalDevice_ = VK_NULL_HANDLE;
    VkDevice vulkanDevice_ = VK_NULL_HANDLE;
    uint32_t vulkanGraphicsFamily_ = 0;
    VkQueue vulkanGraphicsQueue_ = VK_NULL_HANDLE;
    uint32_t vulkanPresentFamily_ = 0;
    VkQueue vulkanPresentQueue_ = VK_NULL_HANDLE;
    VkSurfaceKHR vulkanSurface_ = VK_NULL_HANDLE;
    VkSwapchainKHR vulkanSwapchain_ = VK_NULL_HANDLE;
    VkSemaphore vulkanImageAvailableSem_ = VK_NULL_HANDLE;
    VkSemaphore vulkanRenderFinishedSem_ = VK_NULL_HANDLE;
    uint32_t vulkanImageIndex_ = 0;

    nvrhi::Format vulkanSwapFormat_ = nvrhi::Format::RGBA8_UNORM;
    bool vulkanSwapRB_ = false;

    // NVRHI 抽象层对象（一律 nvrhi 前缀）
    nvrhi::vulkan::DeviceHandle nvrhiDeviceHandle_;
    nvrhi::vulkan::IDevice* nvrhiDevice_ = nullptr;  // raw device（持有 semaphore 协调方法）
    std::vector<nvrhi::TextureHandle> rhiTargets_;
    std::vector<VkImage> nativeTargets_;

    // ---- M0.4 场景资源：glTF 模型 / 贴图 / pipeline 由共享 Scene 管理 ----
    Scene                                 scene_;
    Hud                                   hud_;
    GpuTimer                              gpuTimer_;
    nvrhi::TextureHandle                  depth_;
    std::vector<nvrhi::FramebufferHandle> framebuffers_;   // 每个 swapchain slot 一个

    // Vulkan loader 模块句柄（运行时加载，避免依赖 vulkan-1.lib 导入库）
    HMODULE vulkanModule_ = nullptr;
    // 从 loader 动态解析的 Vulkan 函数指针（本机无静态导出符号可链接）
    PFN_vkGetInstanceProcAddr getInstanceProcAddr_ = nullptr;
    PFN_vkCreateInstance      vkCreateInstance_ = nullptr;
    PFN_vkDestroyInstance     vkDestroyInstance_ = nullptr;
    PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices_ = nullptr;
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties_ = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR_ = nullptr;
    PFN_vkCreateWin32SurfaceKHR vkCreateWin32SurfaceKHR_ = nullptr;
    PFN_vkCreateDevice        vkCreateDevice_ = nullptr;
    PFN_vkGetDeviceQueue      vkGetDeviceQueue_ = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR_ = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR_ = nullptr;
    PFN_vkCreateSwapchainKHR  vkCreateSwapchainKHR_ = nullptr;
    PFN_vkCreateSemaphore     vkCreateSemaphore_ = nullptr;
    PFN_vkDestroySemaphore    vkDestroySemaphore_ = nullptr;
    PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR_ = nullptr;
    PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR_ = nullptr;
    PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR_ = nullptr;
    PFN_vkQueuePresentKHR     vkQueuePresentKHR_ = nullptr;
    PFN_vkDestroySurfaceKHR   vkDestroySurfaceKHR_ = nullptr;
    PFN_vkDestroyDevice       vkDestroyDevice_ = nullptr;
};

    void VkBackend::ensurePipeline(uint32_t index) {
        if (framebuffers_.size() != rhiTargets_.size())
            framebuffers_.assign(rhiTargets_.size(), nullptr);
        if (index < framebuffers_.size() && !framebuffers_[index] && depth_) {
            nvrhi::FramebufferDesc fbDesc;
            fbDesc.addColorAttachment(rhiTargets_[index]);
            fbDesc.setDepthAttachment(depth_);
            framebuffers_[index] = nvrhiDevice_->createFramebuffer(fbDesc);
        }
        // pipeline 绑定 framebuffer 格式（color + depth），建一次即可；resize 后由
        // scene_.invalidatePipelines() 触发重建。两套分别对应开/关背面剔除。
        if (!scene_.ready() && !framebuffers_.empty() && framebuffers_[0]) {
            std::string err;
            if (!scene_.ensurePipelines(nvrhiDevice_, framebuffers_[0]->getFramebufferInfo(), err))
                nl::core::log::deviceError("Vulkan", err);
        }
    }

} // namespace (anonymous)

std::unique_ptr<Backend> createBackend() {
    return std::make_unique<VkBackend>();
}

} // namespace nl::renderer::detail

#endif // NL_BACKEND_VULKAN
