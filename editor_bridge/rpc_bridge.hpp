#pragma once
// rpc_bridge.hpp — 引擎侧 RPC client（连接编辑器 server）
// 角色：引擎 = Client，编辑器 = Server（对齐 Sunshine 引擎侧连回编辑器 server 模型）。
// 依赖：msgpack_min.hpp（同目录，零依赖）；winsock2（Windows 自带）。
// 协议：RPC-SPEC.md §3~§11（msgpack + 4 字节长度前缀 + JSON-RPC 2.0 信封语义）。
#include "msgpack_min.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>

namespace nl::editor {

using RpcValue = msgpack::Value;
struct RpcError { int code = 0; std::string message; };
using RpcResult = std::variant<RpcValue, RpcError>;
using HandlerFn = std::function<RpcResult(const RpcValue&)>;
using NotifyFn  = std::function<void(const RpcValue&)>;

class RpcBridge {
public:
    RpcBridge();
    ~RpcBridge();
    RpcBridge(const RpcBridge&) = delete;
    RpcBridge& operator=(const RpcBridge&) = delete;

    // 引擎侧为 client，连接编辑器（默认 127.0.0.1:29110）
    bool connect(const std::string& endpoint = "tcp://127.0.0.1:29110");
    void update();               // 必须由引擎主循环每帧调用：泵入站 + 心跳
    void shutdown();
    bool isConnected() const;    // M-E4a：心跳超时/对端关闭后转 false（供上层重连判定）

    // 注册引擎侧可被执行的方法（编辑器 → 引擎调用，如 engine.echo）
    void registerHandler(std::string_view method, HandlerFn);
    void registerNotify (std::string_view method, NotifyFn);

    // 出站（编辑器 → 引擎命令的返回 / 引擎主动上报）。call 为同步阻塞（M-E1 最小通路用）
    RpcResult call(std::string_view method, const RpcValue& params, int timeoutMs = 5000);
    void      notify(std::string_view method, const RpcValue& params);

    // 生命周期回调
    std::function<void()>                       onReady;     // 连上编辑器
    std::function<void()>                       onClose;     // 掉线（崩溃隔离：引擎继续自渲染）
    std::function<void(uint64_t /*hwnd*/, int /*pid*/)> onAnnounce; // 收到编辑器确认（本侧 announce 由编辑器消费）

    void setHeartbeat(bool on, int timeoutMs = 6000);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void     dispatch(const RpcValue& env);
    static   RpcResult extractResp(const msgpack::Map& m);
};

} // namespace nl::editor
