// M-A4b/M-A5：rigged glTF/GLB 加载实现（口径见 gltf_anim.hpp 文件头）。
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

constexpr std::size_t kMaxJoints = 256;   // bevy MAX_JOINTS 同值（skin.rs:28）

// 读 float 数组访问器（times / translation / rotation / weights / IBM 共用）。
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

// 读整型 VEC4 访问器（JOINTS_0：ubyte/ushort 统一物化为 int，bevy
// into_joint_index_values 同款物化语义）。
bool readJointAccessor(const Model& model, int accessorIndex,
                       std::vector<std::array<int, 4>>& out) {
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
        return false;
    const tinygltf::Accessor& acc = model.accessors[static_cast<std::size_t>(accessorIndex)];
    if (tinygltf::GetNumComponentsInType(static_cast<uint32_t>(acc.type)) != 4) return false;
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
        acc.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
        return false;
    const std::size_t compSize = acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ? 1 : 2;
    const tinygltf::BufferView& bv = model.bufferViews[static_cast<std::size_t>(acc.bufferView)];
    const tinygltf::Buffer& buf = model.buffers[static_cast<std::size_t>(bv.buffer)];
    const std::size_t stride = bv.byteStride ? static_cast<std::size_t>(bv.byteStride) : 4 * compSize;
    const std::uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;
    out.resize(static_cast<std::size_t>(acc.count));
    for (std::size_t i = 0; i < static_cast<std::size_t>(acc.count); ++i) {
        const std::uint8_t* p = base + i * stride;
        for (int k = 0; k < 4; ++k) {
            int v = 0;
            if (compSize == 1) v = p[k];
            else v = static_cast<int>(p[k * 2]) | (static_cast<int>(p[k * 2 + 1]) << 8);
            out[i][static_cast<std::size_t>(k)] = v;
        }
    }
    return true;
}

// 读 uint 索引访问器（ubyte/ushort/uint）。
bool readIndexAccessor(const Model& model, int accessorIndex,
                       std::vector<std::uint32_t>& out) {
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
        return false;
    const tinygltf::Accessor& acc = model.accessors[static_cast<std::size_t>(accessorIndex)];
    if (tinygltf::GetNumComponentsInType(static_cast<uint32_t>(acc.type)) != 1) return false;
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
        acc.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
        acc.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
        return false;
    const std::size_t compSize =
        acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ? 1 :
        acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ? 2 : 4;
    const tinygltf::BufferView& bv = model.bufferViews[static_cast<std::size_t>(acc.bufferView)];
    const tinygltf::Buffer& buf = model.buffers[static_cast<std::size_t>(bv.buffer)];
    const std::uint8_t* base = buf.data.data() + bv.byteOffset + acc.byteOffset;
    out.resize(static_cast<std::size_t>(acc.count));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint8_t* p = base + i * compSize;
        if (compSize == 1) out[i] = p[0];
        else if (compSize == 2) out[i] = static_cast<std::uint32_t>(p[0]) |
                                         (static_cast<std::uint32_t>(p[1]) << 8);
        else out[i] = static_cast<std::uint32_t>(p[0]) |
                      (static_cast<std::uint32_t>(p[1]) << 8) |
                      (static_cast<std::uint32_t>(p[2]) << 16) |
                      (static_cast<std::uint32_t>(p[3]) << 24);
    }
    return true;
}

// 读 float VEC3/VEC4 属性（POSITION / NORMAL / WEIGHTS_0）。
bool readVecAccessor(const Model& model, int accessorIndex, std::size_t comps,
                     std::vector<float>& out) {
    return readFloatAccessor(model, accessorIndex, comps, out);
}

// DFS 收 scene 全部节点：拓扑序（父先于子）+ 名字 + 静态 TRS。
// ⚠ glTF 不保证 nodes 数组序 = 拓扑序（CesiumMan/RiggedFigure 实测父在子后），
// parentIndices 必须存「骨架表下标」，不是原始 node id——nodeId -> boneIndex
// 映射表修此 bug（Fox 恰好两序一致，之前没暴露）。M-A5：映射表外提为输出参数，
// 蒙皮 skin.joints / 动画通道 target 都靠它换算（动画通道原样用 node id 是
// 同族隐患，M-A5 一并修）。
bool collectBones(const Model& model, const tinygltf::Scene& scene, Skeleton& skeleton,
                  std::vector<int>& nodeIdToBone, std::string& error) {
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
    nodeIdToBone.assign(model.nodes.size(), -1);
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

// 共享核心：解析 GLB -> 骨架 + clips（+ 可选输出 node 映射，供蒙皮扩展）。
bool loadGltfCore(const std::string& path, Skeleton& skeleton,
                  std::vector<AnimClip>& clips, Model* modelOut,
                  std::vector<int>* nodeIdToBoneOut, std::string& error) {
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
    std::vector<int> nodeIdToBone;
    if (!collectBones(model, model.scenes[static_cast<std::size_t>(model.defaultScene)],
                      skeleton, nodeIdToBone, error))
        return false;
    if (const std::string bad = validateSkeleton(skeleton); !bad.empty()) {
        error = bad;
        return false;
    }

    // 静态 TRS -> 单关键帧轨道（每个骨骼先给常量轨道，动画通道覆盖之）。
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
                ch.target_node >= static_cast<int>(nodeIdToBone.size()) ||
                nodeIdToBone[static_cast<std::size_t>(ch.target_node)] < 0) {
                error = "gltf: animation targets node outside skeleton";
                return false;
            }
            const int bone = nodeIdToBone[static_cast<std::size_t>(ch.target_node)];
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
                                        [static_cast<std::size_t>(bone)];
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
    if (modelOut) *modelOut = std::move(model);
    if (nodeIdToBoneOut) *nodeIdToBoneOut = std::move(nodeIdToBone);
    return true;
}

// 权重归一化（bevy try_normalize_joint_weights 语义：负→0、零和→w0=1、
// 否则 ×1/sum）。加载即归一，validateSkinBinding 的 Σw≈1 才对真实资产成立。
void normalizeJointWeights(std::vector<std::array<float, 4>>& weights) {
    for (std::array<float, 4>& w : weights) {
        float sum = 0.0f;
        for (float v : w) sum += v > 0.0f ? v : 0.0f;
        for (float& v : w) if (v < 0.0f) v = 0.0f;
        if (sum <= 1e-8f) {
            w = { 1.0f, 0.0f, 0.0f, 0.0f };
        } else if (std::abs(sum - 1.0f) > 1e-6f) {
            const float inv = 1.0f / sum;
            for (float& v : w) v *= inv;
        }
    }
}

} // namespace

bool loadGltfAnim(const std::string& path, Skeleton& skeleton,
                  std::vector<AnimClip>& clips, std::string& error) {
    return loadGltfCore(path, skeleton, clips, nullptr, nullptr, error);
}

bool loadGltfSkinnedModel(const std::string& path, Skeleton& skeleton,
                          std::vector<AnimClip>& clips, SkinnedMeshData& mesh,
                          SkinBinding& skin, std::string& error) {
    tinygltf::Model model;
    std::vector<int> nodeIdToBone;
    if (!loadGltfCore(path, skeleton, clips, &model, &nodeIdToBone, error))
        return false;
    mesh = SkinnedMeshData{};
    skin = SkinBinding{};

    // ---- 1. 皮肤：第一个 skin（ joints 数 > 256 拒绝，bevy MAX_JOINTS 同值）----
    if (model.skins.empty()) {
        error = "gltf: no skin in file (use loadGltfAnim for animation-only)";
        return false;
    }
    const tinygltf::Skin& gltfSkin = model.skins.front();
    if (gltfSkin.joints.size() > kMaxJoints) {
        error = "gltf: skin joint count " + std::to_string(gltfSkin.joints.size()) +
                " exceeds supported " + std::to_string(kMaxJoints);
        return false;
    }

    // inverseBindMatrices：MAT4 float，按 skin.joints 顺序；缺失 → identity 兜底
    // （bevy loader unwrap_or_else(IDENTITY) 同款回退）。
    skin.inverseBind.assign(skeleton.boneCount(), mat4Identity());
    if (gltfSkin.inverseBindMatrices >= 0) {
        std::vector<float> ibm;
        if (!readFloatAccessor(model, gltfSkin.inverseBindMatrices, 16, ibm)) {
            error = "gltf: bad inverseBindMatrices accessor";
            return false;
        }
        if (ibm.size() != gltfSkin.joints.size() * 16) {
            error = "gltf: inverseBindMatrices count != skin joints count";
            return false;
        }
        for (std::size_t k = 0; k < gltfSkin.joints.size(); ++k) {
            const int node = gltfSkin.joints[k];
            if (node < 0 || node >= static_cast<int>(nodeIdToBone.size()) ||
                nodeIdToBone[static_cast<std::size_t>(node)] < 0) {
                error = "gltf: skin joint node outside skeleton";
                return false;
            }
            const std::size_t b = static_cast<std::size_t>(
                nodeIdToBone[static_cast<std::size_t>(node)]);
            std::memcpy(skin.inverseBind[b].m, &ibm[k * 16], sizeof(Mat4));
        }
    }

    // ---- 2. 网格：第一个含 mesh 的节点 + primitives[0]（TRIANGLES）----
    // glTF 规范：蒙皮网格节点的自身变换被忽略，顶点按 bind 空间原样使用。
    int meshNode = -1;
    for (std::size_t n = 0; n < model.nodes.size(); ++n)
        if (model.nodes[n].mesh >= 0) { meshNode = static_cast<int>(n); break; }
    if (meshNode < 0) {
        error = "gltf: no mesh node in file";
        return false;
    }
    const tinygltf::Mesh& gmesh = model.meshes[static_cast<std::size_t>(
        model.nodes[static_cast<std::size_t>(meshNode)].mesh)];
    if (gmesh.primitives.empty()) {
        error = "gltf: mesh has no primitives";
        return false;
    }
    const tinygltf::Primitive& prim = gmesh.primitives.front();
    if (prim.mode != TINYGLTF_MODE_TRIANGLES) {
        error = "gltf: primitive mode is not TRIANGLES";
        return false;
    }
    auto attr = [&](const char* name) -> int {
        auto it = prim.attributes.find(name);
        return it == prim.attributes.end() ? -1 : it->second;
    };
    const int posAcc = attr("POSITION"), nrmAcc = attr("NORMAL");
    const int jointAcc = attr("JOINTS_0"), weightAcc = attr("WEIGHTS_0");
    if (posAcc < 0 || nrmAcc < 0 || jointAcc < 0 || weightAcc < 0) {
        error = "gltf: primitive lacks POSITION/NORMAL/JOINTS_0/WEIGHTS_0";
        return false;
    }
    std::vector<float> px, nx, wx;
    std::vector<std::array<int, 4>> jx;
    if (!readVecAccessor(model, posAcc, 3, px) || !readVecAccessor(model, nrmAcc, 3, nx) ||
        !readJointAccessor(model, jointAcc, jx) ||
        !readVecAccessor(model, weightAcc, 4, wx)) {
        error = "gltf: bad mesh attribute accessor";
        return false;
    }
    const std::size_t vertexCount = px.size() / 3;
    if (nx.size() != vertexCount * 3 || jx.size() != vertexCount ||
        wx.size() != vertexCount * 4) {
        error = "gltf: mesh attribute sizes mismatch";
        return false;
    }
    if (prim.indices < 0 || !readIndexAccessor(model, prim.indices, mesh.indices)) {
        error = "gltf: bad mesh indices";
        return false;
    }
    for (std::uint32_t idx : mesh.indices)
        if (idx >= vertexCount) {
            error = "gltf: index out of range";
            return false;
        }

    mesh.positions.resize(vertexCount);
    mesh.normals.resize(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        mesh.positions[i] = { px[i * 3], px[i * 3 + 1], px[i * 3 + 2] };
        mesh.normals[i] = { nx[i * 3], nx[i * 3 + 1], nx[i * 3 + 2] };
    }

    // joints 换算到骨架下标 + 权重归一化（bevy 语义）。
    skin.joints.resize(vertexCount);
    skin.weights.resize(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        for (int k = 0; k < 4; ++k) {
            const int node = jx[i][static_cast<std::size_t>(k)];
            skin.joints[i][static_cast<std::size_t>(k)] =
                node >= 0 && node < static_cast<int>(nodeIdToBone.size())
                    ? nodeIdToBone[static_cast<std::size_t>(node)]
                    : -1;
        }
        skin.weights[i] = { wx[i * 4], wx[i * 4 + 1], wx[i * 4 + 2], wx[i * 4 + 3] };
    }
    normalizeJointWeights(skin.weights);

    if (const std::string bad =
            validateSkinBinding(skin, vertexCount, skeleton.boneCount()); !bad.empty()) {
        error = bad;
        return false;
    }
    return true;
}

} // namespace nl::anim
