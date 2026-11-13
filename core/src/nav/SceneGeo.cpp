// P8 M-N3：E1 场景几何汇集（nl::scene World JSON -> nl::nav Geometry）。
// 口径：与 viewer --scene 同一份组件注册 / deserialise / O(N) 遍历，吃
// ModelComponent.meshFile + TransformComponent.getWorldMatrix()（含父子层级
// 复合），按 meshFile 缓存 glb 几何避免 50 物体重复加载。零图形符号。
#include "nl/nav/Nav.hpp"

#include "nl/scene/Component.hpp"
#include "nl/scene/World.hpp"
#include "nl/scene/AssetName.hpp"   // M-SC7：白名单单源（本文件 validMeshName 原为语义复制点）
#include "json.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace nl::nav {

// meshFile 白名单：M-SC7 起实现单源 = nl::scene::validGlbAssetName
//（本函数历史语义复制点，收编后删除；放行字符集由 alnum 收窄为可打印 ASCII，
// 路径穿越防护语义不变——AssetName.hpp 头注）。

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
        if (!nl::scene::validGlbAssetName(model->meshFile.c_str())) {
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
