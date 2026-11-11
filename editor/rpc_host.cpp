// rpc_host.cpp — 编辑器侧 RPC server（M-E6：传输抽象 + 多 client / viewId 路由）
#include "rpc_host.hpp"
#include "transport.hpp"
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <map>
#include <vector>

namespace nl::editor::host {

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 线格式：4 字节**大端**长度前缀（M-E1 实现 + M-E3/E4/E5 全部 python 探针一致；
// RPC-SPEC §4 早期写「小端」是笔误，M-E6 勘误文档但不改线格式，避免破坏历史基线）。
void sendEnvelope(nl::editor::transport::Stream& s, const msgpack::Value& v) {
    auto buf = msgpack::encode(v);
    uint8_t hdr[4];
    hdr[0] = (buf.size() >> 24) & 0xff; hdr[1] = (buf.size() >> 16) & 0xff;
    hdr[2] = (buf.size() >> 8) & 0xff;  hdr[3] = buf.size() & 0xff;
    if (s.sendAll(hdr, 4) != 4) return;
    if (!buf.empty()) (void)s.sendAll(buf.data(), static_cast<int>(buf.size()));
}

bool tryParseOne(std::vector<uint8_t>& buf, msgpack::Value& out) {
    if (buf.size() < 4) return false;
    uint32_t len = (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) |
                   (uint32_t(buf[2]) << 8) | uint32_t(buf[3]);
    if (len > 8u * 1024u * 1024u) { buf.clear(); return false; }   // §4 上限 8 MiB
    if (buf.size() < 4 + len) return false;
    out = msgpack::decode(buf.data() + 4, len);
    buf.erase(buf.begin(), buf.begin() + 4 + len);
    return true;
}

void rpcLog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    fprintf(stdout, "[rpc] %s\n", buf);
    fflush(stdout);
}

std::string strField(const msgpack::Map& m, const char* key, const char* def) {
    auto it = m.find(key);
    return (it != m.end() && it->second.isStr()) ? it->second.asStr() : def;
}
int64_t intField(const msgpack::Map& m, const char* key, int64_t def) {
    auto it = m.find(key);
    return (it != m.end() && it->second.isInt()) ? it->second.asInt() : def;
}

} // namespace

struct RpcHost::Client {
    std::unique_ptr<nl::editor::transport::Stream> stream;
    std::vector<uint8_t> recvBuf;
    std::string viewId = "main";
    int64_t     lastRecvMs = 0;
    bool        announced = false;
};

struct RpcHost::Impl {
    std::unique_ptr<nl::editor::transport::Listener> listener;
    nl::editor::transport::Endpoint ep;
    std::vector<std::unique_ptr<Client>> clients;
    std::map<std::string, HandlerFn, std::less<>> handlers;
    std::map<std::string, NotifyFn, std::less<>> notifies;
    uint32_t nextId = 1;
    bool heartbeat = true;
    int  heartbeatTimeoutMs = 6000;
    std::string activeView = "main";
};

RpcHost::RpcHost() : impl_(std::make_unique<Impl>()) {}
RpcHost::~RpcHost() { stop(); }

RpcResult RpcHost::extractResp(const msgpack::Map& m) {
    bool ok = m.count("ok") && m.at("ok").isBool() ? m.at("ok").asBool() : false;
    if (ok) return m.count("result") ? m.at("result") : msgpack::Value();
    RpcError e{500, ""};
    if (m.count("error") && m.at("error").isMap()) {
        auto& e2 = m.at("error").asMap();
        e.code    = e2.count("code")    && e2.at("code").isInt()    ? (int)e2.at("code").asInt()    : 500;
        e.message = e2.count("message") && e2.at("message").isStr() ? e2.at("message").asStr()       : "";
    }
    return e;
}

RpcHost::Client* RpcHost::findView(const std::string& viewId) const {
    auto& i = *impl_;
    for (auto& c : i.clients)
        if (c->viewId == viewId && c->stream && c->stream->valid()) return c.get();
    return nullptr;
}

void RpcHost::dispatch(const RpcValue& env, Client& c) {
    auto& i = *impl_;
    if (!env.isMap() || !c.stream) return;
    const auto& m = env.asMap();
    std::string type = strField(m, "type", "");
    if (type == "notify") {
        std::string method = strField(m, "method", "");
        auto it = i.notifies.find(method);
        if (it != i.notifies.end())
            it->second(m.count("params") ? m.at("params") : msgpack::Value());
        if (method == "engine.announce") {
            const auto& p = (m.count("params") && m.at("params").isMap())
                            ? m.at("params").asMap() : msgpack::Map{};
            std::string viewId = strField(p, "viewId", "main");
            if (viewId.empty()) viewId = "main";
            // 重复 viewId 保留两条连接但打警告（不静默串台）；路由取首个匹配。
            if (viewId != c.viewId && findView(viewId))
                rpcLog("warning: duplicate viewId '%s' (pid=%lld)", viewId.c_str(),
                       (long long)intField(p, "pid", 0));
            c.viewId = viewId;
            c.announced = true;
            uint64_t hwnd = (uint64_t)intField(p, "hwnd", 0);
            int pid       = (int)intField(p, "pid", 0);
            int proto     = (int)intField(p, "proto", 1);
            rpcLog("announce view=%s hwnd=0x%llx pid=%d proto=%d transport=%s",
                   viewId.c_str(), (unsigned long long)hwnd, pid, proto, c.stream->label());
            if (onEngineAnnounce)
                onEngineAnnounce(viewId, hwnd, pid, proto,
                                 p.count("caps") ? p.at("caps") : msgpack::Value());
        } else if (method == "rpc.ping") {
            msgpack::Map pong;
            pong["jsonrpc"] = msgpack::Value(1);
            pong["id"]      = msgpack::Value(0);
            pong["type"]    = msgpack::Value("notify");
            pong["method"]  = msgpack::Value("rpc.pong");
            sendEnvelope(*c.stream, msgpack::Value(pong));
        }
    } else if (type == "req") {
        std::string method = strField(m, "method", "");
        auto it = i.handlers.find(method);
        RpcResult res = it != i.handlers.end()
            ? it->second(m.count("params") ? m.at("params") : msgpack::Value())
            : RpcResult(RpcError{100, "method not found"});
        msgpack::Map resp;
        resp["jsonrpc"] = msgpack::Value(1);
        resp["id"]      = m.count("id") ? m.at("id") : msgpack::Value(0);
        resp["type"]    = msgpack::Value("resp");
        if (std::holds_alternative<RpcError>(res)) {
            auto& e = std::get<RpcError>(res);
            resp["ok"] = msgpack::Value(false);
            msgpack::Map err;
            err["code"] = msgpack::Value(int64_t(e.code));
            err["message"] = msgpack::Value(e.message);
            resp["error"] = msgpack::Value(err);
        } else {
            resp["ok"] = msgpack::Value(true);
            resp["result"] = std::get<RpcValue>(res);
        }
        sendEnvelope(*c.stream, msgpack::Value(resp));
    }
}

bool RpcHost::start(const std::string& endpoint) {
    auto& i = *impl_;
    std::string err;
    if (!nl::editor::transport::parseEndpoint(endpoint, i.ep, err)) {
        rpcLog("start failed: bad endpoint '%s' (%s)", endpoint.c_str(), err.c_str());
        return false;
    }
    i.listener = nl::editor::transport::createListener(i.ep, err);
    if (!i.listener) {
        rpcLog("start failed: %s (%s)", i.ep.display().c_str(), err.c_str());
        return false;
    }
    rpcLog("listening on %s", i.ep.display().c_str());
    return true;
}

std::string RpcHost::endpoint() const { return impl_->ep.display(); }

void RpcHost::update() {
    auto& i = *impl_;
    // 1) 非阻塞 accept：一帧可接多条连接（多视口/多引擎进程）
    if (i.listener && i.listener->valid()) {
        for (int n = 0; n < 16; ++n) {
            auto s = i.listener->pollAccept();
            if (!s) break;
            auto c = std::make_unique<Client>();
            c->stream = std::move(s);
            c->lastRecvMs = nowMs();
            i.clients.push_back(std::move(c));
            rpcLog("client connected (%zu total, transport=%s)", i.clients.size(),
                   i.clients.back()->stream->label());
            if (onEngineReady) onEngineReady();
        }
    }
    // 2) 逐 client 泵入站 + 心跳超时
    for (size_t k = 0; k < i.clients.size();) {
        Client& c = *i.clients[k];
        bool alive = c.stream && c.stream->valid();
        if (alive && c.stream->waitReadable(0)) {
            uint8_t tmp[8192];
            int r = c.stream->recvSome(tmp, sizeof tmp);
            if (r < 0) alive = false;
            else if (r > 0) {
                c.recvBuf.insert(c.recvBuf.end(), tmp, tmp + r);
                c.lastRecvMs = nowMs();
            }
        }
        if (alive) {
            msgpack::Value env;
            while (tryParseOne(c.recvBuf, env)) { if (env.isMap()) dispatch(env, c); }
        }
        if (alive && i.heartbeat && nowMs() - c.lastRecvMs > i.heartbeatTimeoutMs)
            alive = false;
        if (!alive) {
            std::string id = c.viewId;
            c.stream->close();
            rpcLog("client closed view=%s (%zu remain)", id.c_str(), i.clients.size() - 1);
            if (onEngineClose) onEngineClose(id);
            i.clients.erase(i.clients.begin() + static_cast<ptrdiff_t>(k));
        } else {
            ++k;
        }
    }
}

bool RpcHost::isConnected() const { return !impl_->clients.empty(); }
size_t RpcHost::clientCount() const { return impl_->clients.size(); }

std::vector<std::string> RpcHost::viewIds() const {
    std::vector<std::string> out;
    for (auto& c : impl_->clients) out.push_back(c->viewId);
    return out;
}
void RpcHost::setActiveView(const std::string& viewId) { impl_->activeView = viewId; }
const std::string& RpcHost::activeView() const { return impl_->activeView; }

void RpcHost::stop() {
    auto& i = *impl_;
    for (auto& c : i.clients) if (c->stream) c->stream->close();
    i.clients.clear();
    i.listener.reset();
}

RpcResult RpcHost::callOn(Client& c, std::string_view method,
                          const RpcValue& params, int timeoutMs) {
    auto& i = *impl_;
    if (!c.stream || !c.stream->valid()) return RpcError{102, "engine not connected"};
    uint32_t id = i.nextId++;
    msgpack::Map env;
    env["jsonrpc"] = msgpack::Value(1);
    env["id"]      = msgpack::Value(int64_t(id));
    env["type"]    = msgpack::Value("req");
    env["method"]  = msgpack::Value(std::string(method));
    env["params"]  = params;   // RPC-SPEC：params 统一命名 Map
    sendEnvelope(*c.stream, msgpack::Value(env));

    int64_t start = nowMs();
    while (nowMs() - start < timeoutMs) {
        if (c.stream->waitReadable(2)) {
            uint8_t tmp[8192];
            int r = c.stream->recvSome(tmp, sizeof tmp);
            if (r < 0) return RpcError{500, "recv closed"};
            if (r > 0) {
                c.recvBuf.insert(c.recvBuf.end(), tmp, tmp + r);
                c.lastRecvMs = nowMs();
            }
            msgpack::Value env2;
            while (tryParseOne(c.recvBuf, env2)) {
                if (!env2.isMap()) continue;
                const auto& m = env2.asMap();
                std::string t = strField(m, "type", "");
                if (t == "resp" && m.count("id") && m.at("id").isInt() &&
                    m.at("id").asInt() == static_cast<int64_t>(id))
                    return extractResp(m);
                dispatch(env2, c);   // 同连接上的 notify/其它 req 照常派发
            }
        }
    }
    return RpcError{408, "timeout"};
}

RpcResult RpcHost::call(std::string_view method, const RpcValue& params, int timeoutMs) {
    auto& i = *impl_;
    Client* c = findView(i.activeView);
    if (!c && !i.clients.empty()) c = i.clients.front().get();
    if (!c) return RpcError{102, "engine not connected"};
    return callOn(*c, method, params, timeoutMs);
}

RpcResult RpcHost::callTo(const std::string& viewId, std::string_view method,
                          const RpcValue& params, int timeoutMs) {
    Client* c = findView(viewId);
    if (!c) return RpcError{103, "unknown view"};
    return callOn(*c, method, params, timeoutMs);
}

void RpcHost::notify(std::string_view method, const RpcValue& params) {
    auto& i = *impl_;
    Client* c = findView(i.activeView);
    if (!c && !i.clients.empty()) c = i.clients.front().get();
    (void)notifyTo(c ? c->viewId : i.activeView, method, params);
}

bool RpcHost::notifyTo(const std::string& viewId, std::string_view method,
                       const RpcValue& params) {
    Client* c = findView(viewId);
    if (!c) return false;
    msgpack::Map env;
    env["jsonrpc"] = msgpack::Value(1);
    env["id"]      = msgpack::Value(0);
    env["type"]    = msgpack::Value("notify");
    env["method"]  = msgpack::Value(std::string(method));
    env["params"]  = params;
    sendEnvelope(*c->stream, msgpack::Value(env));
    return true;
}

void RpcHost::registerHandler(std::string_view method, HandlerFn fn) {
    impl_->handlers[std::string(method)] = std::move(fn);
}
void RpcHost::registerNotify(std::string_view method, NotifyFn fn) {
    impl_->notifies[std::string(method)] = std::move(fn);
    // rpc.ping 的 pong 现在由 dispatch 直接按 Client 回（多 client 必须定向回，
    // 不能再像 M-E1 那样闭包里抓「唯一那个 socket」）。
}
void RpcHost::setHeartbeat(bool on, int timeoutMs) {
    impl_->heartbeat = on; impl_->heartbeatTimeoutMs = timeoutMs;
}

} // namespace nl::editor::host
