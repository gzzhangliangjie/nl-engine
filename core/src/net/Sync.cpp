// NET M-NET2：同步层实现（控制消息严格解码 + 插值缓冲 + 会话状态机）。
// 依赖面：nl/net/Net.hpp（传输 PIMPL）+ nl/net/Snapshot.hpp（快照编解码），
// 本 TU 无任何第三方类型（enet_/bitsery 符号仍只在各自单 TU）。
// 单线程纪律：ServerSession / ClientSession 与其底层 host 在同一线程泵。

#include "nl/net/Sync.hpp"

#include <cmath>
#include <cstring>

namespace nl::net {

namespace {

// 线格式尺寸锁（pack(1) POD；改任何字段必须 bump kSessionProto 并重拍 M-NET2 起基线）。
static_assert(sizeof(CtrlHello) == 25,      "CtrlHello wire size");
static_assert(sizeof(CtrlWelcome) == 16,    "CtrlWelcome wire size");
static_assert(sizeof(CtrlInput) == 28,      "CtrlInput wire size");
static_assert(sizeof(CtrlEntityGone) == 14, "CtrlEntityGone wire size");
static_assert(sizeof(CtrlLeave) == 9,       "CtrlLeave wire size");


// 控制消息公共头校验：至少 magic+proto+type 三字节段齐 + 魔数/协议号匹配。
// 返回 type 供分发；长度逐消息再验（struct 尺寸 static_assert 锁定）。
bool ctrlHeadOk(const uint8_t* d, size_t n, CtrlType want) {
    if (n < 9) return false;
    uint32_t magic = 0, proto = 0;
    std::memcpy(&magic, d, 4);
    std::memcpy(&proto, d + 4, 4);
    if (magic != kSessionMagic || proto != kSessionProto) return false;
    if (d[8] != static_cast<uint8_t>(want)) return false;
    return true;
}

} // namespace

// ---------------------------------------------------------------- 控制解码

bool decodeCtrlHello(const uint8_t* d, size_t n, CtrlHello& out) {
    if (!ctrlHeadOk(d, n, CtrlType::Hello) || n != sizeof(CtrlHello)) return false;
    std::memcpy(&out, d, sizeof(CtrlHello));
    return true;
}

bool decodeCtrlWelcome(const uint8_t* d, size_t n, CtrlWelcome& out) {
    if (!ctrlHeadOk(d, n, CtrlType::Welcome) || n != sizeof(CtrlWelcome)) return false;
    std::memcpy(&out, d, sizeof(CtrlWelcome));
    return true;
}

bool decodeCtrlInput(const uint8_t* d, size_t n, CtrlInput& out) {
    if (!ctrlHeadOk(d, n, CtrlType::Input) || n != sizeof(CtrlInput)) return false;
    std::memcpy(&out, d, sizeof(CtrlInput));
    // 非有限输入拒绝（NaN 污染物理模拟不可接受；pack POD 无编解码层兜底）。
    if (!std::isfinite(out.forward) || !std::isfinite(out.right) ||
        !std::isfinite(out.brake) || !std::isfinite(out.handBrake)) return false;
    return true;
}

bool decodeCtrlEntityGone(const uint8_t* d, size_t n, CtrlEntityGone& out) {
    if (!ctrlHeadOk(d, n, CtrlType::EntityGone) || n != sizeof(CtrlEntityGone)) return false;
    std::memcpy(&out, d, sizeof(CtrlEntityGone));
    return true;
}

bool decodeCtrlLeave(const uint8_t* d, size_t n, CtrlLeave& out) {
    if (!ctrlHeadOk(d, n, CtrlType::Leave) || n != sizeof(CtrlLeave)) return false;
    std::memcpy(&out, d, sizeof(CtrlLeave));
    return true;
}

// ---------------------------------------------------------------- InterpBuffer

void InterpBuffer::push(const VehicleSnapshot& s) {
    if (count_ > 0 && s.serverTick <= ring_[tail_].serverTick) return; // 旧/重复丢弃
    if (count_ < kCapacity) {
        tail_ = (count_ == 0) ? 0 : (tail_ + 1) % kCapacity;
        ring_[tail_] = s;
        ++count_;
        if (count_ == 1) head_ = tail_;
        return;
    }
    // 满环：丢最老，新值追加在最新位置（head 前移一格复用）。
    head_ = (head_ + 1) % kCapacity;
    tail_ = (tail_ + 1) % kCapacity;
    ring_[tail_] = s;
}

bool InterpBuffer::eval(float delayTicks, VehicleSnapshot& out, float& evalTickOut,
                        float extraTicks) const {
    if (count_ == 0) return false;
    const float target = static_cast<float>(ring_[tail_].serverTick) - delayTicks + extraTicks;
    const float oldest = static_cast<float>(ring_[head_].serverTick);
    if (target < oldest) return false; // 缓冲未覆盖目标 tick（未预热），调用方不消费
    // 找相邻对：ring_ 按 tick 升序（head -> tail）。
    size_t i = head_;
    for (;;) {
        const size_t next = (i + 1) % kCapacity;
        if (i == tail_ || static_cast<float>(ring_[next].serverTick) >= target) {
            if (i == tail_) { // target == latestTick：直接取最新
                out = ring_[tail_];
                evalTickOut = target;
                return true;
            }
            const VehicleSnapshot& a = ring_[i];
            const VehicleSnapshot& b = ring_[next];
            const float span = static_cast<float>(b.serverTick) -
                               static_cast<float>(a.serverTick);
            const float t = span > 0.f ? (target - static_cast<float>(a.serverTick)) / span
                                       : 0.f;
            out = b; // entityId 取段右端（同一实体恒等）
            out.px = a.px + (b.px - a.px) * t;
            out.py = a.py + (b.py - a.py) * t;
            out.pz = a.pz + (b.pz - a.pz) * t;
            out.vx = a.vx + (b.vx - a.vx) * t;
            out.vy = a.vy + (b.vy - a.vy) * t;
            out.vz = a.vz + (b.vz - a.vz) * t;
            // 四元数 nlerp（短弧：dot<0 时整段取反；退化时取端点 a）。
            float d = a.qx * b.qx + a.qy * b.qy + a.qz * b.qz + a.qw * b.qw;
            float bx = b.qx, by = b.qy, bz = b.qz, bw = b.qw;
            if (d < 0.f) { bx = -bx; by = -by; bz = -bz; bw = -bw; d = -d; }
            const float qx = a.qx + (bx - a.qx) * t;
            const float qy = a.qy + (by - a.qy) * t;
            const float qz = a.qz + (bz - a.qz) * t;
            const float qw = a.qw + (bw - a.qw) * t;
            const float n2 = qx * qx + qy * qy + qz * qz + qw * qw;
            if (n2 > 1e-12f) {
                const float inv = 1.f / std::sqrt(n2);
                out.qx = qx * inv; out.qy = qy * inv;
                out.qz = qz * inv; out.qw = qw * inv;
            } else {
                out.qx = a.qx; out.qy = a.qy; out.qz = a.qz; out.qw = a.qw;
            }
            out.serverTick = b.serverTick;
            evalTickOut = target;
            return true;
        }
        i = next;
    }
}

// ---------------------------------------------------------------- ServerSession

ServerSession::~ServerSession() {
    stop();
}

bool ServerSession::start(uint16_t port, uint32_t maxClients) {
    started_ = server_.start(port, maxClients);
    return started_;
}

void ServerSession::stop() {
    if (started_) server_.stop();
    started_ = false;
    for (PeerSlot& p : peers_) p = PeerSlot{};
    for (EntityInput& e : inputs_) e = EntityInput{};
}

uint16_t ServerSession::claimLocalEntity() {
    const uint16_t id = nextEntityId_;
    if (nextEntityId_ < 0xFFFFu) ++nextEntityId_;
    return id;
}

void ServerSession::pump(uint32_t serverTick, std::vector<uint16_t>& joinedOut,
                         std::vector<uint16_t>& leftOut) {
    if (!started_) return;
    std::vector<NetEvent> evs;
    server_.service(0, evs);
    for (const NetEvent& e : evs) {
        if (e.type == EventType::PeerDisconnected) {
            const uint16_t ent = entityOfPeer(e.peerId);
            if (ent != 0) {
                for (PeerSlot& p : peers_) {
                    if (p.peerId == e.peerId) p = PeerSlot{};
                }
                for (EntityInput& i : inputs_) {
                    if (i.entityId == ent) i = EntityInput{};
                }
                leftOut.push_back(ent);
            }
            continue;
        }
        if (e.type != EventType::Data) continue;
        if (e.channel == Channel::Reliable) {
            // 控制面：Hello（建实体）/ Input（缓存最新）/ Leave（回收）。
            if (e.data.size() >= 9) {
                const uint8_t t = e.data[8];
                if (t == static_cast<uint8_t>(CtrlType::Hello)) {
                    CtrlHello h;
                    if (decodeCtrlHello(e.data.data(), e.data.size(), h) &&
                        entityOfPeer(e.peerId) == 0) {
                        // 分配实体（占用一个输入槽；槽满 = 拒绝加入，不回 Welcome）。
                        uint16_t slot = kMaxEntities;
                        for (uint16_t k = 0; k < kMaxEntities; ++k) {
                            if (peers_[k].peerId == 0) { slot = k; break; }
                        }
                        if (slot != kMaxEntities) {
                            const uint16_t ent = nextEntityId_;
                            if (nextEntityId_ < 0xFFFFu) ++nextEntityId_;
                            peers_[slot].peerId = e.peerId;
                            peers_[slot].entityId = ent;
                            inputs_[slot].entityId = ent;   // 输入槽与实体同号建立
                            inputs_[slot].input = InputState{};
                            joinedOut.push_back(ent);
                            CtrlWelcome w;
                            w.serverTick = serverTick;
                            w.entityId = ent;
                            const uint8_t* raw = reinterpret_cast<const uint8_t*>(&w);
                            server_.send(e.peerId, Channel::Reliable, raw, sizeof(w));
                        }
                    }
                } else if (t == static_cast<uint8_t>(CtrlType::Input)) {
                    CtrlInput in;
                    if (decodeCtrlInput(e.data.data(), e.data.size(), in)) {
                        const uint16_t ent = entityOfPeer(e.peerId);
                        if (ent != 0) {
                            for (EntityInput& i : inputs_) {
                                if (i.entityId == ent) {
                                    i.input.forward = in.forward;
                                    i.input.right = in.right;
                                    i.input.brake = in.brake;
                                    i.input.handBrake = in.handBrake;
                                    break;
                                }
                            }
                        }
                    }
                } else if (t == static_cast<uint8_t>(CtrlType::Leave)) {
                    CtrlLeave lv;
                    if (decodeCtrlLeave(e.data.data(), e.data.size(), lv)) {
                        const uint16_t ent = entityOfPeer(e.peerId);
                        if (ent != 0) {
                            for (PeerSlot& p : peers_) {
                                if (p.peerId == e.peerId) p = PeerSlot{};
                            }
                            for (EntityInput& i : inputs_) {
                                if (i.entityId == ent) i = EntityInput{};
                            }
                            leftOut.push_back(ent);
                        }
                    }
                }
                // 未知类型：严格纪律 = 丢弃不崩（版本协商由 proto 号把门）。
            }
        }
        // 不可靠通道数据包到 server = 协议违规（快照只从 server 发），丢弃。
    }
}

bool ServerSession::hasInput(uint16_t entityId) const {
    for (const EntityInput& i : inputs_) {
        if (i.entityId == entityId) return true;
    }
    return false;
}

InputState ServerSession::inputOf(uint16_t entityId) const {
    for (const EntityInput& i : inputs_) {
        if (i.entityId == entityId) return i.input;
    }
    return InputState{};
}

uint16_t ServerSession::entityOfPeer(uint32_t peerId) const {
    for (const PeerSlot& p : peers_) {
        if (p.peerId == peerId) return p.entityId;
    }
    return 0;
}

void ServerSession::broadcastSnapshot(const VehicleSnapshot& s) {
    std::vector<uint8_t> bytes;
    encodeSnapshot(s, bytes);
    if (bytes.empty()) return;
    server_.broadcast(Channel::Unreliable, bytes.data(), bytes.size());
}

void ServerSession::notifyEntityGone(uint16_t entityId) {
    CtrlEntityGone g;
    g.entityId = entityId;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&g);
    server_.broadcast(Channel::Reliable, raw, sizeof(g));
}

uint32_t ServerSession::peerCount() const {
    uint32_t n = 0;
    for (const PeerSlot& p : peers_) {
        if (p.peerId != 0) ++n;
    }
    return n;
}

// ---------------------------------------------------------------- ClientSession

ClientSession::~ClientSession() {
    disconnect();
}

bool ClientSession::beginConnect(const char* host, uint16_t port) {
    return client_.beginConnect(host, port);
}

void ClientSession::disconnect() {
    client_.disconnect();
    // 直接拆除（不经 pump 的 PeerDisconnected 路径），会话态同步复位——
    // 断线后 localEntityId / 缓冲不许残留（M-NET3 异常路径纪律的前置）。
    welcomed_ = false;
    localEntityId_ = 0;
    welcomeTick_ = 0;
    for (EntityBuf& b : bufs_) b = EntityBuf{};
}

bool ClientSession::connected() const {
    return client_.connected();
}

bool ClientSession::welcomed() const {
    return welcomed_;
}

void ClientSession::pump() {
    std::vector<NetEvent> evs;
    client_.service(0, evs);
    for (const NetEvent& e : evs) {
        if (e.type == EventType::PeerConnected) {
            // ENet 链路建立即发 Hello（应用侧 name 留待 M-NET3；此处占位空串）。
            CtrlHello h;
            const uint8_t* raw = reinterpret_cast<const uint8_t*>(&h);
            client_.send(Channel::Reliable, raw, sizeof(h));
            continue;
        }
        if (e.type == EventType::PeerDisconnected) {
            welcomed_ = false;
            localEntityId_ = 0;
            for (EntityBuf& b : bufs_) b = EntityBuf{};
            continue;
        }
        if (e.type != EventType::Data) continue;
        if (e.channel == Channel::Reliable) {
            if (e.data.size() >= 9) {
                const uint8_t t = e.data[8];
                if (t == static_cast<uint8_t>(CtrlType::Welcome)) {
                    CtrlWelcome w;
                    if (decodeCtrlWelcome(e.data.data(), e.data.size(), w)) {
                        welcomed_ = true;
                        localEntityId_ = w.entityId;
                        welcomeTick_ = w.serverTick;
                    }
                } else if (t == static_cast<uint8_t>(CtrlType::EntityGone)) {
                    CtrlEntityGone g;
                    if (decodeCtrlEntityGone(e.data.data(), e.data.size(), g)) {
                        forgetEntity(g.entityId);
                    }
                }
            }
            continue;
        }
        // 不可靠通道 = 快照流。
        VehicleSnapshot s;
        if (decodeSnapshot(e.data, s)) {
            auto deliver = [&s](EntityBuf& b) {
                b.used = true;
                b.entityId = s.entityId;
                b.buf.clear();
                b.buf.push(s);
            };
            bool done = false;
            for (EntityBuf& b : bufs_) {
                if (b.used && b.entityId == s.entityId) { b.buf.push(s); done = true; break; }
            }
            if (!done) {
                for (EntityBuf& b : bufs_) {
                    if (!b.used) { deliver(b); done = true; break; }
                }
            }
            // 桶满（> kMaxEntities）：丢新（最小闭环实体上限纪律）。
        }
    }
}

void ClientSession::sendInput(const InputState& in) {
    if (!welcomed_) return;
    CtrlInput msg;
    msg.forward = in.forward;
    msg.right = in.right;
    msg.brake = in.brake;
    msg.handBrake = in.handBrake;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&msg);
    client_.send(Channel::Reliable, raw, sizeof(msg));
}

size_t ClientSession::entityCount() const {
    size_t n = 0;
    for (const EntityBuf& b : bufs_) {
        if (b.used) ++n;
    }
    return n;
}

uint16_t ClientSession::entityAt(size_t i) const {
    size_t n = 0;
    for (const EntityBuf& b : bufs_) {
        if (b.used) {
            if (n == i) return b.entityId;
            ++n;
        }
    }
    return 0;
}

const InterpBuffer* ClientSession::bufferOf(uint16_t entityId) const {
    for (const EntityBuf& b : bufs_) {
        if (b.used && b.entityId == entityId) return &b.buf;
    }
    return nullptr;
}

bool ClientSession::hasEntity(uint16_t entityId) const {
    return bufferOf(entityId) != nullptr;
}

void ClientSession::forgetEntity(uint16_t entityId) {
    for (EntityBuf& b : bufs_) {
        if (b.used && b.entityId == entityId) b = EntityBuf{};
    }
}

} // namespace nl::net
