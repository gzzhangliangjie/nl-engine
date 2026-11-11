// rpc_self_test.cpp — M-E1 验收：同进程内 server(RpcHost) + client(RpcBridge) 双线程
// 验证 握手 / engine.announce / 双向 echo / 心跳 pong，无需 GUI、可无头运行。
// 编辑器侧每帧驱动 host.update()（与真实编辑器主循环一致）；引擎侧每帧驱动 bridge.update()。
#include "rpc_host.hpp"   // 编辑器侧 server
#include "rpc_bridge.hpp" // 引擎侧 client
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <windows.h>

// 本测试直接用底层 msgpack 类型构造信封；其命名空间是 nl::editor::msgpack
namespace msgpack = nl::editor::msgpack;

using namespace std::chrono_literals;

static std::atomic<bool> g_stop{false};
static std::atomic<int>  g_failures{0};

// 引擎 announce 上报捕获（onEngineAnnounce 在主线程 host.update() 内被调用）
static uint64_t g_annHwnd = 0;
static int      g_annPid  = 0;
static int      g_annProto = 0;
static std::string g_annView;   // M-E6：announce 的 viewId

// 反向 echo：client.call(editor.echo) 在 client 线程执行，主线程泵 host.update() 派发
static std::atomic<bool> g_startClientEcho{false};
static std::atomic<bool> g_clientEchoDone{false};
static std::atomic<bool> g_clientEchoOk{false};

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", msg); ++g_failures; } \
    else printf("  [ ok ] %s\n", msg); \
} while (0)

int main() {
    printf("=== M-E1 RPC self-test ===\n");

    nl::editor::host::RpcHost host;
    host.registerHandler("editor.echo", [](const nl::editor::host::RpcValue& p) { return p; });
    host.registerNotify("editor.log", [](const nl::editor::host::RpcValue& p) {
        if (p.isStr()) printf("  [notify] editor.log: %s\n", p.asStr().c_str());
    });
    host.registerNotify("rpc.ping", [](const nl::editor::host::RpcValue&) {}); // 自动回 pong
    host.onEngineAnnounce = [](const std::string& viewId, uint64_t hwnd, int pid,
                               int proto, const nl::editor::host::RpcValue&) {
        g_annHwnd = hwnd; g_annPid = pid; g_annProto = proto; g_annView = viewId;
        printf("  [event] onEngineAnnounce view=%s hwnd=0x%llx pid=%d proto=%d\n",
               viewId.c_str(), (unsigned long long)hwnd, pid, proto);
    };
    bool started = host.start();
    CHECK(started, "RpcHost.start() 监听 127.0.0.1:29110");

    std::thread client([&]() {
        nl::editor::RpcBridge bridge;
        bridge.registerHandler("engine.echo", [](const nl::editor::host::RpcValue& p) { return p; });
        bridge.onReady = []() { printf("  [event] client onReady\n"); };
        bridge.onClose = []() { printf("  [event] client onClose\n"); };
        if (!bridge.connect()) { printf("  [FAIL] bridge.connect()\n"); ++g_failures; return; }

        // engine.announce（带 hwnd/pid/proto/caps）
        msgpack::Map a;
        a["hwnd"]  = msgpack::Value(int64_t(0xDEADBEEF));
        a["pid"]   = msgpack::Value(int64_t(GetCurrentProcessId()));
        a["proto"] = msgpack::Value(int64_t(1));
        msgpack::Map caps; caps["backend"] = msgpack::Value(std::string("DX12"));
        a["caps"]  = msgpack::Value(caps);
        a["viewId"] = msgpack::Value(std::string("self-test"));   // M-E6 多视口路由键
        bridge.notify("engine.announce", msgpack::Value(a));

        // 等待主线程完成正向 echo（host.call），再做反向 echo
        // 注意：等待期间必须持续泵 bridge.update()，否则收不到 host 的 engine.echo 请求
        int w = 0;
        while (!g_startClientEcho && w++ < 500) { bridge.update(); std::this_thread::sleep_for(10ms); }
        if (g_startClientEcho) {
            auto cr = bridge.call("editor.echo", msgpack::Value(std::string("hello-editor")));
            if (std::holds_alternative<msgpack::Value>(cr)) {
                auto& v = std::get<msgpack::Value>(cr);
                g_clientEchoOk = v.isStr() && v.asStr() == "hello-editor";
            }
        }
        g_clientEchoDone = true;

        while (!g_stop) { bridge.update(); std::this_thread::sleep_for(16ms); }
        bridge.shutdown();
    });

    // 阶段 A：主线程泵 host.update() 直到 accept 完成 + announce 上报
    {
        int tries = 0;
        while (tries++ < 200) {
            host.update();
            if (host.isConnected() && g_annHwnd != 0) break;
            std::this_thread::sleep_for(10ms);
        }
    }
    CHECK(host.isConnected(), "引擎 client 已连上（accept 成功）");
    CHECK(g_annHwnd == 0xDEADBEEF, "engine.announce 上报 hwnd == 0xDEADBEEF");
    CHECK(g_annPid == (int)GetCurrentProcessId(), "engine.announce 上报 pid 正确");
    CHECK(g_annProto == 1, "engine.announce 上报 proto == 1");
    CHECK(g_annView == "self-test", "M-E6 announce viewId == self-test");
    CHECK(host.clientCount() == 1 && host.viewIds().size() == 1 && host.viewIds()[0] == "self-test",
          "M-E6 clientCount/viewIds 单视图口径");

    // 阶段 B：正向 echo：host.call -> client handler -> 回包（含 callTo 显式路由）
    auto r = host.call("engine.echo", msgpack::Value(std::string("hello-engine")));
    CHECK(std::holds_alternative<msgpack::Value>(r), "host.call(engine.echo) 成功返回 RpcValue");
    if (std::holds_alternative<msgpack::Value>(r)) {
        auto& v = std::get<msgpack::Value>(r);
        CHECK(v.isStr() && v.asStr() == "hello-engine", "回包内容 == 请求内容 (echo)");
    }
    host.setActiveView("self-test");
    auto rt = host.callTo("self-test", "engine.echo", msgpack::Value(std::string("routed")));
    CHECK(std::holds_alternative<msgpack::Value>(rt) &&
          std::get<msgpack::Value>(rt).isStr() &&
          std::get<msgpack::Value>(rt).asStr() == "routed",
          "M-E6 callTo(viewId) 定向路由 echo");
    auto rbad = host.callTo("no-such-view", "engine.echo", msgpack::Value());
    CHECK(std::holds_alternative<nl::editor::host::RpcError>(rbad) &&
          std::get<nl::editor::host::RpcError>(rbad).code == 103,
          "M-E6 callTo(未知视图) -> error 103");

    // 阶段 C：反向 echo：client.call(editor.echo) -> host handler -> 回包
    // 主线程需持续泵 host.update()，host 才能派发 client 的 req
    g_startClientEcho = true;
    {
        int tries = 0;
        while (!g_clientEchoDone && tries++ < 200) {
            host.update();
            std::this_thread::sleep_for(10ms);
        }
    }
    CHECK(g_clientEchoOk, "client.call(editor.echo) 回包内容 == 请求内容 (反向 echo)");

    // 阶段 D：心跳：让 client 跑 ~2.6s，ping/pong 应持续，host 不应误判掉线
    {
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < 2600ms) {
            host.update(); // 处理 client 的 rpc.ping -> 回 rpc.pong
            std::this_thread::sleep_for(10ms);
        }
    }
    CHECK(host.isConnected(), "心跳 2.6s 内持续 ping/pong，host 未误判掉线");

    g_stop = true;
    client.join();
    host.stop();

    // 阶段 E（M-E6）：命名管道传输全链路——同一套上层协议换 endpoint 即走 Win32 pipe
    printf("  -- M-E6 named-pipe transport --\n");
    {
        nl::editor::host::RpcHost ph;
        ph.registerHandler("editor.echo",
                           [](const nl::editor::host::RpcValue& p) { return p; });
        ph.registerNotify("rpc.ping", [](const nl::editor::host::RpcValue&) {});
        const bool phStarted = ph.start("pipe://nl-editor-selftest");
        CHECK(phStarted, "RpcHost.start(pipe://nl-editor-selftest)");
        CHECK(ph.endpoint() == "pipe://nl-editor-selftest", "M-E6 endpoint() 回读管道名");

        std::atomic<bool> pstop{false};
        std::atomic<bool> pconnected{false};
        std::thread pc([&]() {
            nl::editor::RpcBridge b;
            b.registerHandler("engine.echo",
                              [](const nl::editor::host::RpcValue& p) { return p; });
            if (!b.connect("pipe://nl-editor-selftest")) {
                printf("  [FAIL] bridge.connect(pipe://...) \n"); ++g_failures; return;
            }
            pconnected = true;
            msgpack::Map a;
            a["hwnd"] = msgpack::Value(int64_t(0x1234));
            a["pid"]  = msgpack::Value(int64_t(GetCurrentProcessId()));
            a["viewId"] = msgpack::Value(std::string("pipe-view"));
            b.notify("engine.announce", msgpack::Value(a));
            while (!pstop) { b.update(); std::this_thread::sleep_for(5ms); }
            b.shutdown();
        });
        int tries = 0;
        while (tries++ < 400 && !ph.isConnected()) { ph.update(); std::this_thread::sleep_for(5ms); }
        CHECK(ph.isConnected(), "M-E6 命名管道 accept 成功");
        CHECK(pconnected, "M-E6 命名管道 client.connect 成功");
        auto pr = ph.call("engine.echo", msgpack::Value(std::string("pipe-ok")), 3000);
        CHECK(std::holds_alternative<msgpack::Value>(pr) &&
              std::get<msgpack::Value>(pr).isStr() &&
              std::get<msgpack::Value>(pr).asStr() == "pipe-ok",
              "M-E6 管道上 host.call(engine.echo) 往返一致");
        pstop = true;
        pc.join();
        ph.stop();
    }

    printf("=== M-E1 self-test %s (%d failures) ===\n",
           g_failures == 0 ? "PASS" : "FAIL", (int)g_failures);
    return g_failures == 0 ? 0 : 1;
}
