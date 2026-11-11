#pragma once
// transport.hpp — M-E6：RPC 传输抽象（TCP / Windows 命名管道双实现）
// 上层协议（msgpack + 4 字节长度前缀 + JSON-RPC 信封）与传输无关：
//   endpoint 字符串决定用哪套 I/O，RpcHost(server) / RpcBridge(client) 只认 Stream。
// 头文件不引 windows.h / winsock2.h（避免公共头污染、宏冲突），句柄全部藏在 .cpp。
// 命名管道口径：\\.\pipe\<name>，字节流模式（PIPE_TYPE_BYTE），长度前缀自己切帧，
// 不用 PIPE_TYPE_MESSAGE（消息模式会把一次 sendAll 拆成多条，与 TCP 语义不等价）。
#include <cstdint>
#include <memory>
#include <string>

namespace nl::editor::transport {

enum class Kind { Tcp, Pipe };

struct Endpoint {
    Kind        kind = Kind::Tcp;
    std::string host = "127.0.0.1";
    uint16_t    port = 29110;
    std::string pipe = "nl-editor-rpc";   // 不含 \\.\pipe\ 前缀
    std::string display() const;           // "tcp://127.0.0.1:29110" / "pipe://name"
};

// 支持：tcp://127.0.0.1:29110、tcp://127.0.0.1、pipe://nl-editor-rpc、裸端口 "29110"
// 失败返回 false 并写 err（调用方决定是否退出，绝不静默回落默认端口）。
bool parseEndpoint(const std::string& text, Endpoint& out, std::string& err);

// 已建立的一条双向字节流。recvSome 返回 0 = 当前无数据（不是断线），<0 = 断开/出错。
class Stream {
public:
    virtual ~Stream() = default;
    virtual int  sendAll(const uint8_t* data, int len) = 0;
    virtual int  recvSome(uint8_t* buf, int cap) = 0;
    virtual bool waitReadable(int timeoutMs) = 0;
    virtual void close() = 0;
    virtual bool valid() const = 0;
    virtual const char* label() const = 0;   // "tcp" / "pipe"
};

// 服务端监听者。pollAccept 必须非阻塞：多 client 场景下一帧可接多个连接。
class Listener {
public:
    virtual ~Listener() = default;
    virtual std::unique_ptr<Stream> pollAccept() = 0;
    virtual bool valid() const = 0;
    virtual Endpoint endpoint() const = 0;
};

std::unique_ptr<Listener> createListener(const Endpoint& ep, std::string& err);
std::unique_ptr<Stream>   createStream  (const Endpoint& ep, std::string& err);

} // namespace nl::editor::transport
