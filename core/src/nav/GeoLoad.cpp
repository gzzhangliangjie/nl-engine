// P8 M-N2：nl/nav 几何汇集共享层。原 nav-tools main.cpp 私有实现抽出——
// viewer --nav-panel 面板烘焙与 nav-tools 吃同一份 glb 展平逻辑（同输入同
// Geometry，保证两处 bake 产物逐字节一致）。.nmesh fallback 分支留在
// nav-tools（viewer 面板烘焙只走 glb；nav-tools CLI 保持 M-N0 全部输入面）。
// 零第三方符号出头文件：tinygltf 经 nl::core::loadGltfScene（mesh_io）消费。

#include "nl/nav/Nav.hpp"

#include "nl/core/mesh_io.hpp"

namespace nl::nav {

bool loadGeometryFromGltf(const char* path, Geometry& out, std::string& outError) {
    outError.clear();
    nl::core::SceneMeshData scene;
    if (!nl::core::loadGltfScene(path, scene, outError)) return false;
    for (const nl::core::PrimitiveMesh& m : scene.meshes) {
        const std::uint32_t base =
            static_cast<std::uint32_t>(out.verts.size() / 3);
        for (const nl::core::MeshVertex& v : m.vertices)
            out.verts.insert(out.verts.end(), v.pos, v.pos + 3);
        for (const std::uint32_t idx : m.indices)
            out.tris.push_back(base + idx);
    }
    if (out.tris.empty()) {
        outError = std::string(path) + ": no triangles in glTF scene";
        return false;
    }
    return true;
}

} // namespace nl::nav
