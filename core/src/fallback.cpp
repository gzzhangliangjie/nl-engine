// M3.1：fallback mesh 拼接实现。口径见 fallback.hpp 头注释。
// 确定性：weldPositions 是排序 + 相邻比较（grouping.hpp 同一函数），
// 输出顶点顺序由首现位置决定，同输入同输出。

#include "nl/core/fallback.hpp"

#include "nl/core/grouping.hpp"   // weldPositions（位级相等 weld 唯一口径）

#include <cmath>

namespace nl::core {

bool buildFallbackMesh(const NMeshData& top, FallbackMesh& out, std::string& outError) {
    out = FallbackMesh{};

    if (top.meshlets.empty()) {
        outError = "fallback: input has no clusters";
        return false;
    }

    // 1) 位级位置 weld：canonical 点编号。边界点两侧是同一 float 的拷贝，
    //    在这里自然合并；normal/uv 取首现顶点。
    std::vector<std::uint32_t> canon;
    const std::uint32_t pointCount = weldPositions(top.vertices, canon);

    // 2) canonical 点 → 输出顶点（首现位置持有完整属性）。
    std::vector<std::uint32_t> remap(pointCount, 0xFFFFFFFFu);
    out.vertices.reserve(pointCount);
    for (std::size_t i = 0; i < top.vertices.size(); ++i) {
        const std::uint32_t p = canon[i];
        if (remap[p] == 0xFFFFFFFFu) {
            remap[p] = static_cast<std::uint32_t>(out.vertices.size());
            out.vertices.push_back(top.vertices[i]);
        }
    }

    // 3) 三角索引：簇局部索引 → 全局顶点窗口 → 输出编号。
    out.indices.reserve(std::size_t(top.triangles.size()) * 3);
    for (std::size_t mi = 0; mi < top.meshlets.size(); ++mi) {
        const Meshlet& m = top.meshlets[mi];
        for (std::uint32_t t = 0; t < m.triangleCount; ++t) {
            // triangleOffset 是【字节偏移】（M1.1 口径：triOffset =
            // out->triangles.size()，每三角 3 字节），不是三角号——按三角号
            // ×3 会读错窗口（M3.1 已踩，全仓其余使用点均为字节语义）。
            const std::size_t base =
                std::size_t(m.triangleOffset) + std::size_t(t) * 3;
            for (int k = 0; k < 3; ++k) {
                const std::uint8_t local = top.triangles[base + std::size_t(k)];
                if (std::size_t(local) >= m.vertexCount) {
                    outError = "fallback: cluster " + std::to_string(mi) +
                               " (vo=" + std::to_string(m.vertexOffset) +
                               " vc=" + std::to_string(m.vertexCount) +
                               " to=" + std::to_string(m.triangleOffset) +
                               " tc=" + std::to_string(m.triangleCount) +
                               ") tri " + std::to_string(t) +
                               " local index " + std::to_string(local) +
                               " >= vertexCount " + std::to_string(m.vertexCount);
                    return false;
                }
                const std::uint32_t p =
                    canon[std::size_t(m.vertexOffset) + std::size_t(local)];
                out.indices.push_back(remap[p]);
            }
        }
    }

    // 4) 退化三角拒绝（拼接是恒等变换，出现退化 = 上游已坏，当场挡下）。
    for (std::size_t t = 0; t < out.indices.size(); t += 3) {
        const MeshVertex& a = out.vertices[out.indices[t]];
        const MeshVertex& b = out.vertices[out.indices[t + 1]];
        const MeshVertex& c = out.vertices[out.indices[t + 2]];
        const float u[3] = { b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2] };
        const float w[3] = { c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2] };
        const float cr[3] = { u[1] * w[2] - u[2] * w[1],
                              u[2] * w[0] - u[0] * w[2],
                              u[0] * w[1] - u[1] * w[0] };
        if (cr[0] == 0.f && cr[1] == 0.f && cr[2] == 0.f) {
            outError = "fallback: degenerate triangle at index " +
                       std::to_string(t / 3);
            return false;
        }
    }

    out.sourceLevel = top.lodMeta.present ? top.lodMeta.level : 0;
    out.maxError = top.lodMeta.present ? top.lodMeta.maxError : 0.f;
    return true;
}

} // namespace nl::core
