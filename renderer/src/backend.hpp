#pragma once

// renderer 内部接口：两个后端各实现一份，renderer.cpp 只认这个抽象。
#include "nl/renderer/renderer.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nl::renderer::detail {

// 读二进制文件到 out（用于加载 Slang 编译出的 SPIR-V / DXIL 字节码）。
// 失败返回 false（文件不存在 / 读不全），由调用方决定报错还是回退。
inline bool readFileBytes(const char* path, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    const long sz = std::ftell(f);
    if (sz < 0) { std::fclose(f); return false; }
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(sz));
    const size_t rd = sz ? std::fread(out.data(), 1, static_cast<size_t>(sz), f) : 0;
    std::fclose(f);
    return rd == static_cast<size_t>(sz);
}

class Backend {
public:
    virtual ~Backend() = default;

    // modelFile：assets/models/ 下的 .glb（clusterMode=false && cullMode=false）或
    // assets/meshlets/ 下的 .nmesh（clusterMode=true，M1.3；cullMode=true，M4.0 GPU
    // vertex pulling）；上层已做白名单校验。frustumCull（M4.1）仅 cull 模式消费：
    // true = 建 cull compute pass 并每帧剔除；false = 恒等可见列表（M4.0 行为）。
    // dagMode（M5.1）优先于 cullMode：多层 .lodN 驻留 + DAG 遍历 compute 选层。
    // hizMode（M6.0）优先于 dagMode：dag 遍历之上再加深度预通道 + min 金字塔。
    // sceneMode（M-E1b.0）为 true 时不初始化 Scene（其余 swapchain/depth/
    // framebuffer/present 照常）——场景资源由 loadEcsScene 建立后才可绘制。
    // visbufMode（M7.0）独立优先级最高：吃 .nmesh，但走 vis buffer 三段式
    // 光栅而不是 S4 的常规着色路径。
    // rtMode（M8.2）走 initDag 资产链拿 BLAS/TLAS，渲染只做 RayQuery compute + 呈现。
    // shadowMode（M9.0）走 initVisbuf(pulling) 资产链 + 共用 rtTarget_ 呈现。
    // probeMode（M10.0）走 initVisbuf(pulling) 资产链 + velocity prepass + cascade
    // 布点 compute + 探针点可视化。
    // clusterBlas（M13.0）= --cluster-blas：initDag 链上逐簇预建 BLAS（S13-PLAN
    // M13.0）；缺省关，仅 dag/hiz/rt 模式消费，其余模式零扰动。
    virtual bool init(void* nativeWindow, Extent extent, bool enableDebugLayer,
                      const char* modelFile, bool clusterMode, bool cullMode,
                      bool frustumCull, bool dagMode, bool skinMode, bool hizMode,
                     bool sceneMode, bool visbufMode, bool pullingMode, bool rtMode,
                     bool shadowMode, bool referenceMode, bool probeMode,
                     bool clusterBlas, bool waterEnable, std::string& outError) = 0;
    virtual void resize(Extent extent) = 0;

    // M2.3：切 LOD 层（仅 cluster 场景）。读 <stem>.lod<level>.nmesh 重建 VB/IB，
    // 成功后更新 info().triangleCount / lodLevel / lodMaxError；失败保持原层。
    virtual bool setLodLevel(int level, std::string& outError) = 0;

    // M3.2：dag-top 组原子下钻。group < 0 = 回到纯顶层（渲染最粗层全部簇）；
    // group >= 0 = 挖掉该组在顶层生成的父簇区间、换回组内子簇。越界 / 无血缘
    // / 层数不足返回 false 且保持原画面。
    virtual bool setDagCut(int group, std::string& outError) = 0;

    // M-E1b.0：ECS 场景加载（sceneMode=true 的 init 之后调用一次）。转发
    // scene_.initScene 建 shader/批次/实例与灯缓冲/绑定集；成功后更新
    // info().modelName / triangleCount / instanceCount / assetSphere。
    virtual bool loadEcsScene(const SceneObjectDesc* objects, std::uint32_t objectCount,
                              const SceneLightDesc* lights, std::uint32_t lightCount,
                              float ambient, const char* sceneName,
                              std::string& outError) = 0;

    // M-E1b.5 渲染用例：每帧刷新实例数据（世界矩阵/tint/材质）。拓扑与灯不动。
    virtual bool updateEcsObjects(const SceneObjectDesc* objects, std::uint32_t objectCount,
                                  std::string& outError) = 0;

    // M-PT1：.nfx v1 加载端（viewer --nfx）。转发 scene_.loadParticlesNfx，
    // 成功后模拟走 nfx 参数面（fp.particles 仍由 viewer 控制）。
    virtual bool loadParticlesNfx(const char* path, std::string& outError) = 0;

    // M10.3：--compare <png> 参考图加载（转发 scene_.loadCompareImage）。
    virtual bool loadCompareImage(const char* path, std::string& outError) = 0;

    // 一帧：clear → 模型绘制（MVP / cull）→（可选）ImGui HUD 叠加
    // →（可选）回读 GPU 像素写 PNG → present。hud 为空时不碰 ImGui（历史基线不变）。
    virtual bool renderFrame(ClearColor color, const FrameParams& frame,
                             const char* screenshotPath, HudFrame* hud,
                             std::string& outError) = 0;

    // 调试层消息转日志；返回新增设备错误数
    virtual int  drainMessages() = 0;

    virtual const DeviceInfo& info() const = 0;
    virtual Extent extent() const = 0;
};

// backend_dx12.cpp / backend_vulkan.cpp 各自按编译期宏提供唯一实现。
std::unique_ptr<Backend> createBackend();

// pixels 是 RGBA8，rowPitch 是每行字节数（可能大于 width*4）。
bool writePngRGBA8(const char* path, const void* pixels,
                   std::uint32_t width, std::uint32_t height,
                   std::uint32_t rowPitch);

// ---- M2.3：分层 .nmesh 命名与探测（两个后端共用一份，避免各写一套路径规则）----
// nanite-tools lod <base>.nmesh -o <dir>/<stem> 产出 <stem>.lod0.nmesh .. <stem>.lodN.nmesh。
inline std::string meshletStem(const char* nmeshFile) {
    std::string s(nmeshFile ? nmeshFile : "");
    const std::string ext = ".nmesh";
    if (s.size() >= ext.size() && s.compare(s.size() - ext.size(), ext.size(), ext) == 0)
        s.resize(s.size() - ext.size());
    return s;
}

inline std::string lodMeshFileName(const std::string& stem, int level) {
    return stem + ".lod" + std::to_string(level) + ".nmesh";
}

// 从 0 起连续探测，遇到第一个缺失就停（层号不连续视为损坏，宁可少报也不跳号）。
// 上限 64 只是防目录里有病态的大量文件。
inline int probeLodLevelCount(const std::string& stem) {
    if (stem.empty()) return 0;
    int count = 0;
    while (count < 64) {
        std::error_code ec;
        const auto path = std::filesystem::path(NL_MESHLET_DIR) / lodMeshFileName(stem, count);
        if (!std::filesystem::is_regular_file(path, ec) || ec) break;
        ++count;
    }
    return count;
}

} // namespace nl::renderer::detail
