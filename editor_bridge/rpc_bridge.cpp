// rpc_bridge.cpp — 引擎侧 RPC client（M-E6：I/O 走 transport::Stream，TCP/管道同协议）
#include "rpc_bridge.hpp"
#include "transport.hpp"
#include <chrono>
#include <cstdio>
#include <map>
#include <vector>

namespace nl::editor {

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

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
    if (len > 8u * 1024u * 1024u) { buf.clear(); return false; }
    if (buf.size() < 4 + len) return false;
    out = msgpack::decode(buf.data() + 4, len);
    buf.erase(buf.begin(), buf.begin() + 4 + len);
    return true;
}

std::string strField(const msgpack::Map& m, const char* key, const char* def) {
    auto it = m.find(key);
    return (it != m.end() && it->second.isStr()) ? it->second.asStr() : def;
}

} // namespace

struct RpcBridge::Impl {
    std::unique_ptr<nl::editor::transport::Stream> stream;
    nl::editor::transport::Endpoint ep;
    std::map<std::string, HandlerFn, std::less<>> handlers;
    std::map<std::string, NotifyFn, std::less<>> notifies;
    std::vector<uint8_t> recvBuf;
    uint32_t nextId = 1;
    bool heartbeat = true;
    int  heartbeatTimeoutMs = 6000;
    int64_t lastRecvMs = 0;
    int64_t lastPingMs = 0;
    bool connected = false;
};

RpcBridge::RpcBridge() : impl_(std::make_unique<Impl>()) {}
RpcBridge::~RpcBridge() { shutdown(); }

RpcResult RpcBridge::extractResp(const msgpack::Map& m) {
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

void RpcBridge::dispatch(const RpcValue& env) {
    auto& i = *impl_;
    if (!env.isMap() || !i.stream) return;
    const auto& m = env.asMap();
    std::string type = strField(m, "type", "");
    if (type == "notify") {
        std::string method = strField(m, "method", "");
        auto it = i.notifies.find(method);
        if (it != i.notifies.end())
            it->second(m.count("params") ? m.at("params") : msgpack::Value());
        if (method == "engine.announce" && onAnnounce) {
            const auto& p = (m.count("params") && m.at("params").isMap())
                            ? m.at("params").asMap() : msgpack::Map{};
            auto hit = p.find("hwnd");
            uint64_t hwnd = (hit != p.end() && hit->second.isInt())
                            ? static_cast<uint64_t>(hit->second.asInt()) : 0;
            auto pit = p.find("pid");
            int pid = (pit != p.end() && pit->second.isInt()) ? (int)pit->second.asInt() : 0;
            onAnnounce(hwnd, pid);
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
        sendEnvelope(*i.stream, msgpack::Value(resp));
    }
}

bool RpcBridge::connect(const std::string& endpoint) {
    auto& i = *impl_;
    std::string err;
    nl::editor::transport::Endpoint ep;
    if (!nl::editor::transport::parseEndpoint(endpoint, ep, err)) {
        fprintf(stderr, "[editor-bridge] bad endpoint '%s': %s\n", endpoint.c_str(), err.c_str());
        return false;
    }
    // M-E4a 重连前置：心跳超时路径只置 connected=false 不关句柄，先清陈旧连接
    if (i.stream) { i.stream->close(); i.stream.reset(); }
    i.connected = false;
    i.recvBuf.clear();
    i.stream = nl::editor::transport::createStream(ep, err);
    if (!i.stream) return false;
    i.ep = ep;
    i.connected = true;
    i.lastRecvMs = i.lastPingMs = nowMs();
    i.nextId = 1;
    if (onReady) onReady();
    return true;
}

std::string RpcBridge::endpoint() const { return impl_->ep.display(); }

void RpcBridge::update() {
    auto& i = *impl_;
    if (!i.connected || !i.stream) return;
    if (i.stream->waitReadable(0)) {
        uint8_t tmp[8192];
        int r = i.stream->recvSome(tmp, sizeof tmp);
        if (r < 0) { i.connected = false; if (onClose) onClose(); return; }
        if (r > 0) {
            i.recvBuf.insert(i.recvBuf.end(), tmp, tmp + r);
            i.lastRecvMs = nowMs();
        }
    }
    msgpack::Value env;
    while (tryParseOne(i.recvBuf, env)) { if (env.isMap()) dispatch(env); }
    if (i.heartbeat) {
        if (nowMs() - i.lastRecvMs > i.heartbeatTimeoutMs) {
            i.connected = false; if (onClose) onClose(); return;
        }
        if (nowMs() - i.lastPingMs > 2000) {
            i.lastPingMs = nowMs();
            msgpack::Map p;
            p["jsonrpc"] = msgpack::Value(1);
            p["id"]      = msgpack::Value(0);
            p["type"]    = msgpack::Value("notify");
            p["method"]  = msgpack::Value("rpc.ping");
            sendEnvelope(*i.stream, msgpack::Value(p));
        }
    }
}

RpcResult RpcBridge::call(std::string_view method, const RpcValue& params, int timeoutMs) {
    auto& i = *impl_;
    if (!i.connected || !i.stream) return RpcError{102, "not connected"};
    uint32_t id = i.nextId++;
    msgpack::Map env;
    env["jsonrpc"] = msgpack::Value(1);
    env["id"]      = msgpack::Value(int64_t(id));
    env["type"]    = msgpack::Value("req");
    env["method"]  = msgpack::Value(std::string(method));
    env["params"]  = params;
    sendEnvelope(*i.stream, msgpack::Value(env));

    int64_t start = nowMs();
    while (nowMs() - start < timeoutMs) {
        if (i.stream->waitReadable(2)) {
            uint8_t tmp[8192];
            int r = i.stream->recvSome(tmp, sizeof tmp);
            if (r < 0) { i.connected = false; return RpcError{500, "recv closed"}; }
            if (r > 0) {
                i.recvBuf.insert(i.recvBuf.end(), tmp, tmp + r);
                i.lastRecvMs = nowMs();
            }
            msgpack::Value env2;
            while (tryParseOne(i.recvBuf, env2)) {
                if (!env2.isMap()) continue;
                const auto& m = env2.asMap();
                std::string t = strField(m, "type", "");
                if (t == "resp" && m.count("id") && m.at("id").isInt() &&
                    m.at("id").asInt() == static_cast<int64_t>(id))
                    return extractResp(m);
                dispatch(env2);
            }
        }
    }
    return RpcError{408, "timeout"};
}

void RpcBridge::notify(std::string_view method, const RpcValue& params) {
    auto& i = *impl_;
    if (!i.connected || !i.stream) return;
    msgpack::Map env;
    env["jsonrpc"] = msgpack::Value(1);
    env["id"]      = msgpack::Value(0);
    env["type"]    = msgpack::Value("notify");
    env["method"]  = msgpack::Value(std::string(method));
    env["params"]  = params;
    sendEnvelope(*i.stream, msgpack::Value(env));
}

void RpcBridge::registerHandler(std::string_view method, HandlerFn fn) {
    impl_->handlers[std::string(method)] = std::move(fn);
}
void RpcBridge::registerNotify(std::string_view method, NotifyFn fn) {
    impl_->notifies[std::string(method)] = std::move(fn);
}
void RpcBridge::setHeartbeat(bool on, int timeoutMs) {
    impl_->heartbeat = on; impl_->heartbeatTimeoutMs = timeoutMs;
}

void RpcBridge::shutdown() {
    auto& i = *impl_;
    if (i.stream) { i.stream->close(); i.stream.reset(); }
    i.connected = false;
}

bool RpcBridge::isConnected() const {
    return impl_->connected && impl_->stream && impl_->stream->valid();
}

} // namespace nl::editor
