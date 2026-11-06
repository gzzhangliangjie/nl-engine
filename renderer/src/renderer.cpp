#include "nl/renderer/renderer.hpp"
#include "backend.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

namespace nl::renderer {
namespace {
#if defined(NL_BACKEND_VULKAN)
constexpr const char* kBackend = "vulkan";
#else
constexpr const char* kBackend = "dx12";
#endif
constexpr const char* kStage = "M0.5 ImGui HUD + S0 final acceptance (Slang dual-backend)";
} // namespace

Status status() {
    return Status{ kBackend, kStage, false };
}

bool shaderArtifactsSelfTest(std::string& report) {
    static const char* kSources[] = { "model", "hud" };
    static const char* kStages[]  = { "vs", "ps" };
    static const char* kTargets[] = { "spv", "dxil" };
    constexpr std::uint32_t kSpirvMagic = 0x07230203u;

    std::ostringstream os;
    int ok = 0;
    for (const char* src : kSources) {
        for (const char* stage : kStages) {
            for (const char* target : kTargets) {
                std::string name = std::string(src) + "." + stage + "." + target;
                const std::string path = std::string(NL_SHADER_DIR) + "/" + name;
                std::ifstream f(path, std::ios::binary | std::ios::ate);
                std::int64_t size = -1;
                if (f) size = static_cast<std::int64_t>(f.tellg());
                std::uint32_t magic = 0;
                if (f && size >= 4) {
                    f.seekg(0);
                    f.read(reinterpret_cast<char*>(&magic), 4);
                }
                const bool isSpv = (std::strcmp(target, "spv") == 0);
                const bool good = f.good() && size >= 4 &&
                    (isSpv ? magic == kSpirvMagic : std::memcmp(&magic, "DXBC", 4) == 0);
                if (good) ++ok;
                os << ' ' << name << (good ? "" : "!BAD") << '(' << size << "B)";
            }
        }
    }
    report = os.str() + "  [" + std::to_string(ok) + "/8 ok]";
    return ok == 8;
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

bool Renderer::renderFrame(ClearColor color, const FrameParams& frame,
                           const char* screenshotPath, HudFrame* hud) {
    if (!impl_->backend) return false;
    return impl_->backend->renderFrame(color, frame, screenshotPath, hud, impl_->error);
}

int Renderer::drainMessages() {
    return impl_->backend ? impl_->backend->drainMessages() : 0;
}

} // namespace nl::renderer
