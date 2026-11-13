// NET M-NET0：nl/net 传输层薄封装实现。vendored ENet 符号只出现在本 TU
//（nl/net PIMPL 纪律，对齐 core/src/nav/Nav.cpp / core/src/physics/World.cpp 先例，
// thirdparty/enet/VENDOR.md 隔离条目）。逐步检查 enet_* 返回状态，不吞错。
// 版本锁 1.3.18 @ 5a9c537（2026-11-13，gh-proxy 浅克隆）。

#include "nl/net/Net.hpp"

#include <enet/enet.h>

#include <cstdio>
#include <map>
#include <utility>

namespace nl::net {

namespace {

// 通道 0 = 可靠有序（ENET_PACKET_FLAG_RELIABLE），1 = 不可靠。
constexpr enet_uint8 kChannelIndex(uint8_t ch) {
    return static_cast<enet_uint8>(ch);
}

constexpr enet_uint32 kPacketFlags(Channel ch) {
    return ch == Channel::Reliable ? ENET_PACKET_FLAG_RELIABLE : 0u;
}

constexpr int kChannelCount = 2;

} // namespace

bool netInit() {
    return enet_initialize() == 0;
}

void netShutdown() {
    enet_deinitialize();
}

const char* version() {
    // 1.3.18 无 enet_linked_version_string（上游只有 ENET_VERSION 宏族）；
    // 纯源码直编，编译头即链接版本，宏拼接语义等价。
    static char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u",
                  ENET_VERSION_GET_MAJOR(ENET_VERSION),
                  ENET_VERSION_GET_MINOR(ENET_VERSION),
                  ENET_VERSION_GET_PATCH(ENET_VERSION));
    return buf;
}

// ---------------------------------------------------------------- NetServer

struct NetServer::Impl {
    ENetHost* host = nullptr;
    std::map<ENetPeer*, uint32_t> peerIds;
    uint32_t nextPeerId = 1;

    void destroy() {
        if (host) {
            // M-NET3 优雅关服：逐 peer 发 disconnect 并 flush，对端立即感知断链
            //（enet_host_destroy 直拆 socket 不通知 peer，client 只能等默认超时）。
            for (const auto& kv : peerIds) {
                if (kv.first &&
                    kv.first->state != ENET_PEER_STATE_DISCONNECTED &&
                    kv.first->state != ENET_PEER_STATE_ZOMBIE) {
                    enet_peer_disconnect(kv.first, 0);
                }
            }
            enet_host_flush(host);
            enet_host_destroy(host);
            host = nullptr;
        }
        peerIds.clear();
    }
};

NetServer::NetServer() = default;
NetServer::~NetServer() {
    delete impl_;
}

bool NetServer::start(uint16_t port, uint32_t maxClients) {
    delete impl_;
    impl_ = new Impl;
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;
    impl_->host = enet_host_create(&address, maxClients, kChannelCount, 0, 0);
    return impl_->host != nullptr;
}

void NetServer::stop() {
    if (impl_) impl_->destroy();
}

bool NetServer::running() const {
    return impl_ && impl_->host != nullptr;
}

uint32_t NetServer::peerCount() const {
    return impl_ ? static_cast<uint32_t>(impl_->peerIds.size()) : 0u;
}

void NetServer::service(uint32_t timeoutMs, std::vector<NetEvent>& out) {
    if (!impl_ || !impl_->host) return;
    ENetEvent ev;
    // 最多阻塞等一个事件，随后把已就绪事件取空（timeoutMs=0 时纯非阻塞）。
    for (;;) {
        const int rc = enet_host_service(impl_->host, &ev, out.empty() ? timeoutMs : 0);
        if (rc <= 0) break;
        NetEvent e;
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            e.type = EventType::PeerConnected;
            e.peerId = impl_->nextPeerId++;
            impl_->peerIds[ev.peer] = e.peerId;
            ev.peer->data = reinterpret_cast<void*>(static_cast<uintptr_t>(e.peerId));
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            e.type = EventType::PeerDisconnected;
            {
                const auto it = impl_->peerIds.find(ev.peer);
                e.peerId = it != impl_->peerIds.end() ? it->second : 0u;
                impl_->peerIds.erase(ev.peer);
            }
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            e.type = EventType::Data;
            {
                const auto it = impl_->peerIds.find(ev.peer);
                e.peerId = it != impl_->peerIds.end() ? it->second : 0u;
            }
            e.channel = ev.channelID == 0 ? Channel::Reliable : Channel::Unreliable;
            e.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
            break;
        default:
            break;
        }
        if (e.type != EventType::None) out.push_back(std::move(e));
    }
}

bool NetServer::send(uint32_t peerId, Channel ch, const uint8_t* data, size_t bytes) {
    if (!impl_ || !impl_->host) return false;
    ENetPeer* peer = nullptr;
    for (const auto& [p, id] : impl_->peerIds) {
        if (id == peerId) { peer = p; break; }
    }
    if (!peer) return false;
    ENetPacket* packet = enet_packet_create(data, bytes, kPacketFlags(ch));
    if (!packet) return false;
    if (enet_peer_send(peer, kChannelIndex(static_cast<uint8_t>(ch)), packet) != 0) {
        enet_packet_destroy(packet);
        return false;
    }
    enet_host_flush(impl_->host);
    return true;
}

void NetServer::broadcast(Channel ch, const uint8_t* data, size_t bytes) {
    if (!impl_ || !impl_->host) return;
    ENetPacket* packet = enet_packet_create(data, bytes, kPacketFlags(ch));
    if (!packet) return;
    enet_host_broadcast(impl_->host, kChannelIndex(static_cast<uint8_t>(ch)), packet);
    enet_host_flush(impl_->host);
}

// ---------------------------------------------------------------- NetClient

struct NetClient::Impl {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;

    void destroy() {
        if (host) {
            enet_host_flush(host);
            enet_host_destroy(host);
            host = nullptr;
        }
        peer = nullptr;
    }
};

NetClient::NetClient() = default;
NetClient::~NetClient() {
    delete impl_;
}

bool NetClient::beginConnect(const char* hostStr, uint16_t port) {
    delete impl_;
    impl_ = new Impl;
    impl_->host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
    if (!impl_->host) return false;
    ENetAddress address;
    if (enet_address_set_host(&address, hostStr) != 0) {
        impl_->destroy();
        return false;
    }
    address.port = port;
    impl_->peer = enet_host_connect(impl_->host, &address, kChannelCount, 0);
    if (!impl_->peer) {
        impl_->destroy();
        return false;
    }
    return true;
}

bool NetClient::connect(const char* hostStr, uint16_t port, uint32_t timeoutMs) {
    if (!beginConnect(hostStr, port)) return false;
    // 阻塞握手：deadline 内等到 CONNECT 视为成功；DISCONNECT 视为拒绝。
    const enet_uint32 deadline = enet_time_get() + timeoutMs;
    for (;;) {
        const enet_uint32 now = enet_time_get();
        if (now >= deadline) {
            impl_->destroy();
            return false;
        }
        ENetEvent ev;
        const int rc = enet_host_service(impl_->host, &ev, deadline - now);
        if (rc <= 0) continue; // 本片超时或瞬时失败，直至 deadline
        if (ev.type == ENET_EVENT_TYPE_CONNECT) return true;
        if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            impl_->destroy();
            return false;
        }
        // 握手期其它事件（极少见）忽略，继续等 CONNECT。
    }
}

void NetClient::disconnect() {
    if (impl_ && impl_->peer) {
        enet_peer_disconnect(impl_->peer, 0);
        impl_->peer = nullptr;
    }
    if (impl_) impl_->destroy();
}

bool NetClient::connected() const {
    return impl_ && impl_->peer && impl_->peer->state == ENET_PEER_STATE_CONNECTED;
}

void NetClient::service(uint32_t timeoutMs, std::vector<NetEvent>& out) {
    if (!impl_ || !impl_->host) return;
    ENetEvent ev;
    for (;;) {
        const int rc = enet_host_service(impl_->host, &ev, out.empty() ? timeoutMs : 0);
        if (rc <= 0) break;
        NetEvent e;
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            e.type = EventType::PeerConnected;
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            e.type = EventType::PeerDisconnected;
            impl_->peer = nullptr;
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            e.type = EventType::Data;
            e.channel = ev.channelID == 0 ? Channel::Reliable : Channel::Unreliable;
            e.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
            break;
        default:
            break;
        }
        if (e.type != EventType::None) out.push_back(std::move(e));
        if (impl_->peer == nullptr && ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
    }
}

bool NetClient::send(Channel ch, const uint8_t* data, size_t bytes) {
    if (!connected()) return false;
    ENetPacket* packet = enet_packet_create(data, bytes, kPacketFlags(ch));
    if (!packet) return false;
    if (enet_peer_send(impl_->peer, kChannelIndex(static_cast<uint8_t>(ch)), packet) != 0) {
        enet_packet_destroy(packet);
        return false;
    }
    enet_host_flush(impl_->host);
    return true;
}

uint32_t NetClient::rttMs() const {
    return (impl_ && impl_->peer) ? impl_->peer->roundTripTime : 0u;
}

} // namespace nl::net
