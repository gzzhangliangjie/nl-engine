// P8 M-N3：E1 场景几何汇集（nl::scene World JSON -> nl::nav Geometry）。
// 口径：与 viewer --scene 同一份组件注册 / deserialise / O(N) 遍历，吃
// ModelComponent.meshFile + TransformComponent.getWorldMatrix()（含父子层级
// 复合），按 meshFile 缓存 glb 几何避免 50 物体重复加载。零图形符号。
#include "nl/nav/Nav.hpp"

#include "nl/scene/Component.hpp"
#include "nl/scene/World.hpp"
#include "json.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace nl::nav {

// meshFile 白名单（与 viewer --scene validGlbAssetName 同款：basename + .glb，
// 拒路径分隔符与 ".."）。场景文件来自用户，防路径穿越。
static bool validMeshName(const std::string& s) {
    if (s.empty()) return false;
    if (s.find('/') != std::string::npos ||
        s.find('\\') != std::string::npos) return false;
    if (s.find("..") != std::string::npos) return false;
    const std::string ext = ".glb";
    if (s.size() < ext.size() + 1 ||
        s.compare(s.size() - ext.size(), ext.size(), ext) != 0) return false;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (!(u >= 'a' && u <= 'z') && !(u >= 'A' && u <= 'Z') &&
            !(u >= '0' && u <= '9') && c != '_' && c != '-' && c != '.')
            return false;
    }
    return true;
}

int geometryEntityCount(const char* scenePath, std::string& outError) {
    outError.clear();
    if (!scenePath) { outError = "null scene path"; return -1; }
    std::ifstream in(scenePath, std::ios::binary);
    if (!in) { outError = std::string("cannot open scene: ") + scenePath; return -1; }
    nlohmann::json doc;
    try {
        in >> doc;
        if (!doc.contains("entities") || !doc["entities"].is_array()) {
            outError = "scene has no entities array";
            return -1;
        }
        int n = 0;
        for (const auto& e : doc["entities"])
            if (e.is_object() && e.contains("components") &&
                e["components"].is_object() &&
                e["components"].contains("Model")) ++n;
        return n;
    } catch (const std::exception& ex) {
        outError = std::string("malformed scene json: ") + ex.what();
        return -1;
    }
}

bool loadGeometryFromScene(const char* scenePath, const char* meshDir,
                           Geometry& out, std::string& outError) {
    outError.clear();
    out.verts.clear();
    out.tris.clear();
    if (!scenePath || !meshDir) { outError = "null scene/meshDir path"; return false; }

    std::ifstream in(scenePath, std::ios::binary);
    if (!in) {
        outError = std::string("cannot open scene: ") + scenePath;
        return false;
    }
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const std::exception& ex) {
        outError = std::string("malformed scene json: ") + ex.what();
        return false;
    }

    nl::scene::World world;
    world.getComponentLibrary().registerComponent<nl::scene::TransformComponent>();
    world.getComponentLibrary().registerComponent<nl::scene::ModelComponent>();
    world.getComponentLibrary().registerComponent<nl::scene::LightComponent>();
    std::string err;
    bool ok = false;
    try {
        ok = world.deserialise(doc, err);
    } catch (const std::exception& ex) {
        outError = std::string("scene deserialise threw: ") + ex.what();
        return false;
    }
    if (!ok) {
        outError = "scene deserialise rejected: " + err;
        return false;
    }

    std::unordered_map<std::string, Geometry> cache;
    int modelCount = 0;
    for (const nl::scene::EntityID id : world.getEntityIDs()) {
        const auto* model = world.getComponent<nl::scene::ModelComponent>(id);
        if (!model) continue;
        const auto* tr = world.getComponent<nl::scene::TransformComponent>(id);
        if (!tr) {
            outError = "entity " + std::to_string(id) +
                       " has Model without Transform";
            return false;
        }
        if (!validMeshName(model->meshFile)) {
            outError = "entity " + std::to_string(id) + " meshFile '" +
                       model->meshFile + "' rejected by whitelist (.glb basename)";
            return false;
        }
        auto it = cache.find(model->meshFile);
        if (it == cache.end()) {
            Geometry g;
            const std::string path = std::string(meshDir) + "/" + model->meshFile;
            if (!loadGeometryFromGltf(path.c_str(), g, err)) {
                outError = "entity " + std::to_string(id) + " mesh '" +
                           model->meshFile + "': " + err;
                return false;
            }
            it = cache.emplace(model->meshFile, std::move(g)).first;
        }
        const Geometry& g = it->second;
        const auto wm = tr->getWorldMatrix();  // 列主序 m[col*4+row]
        const std::uint32_t base =
            static_cast<std::uint32_t>(out.verts.size() / 3);
        for (std::size_t v = 0; v + 2 < g.verts.size(); v += 3) {
            const float x = g.verts[v], y = g.verts[v + 1], z = g.verts[v + 2];
            out.verts.push_back(wm.m[0] * x + wm.m[4] * y + wm.m[8]  * z + wm.m[12]);
            out.verts.push_back(wm.m[1] * x + wm.m[5] * y + wm.m[9]  * z + wm.m[13]);
            out.verts.push_back(wm.m[2] * x + wm.m[6] * y + wm.m[10] * z + wm.m[14]);
        }
        for (const std::uint32_t idx : g.tris) out.tris.push_back(base + idx);
        ++modelCount;
    }
    if (modelCount == 0 || out.tris.empty()) {
        outError = "scene has no Model entities with geometry";
        return false;
    }
    return true;
}

} // namespace nl::nav
