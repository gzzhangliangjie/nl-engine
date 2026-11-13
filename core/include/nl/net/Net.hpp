// NET M-NET0：nl/net 传输层薄封装（ENet vendored，PIMPL）。
// 隔离纪律（thirdparty/enet/VENDOR.md）：enet_ 类型只出现在 core/src/net/Net.cpp
// 单一 TU；业务代码不 include ENet 头。本层零游戏语义——只有建链 / 断链 / 双通道
// 发收 / 事件泵；快照编解码（M-NET1 bitsery）与同步模型（M-NET2）不在此层。
// 通道口径：0 = 可靠有序（控制消息），1 = 不可靠（快照流，丢旧保新由消费方处理）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nl::net {

enum class Channel : uint8_t { Reliable = 0, Unreliable = 1 };

enum class EventType : uint8_t { None, PeerConnected, PeerDisconnected, Data };

struct NetEvent {
    EventType type = EventType::None;
    uint32_t peerId = 0;       // server 侧对端编号（CONNECT 时分配；0 = 无效）
    Channel channel = Channel::Reliable;
    std::vector<uint8_t> data; // 仅 Data 事件填充（拷贝出 ENet 包后立即销毁）
};

// 进程级初始化（引用计数由调用方保证一比一；net-tools / viewer 各自调用）。
bool netInit();
void netShutdown();

// vendored 版本串（ENET_VERSION，当前 "1.3.18"）。
const char* version();

// 服务器：监听 UDP 端口，接受 maxClients 个对端。
class NetServer {
public:
    NetServer();
    ~NetServer();
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    bool start(uint16_t port, uint32_t maxClients);
    void stop();
    bool running() const;
    uint32_t peerCount() const;

    // 泵事件。timeoutMs > 0 时最多阻塞等待一个事件，随后把已就绪事件取空；
    // timeoutMs = 0 纯非阻塞。事件不跨调用排队。
    void service(uint32_t timeoutMs, std::vector<NetEvent>& out);

    // 定向发送（peerId 来自事件）；对端无效返回 false。立即 flush。
    bool send(uint32_t peerId, Channel ch, const uint8_t* data, size_t bytes);
    void broadcast(Channel ch, const uint8_t* data, size_t bytes);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// 客户端：连一台服务器（单对端）。
class NetClient {
public:
    NetClient();
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // 阻塞握手直到 CONNECT / DISCONNECT / 超时；失败返回 false（含连接被拒）。
    // 仅适合对端已在泵事件的场景（双进程：server 在自己线程/进程 service）。
    // 单进程回环（同线程双 host）必须用 beginConnect + service 轮询，
    // 否则 server 无机会处理握手包，阻塞等待必然超时。
    bool connect(const char* host, uint16_t port, uint32_t timeoutMs);
    // 非阻塞发起连接：只创建 host 与 peer，握手由 service() 泵出 PeerConnected。
    bool beginConnect(const char* host, uint16_t port);
    void disconnect();
    bool connected() const;

    void service(uint32_t timeoutMs, std::vector<NetEvent>& out);
    bool send(Channel ch, const uint8_t* data, size_t bytes);

    // ENet 往返计时（毫秒；未连接返回 0）。
    uint32_t rttMs() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace nl::net
