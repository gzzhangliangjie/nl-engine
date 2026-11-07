// M1.2：.nmesh 二进制读写实现。布局约定见 meshlet_io.hpp 头注释。
// Windows 目标 = little-endian x64；直接内存映像写读，不做字节交换
// （跨架构支持等真有需求再加，别提前设计）。

#include "nl/core/meshlet_io.hpp"

#include <cstdio>
#include <cstring>

namespace nl::core {
namespace {

// RAII 文件句柄，防中途 return 泄漏
struct FileHandle {
    std::FILE* fp = nullptr;
    ~FileHandle() { if (fp) std::fclose(fp); }
};

bool readFileAll(const char* path, std::vector<std::uint8_t>& out, std::string& outError) {
    FileHandle h;
    h.fp = std::fopen(path, "rb");
    if (!h.fp) {
        outError = std::string("cannot open for read: ") + path;
        return false;
    }
    std::fseek(h.fp, 0, SEEK_END);
    const long size = std::ftell(h.fp);
    std::fseek(h.fp, 0, SEEK_SET);
    if (size < 0) {
        outError = std::string("ftell failed: ") + path;
        return false;
    }
    out.resize(static_cast<size_t>(size));
    if (size > 0 &&
        std::fread(out.data(), 1, static_cast<size_t>(size), h.fp) != static_cast<size_t>(size)) {
        outError = std::string("short read: ") + path;
        return false;
    }
    return true;
}

} // namespace

bool saveNMesh(const char* path, const NMeshData& data, std::string& outError) {
    if (!path || !*path) { outError = "empty output path"; return false; }
    if (data.meshlets.size() != data.bounds.size()) {
        outError = "meshlets/bounds size mismatch"; return false;
    }
    if (data.meshlets.size() > 0xFFFFFFFFu || data.vertices.size() > 0xFFFFFFFFu ||
        data.triangles.size() > 0xFFFFFFFFu) {
        outError = "section size exceeds uint32 range"; return false;
    }
    const std::string name = data.name.substr(0, 0xFFFF);

    NMeshHeader header{};
    header.magic = kNMeshMagic;
    // M2.2：带 LOD 元数据的分层资产写 version 2（只加尾部 section）；
    // 普通资产仍写 version 1，逐字节与 M1.2 一致。
    header.version = data.lodMeta.present ? kNMeshVersionLod : kNMeshVersion;
    header.nameLen = static_cast<std::uint16_t>(name.size());
    header.meshletCount = static_cast<std::uint32_t>(data.meshlets.size());
    header.vertexCount = static_cast<std::uint32_t>(data.vertices.size());
    header.triangleCount = static_cast<std::uint32_t>(data.triangles.size() / 3);

    // 组装整块内存一次写出：write(tmp) → rename，失败不留半个文件
    std::vector<std::uint8_t> blob;
    blob.reserve(sizeof(header) + name.size() +
                 data.meshlets.size() * sizeof(NMeshletRecord) +
                 data.vertices.size() * sizeof(MeshVertex) + data.triangles.size());
    const auto* hp = reinterpret_cast<const std::uint8_t*>(&header);
    blob.insert(blob.end(), hp, hp + sizeof(header));
    blob.insert(blob.end(), name.begin(), name.end());
    for (const auto& m : data.meshlets) {
        NMeshletRecord rec{};
        rec.vertexOffset = m.vertexOffset;
        rec.vertexCount = m.vertexCount;
        rec.triangleOffset = m.triangleOffset;
        rec.triangleCount = m.triangleCount;
        static_assert(sizeof(MeshletBounds::sphere) == sizeof(rec.sphere), "sphere size");
        static_assert(sizeof(MeshletBounds::coneAxis) == sizeof(rec.coneAxis), "cone size");
        std::memcpy(rec.sphere, data.bounds[&m - data.meshlets.data()].sphere, sizeof(rec.sphere));
        std::memcpy(rec.coneAxis, data.bounds[&m - data.meshlets.data()].coneAxis, sizeof(rec.coneAxis));
        rec.coneCutoff = data.bounds[&m - data.meshlets.data()].coneCutoff;
        rec.error = data.bounds[&m - data.meshlets.data()].error;
        const auto* rp = reinterpret_cast<const std::uint8_t*>(&rec);
        blob.insert(blob.end(), rp, rp + sizeof(rec));
    }
    {
        const auto* vp = reinterpret_cast<const std::uint8_t*>(data.vertices.data());
        blob.insert(blob.end(), vp,
                    vp + data.vertices.size() * sizeof(MeshVertex));
    }
    blob.insert(blob.end(), data.triangles.begin(), data.triangles.end());

    if (data.lodMeta.present) {
        const std::size_t floats = data.lodMeta.lockedPositions.size();
        if (floats % 3 != 0 || floats * sizeof(float) > 0xFFFFFFFFu) {
            outError = "lod meta: lockedPositions size invalid";
            return false;
        }
        // M3.0：dagPresent 时写 meta version 2（锁点 + DAG 血缘）。
        const bool dag = data.lodMeta.dagPresent;
        if (dag && data.lodMeta.clusterGroup.size() != data.meshlets.size()) {
            outError = "lod meta: clusterGroup size must match meshlet count";
            return false;
        }
        if (dag && data.lodMeta.clusterGroup.size() > 0xFFFFFFFFu) {
            outError = "lod meta: cluster count exceeds uint32 range";
            return false;
        }
        NMeshMetaHeader meta{};
        meta.magic = kNMeshLodMagic;
        meta.version = dag ? kNMeshMetaVersionDag : kNMeshMetaVersion;
        meta.level = data.lodMeta.level;
        meta.maxError = data.lodMeta.maxError;
        meta.lockedCount = static_cast<std::uint32_t>(floats / 3);
        // payload = 锁点 + [DAG 段：u32 groupCount + 组记录 + clusterGroup]
        meta.payloadBytes = static_cast<std::uint32_t>(floats * sizeof(float));
        if (dag) {
            meta.payloadBytes += sizeof(std::uint32_t) +
                static_cast<std::uint32_t>(data.lodMeta.dagGroups.size() *
                                           sizeof(NMeshDagGroupRecord)) +
                static_cast<std::uint32_t>(data.lodMeta.clusterGroup.size() *
                                           sizeof(std::uint32_t));
        }
        const auto* mp = reinterpret_cast<const std::uint8_t*>(&meta);
        blob.insert(blob.end(), mp, mp + sizeof(meta));
        if (floats) {
            const auto* fp = reinterpret_cast<const std::uint8_t*>(
                data.lodMeta.lockedPositions.data());
            blob.insert(blob.end(), fp, fp + floats * sizeof(float));
        }
        if (dag) {
            if (data.lodMeta.dagGroups.size() > 0xFFFFFFFFu) {
                outError = "lod meta: dag group count exceeds uint32 range";
                return false;
            }
            const std::uint32_t groupCount =
                static_cast<std::uint32_t>(data.lodMeta.dagGroups.size());
            const auto* gp = reinterpret_cast<const std::uint8_t*>(&groupCount);
            blob.insert(blob.end(), gp, gp + sizeof(groupCount));
            for (const auto& g : data.lodMeta.dagGroups) {
                NMeshDagGroupRecord rec{};
                rec.error = g.error;
                rec.simplifyError = g.simplifyError;
                std::memcpy(rec.sphere, g.sphere, sizeof(rec.sphere));
                rec.genFirst = g.genFirst;
                rec.genCount = g.genCount;
                const auto* rp = reinterpret_cast<const std::uint8_t*>(&rec);
                blob.insert(blob.end(), rp, rp + sizeof(rec));
            }
            const auto* cg = reinterpret_cast<const std::uint8_t*>(
                data.lodMeta.clusterGroup.data());
            blob.insert(blob.end(), cg,
                        cg + data.lodMeta.clusterGroup.size() * sizeof(std::uint32_t));
        }
    }

    const std::string tmpPath = std::string(path) + ".tmp";
    {
        FileHandle h;
        h.fp = std::fopen(tmpPath.c_str(), "wb");
        if (!h.fp) {
            outError = "cannot open for write: " + tmpPath;
            return false;
        }
        if (std::fwrite(blob.data(), 1, blob.size(), h.fp) != blob.size()) {
            outError = "short write: " + tmpPath;
            std::remove(tmpPath.c_str());
            return false;
        }
    }
    std::remove(path);
    if (std::rename(tmpPath.c_str(), path) != 0) {
        outError = "rename failed: " + std::string(tmpPath) + " -> " + path;
        std::remove(tmpPath.c_str());
        return false;
    }
    return true;
}

bool loadNMesh(const char* path, NMeshData& out, std::string& outError) {
    out = NMeshData{};
    std::vector<std::uint8_t> blob;
    if (!readFileAll(path, blob, outError)) return false;
    if (blob.size() < sizeof(NMeshHeader)) {
        outError = "file smaller than header: " + std::string(path);
        return false;
    }

    NMeshHeader header;
    std::memcpy(&header, blob.data(), sizeof(header));
    if (header.magic != kNMeshMagic) {
        outError = "bad magic (not a .nmesh): " + std::string(path);
        return false;
    }
    if (header.version != kNMeshVersion && header.version != kNMeshVersionLod) {
        outError = "unsupported .nmesh version " + std::to_string(header.version) +
                   " (this build reads 1 or 2)";
        return false;
    }

    size_t cursor = sizeof(header);
    const auto need = [&](size_t bytes, const char* what) {
        if (cursor + bytes > blob.size()) {
            outError = std::string("truncated at ") + what + ": " + path;
            return false;
        }
        return true;
    };

    if (!need(header.nameLen, "name")) return false;
    out.name.assign(reinterpret_cast<const char*>(blob.data() + cursor), header.nameLen);
    cursor += header.nameLen;

    if (header.meshletCount == 0 && header.triangleCount != 0) {
        outError = "meshlets empty but triangles present"; return false;
    }
    out.meshlets.resize(header.meshletCount);
    out.bounds.resize(header.meshletCount);
    for (std::uint32_t i = 0; i < header.meshletCount; ++i) {
        if (!need(sizeof(NMeshletRecord), "meshlet record")) return false;
        NMeshletRecord rec;
        std::memcpy(&rec, blob.data() + cursor, sizeof(rec));
        cursor += sizeof(rec);
        Meshlet& m = out.meshlets[i];
        m.vertexOffset = rec.vertexOffset;
        m.vertexCount = rec.vertexCount;
        m.triangleOffset = rec.triangleOffset;
        m.triangleCount = rec.triangleCount;
        MeshletBounds& b = out.bounds[i];
        std::memcpy(b.sphere, rec.sphere, sizeof(b.sphere));
        std::memcpy(b.coneAxis, rec.coneAxis, sizeof(b.coneAxis));
        b.coneCutoff = rec.coneCutoff;
        b.error = rec.error;
    }

    out.vertices.resize(header.vertexCount);
    if (header.vertexCount > 0) {
        const size_t bytes = size_t(header.vertexCount) * sizeof(MeshVertex);
        if (!need(bytes, "vertices")) return false;
        std::memcpy(out.vertices.data(), blob.data() + cursor, bytes);
        cursor += bytes;
    }

    if (header.triangleCount > 0) {
        if (!need(size_t(header.triangleCount) * 3, "triangles")) return false;
        out.triangles.assign(blob.data() + cursor,
                             blob.data() + cursor + size_t(header.triangleCount) * 3);
        cursor += size_t(header.triangleCount) * 3;
    }
    if (header.version == kNMeshVersionLod) {
        if (!need(sizeof(NMeshMetaHeader), "lod meta header")) return false;
        NMeshMetaHeader meta;
        std::memcpy(&meta, blob.data() + cursor, sizeof(meta));
        cursor += sizeof(meta);
        const std::size_t lockedBytes = std::size_t(meta.lockedCount) * 3 * sizeof(float);
        if (meta.magic != kNMeshLodMagic ||
            (meta.version != kNMeshMetaVersion && meta.version != kNMeshMetaVersionDag) ||
            meta.payloadBytes < lockedBytes) {
            outError = "bad lod meta header: " + std::string(path);
            return false;
        }
        if (!need(meta.payloadBytes, "lod meta payload")) return false;
        out.lodMeta.present = true;
        out.lodMeta.level = meta.level;
        out.lodMeta.maxError = meta.maxError;
        out.lodMeta.lockedPositions.resize(std::size_t(meta.lockedCount) * 3);
        if (lockedBytes)
            std::memcpy(out.lodMeta.lockedPositions.data(), blob.data() + cursor, lockedBytes);
        cursor += lockedBytes;
        if (meta.version == kNMeshMetaVersionDag) {
            // M3.0：DAG 血缘段。组记录数先读，clusterGroup 由外层 meshletCount 定长。
            const size_t section = meta.payloadBytes - lockedBytes;
            const size_t minSection = sizeof(std::uint32_t);
            if (section < minSection) {
                outError = "dag section truncated (no group count): " + std::string(path);
                return false;
            }
            std::uint32_t groupCount;
            std::memcpy(&groupCount, blob.data() + cursor, sizeof(groupCount));
            cursor += sizeof(groupCount);
            const size_t expected = sizeof(std::uint32_t) +
                                    std::size_t(groupCount) * sizeof(NMeshDagGroupRecord) +
                                    std::size_t(header.meshletCount) * sizeof(std::uint32_t);
            if (section != expected) {
                outError = "dag section size mismatch (groups " +
                           std::to_string(groupCount) + "): " + std::string(path);
                return false;
            }
            out.lodMeta.dagPresent = true;
            out.lodMeta.dagGroups.resize(groupCount);
            for (std::uint32_t i = 0; i < groupCount; ++i) {
                NMeshDagGroupRecord rec;
                std::memcpy(&rec, blob.data() + cursor, sizeof(rec));
                cursor += sizeof(rec);
                auto& g = out.lodMeta.dagGroups[i];
                g.error = rec.error;
                g.simplifyError = rec.simplifyError;
                std::memcpy(g.sphere, rec.sphere, sizeof(g.sphere));
                g.genFirst = rec.genFirst;
                g.genCount = rec.genCount;
            }
            out.lodMeta.clusterGroup.resize(header.meshletCount);
            if (header.meshletCount) {
                std::memcpy(out.lodMeta.clusterGroup.data(), blob.data() + cursor,
                            std::size_t(header.meshletCount) * sizeof(std::uint32_t));
                cursor += std::size_t(header.meshletCount) * sizeof(std::uint32_t);
            }
            for (std::uint32_t v : out.lodMeta.clusterGroup)
                if (v >= groupCount) {
                    outError = "cluster group id out of range: " + std::string(path);
                    return false;
                }
        }
    }
    if (cursor != blob.size()) {
        outError = "trailing bytes after last section (" +
                   std::to_string(blob.size() - cursor) + "): " + path;
        return false;
    }

    // ---- 窗口与局部索引合法性校验（防手改文件越界读）----
    for (const Meshlet& m : out.meshlets) {
        if (m.vertexCount > kMeshletMaxVertices ||
            m.triangleCount > kMeshletMaxTriangles ||
            size_t(m.vertexOffset) + m.vertexCount > out.vertices.size() ||
            size_t(m.triangleOffset) + size_t(m.triangleCount) * 3 > out.triangles.size()) {
            outError = "meshlet window out of range (record " +
                       std::to_string(&m - out.meshlets.data()) + ")";
            return false;
        }
        for (std::uint32_t t = 0; t < m.triangleCount; ++t) {
            const size_t base = size_t(m.triangleOffset) + t * 3;
            for (int k = 0; k < 3; ++k) {
                if (out.triangles[base + k] >= m.vertexCount) {
                    outError = "triangle local index out of range (meshlet " +
                               std::to_string(&m - out.meshlets.data()) + ")";
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace nl::core
