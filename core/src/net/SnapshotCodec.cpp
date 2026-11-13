// nl/net — M-NET1 快照层实现：bitsery 位打包编解码（单 TU，bitsery 头不外泄）。
//
// 布局契约见 Snapshot.hpp 头注释；字段顺序与位宽两端硬编码一致，任何一端
// 单改 = 线协议漂移（M-NET1 收口后改格式须重拍）。
//
// 量化实现口径（对照 UE NetSerialization.h 的 FVector_NetQuantize 分档思想，
// 代码自写不抄 UE）：固定点 = lround 后饱和钳制；四元数 smallest-three =
// 记最大分量索引(2b) + 该分量符号(1b)，其余三分量各 16b 线性映射到
// [-1/√2, 1/√2]；解码重建最大分量 = sign*sqrt(1-Σ其余²)。

#include "nl/net/Snapshot.hpp"

#include <bitsery/bitsery.h>
#include <bitsery/adapter/buffer.h>
#include <bitsery/traits/vector.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nl::net {
namespace {

// ---- 固定点量化（饱和钳制；写进线上的位型 = 二补码整型位型）----
uint32_t quantPos(float meters) {
    const double mm = std::llround(static_cast<double>(meters) * 1000.0);
    double clamped = mm;
    if (clamped > 2147483647.0) clamped = 2147483647.0;
    if (clamped < -2147483648.0) clamped = -2147483648.0;
    return static_cast<uint32_t>(static_cast<int32_t>(clamped));
}
float dequantPos(uint32_t bits) {
    return static_cast<float>(static_cast<int32_t>(bits)) * 0.001f;
}
uint16_t quantVel(float ms) {
    const double cm = std::llround(static_cast<double>(ms) * 100.0);
    double clamped = cm;
    if (clamped > 32767.0) clamped = 32767.0;
    if (clamped < -32768.0) clamped = -32768.0;
    return static_cast<uint16_t>(static_cast<int16_t>(clamped));
}
float dequantVel(uint16_t bits) {
    return static_cast<float>(static_cast<int16_t>(bits)) * 0.01f;
}

// ---- 四元数 smallest-three ----
struct QuatPacked {
    uint32_t largestIdx : 2;
    uint32_t largestNeg : 1;
    uint16_t v[3];
};
QuatPacked packQuat(const float q[4]) {
    int li = 0;
    float la = std::fabs(q[0]);
    for (int i = 1; i < 4; ++i) {
        const float a = std::fabs(q[i]);
        if (a > la) { la = a; li = i; }
    }
    QuatPacked p{};
    p.largestIdx = static_cast<uint32_t>(li);
    p.largestNeg = q[li] < 0.f ? 1u : 0u;
    int k = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == li) continue;
        const double v = std::clamp(static_cast<double>(q[i]),
                                    static_cast<double>(-kQuatScale),
                                    static_cast<double>(kQuatScale));
        const double u = (v + kQuatScale) / (2.0 * kQuatScale) * kQuatSteps;
        p.v[k++] = static_cast<uint16_t>(std::lround(u));
    }
    return p;
}
void unpackQuat(const QuatPacked& p, float q[4]) {
    const double s = kQuatScale;
    int k = 0;
    double sumSq = 0.0;
    for (int i = 0; i < 4; ++i) {
        if (static_cast<int>(p.largestIdx) == i) continue;
        const double v = static_cast<double>(p.v[k]) / kQuatSteps * 2.0 * s - s;
        q[i] = static_cast<float>(v);
        sumSq += v * v;
        ++k;
    }
    double largest = 1.0 - std::min(sumSq, 1.0);
    largest = std::sqrt(std::max(largest, 0.0));
    q[p.largestIdx] = p.largestNeg ? static_cast<float>(-largest)
                                   : static_cast<float>(largest);
}

} // namespace

size_t encodeSnapshot(const VehicleSnapshot& s, uint8_t* dst, size_t cap) {
    if (cap < kSnapshotMaxBytes) return 0;

    // bitsery 位打包输出：vector 由 adapter 增长，结束后取 writtenBytesCount。
    std::vector<uint8_t> buf;
    buf.reserve(kSnapshotMaxBytes);
    using TAdapter = bitsery::OutputBufferAdapter<std::vector<uint8_t>>;
    bitsery::Serializer<TAdapter> ser(buf);
    ser.enableBitPacking([&ser, &s](auto& bp) {
        bp.adapter().writeBits(s.serverTick, 32);
        bp.adapter().writeBits(static_cast<uint16_t>(s.entityId), 16);
        const float pos[3] = { s.px, s.py, s.pz };
        for (const float p : pos)
            bp.adapter().writeBits(quantPos(p), 32);
        const float q[4] = { s.qx, s.qy, s.qz, s.qw };
        const QuatPacked qp = packQuat(q);
        const uint32_t meta = qp.largestIdx | (qp.largestNeg << 2);
        bp.adapter().writeBits(meta, 3);
        for (const uint16_t v : qp.v)
            bp.adapter().writeBits(v, 16);
        const float vel[3] = { s.vx, s.vy, s.vz };
        for (const float v : vel)
            bp.adapter().writeBits(quantVel(v), 16);
    });
    ser.adapter().flush();

    // 字节数取 writtenBytesCount()：OutputBufferAdapter 对可增长 vector 会
    // 把 size 撑到增长策略的取整值（> 实际写入），buf.size() 不可信。
    const size_t written = ser.adapter().writtenBytesCount();
    if (written > kSnapshotMaxBytes) return 0;
    std::memcpy(dst, buf.data(), written);
    return written;
}

void encodeSnapshot(const VehicleSnapshot& s, std::vector<uint8_t>& out) {
    out.resize(kSnapshotMaxBytes);
    const size_t n = encodeSnapshot(s, out.data(), out.size());
    out.resize(n);
}

bool decodeSnapshot(const uint8_t* src, size_t len, VehicleSnapshot& out) {
    if (len == 0 || len > kSnapshotMaxBytes) return false;

    using TAdapter = bitsery::InputBufferAdapter<const uint8_t*>;
    VehicleSnapshot r{};
    QuatPacked qp{};
    uint32_t meta = 0;
    uint32_t posBits[3] = {};
    uint16_t velBits[3] = {};

    bitsery::Deserializer<TAdapter> des(src, src + len);
    des.enableBitPacking([&des, &r, &qp, &meta, &posBits, &velBits](auto& bp) {
        bp.adapter().readBits(r.serverTick, 32);
        uint16_t id = 0;
        bp.adapter().readBits(id, 16);
        r.entityId = id;
        for (uint32_t& p : posBits)
            bp.adapter().readBits(p, 32);
        bp.adapter().readBits(meta, 3);
        for (uint16_t& v : qp.v)
            bp.adapter().readBits(v, 16);
        for (uint16_t& v : velBits)
            bp.adapter().readBits(v, 16);
    });
    // 尾部对齐残位必须为 0（防残位藏脏数据静默过关）。
    des.adapter().align();
    if (!des.adapter().isCompletedSuccessfully()) return false;
    if (des.adapter().currentReadPos() != len) return false;

    qp.largestIdx = meta & 3u;
    qp.largestNeg = (meta >> 2) & 1u;

    r.px = dequantPos(posBits[0]);
    r.py = dequantPos(posBits[1]);
    r.pz = dequantPos(posBits[2]);
    float q[4];
    unpackQuat(qp, q);
    r.qx = q[0]; r.qy = q[1]; r.qz = q[2]; r.qw = q[3];
    r.vx = dequantVel(velBits[0]);
    r.vy = dequantVel(velBits[1]);
    r.vz = dequantVel(velBits[2]);
    out = r;
    return true;
}

bool decodeSnapshot(const std::vector<uint8_t>& buf, VehicleSnapshot& out) {
    return decodeSnapshot(buf.data(), buf.size(), out);
}

const char* snapshotCodecVersion() {
    return BITSERY_VERSION;
}

} // namespace nl::net
