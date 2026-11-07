#pragma once

// M1.0：glTF 场景读取的公共层。原是 renderer/src/scene.cpp 里的私有解析
// （M0.4 引入），S1 起离线工具 nanite-tools 也要吃同一份 glTF，故抽到 core。
// 纪律：core/ 不得出现任何图形 API 符号——这里只有纯 CPU 数据结构与文件解析。

#include <cstdint>
#include <string>
#include <vector>

namespace nl::core {

// 32 B 顶点，布局与 renderer 的 SceneVertex 逐字段一致
// （pos[12B] + normal[12B] + uv[8B]），两边的 GPU 输入布局共用同一约定。
struct MeshVertex {
    float pos[3];
    float normal[3];
    float uv[2];
};
static_assert(sizeof(MeshVertex) == 32, "MeshVertex must be tightly packed");

// 每个 glTF primitive 一份（即一个子网格）。
struct PrimitiveMesh {
    std::string name;                    // "<mesh 名>#<primitive 序号>"
    std::vector<MeshVertex> vertices;    // 节点世界变换已烘进 pos / normal
    std::vector<std::uint32_t> indices;  // 统一展开成 uint32
    // 材质 baseColor 贴图（解码成 RGBA8）；无贴图材质 hasBaseColor=false。
    bool hasBaseColor = false;
    std::uint32_t texWidth = 0;
    std::uint32_t texHeight = 0;
    std::vector<std::uint8_t> baseColorRGBA;
};

// 整个文件：遍历默认场景的所有 node × mesh × primitive，展平成子网格数组。
struct SceneMeshData {
    std::vector<PrimitiveMesh> meshes;
};

// 读 .glb（二进制）或 .gltf（ASCII，外部 buffer 暂不支持）。返回 false 时
// outError 带可读原因。POSITION 必有；NORMAL / TEXCOORD_0 缺失时补零。
bool loadGltfScene(const char* path, SceneMeshData& out, std::string& outError);

} // namespace nl::core
