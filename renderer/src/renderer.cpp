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
constexpr const char* kStage = "M0.9 full regression gate (all example cases x both backends)";

// M0.7：--model 白名单校验。只放行 assets/models/ 下安全引用的 ASCII .glb 文件名：
// 无路径分隔符（防目录穿越）、无 ".."、可打印字符、以 ".glb" 结尾。
bool validModelFileName(const char* name) {
    if (!name || !*name) return false;
    const std::string s(name);
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return false;
    if (s.find("..") != std::string::npos) return false;
    if (s.size() < 5 || s.compare(s.size() - 4, 4, ".glb") != 0) return false;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7E) return false;   // 可打印 ASCII，不含空格
    }
    return true;
}
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
                                           bool enableDebugLayer,
                                           const char* modelFile) {
    std::unique_ptr<Renderer> r(new Renderer());
    if (!validModelFileName(modelFile)) {
        r->impl_->error = std::string("invalid --model file name: '") +
                          (modelFile ? modelFile : "<null>") +
                          "' (expected an ASCII *.glb file name in assets/models/)";
        return r;
    }
    r->impl_->backend = detail::createBackend();
    if (!r->impl_->backend) {
        r->impl_->error = "no backend compiled in";
        return r;
    }
    if (!r->impl_->backend->init(nativeWindow, extent, enableDebugLayer, modelFile,
                                 r->impl_->error)) {
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
