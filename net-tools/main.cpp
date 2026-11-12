// NET M-NET0：net-tools —— 传输层无头验收工具（A 类轨道 net，PLAN docs/net/NETCODE-PLAN.md）。
// 退出码：0=PASS / 1=错误（建链失败等）/ 2=用法错误或超时（对齐 nav-tools / 展示台 expectExit）。
// 只链 nl_core（nl/net PIMPL 把 ENet 头挡在 core/src/net/Net.cpp），零图形 API。
// 三条命令面：echo（单进程回环，127.0.0.1 双 host 单线程泵，确定性）/
// echo-server + echo-client（双进程，example run.bat 用）/ selftest（机械断言）。

#include "nl/net/Net.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <random>
#include <vector>

#include <chrono>

namespace {

constexpr uint32_t kDefaultPort = 29191;   // 避 editor RPC 29110 / Sunshine 29102
constexpr uint32_t kEchoMagic = 0x4E455430; // 'NET0'

struct EchoArgs {
    uint16_t port = kDefaultPort;
    uint32_t reliable = 32;
    uint32_t unreliable = 64;
    uint32_t payload = 64;
    uint32_t timeoutMs = 10000;
};

bool parseU32(const char* s, uint32_t* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (!end || *end) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

// 从 argv 找 --key <value>；存在但缺值/坏值返回 false。
bool findOptU32(int argc, char** argv, const char* key, uint32_t* out) {
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], key) == 0) {
            if (i + 1 >= argc) return false;
            return parseU32(argv[i + 1], out);
        }
    }
    return true;
}

bool findFlag(int argc, char** argv, const char* key) {
    for (int i = 0; i < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return true;
    return false;
}

struct NetGuard {
    bool ok = false;
    NetGuard() { ok = nl::net::netInit(); }
    ~NetGuard() { if (ok) nl::net::netShutdown(); }
};

double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// 单进程回环 echo：同一进程起 server + client 两个 host，单线程交替泵，
// 全程 127.0.0.1 真实 UDP 栈。可靠通道全量断言；不可靠通道回环无损耗，
// 同样断言全量（带宽无限 + 逐轮泵，ENet 不主动丢弃）。
// 返回 0 = OK，2 = 超时（含丢包）。日志锚行（echo OK / 计数行）不含时变字段。
int echoRun(const EchoArgs& a) {
    NetGuard guard;
    if (!guard.ok) {
        std::fprintf(stderr, "net-tools: enet_initialize failed\n");
        return 1;
    }

    nl::net::NetServer server;
    if (!server.start(a.port, 4)) {
        std::fprintf(stderr, "net-tools: server start failed on port %u\n", a.port);
        return 1;
    }
    nl::net::NetClient client;
    if (!client.beginConnect("127.0.0.1", a.port)) {
        std::fprintf(stderr, "net-tools: beginConnect failed (loopback)\n");
        return 1;
    }

    std::printf("port        : %u\n", a.port);

    uint32_t sentR = 0, recvR = 0, sentU = 0, recvU = 0;
    std::map<uint32_t, double> sendTime;
    std::vector<double> rtts;
    std::vector<uint8_t> buf;
    const double deadline = nowSec() + a.timeoutMs * 0.001;
    bool ok = false;

    while (nowSec() < deadline) {
        // 交替泵双 host：server 不泵就不处理握手/回显（ENet 事件模型）。
        std::vector<nl::net::NetEvent> evs;
        server.service(0, evs);
        for (const auto& e : evs) {
            if (e.type == nl::net::EventType::Data) {
                server.send(e.peerId, e.channel, e.data.data(), e.data.size());
            }
        }
        evs.clear();
        client.service(0, evs);
        for (const auto& e : evs) {
            if (e.type == nl::net::EventType::PeerDisconnected) {
                std::fprintf(stderr, "net-tools: handshake refused (loopback)\n");
                return 1;
            }
            if (e.type != nl::net::EventType::Data || e.data.size() < 6) continue;
            const uint32_t seq = uint32_t(e.data[4]) | (uint32_t(e.data[5]) << 8);
            if (e.channel == nl::net::Channel::Reliable) {
                const auto it = sendTime.find(seq);
                if (it != sendTime.end()) {
                    rtts.push_back((nowSec() - it->second) * 1000.0);
                    sendTime.erase(it);
                }
                ++recvR;
            } else {
                ++recvU;
            }
        }
        if (!client.connected()) continue; // 握手未完成，继续双泵

        if (sentR < a.reliable) {
            const uint32_t seq = sentR++;
            buf.assign({uint8_t(kEchoMagic & 0xFF), uint8_t((kEchoMagic >> 8) & 0xFF),
                        uint8_t((kEchoMagic >> 16) & 0xFF), uint8_t((kEchoMagic >> 24) & 0xFF)});
            buf.push_back(uint8_t(seq & 0xFF));
            buf.push_back(uint8_t((seq >> 8) & 0xFF));
            buf.resize(4 + 2 + a.payload, 0xAB);
            sendTime[seq] = nowSec();
            client.send(nl::net::Channel::Reliable, buf.data(), buf.size());
        }
        if (sentU < a.unreliable) {
            const uint32_t seq = 0x10000u + sentU++;
            buf.assign({uint8_t(kEchoMagic & 0xFF), uint8_t((kEchoMagic >> 8) & 0xFF),
                        uint8_t((kEchoMagic >> 16) & 0xFF), uint8_t((kEchoMagic >> 24) & 0xFF)});
            buf.push_back(uint8_t(seq & 0xFF));
            buf.push_back(uint8_t((seq >> 8) & 0xFF));
            buf.resize(4 + 2 + a.payload, 0xCD);
            client.send(nl::net::Channel::Unreliable, buf.data(), buf.size());
        }

        if (recvR == a.reliable && recvU == a.unreliable) { ok = true; break; }
    }

    std::printf("reliable    : %u/%u\n", recvR, a.reliable);
    std::printf("unreliable  : %u/%u\n", recvU, a.unreliable);
    if (!rtts.empty()) {
        double mn = rtts[0], mx = rtts[0], sum = 0.0;
        for (double v : rtts) { mn = mn < v ? mn : v; mx = mx > v ? mx : v; sum += v; }
        std::printf("rtt ms      : min=%.1f avg=%.1f max=%.1f (n=%zu)\n",
                    mn, sum / rtts.size(), mx, rtts.size());
    }
    if (!ok) {
        std::fprintf(stderr, "echo        : TIMEOUT (reliable %u/%u unreliable %u/%u)\n",
                     recvR, a.reliable, recvU, a.unreliable);
        return 2;
    }
    std::printf("echo        : OK\n");
    return 0;
}

// echo-server：双进程模式的服务端。服务满 packets 个回显后退出（run.bat 用）；
// --linger-ms N：服务满后继续存活 N 毫秒再退出（丢包注入 demo 用——client 模拟
// 入站丢包后，被丢的回显没有重发机会，server 必须撑到 client 先打出统计）。
int cmdEchoServer(int argc, char** argv) {
    if (argc < 1) {
        std::fprintf(stderr, "usage: net-tools echo-server <port> [--packets N] [--max-clients K] [--linger-ms N] [--timeout-ms T]\n");
        return 2;
    }
    uint32_t port = 0, packets = 64, maxClients = 4, timeoutMs = 30000, lingerMs = 0;
    if (!parseU32(argv[0], &port) || port == 0 || port > 65535) {
        std::fprintf(stderr, "echo-server: bad <port>\n");
        return 2;
    }
    if (!findOptU32(argc, argv, "--packets", &packets) ||
        !findOptU32(argc, argv, "--max-clients", &maxClients) ||
        !findOptU32(argc, argv, "--linger-ms", &lingerMs) ||
        !findOptU32(argc, argv, "--timeout-ms", &timeoutMs)) {
        std::fprintf(stderr, "echo-server: bad option value\n");
        return 2;
    }
    NetGuard guard;
    if (!guard.ok) return 1;
    nl::net::NetServer server;
    if (!server.start(static_cast<uint16_t>(port), maxClients)) {
        std::fprintf(stderr, "echo-server : START-FAILED port=%u\n", port);
        return 1;
    }
    const double deadline = nowSec() + timeoutMs * 0.001;
    const double startedAt = nowSec();
    // linger=0：服务满立即退出（>0：服务满后再存活 N 毫秒，供丢包 demo 收尾）
    const double lingerEnd = lingerMs > 0 ? startedAt + lingerMs * 0.001 : startedAt;
    uint32_t served = 0;
    std::vector<nl::net::NetEvent> evs;
    while (nowSec() < deadline) {
        evs.clear();
        server.service(50, evs);
        for (const auto& e : evs) {
            if (e.type == nl::net::EventType::Data) {
                server.send(e.peerId, e.channel, e.data.data(), e.data.size());
                ++served;
            }
        }
        if (served >= packets && nowSec() >= lingerEnd) break;
    }
    std::printf("echo-server : served=%u port=%u\n", served, port);
    return served == packets ? 0 : 2;
}

// echo-client：双进程模式的客户端。--loss 模拟入站丢包（确定性伪随机）。
int cmdEchoClient(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: net-tools echo-client <host> <port> [--packets N] [--rate R] [--loss P] [--payload B] [--timeout-ms T]\n");
        return 2;
    }
    const char* host = argv[0];
    uint32_t port = 0, packets = 32, rate = 200, loss = 0, payload = 64, timeoutMs = 10000;
    if (!parseU32(argv[1], &port) || port == 0 || port > 65535) {
        std::fprintf(stderr, "echo-client: bad <port>\n");
        return 2;
    }
    if (!findOptU32(argc, argv, "--packets", &packets) ||
        !findOptU32(argc, argv, "--rate", &rate) ||
        !findOptU32(argc, argv, "--loss", &loss) ||
        !findOptU32(argc, argv, "--payload", &payload) ||
        !findOptU32(argc, argv, "--timeout-ms", &timeoutMs) ||
        loss > 100) {
        std::fprintf(stderr, "echo-client: bad option value\n");
        return 2;
    }
    NetGuard guard;
    if (!guard.ok) return 1;
    nl::net::NetClient client;
    if (!client.connect(host, static_cast<uint16_t>(port), 3000)) {
        std::printf("connect     : FAILED host=%s port=%u\n", host, port);
        return 1;
    }

    std::mt19937 rng(1234); // 固定种子：--loss 序列确定性
    std::uniform_int_distribution<int> drop(0, 99);
    std::vector<uint8_t> buf;
    const double minInterval = rate > 0 ? 1.0 / rate : 0.0;
    const double deadline = nowSec() + timeoutMs * 0.001;
    double lastSend = -1e9;
    uint32_t sent = 0, recv = 0;
    std::map<uint32_t, double> sendTime;
    std::vector<double> rtts;
    std::vector<nl::net::NetEvent> evs;

    while (nowSec() < deadline) {
        const double t = nowSec();
        if (sent < packets && t - lastSend >= minInterval) {
            const uint32_t seq = sent++;
            buf.assign({uint8_t(kEchoMagic & 0xFF), uint8_t((kEchoMagic >> 8) & 0xFF),
                        uint8_t((kEchoMagic >> 16) & 0xFF), uint8_t((kEchoMagic >> 24) & 0xFF)});
            buf.push_back(uint8_t(seq & 0xFF));
            buf.push_back(uint8_t((seq >> 8) & 0xFF));
            buf.resize(6 + payload, 0xAB);
            sendTime[seq] = t;
            client.send(nl::net::Channel::Reliable, buf.data(), buf.size());
            lastSend = t;
        }
        evs.clear();
        client.service(2, evs);
        for (const auto& e : evs) {
            if (e.type == nl::net::EventType::PeerDisconnected) {
                std::fprintf(stderr, "echo-client : DISCONNECTED\n");
                return 1;
            }
            if (e.type != nl::net::EventType::Data || e.data.size() < 6) continue;
            if (loss > 0 && drop(rng) < static_cast<int>(loss)) continue; // 模拟入站丢包
            const uint32_t seq = uint32_t(e.data[4]) | (uint32_t(e.data[5]) << 8);
            const auto it = sendTime.find(seq);
            if (it != sendTime.end()) {
                rtts.push_back((nowSec() - it->second) * 1000.0);
                sendTime.erase(it);
            }
            ++recv;
        }
        if (recv == packets) break;
    }

    const double lossPct = sent ? 100.0 * (sent - recv) / sent : 0.0;
    double avg = 0.0;
    if (!rtts.empty()) { for (double v : rtts) avg += v; avg /= rtts.size(); }
    std::printf("echo-client : sent=%u recv=%u loss=%.1f%% rttAvg=%.1fms n=%zu\n",
                sent, recv, lossPct, avg, rtts.size());
    return recv == sent ? 0 : 2;
}

int cmdSelftest() {
    EchoArgs a;
    a.reliable = 16;
    a.unreliable = 32;
    a.payload = 32;
    a.timeoutMs = 10000;
    // echoRun 自带打印；PASS 判定 = 返回 0（可靠+不可靠全量回显）。
    const int rc = echoRun(a);
    if (rc == 0) {
        std::printf("selftest net: PASS\n");
        return 0;
    }
    std::fprintf(stderr, "selftest net: FAIL (echoRun rc=%d)\n", rc);
    return 1;
}

void printUsage() {
    std::printf(
        "net-tools - nl-engine transport acceptance tool (NET M-NET0, vendored ENet %s)\n"
        "usage:\n"
        "  net-tools echo [--port P] [--reliable N] [--unreliable N] [--payload B] [--timeout-ms T]\n"
        "                        single-process loopback echo (127.0.0.1, one-thread pump); exit 0=ok 2=timeout\n"
        "  net-tools echo-server <port> [--packets N] [--max-clients K] [--timeout-ms T]\n"
        "                        run echo server until N packets served; exit 0/1/2\n"
        "  net-tools echo-client <host> <port> [--packets N] [--rate R] [--loss P] [--payload B] [--timeout-ms T]\n"
        "                        run echo client; exit 0=all echoed 1=connect fail 2=loss/timeout\n"
        "  net-tools selftest    deterministic loopback assertions; exit 0=PASS 1=FAIL\n"
        "  net-tools version     print vendored ENet version\n",
        nl::net::version());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { printUsage(); return 2; }
    const char* cmd = argv[1];
    if (std::strcmp(cmd, "echo") == 0) {
        EchoArgs a;
        uint32_t port = a.port, rel = a.reliable, unr = a.unreliable, pay = a.payload, tmo = a.timeoutMs;
        if (!findOptU32(argc - 2, argv + 2, "--port", &port) || port == 0 || port > 65535 ||
            !findOptU32(argc - 2, argv + 2, "--reliable", &rel) ||
            !findOptU32(argc - 2, argv + 2, "--unreliable", &unr) ||
            !findOptU32(argc - 2, argv + 2, "--payload", &pay) ||
            !findOptU32(argc - 2, argv + 2, "--timeout-ms", &tmo)) {
            std::fprintf(stderr, "echo: bad option value\n");
            return 2;
        }
        a.port = static_cast<uint16_t>(port);
        a.reliable = rel; a.unreliable = unr; a.payload = pay; a.timeoutMs = tmo;
        return echoRun(a);
    }
    if (std::strcmp(cmd, "echo-server") == 0) return cmdEchoServer(argc - 2, argv + 2);
    if (std::strcmp(cmd, "echo-client") == 0) return cmdEchoClient(argc - 2, argv + 2);
    if (std::strcmp(cmd, "selftest") == 0) return cmdSelftest();
    if (std::strcmp(cmd, "version") == 0) {
        std::printf("enet        : %s\n", nl::net::version());
        return 0;
    }
    printUsage();
    return 2;
}
