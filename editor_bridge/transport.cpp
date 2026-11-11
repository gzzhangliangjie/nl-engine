// transport.cpp — M-E6：TCP / 命名管道双实现（Win32，C++17）
// 设计要点：
//  * 数据 socket 保持「阻塞」语义 + 每次 recv 前先 waitReadable(select) —— 与 M-E1~E5
//    的非阻塞 FIONBIO 行为等价，但 sendAll 不会撞 WSAEWOULDBLOCK（小信封也稳）。
//  * 命名管道两端都以 FILE_FLAG_OVERLAPPED 打开：服务端 ConnectNamedPipe 需要重叠
//    I/O 才能非阻塞 accept；统一走 OVERLAPPED + GetOverlappedResult(TRUE) 收尾，
//    避免服务端/客户端两套读写路径不对称。
//  * 服务端每 accept 一个实例就立刻 arm 下一个实例（PIPE_UNLIMITED_INSTANCES），
//    这样「多视口 = 多引擎进程」能并发连入，不需串行等待。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>   // 必须先于 windows.h（否则旧 winsock.h 抢先定义 SOCKET 冲突）
#include <ws2tcpip.h>
#include <windows.h>

#include "transport.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#pragma comment(lib, "ws2_32.lib")

namespace nl::editor::transport {

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string lastErrorText(DWORD e) {
    char buf[160];
    snprintf(buf, sizeof buf, "win32 error %lu", (unsigned long)e);
    return buf;
}

// Winsock 进程级一次性初始化：不 WSACleanup（进程退出由系统回收），
// 避免 host.stop() / bridge.shutdown() 多次起停时把别的实例的连接一起废掉。
void ensureWsa() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa{};
        (void)WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}

void prepOverlapped(OVERLAPPED& ov, HANDLE ev) {
    ZeroMemory(&ov, sizeof ov);
    ov.hEvent = ev;
    ResetEvent(ev);
}

// ---------------- TCP ----------------
class TcpStream final : public Stream {
public:
    explicit TcpStream(SOCKET s) : s_(s) {}
    ~TcpStream() override { close(); }

    int sendAll(const uint8_t* data, int len) override {
        int total = 0;
        while (total < len) {
            int r = ::send(s_, reinterpret_cast<const char*>(data) + total, len - total, 0);
            if (r <= 0) return -1;
            total += r;
        }
        return total;
    }
    int recvSome(uint8_t* buf, int cap) override {
        int r = ::recv(s_, reinterpret_cast<char*>(buf), cap, 0);
        if (r == 0) return -1;                       // 对端有序关闭
        if (r < 0) return (WSAGetLastError() == WSAEWOULDBLOCK) ? 0 : -1;
        return r;
    }
    bool waitReadable(int timeoutMs) override {
        if (s_ == INVALID_SOCKET) return false;
        fd_set rd; FD_ZERO(&rd); FD_SET(s_, &rd);
        timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        return ::select(0, &rd, nullptr, nullptr, &tv) > 0;
    }
    void close() override {
        if (s_ != INVALID_SOCKET) { ::closesocket(s_); s_ = INVALID_SOCKET; }
    }
    bool valid() const override { return s_ != INVALID_SOCKET; }
    const char* label() const override { return "tcp"; }
private:
    SOCKET s_ = INVALID_SOCKET;
};

class TcpListener final : public Listener {
public:
    bool open(const Endpoint& ep, std::string& err) {
        ep_ = ep;
        ensureWsa();
        s_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s_ == INVALID_SOCKET) { err = "socket() failed"; return false; }
        int yes = 1;
        (void)::setsockopt(s_, SOL_SOCKET, SO_REUSEADDR,
                           reinterpret_cast<const char*>(&yes), sizeof yes);
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(ep.port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(s_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            err = "bind() failed: " + lastErrorText(::WSAGetLastError());
            ::closesocket(s_); s_ = INVALID_SOCKET; return false;
        }
        if (::listen(s_, 16) != 0) {           // M-E6 多 client：backlog 不再是 1
            err = "listen() failed";
            ::closesocket(s_); s_ = INVALID_SOCKET; return false;
        }
        return true;
    }
    std::unique_ptr<Stream> pollAccept() override {
        if (s_ == INVALID_SOCKET) return nullptr;
        fd_set rd; FD_ZERO(&rd); FD_SET(s_, &rd);
        timeval tv{0, 0};
        if (::select(0, &rd, nullptr, nullptr, &tv) <= 0) return nullptr;
        SOCKET c = ::accept(s_, nullptr, nullptr);
        if (c == INVALID_SOCKET) return nullptr;
        return std::make_unique<TcpStream>(c);
    }
    bool valid() const override { return s_ != INVALID_SOCKET; }
    Endpoint endpoint() const override { return ep_; }
    ~TcpListener() override {
        if (s_ != INVALID_SOCKET) { ::closesocket(s_); s_ = INVALID_SOCKET; }
    }
private:
    SOCKET   s_ = INVALID_SOCKET;
    Endpoint ep_;
};

// ---------------- Named pipe ----------------
std::string pipeFullName(const Endpoint& ep) {
    return std::string("\\\\.\\pipe\\") + ep.pipe;
}

class PipeStream final : public Stream {
public:
    PipeStream(HANDLE h, bool serverSide) : h_(h), serverSide_(serverSide) {
        ev_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~PipeStream() override { close(); }

    int sendAll(const uint8_t* data, int len) override {
        if (h_ == INVALID_HANDLE_VALUE || !ev_) return -1;
        int total = 0;
        while (total < len) {
            prepOverlapped(ov_, ev_);
            DWORD wrote = 0;
            BOOL ok = ::WriteFile(h_, data + total, static_cast<DWORD>(len - total), &wrote, &ov_);
            if (!ok) {
                if (::GetLastError() != ERROR_IO_PENDING) return -1;
                if (!::GetOverlappedResult(h_, &ov_, &wrote, TRUE)) return -1;
            }
            if (wrote == 0) return -1;
            total += static_cast<int>(wrote);
        }
        return total;
    }
    int recvSome(uint8_t* buf, int cap) override {
        if (h_ == INVALID_HANDLE_VALUE || !ev_) return -1;
        DWORD avail = 0;
        if (!::PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) return -1;
        if (avail == 0) return 0;
        DWORD want = (std::min)(avail, static_cast<DWORD>(cap));
        prepOverlapped(ov_, ev_);
        DWORD got = 0;
        BOOL ok = ::ReadFile(h_, buf, want, &got, &ov_);
        if (!ok) {
            if (::GetLastError() != ERROR_IO_PENDING) return -1;
            if (!::GetOverlappedResult(h_, &ov_, &got, TRUE)) return -1;
        }
        return static_cast<int>(got);
    }
    bool waitReadable(int timeoutMs) override {
        if (h_ == INVALID_HANDLE_VALUE) return false;
        const int64_t until = nowMs() + timeoutMs;
        for (;;) {
            DWORD avail = 0;
            if (!::PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) return false;
            if (avail > 0) return true;
            if (nowMs() >= until) return false;
            ::Sleep(1);
        }
    }
    void close() override {
        if (h_ != INVALID_HANDLE_VALUE) {
            if (serverSide_) (void)::DisconnectNamedPipe(h_);
            ::CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
        if (ev_) { ::CloseHandle(ev_); ev_ = nullptr; }
    }
    bool valid() const override { return h_ != INVALID_HANDLE_VALUE; }
    const char* label() const override { return "pipe"; }
private:
    HANDLE     h_ = INVALID_HANDLE_VALUE;
    HANDLE     ev_ = nullptr;
    OVERLAPPED ov_{};
    bool       serverSide_ = false;
};

class PipeListener final : public Listener {
public:
    bool open(const Endpoint& ep, std::string& err) {
        ep_ = ep;
        full_ = pipeFullName(ep);
        return arm(err);
    }

    // 预挂一个「等待连接」的实例；accept 成功后再 arm 下一个，支持并发多 client。
    bool arm(std::string& err) {
        pending_ = ::CreateNamedPipeA(
            full_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
        if (pending_ == INVALID_HANDLE_VALUE) {
            err = "CreateNamedPipe failed: " + lastErrorText(::GetLastError());
            return false;
        }
        ev_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        prepOverlapped(ov_, ev_);
        BOOL ok = ::ConnectNamedPipe(pending_, &ov_);
        DWORD e = ok ? ERROR_SUCCESS : ::GetLastError();
        if (!ok && e == ERROR_PIPE_CONNECTED) {
            ::SetEvent(ev_);          // 客户端抢在 ConnectNamedPipe 之前连上了
        } else if (!ok && e != ERROR_IO_PENDING) {
            err = "ConnectNamedPipe failed: " + lastErrorText(e);
            ::CloseHandle(ev_); ev_ = nullptr;
            ::CloseHandle(pending_); pending_ = INVALID_HANDLE_VALUE;
            return false;
        }
        armed_ = true;
        return true;
    }

    std::unique_ptr<Stream> pollAccept() override {
        if (!armed_ || !ev_) return nullptr;
        if (::WaitForSingleObject(ev_, 0) != WAIT_OBJECT_0) return nullptr;
        DWORD n = 0;
        (void)::GetOverlappedResult(pending_, &ov_, &n, FALSE);  // 收尾（忽略返回值）
        HANDLE h = pending_;
        pending_ = INVALID_HANDLE_VALUE;
        ::CloseHandle(ev_); ev_ = nullptr;
        armed_ = false;
        auto st = std::make_unique<PipeStream>(h, true);
        std::string ignore;
        (void)arm(ignore);           // 下一实例（失败也不影响本次已接受的连接）
        return st;
    }
    bool valid() const override { return armed_ && pending_ != INVALID_HANDLE_VALUE; }
    Endpoint endpoint() const override { return ep_; }
    ~PipeListener() override {
        if (pending_ != INVALID_HANDLE_VALUE) {
            (void)::CancelIoEx(pending_, &ov_);
            ::CloseHandle(pending_);
            pending_ = INVALID_HANDLE_VALUE;
        }
        if (ev_) { ::CloseHandle(ev_); ev_ = nullptr; }
    }
private:
    Endpoint   ep_;
    std::string full_;
    HANDLE     pending_ = INVALID_HANDLE_VALUE;
    HANDLE     ev_ = nullptr;
    OVERLAPPED ov_{};
    bool       armed_ = false;
};

} // namespace

std::string Endpoint::display() const {
    if (kind == Kind::Pipe) return "pipe://" + pipe;
    return "tcp://" + host + ":" + std::to_string(static_cast<unsigned>(port));
}

bool parseEndpoint(const std::string& text, Endpoint& out, std::string& err) {
    out = Endpoint{};
    if (text.rfind("pipe://", 0) == 0) {
        out.kind = Kind::Pipe;
        out.pipe = text.substr(7);
        if (out.pipe.empty() || out.pipe.find('\\') != std::string::npos ||
            out.pipe.find('/') != std::string::npos || out.pipe.size() > 200) {
            err = "pipe://<name>: name must be a plain local pipe name";
            return false;
        }
        return true;
    }
    std::string rest;
    if (text.rfind("tcp://", 0) == 0) {
        rest = text.substr(6);
    } else if (!text.empty() && text.find_first_not_of("0123456789") == std::string::npos) {
        long p = std::strtol(text.c_str(), nullptr, 10);   // 裸端口（兼容旧口径）
        if (p < 1 || p > 65535) { err = "port out of range 1..65535"; return false; }
        out.port = static_cast<uint16_t>(p);
        return true;
    } else {
        err = "endpoint must be tcp://host:port, pipe://name, or a bare port number";
        return false;
    }
    const size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
        out.host = rest.substr(0, colon);
        const std::string portText = rest.substr(colon + 1);
        if (portText.empty() || portText.find_first_not_of("0123456789") != std::string::npos) {
            err = "tcp://host:port: bad port"; return false;
        }
        long p = std::strtol(portText.c_str(), nullptr, 10);
        if (p < 1 || p > 65535) { err = "port out of range 1..65535"; return false; }
        out.port = static_cast<uint16_t>(p);
    } else {
        out.host = rest;
    }
    if (out.host.empty()) out.host = "127.0.0.1";
    // 本机本地回路是硬约束：编辑器与引擎同机，不接受任意 host（§3 安全边界）。
    if (out.host != "127.0.0.1" && out.host != "localhost") {
        err = "only loopback host is allowed (127.0.0.1 / localhost)";
        return false;
    }
    return true;
}

std::unique_ptr<Listener> createListener(const Endpoint& ep, std::string& err) {
    if (ep.kind == Kind::Pipe) {
        auto l = std::make_unique<PipeListener>();
        if (!l->open(ep, err)) return nullptr;
        return l;
    }
    auto l = std::make_unique<TcpListener>();
    if (!l->open(ep, err)) return nullptr;
    return l;
}

std::unique_ptr<Stream> createStream(const Endpoint& ep, std::string& err) {
    if (ep.kind == Kind::Pipe) {
        const std::string full = pipeFullName(ep);
        HANDLE h = ::CreateFileA(full.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD e = ::GetLastError();
            if (e == ERROR_PIPE_BUSY && ::WaitNamedPipeA(full.c_str(), 200)) {
                h = ::CreateFileA(full.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            }
        }
        if (h == INVALID_HANDLE_VALUE) {
            err = "CreateFile(pipe) failed: " + lastErrorText(::GetLastError());
            return nullptr;
        }
        return std::make_unique<PipeStream>(h, false);
    }
    ensureWsa();
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { err = "socket() failed"; return nullptr; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(ep.port);
    (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        err = "connect() failed: " + lastErrorText(::WSAGetLastError());
        ::closesocket(s);
        return nullptr;
    }
    return std::make_unique<TcpStream>(s);
}

} // namespace nl::editor::transport
