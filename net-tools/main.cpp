// NET M-NET0：net-tools —— 传输层无头验收工具（A 类轨道 net，PLAN docs/net/NETCODE-PLAN.md）。
// 退出码：0=PASS / 1=错误（建链失败等）/ 2=用法错误或超时（对齐 nav-tools / 展示台 expectExit）。
// 只链 nl_core（nl/net PIMPL 把 ENet 头挡在 core/src/net/Net.cpp），零图形 API。
// 三条命令面：echo（单进程回环，127.0.0.1 双 host 单线程泵，确定性）/
// echo-server + echo-client（双进程，example run.bat 用）/ selftest（机械断言）。

#include "nl/net/Net.hpp"
#include "nl/net/Snapshot.hpp"
#include "nl/net/Sync.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

// loss-sweep：单进程回环逐级丢包实测（0..90% 步进 10，种子 1234 确定性）。
// 每级 fresh server+client，可靠通道 32 包全量重传必达，client 侧模拟丢弃后
// recv = 32 - 模拟丢包数。逐行打印真实测量值，供证据图渲染（纯工具侧，零游戏语义）。
int cmdLossSweep(int argc, char** argv) {
    uint32_t port = 29194, packets = 32, payload = 64, timeoutMs = 5000;
    if (!findOptU32(argc, argv, "--port", &port) || port == 0 || port > 65535 ||
        !findOptU32(argc, argv, "--packets", &packets) ||
        !findOptU32(argc, argv, "--payload", &payload) ||
        !findOptU32(argc, argv, "--timeout-ms", &timeoutMs)) {
        std::fprintf(stderr, "loss-sweep: bad option value\n");
        return 2;
    }
    NetGuard guard;
    if (!guard.ok) return 1;
    static const int kLevels[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90};
    std::vector<uint8_t> buf;
    bool ok = true;
    for (int loss : kLevels) {
        nl::net::NetServer server;
        if (!server.start(static_cast<uint16_t>(port), 4)) {
            std::fprintf(stderr, "loss-sweep: server start failed port=%u\n", port);
            return 1;
        }
        nl::net::NetClient client;
        if (!client.beginConnect("127.0.0.1", static_cast<uint16_t>(port))) {
            std::fprintf(stderr, "loss-sweep: beginConnect failed\n");
            return 1;
        }
        std::mt19937 rng(1234); // 固定种子：序列确定性
        std::uniform_int_distribution<int> drop(0, 99);
        std::map<uint32_t, bool> seen; // seq -> 到达（丢弃模拟前）
        const double deadline = nowSec() + timeoutMs * 0.001;
        uint32_t sent = 0, recv = 0;
        std::vector<nl::net::NetEvent> evs;
        while (nowSec() < deadline && seen.size() < packets) {
            std::vector<nl::net::NetEvent> sevs;
            server.service(0, sevs);
            for (const auto& e : sevs) {
                if (e.type == nl::net::EventType::Data) {
                    server.send(e.peerId, e.channel, e.data.data(), e.data.size());
                }
            }
            evs.clear();
            client.service(0, evs);
            for (const auto& e : evs) {
                if (e.type == nl::net::EventType::PeerDisconnected) {
                    std::fprintf(stderr, "loss-sweep: disconnected at loss=%d%%\n", loss);
                    return 1;
                }
                if (e.type != nl::net::EventType::Data || e.data.size() < 6) continue;
                const uint32_t seq = uint32_t(e.data[4]) | (uint32_t(e.data[5]) << 8);
                if (seen.count(seq)) continue;      // 可靠重传的同一包只算一次
                seen[seq] = true;
                if (drop(rng) >= loss) ++recv;      // 模拟入站丢包
            }
            if (!client.connected()) continue;      // 握手未完成，继续双泵
            if (seen.size() < packets && sent < packets) {
                const uint32_t seq = sent++;
                buf.assign({uint8_t(kEchoMagic & 0xFF), uint8_t((kEchoMagic >> 8) & 0xFF),
                            uint8_t((kEchoMagic >> 16) & 0xFF), uint8_t((kEchoMagic >> 24) & 0xFF)});
                buf.push_back(uint8_t(seq & 0xFF));
                buf.push_back(uint8_t((seq >> 8) & 0xFF));
                buf.resize(6 + payload, 0xAB);
                client.send(nl::net::Channel::Reliable, buf.data(), buf.size());
            }
        }
        std::printf("loss %2d%%   : recv %u/%u\n", loss, recv, packets);
        if (seen.size() < packets) ok = false;
        client.disconnect();
        server.stop();
    }
    if (!ok) {
        std::fprintf(stderr, "sweep       : TIMEOUT\n");
        return 2;
    }
    std::printf("sweep       : OK\n");
    return 0;
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

// ---- M-NET1 快照层（nl/net/Snapshot.hpp，bitsery 位打包）----
using nl::net::VehicleSnapshot;
using nl::net::kSnapshotBits;
using nl::net::kSnapshotMaxBytes;
using nl::net::encodeSnapshot;
using nl::net::decodeSnapshot;

// 确定性 LCG（与 loss 注入同源约定：种子 1234，无 <random> 平台漂移）。
struct Lcg {
    uint64_t s = 1234;
    uint32_t next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(s >> 32);
    }
    float nextF(float lo, float hi) {
        return lo + (hi - lo) * (static_cast<float>(next() >> 8) / 16777215.0f);
    }
};

void normalizeQuat(float q[4]) {
    const float n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (n > 1e-8f) {
        for (int i = 0; i < 4; ++i) q[i] /= n;
        return;
    }
    q[0] = q[1] = q[2] = 0.f; q[3] = 1.f;
}

// 四元数夹角（度）：|qa·conj(qb)| 的 w 分量 = |dot|，角 = 2*acos(|dot|)。
float quatAngleDeg(const float a[4], const float b[4]) {
    const float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    const float c = std::clamp(std::fabs(dot), 0.0f, 1.0f);
    return std::acos(c) * 2.0f * 57.29577951308232f;
}

VehicleSnapshot makeSample(Lcg& rng, uint32_t i) {
    VehicleSnapshot s;
    s.serverTick = i * 7 + 1;
    s.entityId = static_cast<uint16_t>(rng.next() & 0xFFFF);
    s.px = rng.nextF(-2000.f, 2000.f);
    s.py = rng.nextF(-50.f, 200.f);
    s.pz = rng.nextF(-2000.f, 2000.f);
    float q[4] = { rng.nextF(-1.f, 1.f), rng.nextF(-1.f, 1.f),
                   rng.nextF(-1.f, 1.f), rng.nextF(-1.f, 1.f) };
    normalizeQuat(q);
    s.qx = q[0]; s.qy = q[1]; s.qz = q[2]; s.qw = q[3];
    s.vx = rng.nextF(-60.f, 60.f);
    s.vy = rng.nextF(-10.f, 10.f);
    s.vz = rng.nextF(-60.f, 60.f);
    return s;
}

struct QuantErr {
    float maxPosM = 0.f;    // 位置最大误差（米）
    float maxQuatDeg = 0.f; // 朝向最大角误差（度）
    float maxVelMs = 0.f;   // 速度最大误差（米/秒）
};

void accumulateErr(const VehicleSnapshot& in, const VehicleSnapshot& out, QuantErr& e) {
    const float dp = std::max({std::fabs(in.px - out.px), std::fabs(in.py - out.py),
                               std::fabs(in.pz - out.pz)});
    if (dp > e.maxPosM) e.maxPosM = dp;
    const float a[4] = { in.qx, in.qy, in.qz, in.qw };
    const float b[4] = { out.qx, out.qy, out.qz, out.qw };
    const float dq = quatAngleDeg(a, b);
    if (dq > e.maxQuatDeg) e.maxQuatDeg = dq;
    const float dv = std::max({std::fabs(in.vx - out.vx), std::fabs(in.vy - out.vy),
                               std::fabs(in.vz - out.vz)});
    if (dv > e.maxVelMs) e.maxVelMs = dv;
}

// snapshot-selftest：往返 bytes 级一致 + 量化误差 ≤ 声明精度 + 截断/脏残位负向。
// PASS = 全部机械断言过（ctest net-m1-selftest 锚 "snapshot selftest: PASS"）。
int cmdSnapshotSelftest() {
    Lcg rng;
    constexpr uint32_t kCount = 256;
    // 断言阈值 = 声明精度（Snapshot.hpp 表），不是半步：
    // pos 1 mm = 0.001 m（实测 ~0.0006 = 半步 0.0005 + float 存储噪声）
    // quat 0.1 度（线格式分量量化贡献仅 ~0.002 度 = 2·√3·(1/√2)/65535；
    //   端到端 ~0.07 度由输入 float 归一化范数亏差 ~2e-7 经 2·acos 的 sqrt
    //   放大造成，与 UE FQuat 网络压缩同级——实测探针逐分量核对过）
    // vel 1 cm/s = 0.01 m/s（实测 ~0.005 = 半步 + float 存储噪声）
    constexpr float kPosTol = 0.001f;
    constexpr float kQuatTolDeg = 0.1f;
    constexpr float kVelTol = 0.01f;

    std::vector<uint8_t> bytes(kSnapshotMaxBytes), bytes2(kSnapshotMaxBytes);
    QuantErr e{};
    for (uint32_t i = 0; i < kCount; ++i) {
        VehicleSnapshot s = makeSample(rng, i);
        const size_t n = encodeSnapshot(s, bytes.data(), bytes.size());
        if (n != 31) {
            std::fprintf(stderr, "snapshot selftest: FAIL encode size=%zu (expect 31) at i=%u\n", n, i);
            return 1;
        }
        VehicleSnapshot r;
        if (!decodeSnapshot(bytes.data(), n, r)) {
            std::fprintf(stderr, "snapshot selftest: FAIL decode at i=%u\n", i);
            return 1;
        }
        accumulateErr(s, r, e);
        // bytes 级往返：对解码结果再编码，必须逐字节一致（量化值往返精确）。
        const size_t n2 = encodeSnapshot(r, bytes2.data(), bytes2.size());
        if (n2 != n || std::memcmp(bytes.data(), bytes2.data(), n) != 0) {
            std::fprintf(stderr, "snapshot selftest: FAIL roundtrip bytes mismatch at i=%u\n", i);
            return 1;
        }
    }
    // 边界样本：恒等四元数 / 负 largest / 饱和速度 / 巨大位置。
    {
        VehicleSnapshot s;
        const float qs[4] = { 0.f, 0.f, 0.f, 1.f };
        s.qx = qs[0]; s.qy = qs[1]; s.qz = qs[2]; s.qw = qs[3];
        s.px = 2147483.0f; s.vz = 327.0f;
        const size_t n = encodeSnapshot(s, bytes.data(), bytes.size());
        VehicleSnapshot r;
        if (n != 31 || !decodeSnapshot(bytes.data(), n, r)) {
            std::fprintf(stderr, "snapshot selftest: FAIL identity edge case\n");
            return 1;
        }
        accumulateErr(s, r, e);
    }
    // 精度判定（数值断言，验收判据 2）。
    if (e.maxPosM > kPosTol || e.maxQuatDeg > kQuatTolDeg || e.maxVelMs > kVelTol) {
        std::fprintf(stderr, "snapshot selftest: FAIL quant err pos=%.6fm quat=%.6fdeg vel=%.6fms\n",
                     e.maxPosM, e.maxQuatDeg, e.maxVelMs);
        return 1;
    }
    // 负向 1：截断流必须失败。
    {
        VehicleSnapshot s = makeSample(rng, 999);
        std::vector<uint8_t> buf;
        encodeSnapshot(s, buf);
        VehicleSnapshot r;
        if (decodeSnapshot(buf.data(), buf.size() - 1, r)) {
            std::fprintf(stderr, "snapshot selftest: FAIL truncated stream accepted\n");
            return 1;
        }
        // 负向 2：尾部多 1 字节垃圾必须失败（currentReadPos != len）。
        std::vector<uint8_t> ext(buf);
        ext.push_back(0x7F);
        if (decodeSnapshot(ext.data(), ext.size(), r)) {
            std::fprintf(stderr, "snapshot selftest: FAIL trailing byte accepted\n");
            return 1;
        }
        // 负向 3：对齐残位藏脏数据必须失败（243 bits → 末字节 3 bits 用 + 5 pad）。
        std::vector<uint8_t> dirty(buf);
        dirty.back() |= 0xF8;
        if (decodeSnapshot(dirty.data(), dirty.size(), r)) {
            std::fprintf(stderr, "snapshot selftest: FAIL dirty pad bits accepted\n");
            return 1;
        }
    }
    std::printf("snapshot selftest: PASS (%u snapshots, 31 bytes, pos<=%.6fm quat<=%.6fdeg vel<=%.6fms)\n",
                kCount, e.maxPosM, e.maxQuatDeg, e.maxVelMs);
    return 0;
}

// snapshot-bench：确定性样本载荷实测（PLAN 验收判据 3：实测表，README 基准不作数）。
int cmdSnapshotBench(int argc, char** argv) {
    uint32_t count = 20000;
    if (!findOptU32(argc, argv, "--count", &count) || count == 0 || count > 1000000) {
        std::fprintf(stderr, "snapshot-bench: bad --count\n");
        return 2;
    }
    Lcg rng;
    std::vector<uint8_t> buf(kSnapshotMaxBytes);
    QuantErr e{};
    uint64_t totalBytes = 0;
    using Clock = std::chrono::steady_clock;
    double encNs = 0.0, decNs = 0.0;
    for (uint32_t i = 0; i < count; ++i) {
        VehicleSnapshot s = makeSample(rng, i);
        const auto t0 = Clock::now();
        const size_t n = encodeSnapshot(s, buf.data(), buf.size());
        const auto t1 = Clock::now();
        VehicleSnapshot r;
        const bool ok = decodeSnapshot(buf.data(), n, r);
        const auto t2 = Clock::now();
        if (n != 31 || !ok) {
            std::fprintf(stderr, "snapshot-bench: FAIL at i=%u (n=%zu ok=%d)\n", i, n, ok ? 1 : 0);
            return 1;
        }
        encNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        decNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        totalBytes += n;
        accumulateErr(s, r, e);
    }
    std::printf("snapshot-bench : count=%u avgBytes=%.2f wireBits=%u\n",
                count, static_cast<double>(totalBytes) / count, kSnapshotBits);
    std::printf("snapshot-bench : encode=%.0f ns/op decode=%.0f ns/op\n",
                encNs / count, decNs / count);
    std::printf("snapshot-bench : maxPosErr=%.6f m maxQuatErr=%.6f deg maxVelErr=%.6f m/s\n",
                e.maxPosM, e.maxQuatDeg, e.maxVelMs);
    std::printf("field-bits     : tick=32 id=16 pos=96 quat=51 vel=48\n");
    // naive 对照 = 同字段的 float 全量打包：tick4+id2+pos12+quat16+vel12=46 B。
    constexpr double kNaive = 46.0;
    std::printf("baseline       : naive=%.0f B encoded=31 B saving=%.1f%%\n",
                kNaive, (1.0 - 31.0 / kNaive) * 100.0);
    std::printf("snapshot-bench : OK\n");
    return 0;
}

// snapshot-decode <hex>：hex 字节流解码（负向工具卡：短流/脏流显式 FAILED）。
int cmdSnapshotDecode(int argc, char** argv) {
    if (argc < 1 || !argv[0] || !*argv[0]) {
        std::fprintf(stderr, "usage: net-tools snapshot-decode <hex-bytes>\n");
        return 2;
    }
    std::vector<uint8_t> buf;
    const char* hex = argv[0];
    for (const char* p = hex; p[0] && p[1]; p += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nib(p[0]), lo = nib(p[1]);
        if (hi < 0 || lo < 0) {
            std::fprintf(stderr, "snapshot-decode : FAILED (bad hex)\n");
            return 1;
        }
        buf.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    VehicleSnapshot r;
    if (!decodeSnapshot(buf.data(), buf.size(), r)) {
        std::printf("decode       : FAILED (%zu bytes)\n", buf.size());
        return 1;
    }
    std::printf("decode       : OK tick=%u id=%u pos=(%.3f,%.3f,%.3f)\n",
                r.serverTick, r.entityId, r.px, r.py, r.pz);
    return 0;
}

// ---- M-NET2 同步层验收（nl/net/Sync.hpp）----

// 确定性假载具轨迹（解析函数，auth(t) 任意 tick 可复算）：
//   pos = (0.15*t, 0.5, 0.10*t)，yaw = 0.02*t（绕 Y 四元数），vel = 解析导数。
void fakeVehicleState(uint32_t tick, float posX, float posZOff,
                      float* pos, float* quat, float* vel) {
    const float t = static_cast<float>(tick);
    pos[0] = 0.15f * t + posX;
    pos[1] = 0.5f;
    pos[2] = 0.10f * t + posZOff;
    const float yaw = 0.02f * t;
    quat[0] = 0.0f;
    quat[1] = std::sin(yaw * 0.5f);
    quat[2] = 0.0f;
    quat[3] = std::cos(yaw * 0.5f);
    vel[0] = 0.15f;
    vel[1] = 0.0f;
    vel[2] = 0.10f;
}

VehicleSnapshot fakeSnapshot(uint32_t tick, uint16_t ent, float posX, float posZOff) {
    VehicleSnapshot s;
    s.serverTick = tick;
    s.entityId = ent;
    float pos[3], q[4], vel[3];
    fakeVehicleState(tick, posX, posZOff, pos, q, vel);
    s.px = pos[0]; s.py = pos[1]; s.pz = pos[2];
    s.qx = q[0]; s.qy = q[1]; s.qz = q[2]; s.qw = q[3];
    s.vx = vel[0]; s.vy = vel[1]; s.vz = vel[2];
    return s;
}

// session-loopback：单进程 server+client 会话全链路（握手 -> 输入上送 -> 快照 ->
// 插值回看误差断言 -> 断链回收）。泵序每迭代固定：client 泵 -> client 发输入 ->
// server 泵（加入/输入生效）-> server 采样广播 -> client 泵收快照 -> client 回看。
// 回环 UDP 无丢包、服务序确定 -> 数值断言确定性。
int cmdSessionLoopback(int argc, char** argv) {
    uint32_t port = 29197, ticks = 300, timeoutMs = 10000;
    if (!findOptU32(argc, argv, "--port", &port) || port == 0 || port > 65535 ||
        !findOptU32(argc, argv, "--ticks", &ticks) || ticks < 60 ||
        !findOptU32(argc, argv, "--timeout-ms", &timeoutMs)) {
        std::fprintf(stderr, "session-loopback: bad option value\n");
        return 2;
    }
    NetGuard guard;
    if (!guard.ok) return 1;

    nl::net::ServerSession server;
    if (!server.start(static_cast<uint16_t>(port), 4)) {
        std::fprintf(stderr, "session-loopback: server start failed port=%u\n", port);
        return 1;
    }
    const uint16_t localEnt = server.claimLocalEntity();
    nl::net::ClientSession client;
    if (!client.beginConnect("127.0.0.1", static_cast<uint16_t>(port))) {
        std::fprintf(stderr, "session-loopback: beginConnect failed\n");
        return 1;
    }

    constexpr float kDelayTicks = 2.0f;
    const float kQuatSlack = 0.005f;   // 线格式量化 1mm/轴 + 日志舍入余量
    // server 权威轨迹留档（clientEnt 的输入是缓存值，不解析复算，直接留档）
    std::map<uint32_t, std::map<uint16_t, std::array<float, 3>>> authLog;
    bool joinSeen = false, leaveSeen = false;
    uint16_t clientEnt = 0;
    bool inputOk = false;
    size_t samples = 0;
    float maxErr = 0.f, maxBound = 0.f;
    bool interpOk = true;
    const double deadline = nowSec() + timeoutMs * 0.001;

    for (uint32_t t = 1; t <= ticks && nowSec() < deadline; ++t) {
        std::vector<uint16_t> joined, left;
        client.pump();
        // 输入上送：确定性脚本（forward 0..1 三角波，right 常值转向）
        nl::net::InputState in;
        in.forward = 0.5f + 0.5f * std::sin(static_cast<float>(t) * 0.05f);
        in.right = 0.25f;
        client.sendInput(in);
        server.pump(t, joined, left);
        for (const uint16_t ent : joined) {
            clientEnt = ent;
            joinSeen = true;
        }
        for (const uint16_t ent : left) {
            if (ent == clientEnt) leaveSeen = true;
        }
        // 输入上送核对（不锁存：welcome 前丢弃属预期，welcome 后下一迭代必达）
        if (!inputOk && clientEnt != 0) {
            const nl::net::InputState si = server.inputOf(clientEnt);
            inputOk = std::fabs(si.forward - in.forward) < 1e-4f &&
                      std::fabs(si.right - in.right) < 1e-4f;
        }
        // server 逻辑帧：采样广播（本地权威车 + client 车，x 错开避免同点碰撞）
        {
            const nl::net::VehicleSnapshot s1 = fakeSnapshot(t, localEnt, 0.0f, 0.0f);
            server.broadcastSnapshot(s1);
            authLog[t][localEnt] = {s1.px, s1.py, s1.pz};
            if (clientEnt != 0) {
                const nl::net::VehicleSnapshot s2 = fakeSnapshot(t, clientEnt, 2.0f, 0.0f);
                server.broadcastSnapshot(s2);
                authLog[t][clientEnt] = {s2.px, s2.py, s2.pz};
            }
        }
        client.pump();
        // client 插值回看 + 逐样本误差断言（界 = 相邻 auth 位移 + 量化余量）
        for (uint16_t ent = 1; ent <= 16 && interpOk; ++ent) {
            const nl::net::InterpBuffer* buf = client.bufferOf(ent);
            if (!buf) continue;
            nl::net::VehicleSnapshot out;
            float evalTick = 0.f;
            if (!buf->eval(kDelayTicks, out, evalTick)) continue;
            const uint32_t t0 = static_cast<uint32_t>(std::floor(evalTick));
            const auto itA = authLog.find(t0);
            const auto itB = authLog.find(t0 + 1);
            if (itA == authLog.end() || itB == authLog.end() ||
                itA->second.find(ent) == itA->second.end() ||
                itB->second.find(ent) == itB->second.end()) {
                std::fprintf(stderr, "session-loopback: auth missing tick=%u ent=%u\n", t0, ent);
                interpOk = false;
                break;
            }
            const auto& a = itA->second.at(ent);
            const auto& b = itB->second.at(ent);
            const float dx = out.px - a[0], dy = out.py - a[1], dz = out.pz - a[2];
            const float err = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float gx = b[0] - a[0], gy = b[1] - a[1], gz = b[2] - a[2];
            const float bound = std::sqrt(gx * gx + gy * gy + gz * gz) + kQuatSlack;
            if (err > maxErr) maxErr = err;
            if (bound > maxBound) maxBound = bound;
            if (err > bound) {
                std::fprintf(stderr,
                             "session-loopback: interp error tick=%u ent=%u err=%.4f > bound=%.4f\n",
                             t0, ent, err, bound);
                interpOk = false;
                break;
            }
            ++samples;
        }
    }
    if (clientEnt != 0) {
        // 断链回收：disconnect 直拆 -> server pump 报 left；client 会话态复位。
        client.disconnect();
        std::vector<uint16_t> joined, left;
        server.pump(ticks + 1, joined, left);
        for (const uint16_t ent : left) {
            if (ent == clientEnt) leaveSeen = true;
        }
    }
    const bool welcomedOk = client.localEntityId() == 0;   // disconnect 后归零
    // M-NET3 异常路径：server 优雅关服（stop 内部逐 peer disconnect + flush）->
    // client pump 立即收断链事件 -> 会话态复位（welcomed=false / 实体桶清空），
    // 对端消失不再依赖默认超时（~30s）。独立新会话套，不碰上面 M-NET2 断言。
    bool passiveCloseOk = false;
    {
        const uint16_t p2 = static_cast<uint16_t>(port < 65535 ? port + 1 : port - 1);
        nl::net::ServerSession srv2;
        if (srv2.start(p2, 4)) {
            const uint16_t ent2 = srv2.claimLocalEntity();
            nl::net::ClientSession cli2;
            if (cli2.beginConnect("127.0.0.1", p2)) {
                const double dl2 = nowSec() + 3.0;
                while (nowSec() < dl2 && !cli2.welcomed()) {
                    cli2.pump();
                    std::vector<uint16_t> j2, l2;
                    srv2.pump(1, j2, l2);
                    srv2.broadcastSnapshot(fakeSnapshot(1, ent2, 0.0f, 0.0f));
                    cli2.pump();
                }
                if (cli2.welcomed()) {
                    srv2.stop();   // 优雅关服（M-NET3）：对端立即感知，不等超时
                    const double dl3 = nowSec() + 3.0;
                    while (nowSec() < dl3 && cli2.connected()) cli2.pump();
                    passiveCloseOk = !cli2.welcomed() && !cli2.connected() &&
                                     cli2.entityCount() == 0 && cli2.localEntityId() == 0;
                }
            }
        }
    }
    const bool ok = joinSeen && clientEnt == localEnt + 1 && inputOk &&
                    interpOk && samples >= ticks / 4 && leaveSeen && welcomedOk &&
                    passiveCloseOk;
    std::printf("session      : joined=%u welcomed entityId=%u input=%s\n",
                joinSeen ? 1u : 0u, clientEnt, inputOk ? "ok" : "BAD");
    std::printf("session      : interp samples=%zu maxErr=%.4f maxBound=%.4f\n",
                samples, maxErr, maxBound);
    std::printf("session      : leave=%d reconnectState=%s\n",
                leaveSeen ? 1 : 0, welcomedOk ? "clean" : "STALE");
    std::printf("session      : passiveClose=%s\n", passiveCloseOk ? "clean" : "STALE");
    if (ok) {
        std::printf("session-loopback : PASS\n");
        return 0;
    }
    std::fprintf(stderr, "session-loopback : FAIL\n");
    return 1;
}

// ---- verify-m2：client interp 轨迹 vs server auth 轨迹（数值锚） ----
// 界的推导：client 插值在 [t0, t0+1] 段 lerp，|interp(t) - auth(t0)| <=
// |auth(t0+1) - auth(t0)|（段内路径长度界）+ 线格式量化余量 0.005。
// 阈值从 auth 日志逐样本自标定，不依赖场景速度假设。

bool parseNetPoseLine(const std::string& line, const char* tag,
                      uint32_t* tick, uint16_t* ent, float p[3]) {
    const std::size_t at = line.find(tag);
    if (at == std::string::npos) return false;
    // "auth tick=12 ent=1 px=0.1500 py=0.5000 pz=0.1000"
    return std::sscanf(line.c_str() + at, "%*[^=]=%u %*[^=]=%hu %*[^=]=%f %*[^=]=%f %*[^=]=%f",
                       tick, ent, &p[0], &p[1], &p[2]) == 5;
}

struct VerifyStats {
    size_t matched = 0;
    size_t missing = 0;
    size_t violations = 0;
    float maxErr = 0.f;
    float maxRatio = 0.f;   // err / bound 最大值（>1 即违约）
};

// 返回 0 = 全部样本在界内且证据量充足；1 = 违约 / 证据不足。
int runNetVerify(const std::vector<std::string>& authLines,
                 const std::vector<std::string>& interpLines,
                 size_t minSamples, VerifyStats& st) {
    std::map<uint16_t, std::map<uint32_t, std::array<float, 3>>> auth;
    for (const std::string& l : authLines) {
        uint32_t tick = 0; uint16_t ent = 0; float p[3] = {};
        if (parseNetPoseLine(l, "auth tick=", &tick, &ent, p)) auth[ent][tick] = {p[0], p[1], p[2]};
    }
    for (const std::string& l : interpLines) {
        uint32_t tick = 0; uint16_t ent = 0; float p[3] = {};
        if (!parseNetPoseLine(l, "interp tick=", &tick, &ent, p)) continue;
        const auto itE = auth.find(ent);
        if (itE == auth.end()) { ++st.missing; ++st.violations; continue; }
        const auto itA = itE->second.find(tick);
        const auto itB = itE->second.find(tick + 1);
        if (itA == itE->second.end() || itB == itE->second.end()) { ++st.missing; ++st.violations; continue; }
        const float dx = p[0] - itA->second[0];
        const float dy = p[1] - itA->second[1];
        const float dz = p[2] - itA->second[2];
        const float err = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float gx = itB->second[0] - itA->second[0];
        const float gy = itB->second[1] - itA->second[1];
        const float gz = itB->second[2] - itA->second[2];
        const float span = std::sqrt(gx * gx + gy * gy + gz * gz) + 0.005f;
        if (err > st.maxErr) st.maxErr = err;
        const float ratio = span > 0.f ? err / span : 0.f;
        if (ratio > st.maxRatio) st.maxRatio = ratio;
        if (err > span) ++st.violations;
        ++st.matched;
    }
    if (st.matched < minSamples) ++st.violations;
    return st.violations == 0 ? 0 : 1;
}

int cmdNetVerifyM2(int argc, char** argv) {
    bool selftest = false;
    const char* authPath = nullptr;
    const char* interpPath = nullptr;
    uint32_t minSamples = 100;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") selftest = true;
        else if (a == "--auth" && i + 1 < argc) authPath = argv[++i];
        else if (a == "--interp" && i + 1 < argc) interpPath = argv[++i];
        else if (a == "--min-samples" && i + 1 < argc) {
            if (!parseU32(argv[++i], &minSamples)) {
                std::fprintf(stderr, "verify-m2: bad --min-samples\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "verify-m2: unknown option '%s'\n", a.c_str());
            return 2;
        }
    }
    if (selftest) {
        // 合成日志：auth = 解析轨迹；interp = 段内精确 lerp（τ=t+0.3）→ 必 PASS；
        // 再注入一条 +0.5m 偏移样本 → 必须 FAIL（验证检查器灵敏度）。
        std::vector<std::string> authLines, interpLines;
        char buf[192];
        for (uint32_t t = 0; t <= 120; ++t) {
            const nl::net::VehicleSnapshot s = fakeSnapshot(t, 1, 0.0f, 0.0f);
            std::snprintf(buf, sizeof(buf),
                          "[nl:info|net] auth tick=%u ent=1 px=%.4f py=%.4f pz=%.4f #1",
                          t, s.px, s.py, s.pz);
            authLines.push_back(buf);
        }
        for (uint32_t t = 0; t < 118; ++t) {
            const nl::net::VehicleSnapshot a = fakeSnapshot(t, 1, 0.0f, 0.0f);
            const nl::net::VehicleSnapshot b = fakeSnapshot(t + 1, 1, 0.0f, 0.0f);
            const float tau = 0.3f;
            std::snprintf(buf, sizeof(buf),
                          "[nl:info|net] interp tick=%u ent=1 px=%.4f py=%.4f pz=%.4f #1",
                          t,
                          a.px + (b.px - a.px) * tau,
                          a.py + (b.py - a.py) * tau,
                          a.pz + (b.pz - a.pz) * tau);
            interpLines.push_back(buf);
        }
        VerifyStats good;
        if (runNetVerify(authLines, interpLines, 100, good) != 0) {
            std::fprintf(stderr, "verify-m2 selftest: exact-lerp case unexpectedly FAIL "
                                 "(matched=%zu viol=%zu)\n", good.matched, good.violations);
            return 1;
        }
        std::vector<std::string> bad = interpLines;
        // 破坏第 50 条：px 抬高 0.5m（远超相邻位移 ~0.023m）
        {
            uint32_t tick = 0; uint16_t ent = 0; float p[3] = {};
            parseNetPoseLine(bad[50], "interp tick=", &tick, &ent, p);
            std::snprintf(buf, sizeof(buf),
                          "[nl:info|net] interp tick=%u ent=%u px=%.4f py=%.4f pz=%.4f #1",
                          tick, ent, p[0] + 0.5f, p[1], p[2]);
            bad[50] = buf;
        }
        VerifyStats broken;
        if (runNetVerify(authLines, bad, 100, broken) == 0) {
            std::fprintf(stderr, "verify-m2 selftest: corrupted case unexpectedly PASS\n");
            return 1;
        }
        std::printf("verify-m2 selftest: PASS (exact maxErr=%.4f ratio=%.3f; "
                    "corrupted viol=%zu)\n", good.maxErr, good.maxRatio, broken.violations);
        return 0;
    }
    if (!authPath || !interpPath) {
        std::fprintf(stderr, "usage: net-tools verify-m2 --auth <log> --interp <log> "
                             "[--min-samples N] | --selftest\n");
        return 2;
    }
    std::ifstream af(authPath);
    std::ifstream cf(interpPath);
    if (!af || !cf) {
        std::fprintf(stderr, "verify-m2 : FAILED (cannot open %s / %s)\n",
                     authPath, interpPath);
        return 1;
    }
    std::vector<std::string> authLines, interpLines;
    std::string line;
    while (std::getline(af, line)) authLines.push_back(line);
    while (std::getline(cf, line)) interpLines.push_back(line);
    VerifyStats st;
    const int rc = runNetVerify(authLines, interpLines, minSamples, st);
    std::printf("verify-m2    : matched=%zu missing=%zu viol=%zu maxErr=%.4f maxRatio=%.3f\n",
                st.matched, st.missing, st.violations, st.maxErr, st.maxRatio);
    if (rc == 0) {
        std::printf("verify-m2    : PASS (interp within one-snapshot-interval of auth)\n");
        return 0;
    }
    std::fprintf(stderr, "verify-m2    : FAIL (matched=%zu missing=%zu viol=%zu)\n",
                 st.matched, st.missing, st.violations);
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
        "  net-tools loss-sweep [--port P] [--packets N] [--timeout-ms T]\n"
        "                        per-level loss injection 0..90%% (seed 1234); exit 0/2\n"
        "  net-tools selftest    deterministic loopback assertions; exit 0=PASS 1=FAIL\n"
        "  net-tools snapshot-selftest\n"
        "                        M-NET1 codec roundtrip bytes-level + quant asserts; exit 0/1\n"
        "  net-tools snapshot-bench [--count N]\n"
        "                        deterministic payload benchmark (seed 1234); exit 0/1/2\n"
        "  net-tools snapshot-decode <hex-bytes>\n"
        "                        decode hex wire bytes; exit 0=ok 1=FAILED 2=usage\n"
        "  net-tools session-loopback [--port P] [--ticks N] [--timeout-ms T]\n"
        "                        M-NET2 server-authoritative loopback: handshake + input + snapshot\n"
        "                        interp error <= one snapshot interval; exit 0=PASS 1=FAIL 2=timeout\n"
        "  net-tools verify-m2 --selftest | --auth <log> --interp <log>\n"
        "                        M-NET2 numeric anchor: client interp vs server auth trajectory;\n"
        "                        threshold = adjacent auth displacement + quant slack; exit 0=PASS 1=FAIL 2=usage\n"
        "  net-tools version     print vendored ENet + bitsery version\n",
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
    if (std::strcmp(cmd, "loss-sweep") == 0) return cmdLossSweep(argc - 2, argv + 2);
    if (std::strcmp(cmd, "selftest") == 0) return cmdSelftest();
    if (std::strcmp(cmd, "snapshot-selftest") == 0) return cmdSnapshotSelftest();
    if (std::strcmp(cmd, "session-loopback") == 0) return cmdSessionLoopback(argc - 2, argv + 2);
    if (std::strcmp(cmd, "verify-m2") == 0) return cmdNetVerifyM2(argc - 2, argv + 2);
    if (std::strcmp(cmd, "snapshot-bench") == 0) return cmdSnapshotBench(argc - 2, argv + 2);
    if (std::strcmp(cmd, "snapshot-decode") == 0) return cmdSnapshotDecode(argc - 2, argv + 2);
    if (std::strcmp(cmd, "version") == 0) {
        std::printf("enet        : %s\n", nl::net::version());
        std::printf("bitsery     : %s\n", nl::net::snapshotCodecVersion());
        return 0;
    }
    printUsage();
    return 2;
}
