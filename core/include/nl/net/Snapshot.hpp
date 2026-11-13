#pragma once
// nl/net — M-NET1 快照层（序列化）：载具 transform 快照 POD + 位打包编解码 API。
//
// 线协议口径（docs/net/NETCODE-PLAN.md M-NET1 任务 2，量化字段宽度任务书内定死）：
//   ┌────────────┬──────────────────────────────┬─────────┬──────────────────────────────┐
//   │ 字段        │ 量化                          │ 位宽     │ 声明精度 / 范围               │
//   ├────────────┼──────────────────────────────┼─────────┼──────────────────────────────┤
//   │ serverTick │ uint32 原值                   │ 32      │ 1 tick                       │
//   │ entityId   │ uint16 原值                   │ 16      │ 1（最小闭环 ≤65535 实体）     │
//   │ position   │ int32 毫米（逐轴，饱和钳制）    │ 3×32=96 │ 1 mm；范围 ±2147483.647 m     │
//   │ orient     │ 四元数 smallest-three 压缩     │ 2+1+3×16=51 │ 分量误差 ≤1.08e-5         │
//   │            │                               │         │ 角误差 ≤0.1 度（线格式量化    │
//   │            │                               │         │ 贡献 ~0.002 度，其余为输入    │
//   │            │                               │         │ float 归一化噪声的放大）      │
//   │ velocity   │ int16 厘米/秒（逐轴，饱和钳制） │ 3×16=48 │ 1 cm/s；范围 ±327.67 m/s     │
//   └────────────┴──────────────────────────────┴─────────┴──────────────────────────────┘
//   合计 243 bits → 对齐字节后 31 bytes（naive float 全量 46 bytes）。
//   位序 = bitsery OutputBufferAdapter 位打包（LSB-first，双端一致，逐字节稳定）。
//
// 边界：bitsery 头只在 core/src/net/SnapshotCodec.cpp 单 TU 内包含（PIMPL 纪律同
// enet_ 符号隔离）；本头零第三方类型，只有 POD + std::vector 进出的自由函数。

#include <cstdint>
#include <cstddef>
#include <vector>

namespace nl::net {

// 载具 transform 快照（M-NET2 服务器权威模拟的线上形态）。
// 浮点单位与引擎口径一致：位置/速度米（m, m/s），朝向为单位四元数（x,y,z,w）。
struct VehicleSnapshot {
    uint32_t serverTick = 0;
    uint16_t entityId   = 0;
    float    px = 0.f, py = 0.f, pz = 0.f;   // 位置（米）
    float    qx = 0.f, qy = 0.f, qz = 0.f, qw = 1.f; // 朝向单位四元数
    float    vx = 0.f, vy = 0.f, vz = 0.f;   // 线速度（米/秒）
};

// ---- 线格式常量（编解码两端共同契约，改动须重拍 M-NET1 起全部基线）----
constexpr uint32_t kSnapshotBits      = 243; // 32+16+96+51+48
constexpr size_t   kSnapshotMaxBytes  = 32;  // 243 bits 对齐后 31 B，留 1 B 余量
constexpr float    kQuatScale         = 0.70710678118654752440f; // 1/sqrt(2)
constexpr uint16_t kQuatSteps         = 65535;

// ---- 编码：返回写入字节数（恒 31）；cap 不足返回 0（不写半截流）----
size_t encodeSnapshot(const VehicleSnapshot& s, uint8_t* dst, size_t cap);

// 便捷重载：追加进 vector（clear 后写入）。
void encodeSnapshot(const VehicleSnapshot& s, std::vector<uint8_t>& out);

// ---- 解码：流短 / 尾部残位非零 / 读越界一律失败返回 false（不产出半截快照）----
bool decodeSnapshot(const uint8_t* src, size_t len, VehicleSnapshot& out);

// 便捷重载。
bool decodeSnapshot(const std::vector<uint8_t>& buf, VehicleSnapshot& out);

// bitsery 内核版本串（实现侧取 BITSERY_VERSION 宏，与 VENDOR.md 锁定版本对账）。
const char* snapshotCodecVersion();

} // namespace nl::net
