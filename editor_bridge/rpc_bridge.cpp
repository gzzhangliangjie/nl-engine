// rpc_bridge.cpp — 引擎侧 RPC client 实现（winsock2 + 自研 msgpack 分帧）
#include "rpc_bridge.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <map>
#include <vector>
#include <chrono>
#include <cstdio>

#pragma comment(lib, "ws2_32.lib")

namespace nl::editor {

static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct RpcBridge::Impl {
    SOCKET sock = INVALID_SOCKET;
    std::map<std::string, HandlerFn, std::less<>> handlers;
    std::map<std::string, NotifyFn, std::less<>> notifies;
    std::vector<uint8_t> recvBuf;
    uint32_t nextId = 1;
    bool heartbeat = true;
    int  heartbeatTimeoutMs = 6000;
    int64_t lastRecvMs = 0;
    int64_t lastPingMs = 0;
    bool connected = false;
    bool wsainit = false;
};

RpcBridge::RpcBridge() : impl_(std::make_unique<Impl>()) {}
RpcBridge::~RpcBridge() { shutdown(); }

static void sendEnvelope(SOCKET s, const msgpack::Value& v) {
    auto buf = msgpack::encode(v);
    uint8_t hdr[4];
    hdr[0] = (buf.size() >> 24) & 0xff; hdr[1] = (buf.size() >> 16) & 0xff;
    hdr[2] = (buf.size() >> 8) & 0xff;  hdr[3] = buf.size() & 0xff;
    send(s, (const char*)hdr, 4, 0);
    if (!buf.empty()) send(s, (const char*)buf.data(), (int)buf.size(), 0);
}

// 从 recvBuf 前段尝试解析一个完整信封（4 字节长度 + msgpack）；成功则消费并返回 true
static bool tryParseOne(std::vector<uint8_t>& buf, msgpack::Value& out) {
    if (buf.size() < 4) return false;
    uint32_t len = (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) |
                   (uint32_t(buf[2]) << 8) | uint32_t(buf[3]);
    if (buf.size() < 4 + len) return false;
    out = msgpack::decode(buf.data() + 4, len);
    buf.erase(buf.begin(), buf.begin() + 4 + len);
    return true;
}

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
    if (!env.isMap()) return;
    const auto& m = env.asMap();
    std::string type = m.count("type") && m.at("type").isStr() ? m.at("type").asStr() : "";
    if (type == "notify") {
        std::string method = m.count("method") && m.at("method").isStr() ? m.at("method").asStr() : "";
        auto it = i.notifies.find(method);
        if (it != i.notifies.end()) it->second(m.count("params") ? m.at("params") : msgpack::Value());
        if (method == "engine.announce" && onAnnounce) {
            const auto& p = m.count("params") && m.at("params").isMap() ? m.at("params").asMap() : msgpack::Map{};
            uint64_t hwnd = p.count("hwnd") && p.at("hwnd").isInt() ? (uint64_t)p.at("hwnd").asInt() : 0;
            int pid      = p.count("pid")  && p.at("pid").isInt()  ? (int)p.at("pid").asInt()        : 0;
            onAnnounce(hwnd, pid);
        }
    } else if (type == "req") {
        std::string method = m.count("method") && m.at("method").isStr() ? m.at("method").asStr() : "";
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
            msgpack::Map err; err["code"] = msgpack::Value(int64_t(e.code)); err["message"] = msgpack::Value(e.message);
            resp["error"] = msgpack::Value(err);
        } else {
            resp["ok"] = msgpack::Value(true);
            resp["result"] = std::get<RpcValue>(res);
        }
        if (i.sock != INVALID_SOCKET) sendEnvelope(i.sock, msgpack::Value(resp));
    }
}

bool RpcBridge::connect(const std::string& endpoint) {
    (void)endpoint; // M-E1 固定 127.0.0.1:29110；endpoint 预留给 §15 端口/管道扩展
    auto& i = *impl_;
    if (!i.wsainit) {
        WSADATA wsa; if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        i.wsainit = true;
    }
    // M-E4a 重连前置：心跳超时路径只置 connected=false 不关 socket，先清掉旧句柄
    if (i.sock != INVALID_SOCKET) { closesocket(i.sock); i.sock = INVALID_SOCKET; }
    i.sock = socket(AF_INET, SOCK_STREAM, 0);
    if (i.sock == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(29110);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(i.sock, (sockaddr*)&addr, sizeof addr) != 0) {
        closesocket(i.sock); i.sock = INVALID_SOCKET; return false;
    }
    i.connected = true;
    i.lastRecvMs = i.lastPingMs = nowMs();
    if (onReady) onReady();
    return true;
}

void RpcBridge::update() {
    auto& i = *impl_;
    if (!i.connected || i.sock == INVALID_SOCKET) return;
    fd_set rd; FD_ZERO(&rd); FD_SET(i.sock, &rd); timeval tv{0, 0};
    if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
        uint8_t tmp[8192];
        int r = recv(i.sock, (char*)tmp, sizeof tmp, 0);
        if (r <= 0) { i.connected = false; if (onClose) onClose(); return; }
        i.recvBuf.insert(i.recvBuf.end(), tmp, tmp + r);
        i.lastRecvMs = nowMs();
    }
    msgpack::Value env;
    while (tryParseOne(i.recvBuf, env)) { if (env.isMap()) dispatch(env); }
    if (i.heartbeat) {
        if (nowMs() - i.lastRecvMs > i.heartbeatTimeoutMs) { i.connected = false; if (onClose) onClose(); return; }
        if (nowMs() - i.lastPingMs > 2000) {
            i.lastPingMs = nowMs();
            msgpack::Map p; p["jsonrpc"] = msgpack::Value(1); p["id"] = msgpack::Value(0);
            p["type"] = msgpack::Value("notify"); p["method"] = msgpack::Value("rpc.ping");
            sendEnvelope(i.sock, msgpack::Value(p));
        }
    }
}

RpcResult RpcBridge::call(std::string_view method, const RpcValue& params, int timeoutMs) {
    auto& i = *impl_;
    if (!i.connected || i.sock == INVALID_SOCKET) return RpcError{102, "not connected"};
    uint32_t id = i.nextId++;
    msgpack::Map env;
    env["jsonrpc"] = msgpack::Value(1);
    env["id"]      = msgpack::Value(int64_t(id));
    env["type"]    = msgpack::Value("req");
    env["method"]  = msgpack::Value(std::string(method));
    // RPC-SPEC：params 统一为命名 Map（JSON-RPC object）；不再包裹成 array，
    // 否则 engine.announce 等按 Map 读取字段的用例会取不到（asMap 命中失败）。
    env["params"]  = params;
    sendEnvelope(i.sock, msgpack::Value(env));

    int64_t start = nowMs();
    while (nowMs() - start < timeoutMs) {
        fd_set rd; FD_ZERO(&rd); FD_SET(i.sock, &rd); timeval tv{0, 2000};
        if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
            uint8_t tmp[8192];
            int r = recv(i.sock, (char*)tmp, sizeof tmp, 0);
            if (r <= 0) { i.connected = false; return RpcError{500, "recv closed"}; }
            i.recvBuf.insert(i.recvBuf.end(), tmp, tmp + r);
            i.lastRecvMs = nowMs();
            msgpack::Value env2;
            while (tryParseOne(i.recvBuf, env2)) {
                if (!env2.isMap()) continue;
                const auto& m = env2.asMap();
                std::string t = m.count("type") && m.at("type").isStr() ? m.at("type").asStr() : "";
                if (t == "resp" && m.count("id") && m.at("id").isInt() && m.at("id").asInt() == (int64_t)id)
                    return extractResp(m);
                dispatch(env2);
            }
        }
    }
    return RpcError{408, "timeout"};
}

void RpcBridge::notify(std::string_view method, const RpcValue& params) {
    auto& i = *impl_;
    if (!i.connected || i.sock == INVALID_SOCKET) return;
    msgpack::Map env;
    env["jsonrpc"] = msgpack::Value(1);
    env["id"]      = msgpack::Value(0);
    env["type"]    = msgpack::Value("notify");
    env["method"]  = msgpack::Value(std::string(method));
    // RPC-SPEC：params 统一为命名 Map（JSON-RPC object）；不再包裹成 array，
    // 否则 engine.announce 等按 Map 读取字段的用例会取不到（asMap 命中失败）。
    env["params"]  = params;
    sendEnvelope(i.sock, msgpack::Value(env));
}

void RpcBridge::registerHandler(std::string_view method, HandlerFn fn) { impl_->handlers[std::string(method)] = std::move(fn); }
void RpcBridge::registerNotify (std::string_view method, NotifyFn  fn) { impl_->notifies[std::string(method)] = std::move(fn); }
void RpcBridge::setHeartbeat(bool on, int timeoutMs) { impl_->heartbeat = on; impl_->heartbeatTimeoutMs = timeoutMs; }

void RpcBridge::shutdown() {
    auto& i = *impl_;
    if (i.sock != INVALID_SOCKET) { closesocket(i.sock); i.sock = INVALID_SOCKET; }
    i.connected = false;
    if (i.wsainit) { WSACleanup(); i.wsainit = false; }
}

bool RpcBridge::isConnected() const { return impl_->connected && impl_->sock != INVALID_SOCKET; }

} // namespace nl::editor
