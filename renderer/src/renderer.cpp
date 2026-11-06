#include "nl/renderer/renderer.hpp"
#include "backend.hpp"

namespace nl::renderer {
namespace {
#if defined(NL_BACKEND_VULKAN)
constexpr const char* kBackend = "vulkan";
#else
constexpr const char* kBackend = "dx12";
#endif
constexpr const char* kStage = "M0.2 NVRHI device + swapchain + in-app screenshot";
} // namespace

Status status() {
    return Status{ kBackend, kStage, false };
}

struct Renderer::Impl {
    std::unique_ptr<detail::Backend> backend;
    std::string error;
    DeviceInfo  info;
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

std::unique_ptr<Renderer> Renderer::create(void* nativeWindow, Extent extent,
                                           bool enableDebugLayer) {
    std::unique_ptr<Renderer> r(new Renderer());
    r->impl_->backend = detail::createBackend();
    if (!r->impl_->backend) {
        r->impl_->error = "no backend compiled in";
        return r;
    }
    if (!r->impl_->backend->init(nativeWindow, extent, enableDebugLayer, r->impl_->error)) {
        r->impl_->backend.reset();
        if (r->impl_->error.empty()) r->impl_->error = "backend init failed";
        return r;
    }
    r->impl_->info = r->impl_->backend->info();
    return r;
}

bool Renderer::valid() const { return impl_ && impl_->backend != nullptr; }
const std::string& Renderer::lastError() const { return impl_->error; }
const DeviceInfo& Renderer::info() const { return impl_->info; }
Extent Renderer::extent() const {
    return impl_->backend ? impl_->backend->extent() : Extent{};
}

void Renderer::resize(Extent extent) {
    if (impl_->backend) impl_->backend->resize(extent);
}

bool Renderer::renderFrame(ClearColor color, const char* screenshotPath) {
    if (!impl_->backend) return false;
    return impl_->backend->renderFrame(color, screenshotPath, impl_->error);
}

int Renderer::drainMessages() {
    return impl_->backend ? impl_->backend->drainMessages() : 0;
}

} // namespace nl::renderer
