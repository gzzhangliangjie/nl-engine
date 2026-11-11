#pragma once
// rpc_host.hpp — 编辑器侧 RPC server（监听引擎 client）
// 角色：编辑器 = Server，引擎 = Client（对齐 Sunshine 引擎侧连回编辑器 server 模型）。
// 依赖：msgpack_min.hpp（经 CMake 把 editor_bridge/ 加入 include path，零新增依赖）；winsock2。
// 协议：RPC-SPEC.md §3~§12（msgpack + 4 字节长度前缀 + JSON-RPC 2.0 信封语义）。
#include "msgpack_min.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>

namespace nl::editor::host {

using RpcValue = msgpack::Value;
struct RpcError { int code = 0; std::string message; };
using RpcResult = std::variant<RpcValue, RpcError>;
using HandlerFn = std::function<RpcResult(const RpcValue&)>;
using NotifyFn  = std::function<void(const RpcValue&)>;

class RpcHost {
public:
    RpcHost();
    ~RpcHost();
    RpcHost(const RpcHost&) = delete;
    RpcHost& operator=(const RpcHost&) = delete;

    // 编辑器侧为 server，监听引擎 client（默认 127.0.0.1:29110，非阻塞 accept）
    bool start(const std::string& endpoint = "tcp://127.0.0.1:29110");
    void update();   // 由编辑器主循环每帧调用：accept + 泵入站 + 心跳 pong
    void stop();

    bool isConnected() const;   // 引擎 client 是否已连上（供连接状态 UI 显示）

    // 注册引擎→编辑器调用（如 editor.log / editor.sceneChanged）
    void registerHandler(std::string_view method, HandlerFn);
    void registerNotify (std::string_view method, NotifyFn);

    // 出站（编辑器→引擎命令，如 engine.setCamera）
    RpcResult call(std::string_view method, const RpcValue& params, int timeoutMs = 5000);
    void      notify(std::string_view method, const RpcValue& params);

    // 生命周期回调
    std::function<void(uint64_t /*hwnd*/, int /*pid*/, int /*proto*/, const RpcValue& /*caps*/)> onEngineAnnounce;
    std::function<void()> onEngineReady;
    std::function<void()> onEngineClose;   // 掉线 → 空视口 + 自动重连

    void setHeartbeat(bool on, int timeoutMs = 6000);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void     dispatch(const RpcValue& env);
    static   RpcResult extractResp(const msgpack::Map& m);
};

} // namespace nl::editor::host
