#pragma once
// nl/net — M-NET2 同步层（服务器权威最小闭环）：控制消息 + 快照插值缓冲 + 会话。
//
// 口径（docs/net/NETCODE-PLAN.md M-NET2 + NETCODE-OPTIONS.md §6 Q-N2/Q-N5）：
//   - 服务器权威 + 快照插值：server 每逻辑 tick 广播各载具 VehicleSnapshot
//     （M-NET1 线格式 243 bits，本层不碰编解码——复用 Snapshot.hpp 的 encode/decode）；
//     client 不做本地模拟，只上送输入 + 收快照入 InterpBuffer 按 alpha 回看渲染。
//   - 最小闭环：只同步载具 transform + 玩家加入退出；控制面（Hello/Welcome/Input/
//     EntityGone/Leave）走 ENet 可靠通道（channel 0），快照流走不可靠通道（channel 1）。
//   - editor RPC（msgpack_min）与本层零交集（Q-N5 两通道独立拍板）。
//
// 边界纪律：本头零第三方类型（依赖 Net.hpp / Snapshot.hpp 的 PIMPL 面）；
// 单线程使用（调用方每渲染帧 pump，逻辑 tick 推进由 GameClock 组回调驱动）。
// 控制消息 = pack(1) POD + 魔数/协议号/类型三重校验（与 M-NET1「严格解码」同纪律：
// 短流 / 坏魔数 / 坏协议号 / 坏类型一律拒绝，不产出半截状态）。

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nl/net/Net.hpp"
#include "nl/net/Snapshot.hpp"

namespace nl::net {

// ---- 控制消息（可靠通道；pack(1) 定长 POD，x86 小端，bytes 级线契约）----
constexpr uint32_t kSessionMagic = 0x4E4C534Eu; // "NLSN"（nl session）
constexpr uint32_t kSessionProto = 1;           // 线格式版本；改任何字段必须 bump
constexpr size_t   kMaxName      = 16;          // ASCII，截断收尾（无 \0 要求）
constexpr uint16_t kMaxEntities  = 16;          // 最小闭环实体上限（server 本地 1 + client ≤15）

enum class CtrlType : uint8_t {
    Hello     = 1, // C->S：加入请求（name 可选展示串）
    Welcome   = 2, // S->C：接受加入（当前 serverTick + 分配的 entityId）
    Input     = 3, // C->S：驾驶输入（最新覆盖，服务器每 tick 采样）
    EntityGone= 4, // S->C：实体回收（对端断链）
    Leave     = 5, // C->S：主动退出
};

#pragma pack(push, 1)
// 统一线格式头：magic(4) + proto(4) + type(1) 恒在 0..8（ctrlHeadOk 单点校验）。
struct CtrlHello {
    uint32_t magic = kSessionMagic;
    uint32_t proto = kSessionProto;
    uint8_t  type  = static_cast<uint8_t>(CtrlType::Hello);
    char     name[kMaxName] = {};
};
struct CtrlWelcome {
    uint32_t magic = kSessionMagic;
    uint32_t proto = kSessionProto;
    uint8_t  type  = static_cast<uint8_t>(CtrlType::Welcome);
    uint8_t  pad   = 0;
    uint32_t serverTick = 0;
    uint16_t entityId   = 0;
};
struct CtrlInput {
    uint32_t magic = kSessionMagic;
    uint32_t proto = kSessionProto;
    uint8_t  type  = static_cast<uint8_t>(CtrlType::Input);
    uint8_t  pad[3]= {};
    float    forward = 0.f, right = 0.f, brake = 0.f, handBrake = 0.f;
};
struct CtrlEntityGone {
    uint32_t magic = kSessionMagic;
    uint32_t proto = kSessionProto;
    uint8_t  type  = static_cast<uint8_t>(CtrlType::EntityGone);
    uint8_t  pad[3]= {};
    uint16_t entityId = 0;
};
struct CtrlLeave {
    uint32_t magic = kSessionMagic;
    uint32_t proto = kSessionProto;
    uint8_t  type  = static_cast<uint8_t>(CtrlType::Leave);
};
#pragma pack(pop)

// ---- 控制消息解码（严格面：magic/proto/type/长度全验，失败返回 false）----
bool decodeCtrlHello(const uint8_t* d, size_t n, CtrlHello& out);
bool decodeCtrlWelcome(const uint8_t* d, size_t n, CtrlWelcome& out);
bool decodeCtrlInput(const uint8_t* d, size_t n, CtrlInput& out);
bool decodeCtrlEntityGone(const uint8_t* d, size_t n, CtrlEntityGone& out);
bool decodeCtrlLeave(const uint8_t* d, size_t n, CtrlLeave& out);

// ---- 插值缓冲（每实体一份）：快照按 tick 单调入队，eval 回看 delayTicks ----
// eval(target = latestTick - delayTicks)：落在相邻两快照之间做 lerp/nlerp；
// target 未被缓冲覆盖（< 最老 tick）返回 false = 缓冲未预热，调用方不渲染不记证。
// 量化噪声 ≤ 线格式声明精度（1mm / 0.1°）；验收锚「偏差 ≤ 一个快照间隔」由
// net-verify-m2 用 auth 轨迹相邻位移自标定。
class InterpBuffer {
public:
    static constexpr size_t kCapacity = 32;

    // tick 单调入队：tick <= 已有最新 = 乱序/重复，直接丢弃（不可靠通道丢旧保新）。
    void push(const VehicleSnapshot& s);
    // 回看采样。target = latestTick - delayTicks + extraTicks（extra 由调用方给
    // 帧间相位：viewer 用最新快照到达后经过的时间折算 tick 分数，实现真正的
    // alpha 帧间插值；确定性单测传 0 = 恒整点采样）。out = 插值后快照，
    // evalTickOut = 本采样对应的 server tick 坐标（float）。
    bool eval(float delayTicks, VehicleSnapshot& out, float& evalTickOut,
              float extraTicks = 0.0f) const;
    uint32_t latestTick() const { return count_ ? ring_[tail_].serverTick : 0; }
    uint32_t oldestTick() const { return count_ ? ring_[head_].serverTick : 0; }
    size_t   size() const { return count_; }
    bool     empty() const { return count_ == 0; }
    void     clear() { head_ = tail_ = count_ = 0; }

private:
    VehicleSnapshot ring_[kCapacity] = {};
    size_t head_ = 0, tail_ = 0, count_ = 0; // tail_ 指向最新元素
};

// ---- 驾驶输入（net 层 POD；与 nl::physics::DriveInputState 同形但独立声明，
// nl/net 不依赖 physics 头——viewer / tools 各自做 POD 字段拷贝）----
struct InputState {
    float forward = 0.f, right = 0.f, brake = 0.f, handBrake = 0.f;
};

// ---- 服务器会话：peer↔entity 分配回收 + 输入缓存 + 快照广播 ----
// 实体号从 1 起；本地权威载具由应用先 claimLocalEntity() 占 1 号。
// 应用职责（本层零游戏语义）：joined 时 addVehicle、left 时 removeVehicle、
// 每逻辑 tick 读 inputOf + 采样各实体调 broadcastSnapshot。
class ServerSession {
public:
    ServerSession() = default;
    ~ServerSession();
    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;

    bool start(uint16_t port, uint32_t maxClients);
    void stop();
    bool running() const;

    // 本地权威实体占号（首次调用返回 1，之后递增；不对应任何 peer）。
    uint16_t claimLocalEntity();

    // 泵控制面。joined/left 输出本帧发生的实体增删（entityId），
    // serverTick 只进日志诊断（Welcome 携带的当前 tick）。
    void pump(uint32_t serverTick, std::vector<uint16_t>& joinedOut,
              std::vector<uint16_t>& leftOut);

    bool        hasInput(uint16_t entityId) const;
    InputState  inputOf(uint16_t entityId) const;   // 无缓存 = 全零
    uint16_t    entityOfPeer(uint32_t peerId) const; // 未绑 Hello = 0

    // 单快照 = 单 ENet 包（不可靠通道）；编码失败（不会发生：定长 31B）静默丢弃。
    void broadcastSnapshot(const VehicleSnapshot& s);

    // 实体回收广播（可靠通道；其余 client 收到后删各自幽灵位）。
    void notifyEntityGone(uint16_t entityId);

    uint32_t peerCount() const;

private:
    struct PeerSlot {
        uint32_t peerId = 0;      // 0 = 空
        uint16_t entityId = 0;
    };
    struct EntityInput {
        uint16_t entityId = 0;
        InputState input = {};
    };
    NetServer server_;
    PeerSlot peers_[kMaxEntities] = {};
    EntityInput inputs_[kMaxEntities] = {};
    uint16_t nextEntityId_ = 1;
    bool started_ = false;
};

// ---- 客户端会话：hello/welcome + 输入上送 + 快照分桶 ----
class ClientSession {
public:
    ClientSession() = default;
    ~ClientSession();
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // 非阻塞发起（对端 server 必须在泵事件——双进程形态，Net.hpp 注释同口径）。
    bool beginConnect(const char* host, uint16_t port);
    void disconnect();
    bool connected() const;   // ENet 链路已建（未必已 Welcome）
    bool welcomed() const;    // 已拿到 Welcome（entityId 可用）

    // 泵：握手 / 控制消息 / 快照分桶全在此推进。
    void pump();

    // 每渲染帧上送驾驶输入（可靠通道；服务器每逻辑 tick 取最新覆盖值）。
    void sendInput(const InputState& in);

    uint16_t localEntityId() const { return localEntityId_; }
    uint32_t welcomeTick() const { return welcomeTick_; }
    uint32_t rttMs() const { return client_.rttMs(); }

    // 快照分桶（已知实体 ≤ kMaxEntities；未知实体收到首包即建桶）。
    size_t entityCount() const;
    uint16_t entityAt(size_t i) const;              // 越界返回 0
    const InterpBuffer* bufferOf(uint16_t entityId) const; // 未知实体 = nullptr
    bool hasEntity(uint16_t entityId) const;
    void forgetEntity(uint16_t entityId);           // EntityGone 后由 pump 自动调

private:
    struct EntityBuf {
        uint16_t entityId = 0;
        bool used = false;
        InterpBuffer buf;
    };
    NetClient client_;
    EntityBuf bufs_[kMaxEntities] = {};
    uint16_t localEntityId_ = 0;
    uint32_t welcomeTick_ = 0;
    bool welcomed_ = false;
};

} // namespace nl::net
