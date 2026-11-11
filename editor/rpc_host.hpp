#pragma once
// rpc_host.hpp — 编辑器侧 RPC server（监听引擎 client）
// 角色：编辑器 = Server，引擎 = Client（对齐 Sunshine 引擎侧连回编辑器 server 模型）。
// 依赖：msgpack_min.hpp / transport.hpp（editor_bridge/），零新增 vendored 依赖。
// 协议：RPC-SPEC.md §3~§12（msgpack + 4 字节大端长度前缀 + JSON-RPC 2.0 信封语义）。
//
// M-E6 多视口：一个 RpcHost 同时持有多个 Client（每个引擎进程一条连接），
// engine.announce 带可选 viewId（缺省 "main"）。插件照旧调 call()/notify()，
// 打到 activeView；callTo()/notifyTo() 显式指定视图，viewIds() 列全部视图。
#include "msgpack_min.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

    // endpoint: "tcp://127.0.0.1:29110"（默认）/ "pipe://nl-editor-rpc" / 裸端口
    bool start(const std::string& endpoint = "tcp://127.0.0.1:29110");
    std::string endpoint() const;
    void update();   // 由编辑器主循环每帧调用：accept(可多个) + 泵入站 + 心跳超时
    void stop();

    bool isConnected() const;              // 是否至少有一个引擎 client
    size_t clientCount() const;            // M-E6：连入的引擎进程数
    std::vector<std::string> viewIds() const;   // M-E6：各连接声明的 viewId
    void setActiveView(const std::string& viewId);
    const std::string& activeView() const;

    // 注册引擎→编辑器调用（如 editor.log / editor.views）
    void registerHandler(std::string_view method, HandlerFn);
    void registerNotify (std::string_view method, NotifyFn);

    // 出站（编辑器→引擎命令）。call/notify 打给 activeView（无显式视图时取首个连接）。
    RpcResult call(std::string_view method, const RpcValue& params, int timeoutMs = 5000);
    void      notify(std::string_view method, const RpcValue& params);
    // M-E6：按 viewId 精确路由（多视口下插件/探针可向指定视图发命令）。
    RpcResult callTo(const std::string& viewId, std::string_view method,
                     const RpcValue& params, int timeoutMs = 5000);
    bool      notifyTo(const std::string& viewId, std::string_view method,
                       const RpcValue& params);

    // 生命周期回调（M-E6：均带 viewId，单视口时恒为 "main"）
    std::function<void(const std::string& /*viewId*/, uint64_t /*hwnd*/, int /*pid*/,
                       int /*proto*/, const RpcValue& /*caps*/)> onEngineAnnounce;
    std::function<void()> onEngineReady;                         // 每次有新 client 连上
    std::function<void(const std::string& /*viewId*/)> onEngineClose; // 掉线 → 拆该视口

    void setHeartbeat(bool on, int timeoutMs = 6000);

private:
    struct Client;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void     dispatch(const RpcValue& env, Client& c);
    RpcResult callOn(Client& c, std::string_view method, const RpcValue& params, int timeoutMs);
    Client*  findView(const std::string& viewId) const;
    static   RpcResult extractResp(const msgpack::Map& m);
};

} // namespace nl::editor::host
