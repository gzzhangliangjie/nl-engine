// M-A4b：rigged glTF/GLB 加载实现（口径见 gltf_anim.hpp 文件头）。
// tinygltf 的 IMPLEMENTATION 只在 core/src/mesh_io.cpp 展开（单 TU 约定），
// 本 TU 只 include 声明并链接到那份实现。

#include "nl/anim/gltf_anim.hpp"

// ⚠ 宏视图必须与 mesh_io.cpp（IMPLEMENTATION TU）一致：它定义了
// TINYGLTF_NO_STB_IMAGE_WRITE。不对齐会导致 inline 构造函数引用一个
// 被禁用的 WriteImageData 符号（LNK2019，2026-11-08 M-A4b 实测）。
#ifdef _MSC_VER
#  pragma warning(push, 0)
#endif
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace nl::anim {
namespace {

using tinygltf::Model;

// 读 float 数组访问器（times / translation / rotation 共用）。
bool readFloatAccessor(const Model& model, int accessorIndex,
                       std::size_t wantComponents, std::vector<float>& out) {
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
        return false;
    const tinygltf::Accessor& acc = model.accessors[static_cast<std::size_t>(accessorIndex)];
    if (tinygltf::GetNumComponentsInType(static_cast<uint32_t>(acc.type)) !=
        static_cast<int>(wantComponents))
        return false;
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return false;
    const tinygltf::BufferView& bv = model.bufferViews[static_cast<std::size_t>(acc.bufferView)];
    const tinygltf::Buffer& buf = model.buffers[static_cast<std::size_t>(bv.buffer)];
    const std::size_t stride = bv.byteStride ? static_cast<std::size_t>(bv.byteStride)
                                             : wantComponents * sizeof(float);
    const std::uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;
    out.resize(static_cast<std::size_t>(acc.count) * wantComponents);
    for (std::size_t i = 0; i < static_cast<std::size_t>(acc.count); ++i)
        std::memcpy(&out[i * wantComponents], base + i * stride, wantComponents * sizeof(float));
    return true;
}

// DFS 收 scene 全部节点：拓扑序（父先于子）+ 名字 + 静态 TRS。
// ⚠ glTF 不保证 nodes 数组序 = 拓扑序（CesiumMan/RiggedFigure 实测父在子后），
// parentIndices 必须存「骨架表下标」，不是原始 node id——nodeId -> boneIndex
// 映射表修此 bug（Fox 恰好两序一致，之前没暴露）。
bool collectBones(const Model& model, const tinygltf::Scene& scene, Skeleton& skeleton,
                  std::string& error) {
    std::vector<int> parentOf(model.nodes.size(), -1);
    {
        std::vector<int> stack(scene.nodes.begin(), scene.nodes.end());
        while (!stack.empty()) {
            const int node = stack.back();
            stack.pop_back();
            for (const int child : model.nodes[static_cast<std::size_t>(node)].children) {
                if (parentOf[static_cast<std::size_t>(child)] != -1) {
                    error = "gltf: node has multiple parents (scene graph is not a tree)";
                    return false;
                }
                parentOf[static_cast<std::size_t>(child)] = node;
                stack.push_back(child);
            }
        }
    }
    // DFS（显式栈，父展开后子紧随其后追加）=> 输出序即拓扑序。
    std::vector<int> nodeIdToBone(model.nodes.size(), -1);
    struct Frame { int node; bool expanded; };
    std::vector<Frame> work;
    for (const int root : scene.nodes) work.push_back({ root, false });
    while (!work.empty()) {
        Frame& top = work.back();
        if (top.expanded) {
            work.pop_back();
            continue;
        }
        top.expanded = true;
        const tinygltf::Node& n = model.nodes[static_cast<std::size_t>(top.node)];
        std::vector<Frame> children;
        for (const int child : n.children) children.push_back({ child, false });
        // 逆序压栈 = 正序弹出，子节点紧随父节点之后追加。
        for (auto it = children.rbegin(); it != children.rend(); ++it) work.push_back(*it);
        const std::size_t boneIndex = skeleton.boneCount();
        nodeIdToBone[static_cast<std::size_t>(top.node)] = static_cast<int>(boneIndex);
        std::string name = n.name;
        if (name.empty()) name = "bone_" + std::to_string(boneIndex);
        BonePose rest;
        if (!n.translation.empty()) {
            rest.translation = { static_cast<float>(n.translation[0]),
                                 static_cast<float>(n.translation[1]),
                                 static_cast<float>(n.translation[2]) };
        }
        if (!n.rotation.empty()) {
            rest.rotation = normalizeQuat({ static_cast<float>(n.rotation[0]),
                                            static_cast<float>(n.rotation[1]),
                                            static_cast<float>(n.rotation[2]),
                                            static_cast<float>(n.rotation[3]) });
        }
        if (!n.scale.empty()) {
            for (const double s : n.scale)
                if (std::abs(s - 1.0) > 1e-6) {
                    error = "gltf: node static scale not supported (keep it 1)";
                    return false;
                }
        }
        skeleton.boneNames.push_back(name);
        skeleton.parentIndices.push_back(
            parentOf[static_cast<std::size_t>(top.node)] == -1
                ? -1
                : nodeIdToBone[static_cast<std::size_t>(parentOf[static_cast<std::size_t>(top.node)])]);
        skeleton.restPose.push_back(rest);
    }
    return true;
}

} // namespace

bool loadGltfAnim(const std::string& path, Skeleton& skeleton,
                  std::vector<AnimClip>& clips, std::string& error) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string warn, err;
    if (!loader.LoadBinaryFromFile(&model, &err, &warn, path)) {
        error = "gltf: load failed: " + (err.empty() ? warn : err);
        return false;
    }
    if (model.scenes.empty() || model.defaultScene < 0 ||
        model.defaultScene >= static_cast<int>(model.scenes.size())) {
        error = "gltf: no default scene";
        return false;
    }

    skeleton = Skeleton{};
    clips.clear();
    if (!collectBones(model, model.scenes[static_cast<std::size_t>(model.defaultScene)],
                      skeleton, error))
        return false;
    if (const std::string bad = validateSkeleton(skeleton); !bad.empty()) {
        error = bad;
        return false;
    }

    // 静态 TRS -> 单关键帧轨道（每个骨骼先给常量轨道，动画通道覆盖之）。
    clips.clear();
    std::vector<bool> hasTrack(skeleton.boneCount(), false);
    std::vector<BoneTrack> tracks(skeleton.boneCount());
    for (std::size_t i = 0; i < skeleton.boneCount(); ++i) {
        tracks[i].bone = skeleton.boneNames[i];
        tracks[i].translation.times = { 0.0f };
        tracks[i].translation.values = { skeleton.restPose[i].translation };
        tracks[i].rotation.times = { 0.0f };
        tracks[i].rotation.values = { skeleton.restPose[i].rotation };
    }

    // clips[name] = { duration, tracks } 的覆盖语义：先拷贝常量轨道再写动画。
    std::vector<std::string> clipOrder;
    std::vector<float> clipDuration;
    std::vector<std::vector<BoneTrack>> clipTracks;
    auto clipIndexByName = [&](const std::string& name) -> int {
        for (std::size_t i = 0; i < clipOrder.size(); ++i)
            if (clipOrder[i] == name) return static_cast<int>(i);
        clipOrder.push_back(name);
        clipDuration.push_back(0.0f);
        clipTracks.push_back(tracks);   // 以静态 TRS 常量轨道打底
        return static_cast<int>(clipOrder.size()) - 1;
    };

    for (const tinygltf::Animation& anim : model.animations) {
        const std::string clipName =
            anim.name.empty() ? "clip_" + std::to_string(clipOrder.size()) : anim.name;
        const int ci = clipIndexByName(clipName);
        for (const tinygltf::AnimationChannel& ch : anim.channels) {
            if (ch.target_node < 0 ||
                ch.target_node >= static_cast<int>(skeleton.boneCount())) {
                error = "gltf: animation targets node outside skeleton";
                return false;
            }
            const tinygltf::AnimationSampler& sp =
                anim.samplers[static_cast<std::size_t>(ch.sampler)];
            Interp interp;
            if (sp.interpolation == "LINEAR" || sp.interpolation.empty()) interp = Interp::Linear;
            else if (sp.interpolation == "STEP") interp = Interp::Step;
            else {
                error = "gltf: sampler interpolation " + sp.interpolation + " not supported";
                return false;
            }
            std::vector<float> times, values;
            if (!readFloatAccessor(model, sp.input, 1, times) ||
                !readFloatAccessor(model, sp.output,
                                   ch.target_path == "rotation" ? 4 : 3, values)) {
                error = "gltf: bad animation accessor";
                return false;
            }
            if (times.empty() || times.size() * (ch.target_path == "rotation" ? 4 : 3) !=
                                     values.size()) {
                error = "gltf: animation times/values size mismatch";
                return false;
            }
            for (const float t : times) {
                if (!std::isfinite(t)) {
                    error = "gltf: non-finite animation time";
                    return false;
                }
                clipDuration[static_cast<std::size_t>(ci)] =
                    std::max(clipDuration[static_cast<std::size_t>(ci)], t);
            }
            BoneTrack& track = clipTracks[static_cast<std::size_t>(ci)]
                                        [static_cast<std::size_t>(ch.target_node)];
            if (ch.target_path == "translation") {
                track.translation.interp = interp;
                track.translation.times = times;
                track.translation.values.clear();
                for (std::size_t k = 0; k < times.size(); ++k)
                    track.translation.values.push_back(
                        { values[k * 3], values[k * 3 + 1], values[k * 3 + 2] });
            } else if (ch.target_path == "rotation") {
                track.rotation.interp = interp;
                track.rotation.times = times;
                track.rotation.values.clear();
                for (std::size_t k = 0; k < times.size(); ++k)
                    track.rotation.values.push_back(normalizeQuat(
                        { values[k * 4], values[k * 4 + 1], values[k * 4 + 2],
                          values[k * 4 + 3] }));
            } else if (ch.target_path == "scale") {
                // scale 通道口径（2026-11-08 标准资产接入时定）：值恒 1（空操作，
                // CesiumMan/RiggedFigure 等导出器常带）→ 验证后丢弃；非恒 1 →
                // 拒绝（BonePose 无 scale 口径，静默丢非平凡 scale = 假数据）。
                bool allOnes = true;
                for (const float s : values)
                    if (std::abs(s - 1.0f) > 1e-4f) { allOnes = false; break; }
                if (!allOnes) {
                    error = "gltf: non-trivial animation scale channel not supported";
                    return false;
                }
            } else {
                error = "gltf: animation path " + ch.target_path + " not supported";
                return false;
            }
        }
    }

    for (std::size_t i = 0; i < clipOrder.size(); ++i) {
        AnimClip clip;
        clip.duration = clipDuration[i];
        clip.tracks = std::move(clipTracks[i]);
        if (const std::string bad = validateClip(clip); !bad.empty()) {
            error = "gltf: clip " + clipOrder[i] + ": " + bad;
            return false;
        }
        clips.push_back(std::move(clip));
    }
    return true;
}

} // namespace nl::anim
