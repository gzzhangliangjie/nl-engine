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
constexpr const char* kStage =
    "S5.1 GPU DAG traversal (explicit-stack compute, group-atomic cut, M4 indirect interface)";

// M0.7：--model 白名单校验。只放行 assets/ 下安全引用的 ASCII 文件名：
// 无路径分隔符（防目录穿越）、无 ".."、可打印字符、指定后缀。
bool validAssetFileName(const char* name, const char* ext) {
    if (!name || !*name) return false;
    const std::string s(name);
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return false;
    if (s.find("..") != std::string::npos) return false;
    const size_t extLen = std::strlen(ext);
    if (s.size() < extLen + 1 || s.compare(s.size() - extLen, extLen, ext) != 0) return false;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7E) return false;   // 可打印 ASCII，不含空格
    }
    return true;
}
bool validModelFileName(const char* name) {
    return validAssetFileName(name, ".glb");
}
// M1.3：cluster 模式的 .nmesh 白名单（assets/meshlets/）。
bool validNMeshFileName(const char* name) {
    return validAssetFileName(name, ".nmesh");
}
} // namespace

Status status() {
    return Status{ kBackend, kStage, false };
}

bool shaderArtifactsSelfTest(std::string& report) {
// M1.3：model 双入口（vs/vscluster/ps/pscluster）+ M4.0 再加 vscull/pscull 双入口，
// M4.1 加 cull.cs compute 入口，加 hud 单入口，各 × spv/dxil = 18。
struct Artifact { const char* name; bool spv; };
static const Artifact kArtifacts[] = {
    {"model.vs", true}, {"model.vs", false},
    {"model.vscluster", true}, {"model.vscluster", false},
    {"model.vscull", true}, {"model.vscull", false},
    {"model.ps", true}, {"model.ps", false},
    {"model.pscluster", true}, {"model.pscluster", false},
    {"model.pscull", true}, {"model.pscull", false},
    {"cull.cs", true}, {"cull.cs", false},
    {"hud.vs", true}, {"hud.vs", false},
    {"hud.ps", true}, {"hud.ps", false},
};
    constexpr int kArtifactCount =
        static_cast<int>(sizeof(kArtifacts) / sizeof(kArtifacts[0]));
    constexpr std::uint32_t kSpirvMagic = 0x07230203u;

    std::ostringstream os;
    int ok = 0;
    for (const Artifact& a : kArtifacts) {
        const char* target = a.spv ? "spv" : "dxil";
        std::string name = std::string(a.name) + "." + target;
        const std::string path = std::string(NL_SHADER_DIR) + "/" + name;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        std::int64_t size = -1;
        if (f) size = static_cast<std::int64_t>(f.tellg());
        std::uint32_t magic = 0;
        if (f && size >= 4) {
            f.seekg(0);
            f.read(reinterpret_cast<char*>(&magic), 4);
        }
        const bool good = f.good() && size >= 4 &&
            (a.spv ? magic == kSpirvMagic : std::memcmp(&magic, "DXBC", 4) == 0);
        if (good) ++ok;
        os << ' ' << name << (good ? "" : "!BAD") << '(' << size << "B)";
    }
    report = os.str() + "  [" + std::to_string(ok) + "/" + std::to_string(kArtifactCount) +
             " ok]";
    return ok == kArtifactCount;
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
                                           const char* modelFile, bool clusterMode,
                                           bool cullMode, bool frustumCull,
                                           bool dagMode) {
    std::unique_ptr<Renderer> r(new Renderer());
    // cull 模式（M4.0 GPU vertex pulling）与 dag 模式（M5.1 多层遍历）资产同样是
    // assets/meshlets/ 下的 .nmesh。
    const bool isNMesh = clusterMode || cullMode || dagMode;
    const bool nameOk = isNMesh ? validNMeshFileName(modelFile)
                                : validModelFileName(modelFile);
    if (!nameOk) {
        r->impl_->error = std::string("invalid --model file name: '") +
                          (modelFile ? modelFile : "<null>") +
                          (isNMesh
                               ? "' (expected an ASCII *.nmesh file name in assets/meshlets/)"
                               : "' (expected an ASCII *.glb file name in assets/models/)");
        return r;
    }
    r->impl_->backend = detail::createBackend();
    if (!r->impl_->backend) {
        r->impl_->error = "no backend compiled in";
        return r;
    }
    if (!r->impl_->backend->init(nativeWindow, extent, enableDebugLayer, modelFile,
                                 clusterMode, cullMode, frustumCull, dagMode,
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
    const bool ok = impl_->backend->renderFrame(color, frame, screenshotPath, hud, impl_->error);
    // M4.1：剔除统计（visibleClusters）由 backend 每帧 poll 后更新，回拷进
    // Renderer 缓存的 DeviceInfo，viewer 的 const 引用才能看到最新值。
    if (ok) impl_->info = impl_->backend->info();
    return ok;
}

int Renderer::drainMessages() {
    return impl_->backend ? impl_->backend->drainMessages() : 0;
}

bool Renderer::setLodLevel(int level) {
    if (!impl_->backend) return false;
    if (!impl_->backend->setLodLevel(level, impl_->error)) return false;
    impl_->info = impl_->backend->info();   // 三角形数 / 层号 / maxError 随层更新
    return true;
}

int Renderer::lodLevel() const { return impl_->info.lodLevel; }
int Renderer::lodLevelCount() const { return impl_->info.lodLevelCount; }

// M3.2：dag-top 组原子下钻。组数可下钻范围与状态同步都收在 backend 里。
bool Renderer::setDagCut(int group) {
    if (!impl_->backend) return false;
    if (!impl_->backend->setDagCut(group, impl_->error)) return false;
    impl_->info = impl_->backend->info();   // 三角形数 / 下钻状态随 cut 更新
    return true;
}

int Renderer::dagGroupCount() const { return impl_->info.dagGroupCount; }

} // namespace nl::renderer
