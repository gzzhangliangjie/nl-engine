// M1.0：glTF 读取公共实现。访问器 / RGBA8 解码 / 索引展开三个函数从
// renderer/src/scene.cpp 原样搬来（M0.4 起实测过的代码），新增的是场景图
// 遍历与节点变换烘焙——nanite-tools 切簇需要「世界空间的三角」，误差单位
// 从第一天就统一成世界空间长度（S1-PLAN 明令），所以变换必须在这一层烘死。

#include "nl/core/mesh_io.hpp"

// tinygltf 是 header-only：整个工程只在这一个 TU 展开实现（原在 scene.cpp，
// M1.0 随解析层一起搬来）。stb_image 的解码实现也随之只此一份。
#ifdef _MSC_VER
#  pragma warning(push, 0)
#endif
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define STB_IMAGE_IMPLEMENTATION
#include <tiny_gltf.h>
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#include <cmath>
#include <cstring>

namespace nl::core {
namespace {

using tinygltf::Model;

// ---- glTF 访问器读取（scene.cpp 原样搬来）----

// 读 FLOAT 类型的 glTF accessor（POSITION / NORMAL / TEXCOORD_0）。
// 支持 byteStride 交错存储；本项目的资产是紧密排列，这里仍按通用写法处理。
bool readFloatAccessor(const tinygltf::Model& model, int accessorIndex,
                       int wantComponents, std::vector<float>& out,
                       std::string& outError) {
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size())) {
        outError = "glTF accessor index out of range"; return false;
    }
    const auto& acc = model.accessors[accessorIndex];
    if (acc.componentType != 5126 /* FLOAT */ ||
        tinygltf::GetNumComponentsInType(static_cast<uint32_t>(acc.type)) != wantComponents) {
        outError = "glTF accessor is not a float VEC" + std::to_string(wantComponents);
        return false;
    }
    const auto& view = model.bufferViews[acc.bufferView];
    const auto& buffer = model.buffers[view.buffer];
    const size_t elemSize = sizeof(float) * static_cast<size_t>(wantComponents);
    const size_t stride = view.byteStride > 0 ? static_cast<size_t>(view.byteStride) : elemSize;
    const size_t base = static_cast<size_t>(view.byteOffset) + acc.byteOffset;
    if (acc.count == 0 || base + stride * (acc.count - 1) + elemSize > buffer.data.size()) {
        outError = "glTF accessor data truncated"; return false;
    }
    out.resize(acc.count * static_cast<size_t>(wantComponents));
    for (size_t i = 0; i < acc.count; ++i) {
        std::memcpy(out.data() + i * wantComponents,
                    buffer.data.data() + base + i * stride, elemSize);
    }
    return true;
}

// 索引统一展开成 uint32（R32_UINT），不区分 glTF 的 UNSIGNED_BYTE/SHORT/INT。
bool readIndices(const tinygltf::Model& model, int accessorIndex,
                 std::vector<std::uint32_t>& out, std::string& outError) {
    const auto& acc = model.accessors[accessorIndex];
    const auto& view = model.bufferViews[acc.bufferView];
    const auto& buffer = model.buffers[view.buffer];
    const int compSize = tinygltf::GetComponentSizeInBytes(
        static_cast<uint32_t>(acc.componentType));
    const int comps = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(acc.type));
    if (compSize <= 0 || comps != 1 ||
        (acc.componentType != 5121 && acc.componentType != 5123 && acc.componentType != 5125)) {
        outError = "unsupported glTF index component type"; return false;
    }
    const size_t stride = view.byteStride > 0
        ? static_cast<size_t>(view.byteStride) : static_cast<size_t>(compSize);
    const size_t base = static_cast<size_t>(view.byteOffset) + acc.byteOffset;
    if (acc.count == 0 || base + stride * (acc.count - 1) + compSize > buffer.data.size()) {
        outError = "glTF index data truncated"; return false;
    }
    out.resize(acc.count);
    const unsigned char* p = buffer.data.data() + base;
    for (size_t i = 0; i < acc.count; ++i) {
        const unsigned char* src = p + i * stride;
        std::uint32_t v = 0;
        if (acc.componentType == 5121) v = src[0];
        else if (acc.componentType == 5123) { std::uint16_t u; std::memcpy(&u, src, 2); v = u; }
        else { std::memcpy(&v, src, 4); }
        out[i] = v;
    }
    return true;
}

// tinygltf 用 stb_image 解出的像素可能是 1/2/3/4 通道，统一补成 RGBA8。
bool toRGBA8(const tinygltf::Image& image, std::vector<std::uint8_t>& out,
             std::string& outError) {
    if (image.width <= 0 || image.height <= 0 || image.image.empty() || image.bits != 8 ||
        image.component < 1 || image.component > 4) {
        outError = "glTF base color image is not 8-bit decoded RGBA-capable data";
        return false;
    }
    const size_t pixels = static_cast<size_t>(image.width) * image.height;
    if (image.image.size() < pixels * static_cast<size_t>(image.component)) {
        outError = "glTF base color image data truncated"; return false;
    }
    out.assign(pixels * 4, 255);
    for (size_t i = 0; i < pixels; ++i) {
        const unsigned char* s = image.image.data() + i * image.component;
        unsigned char* d = out.data() + i * 4;
        d[0] = s[0];
        d[1] = image.component >= 3 ? s[1] : s[0];
        d[2] = image.component >= 3 ? s[2] : s[0];
        d[3] = image.component == 4 ? s[3] : 255;
    }
    return true;
}

// ---- 4x4 变换（列主序，与 glTF node.matrix 存储顺序一致：m[col*4+row]）----

struct Mat4 {
    float m[16];
};

Mat4 matIdentity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 matMultiply(const Mat4& a, const Mat4& b) {   // a * b（先应用 b 再应用 a）
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

// glTF node 的局部变换：有 matrix 用 matrix；否则按 TRS 组合（规范定义的顺序）。
Mat4 nodeLocalMatrix(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        Mat4 r;
        for (int i = 0; i < 16; ++i) r.m[i] = static_cast<float>(node.matrix[i]);
        return r;
    }
    Mat4 r = matIdentity();
    // T
    if (node.translation.size() == 3) {
        r.m[12] = static_cast<float>(node.translation[0]);
        r.m[13] = static_cast<float>(node.translation[1]);
        r.m[14] = static_cast<float>(node.translation[2]);
    }
    // R：四元数 (x, y, z, w)
    if (node.rotation.size() == 4) {
        const float x = static_cast<float>(node.rotation[0]);
        const float y = static_cast<float>(node.rotation[1]);
        const float z = static_cast<float>(node.rotation[2]);
        const float w = static_cast<float>(node.rotation[3]);
        const float xx = x * x, yy = y * y, zz = z * z;
        const float xy = x * y, xz = x * z, yz = y * z;
        const float wx = w * x, wy = w * y, wz = w * z;
        const float rot[16] = {
            1 - 2 * (yy + zz),     2 * (xy + wz),     2 * (xz - wy), 0,
            2 * (xy - wz),     1 - 2 * (xx + zz),     2 * (yz + wx), 0,
            2 * (xz + wy),         2 * (yz - wx), 1 - 2 * (xx + yy), 0,
            0,                     0,                 0,             1,
        };
        Mat4 rm;
        std::memcpy(rm.m, rot, sizeof(rot));
        r = matMultiply(r, rm);
    }
    // S
    if (node.scale.size() == 3) {
        r.m[0] *= static_cast<float>(node.scale[0]);
        r.m[5] *= static_cast<float>(node.scale[1]);
        r.m[10] *= static_cast<float>(node.scale[2]);
    }
    return r;
}

void transformPoint(const Mat4& m, float p[3]) {
    const float x = p[0], y = p[1], z = p[2];
    const float w = m.m[3] * x + m.m[7] * y + m.m[11] * z + m.m[15];
    p[0] = m.m[0] * x + m.m[4] * y + m.m[8]  * z + m.m[12];
    p[1] = m.m[1] * x + m.m[5] * y + m.m[9]  * z + m.m[13];
    p[2] = m.m[2] * x + m.m[6] * y + m.m[10] * z + m.m[14];
    if (w != 0.0f && w != 1.0f) { p[0] /= w; p[1] /= w; p[2] /= w; }
}

// 法线用逆转置矩阵的左上 3x3（非均匀缩放也正确），结果归一化。
void transformNormal(const Mat4& m, float n[3]) {
    // 3x3 求逆（伴随法）。行列式退化时（非法资产）保持原值，不硬崩。
    const float a00 = m.m[0], a01 = m.m[4], a02 = m.m[8];
    const float a10 = m.m[1], a11 = m.m[5], a12 = m.m[9];
    const float a20 = m.m[2], a21 = m.m[6], a22 = m.m[10];
    const float det = a00 * (a11 * a22 - a12 * a21)
                    - a01 * (a10 * a22 - a12 * a20)
                    + a02 * (a10 * a21 - a11 * a20);
    if (det == 0.0f) return;
    const float inv = 1.0f / det;
    // 逆矩阵的转置 = 逆转置；直接按「原矩阵的余子式 / det」摆到转置位置。
    const float c00 =  (a11 * a22 - a12 * a21) * inv;
    const float c01 = -(a10 * a22 - a12 * a20) * inv;
    const float c02 =  (a10 * a21 - a11 * a20) * inv;
    const float c10 = -(a01 * a22 - a02 * a21) * inv;
    const float c11 =  (a00 * a22 - a02 * a20) * inv;
    const float c12 = -(a00 * a21 - a01 * a20) * inv;
    const float c20 =  (a01 * a12 - a02 * a11) * inv;
    const float c21 = -(a00 * a12 - a02 * a10) * inv;
    const float c22 =  (a00 * a11 - a01 * a10) * inv;
    // n' = transpose(inverse(M3)) * n：行用 c 的列索引。
    const float x = n[0], y = n[1], z = n[2];
    n[0] = c00 * x + c10 * y + c20 * z;
    n[1] = c01 * x + c11 * y + c21 * z;
    n[2] = c02 * x + c12 * y + c22 * z;
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0.0f) { n[0] /= len; n[1] /= len; n[2] /= len; }
}

bool matrixIsIdentity(const Mat4& m) {
    static const Mat4 ident = matIdentity();
    return std::memcmp(m.m, ident.m, sizeof(m.m)) == 0;
}

// ---- 材质 baseColor 提取（scene.cpp 语义原样保留）----

bool extractBaseColor(const Model& model, int materialIndex,
                      PrimitiveMesh& prim, std::string& outError) {
    if (materialIndex < 0 || materialIndex >= static_cast<int>(model.materials.size()))
        return true;   // 无材质不算错：工具侧只关心几何
    const int texIndex =
        model.materials[materialIndex].pbrMetallicRoughness.baseColorTexture.index;
    if (texIndex < 0 || texIndex >= static_cast<int>(model.textures.size()))
        return true;
    const int imageIndex = model.textures[texIndex].source;
    if (imageIndex < 0 || imageIndex >= static_cast<int>(model.images.size()))
        return true;
    if (!toRGBA8(model.images[imageIndex], prim.baseColorRGBA, outError)) return false;
    prim.hasBaseColor = true;
    prim.texWidth = static_cast<std::uint32_t>(model.images[imageIndex].width);
    prim.texHeight = static_cast<std::uint32_t>(model.images[imageIndex].height);
    return true;
}

// ---- 场景图遍历：把世界变换烘进每个 primitive 的顶点 ----

bool visitNode(const Model& model, int nodeIndex, const Mat4& parentWorld,
               SceneMeshData& out, std::string& outError, int depth) {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size())) {
        outError = "glTF node index out of range"; return false;
    }
    if (depth > 256) { outError = "glTF node graph too deep (cycle?)"; return false; }
    const auto& node = model.nodes[nodeIndex];
    const Mat4 world = matMultiply(parentWorld, nodeLocalMatrix(node));

    if (node.mesh >= 0 && node.mesh < static_cast<int>(model.meshes.size())) {
        const auto& mesh = model.meshes[node.mesh];
        const bool bake = !matrixIsIdentity(world);
        for (size_t p = 0; p < mesh.primitives.size(); ++p) {
            const auto& primGltf = mesh.primitives[p];
            if (primGltf.mode != 4 /* TRIANGLES */) continue;

            PrimitiveMesh prim;
            prim.name = mesh.name.empty()
                ? "mesh#" + std::to_string(node.mesh) + "#" + std::to_string(p)
                : mesh.name + "#" + std::to_string(p);

            auto attr = [&primGltf](const char* name) {
                auto it = primGltf.attributes.find(name);
                return it == primGltf.attributes.end() ? -1 : it->second;
            };
            std::vector<float> positions, normals, uvs;
            if (!readFloatAccessor(model, attr("POSITION"), 3, positions, outError))
                return false;
            const size_t vertexCount = positions.size() / 3;
            // NORMAL / TEXCOORD_0 可选：先判属性存在再读，缺失补零；
            // 存在但读失败（类型 / 越界）才是真错误。
            const bool hasNormals = attr("NORMAL") >= 0;
            const bool hasUvs = attr("TEXCOORD_0") >= 0;
            if (hasNormals && !readFloatAccessor(model, attr("NORMAL"), 3, normals, outError))
                return false;
            if (hasUvs && !readFloatAccessor(model, attr("TEXCOORD_0"), 2, uvs, outError))
                return false;
            if (hasNormals && normals.size() / 3 != vertexCount) {
                outError = prim.name + ": NORMAL count disagrees with POSITION"; return false;
            }
            if (hasUvs && uvs.size() / 2 != vertexCount) {
                outError = prim.name + ": TEXCOORD_0 count disagrees with POSITION"; return false;
            }

            prim.vertices.resize(vertexCount);
            for (size_t i = 0; i < vertexCount; ++i) {
                MeshVertex& v = prim.vertices[i];
                v.pos[0] = positions[i * 3]; v.pos[1] = positions[i * 3 + 1];
                v.pos[2] = positions[i * 3 + 2];
                if (hasNormals) {
                    v.normal[0] = normals[i * 3]; v.normal[1] = normals[i * 3 + 1];
                    v.normal[2] = normals[i * 3 + 2];
                } else { v.normal[0] = v.normal[1] = v.normal[2] = 0.0f; }
                if (hasUvs) {
                    v.uv[0] = uvs[i * 2]; v.uv[1] = uvs[i * 2 + 1];
                } else { v.uv[0] = v.uv[1] = 0.0f; }
                if (bake) {
                    transformPoint(world, v.pos);
                    if (hasNormals) transformNormal(world, v.normal);
                }
            }
            if (!readIndices(model, primGltf.indices, prim.indices, outError)) return false;
            if (!extractBaseColor(model, primGltf.material, prim, outError)) return false;
            out.meshes.push_back(std::move(prim));
        }
    }

    for (int child : node.children)
        if (!visitNode(model, child, world, out, outError, depth + 1)) return false;
    return true;
}

} // namespace

bool loadGltfScene(const char* path, SceneMeshData& out, std::string& outError) {
    out.meshes.clear();
    tinygltf::Model gltf;
    tinygltf::TinyGLTF loader;
    std::string err, warn;

    // 按扩展名选 loader：.glb 二进制，其余当 ASCII（tinygltf 会报具体错）。
    const std::string p = path ? path : "";
    const bool binary = p.size() >= 4 && p.compare(p.size() - 4, 4, ".glb") == 0;
    const bool ok = binary
        ? loader.LoadBinaryFromFile(&gltf, &err, &warn, p.c_str())
        : loader.LoadASCIIFromFile(&gltf, &err, &warn, p.c_str());
    if (!ok) {
        outError = "glTF load (" + p + ") failed: " + err;
        return false;
    }
    if (!warn.empty()) outError = "glTF warning: " + warn;   // 警告不阻断，交给调用方提示

    // vendored tinygltf 是老版本：默认场景字段叫 defaultScene，不是新版的 scene。
    int sceneIndex = gltf.defaultScene >= 0 && gltf.defaultScene < static_cast<int>(gltf.scenes.size())
        ? gltf.defaultScene : (gltf.scenes.empty() ? -1 : 0);
    if (sceneIndex < 0) { outError = "glTF file has no scene"; return false; }

    const Mat4 identity = matIdentity();
    for (int nodeIndex : gltf.scenes[sceneIndex].nodes)
        if (!visitNode(gltf, nodeIndex, identity, out, outError, 0)) return false;
    return true;
}

} // namespace nl::core
