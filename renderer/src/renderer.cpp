#include "nl/renderer/renderer.hpp"
#include "backend.hpp"
#include "nl/core/profiler.hpp"   // M-TR0：renderer 帧提交 zone（OFF 空转）

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
    "S8.2 ray query debug closure (per-pixel camera rays)";

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
// 产物名共 124 个（每项 = 1 个文件，spv/dxil 由 bool 选）：model 16（8 入口）+
// cull.cs/dag.cs 4 + hiz 12 + hud 4（M0.5）+ debug_line 4（M-P3 单管线 vs/ps）+
// visbuf 8（M7.0）+ material 4（M7.1，此前漏登）+ tlas_probe.cs 2（M8.1）+
// rt.cs/vsrtpresent/psrtpresent 6（M8.2）+ psrefpresent/blit/tonemap 8 +
// particles 4（M-PT0）+ direct.cs/csref 4（M9.0/M9.2）+
// velocity/probe/probe_viz 10（M10.0）+ probe_trace.cstrace 2（M10.1）+
// probe_gather 4（M10.2）+ direct.csrefgi + rt.pscompare 4（M10.3）+
// world_cache 6（S11 M11.0）+ world_cache 4（M11.1 update/blend）+
// vsm 10（S15 M15.0 vs/ps 4 + M15.1 vsclear/psclear 4 + cs 2；M15.1 补登，
// M15.0 漏登是 M7.1/M8.1 同款教训第三犯）。
// （M10.2 收口注释误写 78，实测 90——2026-11-11 数表勘误；总数随 S12/S15 增至 118，
// M-NL0 bloom 三入口 ×2 再增至 124。）
// 清单只查列出项：新增入口必须同步登记，
// 否则产物丢失时 selftest 仍全绿（M8.1 已踩过一次）。
struct Artifact { const char* name; bool spv; };
static const Artifact kArtifacts[] = {
    {"model.vs", true}, {"model.vs", false},
    {"model.vscluster", true}, {"model.vscluster", false},
    {"model.vscull", true}, {"model.vscull", false},
    {"model.vsscene", true}, {"model.vsscene", false},
    {"model.ps", true}, {"model.ps", false},
    {"model.pscluster", true}, {"model.pscluster", false},
    {"model.pscull", true}, {"model.pscull", false},
    {"model.pscene", true}, {"model.pscene", false},
    {"cull.cs", true}, {"cull.cs", false},
    {"dag.cs", true}, {"dag.cs", false},
    {"hiz.vshizdepth", true}, {"hiz.vshizdepth", false},
    {"hiz.pshizdepth", true}, {"hiz.pshizdepth", false},
    {"hiz.cs", true}, {"hiz.cs", false},
    {"hiz.cstop", true}, {"hiz.cstop", false},
    {"hiz.vshizviz", true}, {"hiz.vshizviz", false},
    {"hiz.pshizviz", true}, {"hiz.pshizviz", false},
    {"hud.vs", true}, {"hud.vs", false},
    {"hud.ps", true}, {"hud.ps", false},
    // M-P3：debug_line 单管线（vsMain/psMain），各 × spv/dxil = 4
    {"debug_line.vs", true}, {"debug_line.vs", false},
    {"debug_line.ps", true}, {"debug_line.ps", false},
    // M7.0：visbuf.slang 四入口 × 双目标 = 8。
    {"visbuf.vsvisraster", true}, {"visbuf.vsvisraster", false},
    {"visbuf.psvisraster", true}, {"visbuf.psvisraster", false},
    {"visbuf.vsvisviz", true}, {"visbuf.vsvisviz", false},
    {"visbuf.psvisviz", true}, {"visbuf.psvisviz", false},
    // M7.1：material.slang 双入口 × 双目标 = 4（M8.1 补登——此前清单漏收，
    // selftest 只查列出项所以一直绿；kStage bump 纪律同理，新入口必须登记）。
    {"material.vsmatresolve", true}, {"material.vsmatresolve", false},
    {"material.psmatresolve", true}, {"material.psmatresolve", false},
    // M8.1：tlas_probe.slang 单 compute 入口（RayQuery 探针）× 双目标 = 2。
    {"tlas_probe.cs", true}, {"tlas_probe.cs", false},
    // M8.2：rt.slang 三入口（RayQuery 每像素射线 compute + 呈现 vs/ps）× 双目标 = 6。
    {"rt.cs", true}, {"rt.cs", false},
    {"rt.vsrtpresent", true}, {"rt.vsrtpresent", false},
    {"rt.psrtpresent", true}, {"rt.psrtpresent", false},
    // M9.2：rt.psrefpresent（参考图÷samplesDone 再 gamma 全屏呈现）× 双目标 = 2。
    {"rt.psrefpresent", true}, {"rt.psrefpresent", false},
    // T0：rt.psblittohdr（rtTarget_→离屏 HDR 直拷）× 双目标 = 2，rt.pstonemap（离屏 HDR→swapchain 直通）× 双目标 = 2。
    {"rt.psblittohdr", true}, {"rt.psblittohdr", false},
    {"rt.pstonemap", true}, {"rt.pstonemap", false},
    // M-NL0：rt.psbloomextract/psbloomblurh/psbloomblurv（bloom 三入口）× 双目标 = 6。
    {"rt.psbloomextract", true}, {"rt.psbloomextract", false},
    {"rt.psbloomblurh", true}, {"rt.psbloomblurh", false},
    {"rt.psbloomblurv", true}, {"rt.psbloomblurv", false},
    // M-PT0：particles.slang 单管线（vsMain/psMain），各 × spv/dxil = 4。
    {"particles.vs", true}, {"particles.vs", false},
    {"particles.ps", true}, {"particles.ps", false},
    // M9.0：direct.slang 单 compute 入口（compute 直光对照）× 双目标 = 2。
    // M9.2：direct.csref（离线参考模式 compute，追踪 64 条抖动阴影射线累积）× 双目标 = 2。
    {"direct.cs", true}, {"direct.cs", false},
    {"direct.csref", true}, {"direct.csref", false},
    // M10.0：velocity.slang 双入口（velocity prepass，Q10-1 UE 口径）× 双目标 = 4；
    // probe.slang 单 compute 入口（cascade 探针布点）× 双目标 = 2；
    // probe_viz.slang 双入口（探针点可视化）× 双目标 = 4。
    {"velocity.vsvelocity", true}, {"velocity.vsvelocity", false},
    {"velocity.psvelocity", true}, {"velocity.psvelocity", false},
    {"probe.csplace", true}, {"probe.csplace", false},
    {"probe_viz.vsprobedots", true}, {"probe_viz.vsprobedots", false},
    {"probe_viz.psprobedots", true}, {"probe_viz.psprobedots", false},
    // M10.1：probe_trace.slang 单 compute 入口（探针追踪，Q12-1/2 拍板）× 双目标 = 2。
    {"probe_trace.cstrace", true}, {"probe_trace.cstrace", false},
    // M10.2：probe_gather.slang 双 compute 入口（final gather + 合成，Q13-1/2/3
    // 拍板）× 双目标 = 4。
    {"probe_gather.csgather", true}, {"probe_gather.csgather", false},
    {"probe_gather.csgathercompose", true}, {"probe_gather.csgathercompose", false},
    // M10.3：direct.csrefgi（Q14-1 GI 参考图 compute）× 双目标 = 2 +
    // rt.pscompare（Q14-2 --compare 右半参考图贴片）× 双目标 = 2。
    {"direct.csrefgi", true}, {"direct.csrefgi", false},
    {"rt.pscompare", true}, {"rt.pscompare", false},
    // S11 M11.0：world_cache.slang 三 compute 入口（fill/decay/viz，Q11-0 方案 A）
    // × 双目标 = 6。
    {"world_cache.cscachefill", true}, {"world_cache.cscachefill", false},
    {"world_cache.cscachedecay", true}, {"world_cache.cscachedecay", false},
    {"world_cache.cscacheviz", true}, {"world_cache.cscacheviz", false},
    // S11 M11.1：cell 更新（update 需 SM6.6 RayQuery；blend 纯 buffer）×2 = 4。
    {"world_cache.cscacheupdate", true}, {"world_cache.cscacheupdate", false},
    {"world_cache.cscacheblend", true}, {"world_cache.cscacheblend", false},
    // S12 M12.0：filter_temporal.slang 单 compute 入口（Q12-A GI 分量独立时域累积）
    // × 双目标 = 2。
    {"filter_temporal.csfiltertemporal", true}, {"filter_temporal.csfiltertemporal", false},
    // S12 M12.1：filter_spatial.slang 单 compute 入口（Q12-C A）× 双目标 = 2。
    {"filter_spatial.csfilterspatial", true}, {"filter_spatial.csfilterspatial", false},
    // S15 M15.0：vsm.slang 深度 pass 双入口 × 双目标 = 4（M15.1 补登——M15.0 落码
    // 时漏登记，selftest 只查列出项所以一直绿；M8.1/M7.1 同款教训两犯，补上）。
    {"vsm.vs", true}, {"vsm.vs", false},
    {"vsm.ps", true}, {"vsm.ps", false},
    // S15 M15.1：vsm.slang 清页 quad 双入口 × 双目标 = 4 + 标页 compute × 双目标 = 2。
    {"vsm.vsclear", true}, {"vsm.vsclear", false},
    {"vsm.psclear", true}, {"vsm.psclear", false},
    {"vsm.cs", true}, {"vsm.cs", false},
    // S15 M15.2：vsm.slang 剔除 4 compute 入口 × 双目标 = 8（HZB 构建 + 剔除三 dispatch）。
    {"vsm.cshzb", true}, {"vsm.cshzb", false},
    {"vsm.cscullcount", true}, {"vsm.cscullcount", false},
    {"vsm.cscullscan", true}, {"vsm.cscullscan", false},
    {"vsm.cscullfill", true}, {"vsm.cscullfill", false},
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
                                           bool dagMode, bool skinMode,
                                           bool hizMode, bool sceneMode, bool visbufMode,
                                           bool pullingMode, bool rtMode, bool shadowMode, bool referenceMode,
                                           bool probeMode,
                                           // M13.0：--cluster-blas（per-cluster BLAS
                                           // 预建；dag/hiz/rt 链消费，缺省关）。
                                           bool clusterBlas) {
    std::unique_ptr<Renderer> r(new Renderer());
    // cull 模式（M4.0 GPU vertex pulling）、dag 模式（M5.1 多层遍历）与 hiz 模式
    // （M6.0 = dag + Hi-Z 金字塔）资产同样是 assets/meshlets/ 下的 .nmesh。
    // M7.0：visbuf 模式同样吃 .nmesh（复用 M4 的簇数据）。
    // M8.2：rt 模式走 initDag 资产路径（BLAS/TLAS 挂在 initDag 链上），同样吃 .nmesh。
    // M9.0：shadow 同样吃 .nmesh（复用 initVisbuf(pulling) 资产链）。
    // M10.0：probe-place 同样吃 .nmesh（复用 initVisbuf(pulling) 资产链）。
    const bool isNMesh = clusterMode || cullMode || dagMode || hizMode || visbufMode ||
                         rtMode || shadowMode || referenceMode || probeMode;
    // M-E1b.0：sceneMode 不加载任何白名单资产（modelFile 可为 null）——几何与
    // 灯数据全部来自 loadEcsScene 的场景描述，白名单在 viewer 侧按 meshFile 校验。
    bool nameOk = false;
    if (sceneMode) {
        nameOk = true;
    } else if (skinMode) {
        // M-A5：skin 模式吃 assets/models/ 下的 .glb（含 rigged/ 子目录——
        // 标准演示资产在 assets/models/rigged/；仍禁 ".." 与其他路径分隔符）。
        const std::string prefix = "rigged/";
        if (modelFile && std::string(modelFile).rfind(prefix, 0) == 0)
            nameOk = validModelFileName(modelFile + prefix.size());
        else
            nameOk = validModelFileName(modelFile);
    } else {
        nameOk = isNMesh ? validNMeshFileName(modelFile) : validModelFileName(modelFile);
    }
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
                                 clusterMode, cullMode, frustumCull, dagMode, skinMode,
                                 hizMode, sceneMode, visbufMode, pullingMode, rtMode,
                                 shadowMode, referenceMode, probeMode, clusterBlas,
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
    // M9.0：把当帧清屏色带给 Scene（--mode shadow 的空像素写进 rtTarget_，
    // 与 psMatResolve 的 discard 等价）。只做一次 POD 浅拷贝，绝不整块 memcpy
    // 进 cbuffer（§2.15）；debugLineData 指针仍归调用方所有。
    FrameParams resolved = frame;
    resolved.clearColor[0] = color.r;
    resolved.clearColor[1] = color.g;
    resolved.clearColor[2] = color.b;
    resolved.clearColor[3] = color.a;
    bool ok = false;
    {
        NL_PROFILE_ZONE("backend.renderFrame");   // M-TR0：renderer 帧提交关键段
        ok = impl_->backend->renderFrame(color, resolved, screenshotPath, hud, impl_->error);
    }
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

// M-E1b.0：ECS 场景加载。转发 backend 的 loadEcsScene（scene_.initScene），
// 成功后 info() 回拷——modelName/triangleCount/instanceCount/assetSphere 随场景更新
//（照 setLodLevel 的写法），viewer 的 const 引用才能看到最新值。
bool Renderer::loadEcsScene(const SceneObjectDesc* objects, std::uint32_t objectCount,
                            const SceneLightDesc* lights, std::uint32_t lightCount,
                            float ambient, const char* sceneName) {
    if (!impl_->backend) return false;
    if (!impl_->backend->loadEcsScene(objects, objectCount, lights, lightCount,
                                      ambient, sceneName, impl_->error)) return false;
    impl_->info = impl_->backend->info();
    return true;
}

// M-E1b.5 渲染用例：转发 backend 的 updateEcsObjects（每帧实例数据刷新）。
bool Renderer::updateEcsObjects(const SceneObjectDesc* objects, std::uint32_t objectCount) {
    if (!impl_->backend) return false;
    std::string err;
    if (!impl_->backend->updateEcsObjects(objects, objectCount, err)) {
        impl_->error = err;
        return false;
    }
    return true;
}

// M-PT1：.nfx v1 加载端。转发 backend 的 scene_.loadParticlesNfx（NFX1 JSON →
// 粒子模拟参数面，运行时零 Effekseer 依赖）。失败原因进 lastError()。
bool Renderer::loadParticlesNfx(const char* path) {
    if (!impl_->backend) return false;
    std::string err;
    if (!impl_->backend->loadParticlesNfx(path, err)) {
        impl_->error = err;
        return false;
    }
    impl_->info = impl_->backend->info();
    return true;
}

// M10.3：--compare 参考图加载端（Q14-2）。转发 backend 的 scene_.loadCompareImage；
// 失败原因进 lastError()，画面保持全 live。
bool Renderer::loadCompareImage(const char* path) {
    if (!impl_->backend) return false;
    std::string err;
    if (!impl_->backend->loadCompareImage(path, err)) {
        impl_->error = err;
        return false;
    }
    return true;
}

} // namespace nl::renderer
