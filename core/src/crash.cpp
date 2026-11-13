// ---- M-CRASH0：进程级崩溃捕获（关联系统，薄封装，落 nl_core） ----
// 捕获未处理 SEH 异常 / std::terminate / 纯虚调用 / CRT 非法参数，在 exe 同目录
// crashdumps/ 下写 <exe>_<时间戳>_<异常码>.dmp + 同名 .json 元数据。
// 零第三方依赖（仅系统 dbghelp.dll，运行时 LoadLibrary 预取）。
// 需要时 WinDbg/VS 加载 .dmp + 同版本 .pdb 直接开。
//
// 正确性硬约束：崩溃上下文里堆可能已坏 → 落盘路径只用 Win32 API + 栈缓冲 +
// snprintf（栈），禁用 std::string / std::ofstream / 任何 CRT 堆分配；先写元数据
// （轻量必成）再尝试写 dmp。

#include "nl/core/crash.hpp"

#include <windows.h>
#include <dbghelp.h>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#ifndef NL_BUILD_GIT_SHA
#define NL_BUILD_GIT_SHA "unknown"
#endif

#ifndef NL_BUILD_TIME
#define NL_BUILD_TIME (__DATE__ " " __TIME__)
#endif

namespace {

// 预取 MiniDumpWriteDump（崩溃时避免 LoadLibrary）
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(
    HANDLE hProcess, DWORD ProcessId, HANDLE hFile, MINIDUMP_TYPE DumpType,
    PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, PVOID UserStreamParam,
    PVOID CallbackParam);

MiniDumpWriteDumpFn g_miniDumpWriteDump = nullptr;
HMODULE g_dbgHelp = nullptr;

// 重入守卫：崩溃处理本身不能再崩出递归
volatile LONG g_handling = 0;

// 最近一次 reason（供无 EXCEPTION_POINTERS 的处理器使用）
const char* g_reason = "unhandled_exception";

// 只写 ASCII 到文件（零堆分配，分段写）
void writeAll(HANDLE h, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        DWORD n = 0;
        DWORD toWrite = (len - off > 0xFFFFFF) ? 0xFFFFFF : (DWORD)(len - off);
        if (!WriteFile(h, data + off, toWrite, &n, nullptr)) break;
        if (n == 0) break;
        off += n;
    }
}

// 取 exe 目录（含末尾 '\'，不含 exe 名）
void getExeDir(wchar_t* out, DWORD outLen) {
    wchar_t path[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* last = nullptr;
    for (DWORD i = 0; i < n; ++i) if (path[i] == L'\\') last = path + i;
    if (last) *last = L'\0';
    wcsncpy_s(out, outLen, path, _TRUNCATE);
}

// 时间戳 YYYYMMDD_HHMMSS（文件名用）
void fileTimestamp(wchar_t* ts, size_t tsLen) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(ts, tsLen, L"%04u%02u%02u_%02u%02u%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}
// 时间戳 YYYY-MM-DD HH:MM:SS（内容用）
void humanTimestamp(char* ts, size_t tsLen) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(ts, tsLen, _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

// exe 路径 / 名 / 命令行 转 UTF-8（栈缓冲，零堆）
void utf8FromWide(const wchar_t* w, char* out, size_t outLen) {
    int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outLen - 1, nullptr, nullptr);
    if (need <= 0) { out[0] = '\0'; return; }
    out[need < (int)outLen ? need : (int)outLen - 1] = '\0';
}
void exePathUtf8(char* out, size_t outLen) {
    wchar_t wpath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, wpath, MAX_PATH);
    utf8FromWide(wpath, out, outLen);
}
void exeNameUtf8(char* out, size_t outLen) {
    wchar_t wpath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, wpath, MAX_PATH);
    wchar_t* last = nullptr;
    for (wchar_t* p = wpath; *p; ++p) if (*p == L'\\') last = p;
    utf8FromWide(last ? last + 1 : wpath, out, outLen);
}
void cmdLineUtf8(char* out, size_t outLen) {
    utf8FromWide(GetCommandLineW(), out, outLen);
}

// JSON 转义（\" 与 \\），写入 dst（dstLen 含结尾）
void jsonEscape(const char* src, char* dst, size_t dstLen) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dstLen; ++i) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= dstLen) break;
            dst[j++] = '\\';
            dst[j++] = c;
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

// 写元数据 JSON（先于 dmp，保证轻量必成）
void writeMeta(const wchar_t* baseNoExt, const char* reason,
               DWORD exceptionCode, uintptr_t exceptionAddress) {
    wchar_t jsonPath[MAX_PATH + 16];
    wcsncpy_s(jsonPath, MAX_PATH + 16, baseNoExt, _TRUNCATE);
    wcscat_s(jsonPath, MAX_PATH + 16, L".json");

    char exepath[1024] = {0}, exename[256] = {0}, cmd[4096] = {0};
    char exepathE[3072] = {0}, exenameE[512] = {0}, cmdE[8192] = {0};
    exePathUtf8(exepath, sizeof(exepath));
    exeNameUtf8(exename, sizeof(exename));
    cmdLineUtf8(cmd, sizeof(cmd));
    jsonEscape(exepath, exepathE, sizeof(exepathE));
    jsonEscape(exename, exenameE, sizeof(exenameE));
    jsonEscape(cmd, cmdE, sizeof(cmdE));

    char ts[32] = {0}; humanTimestamp(ts, sizeof(ts));

    char buf[16384];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "{\n"
        "  \"tool\": \"nl-engine crash capture\",\n"
        "  \"exe_name\": \"%s\",\n"
        "  \"exe_path\": \"%s\",\n"
        "  \"git_sha\": \"%s\",\n"
        "  \"build_time\": \"%s\",\n"
        "  \"command_line\": \"%s\",\n"
        "  \"exception_code\": \"0x%08X\",\n"
        "  \"exception_address\": \"0x%p\",\n"
        "  \"thread_id\": %u,\n"
        "  \"timestamp\": \"%s\",\n"
        "  \"reason\": \"%s\"\n"
        "}\n",
        exenameE, exepathE, NL_BUILD_GIT_SHA, NL_BUILD_TIME, cmdE,
        exceptionCode, (void*)exceptionAddress, GetCurrentThreadId(), ts, reason);

    HANDLE h = CreateFileW(jsonPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        if (n > 0) writeAll(h, buf, (size_t)n);
        CloseHandle(h);
    }
}

// 主写入：dmp + 元数据
void capture(const char* reason, EXCEPTION_POINTERS* ep) {
    if (InterlockedCompareExchange(&g_handling, 1, 0) != 0) return; // 重入守卫

    DWORD code = 0;
    uintptr_t addr = 0;
    if (ep && ep->ExceptionRecord) {
        code = ep->ExceptionRecord->ExceptionCode;
        addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    }

    wchar_t dir[MAX_PATH] = {0};
    getExeDir(dir, MAX_PATH);

    // exe 名（去 .exe 后缀）
    wchar_t wpath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, wpath, MAX_PATH);
    wchar_t* last = nullptr;
    for (wchar_t* p = wpath; *p; ++p) if (*p == L'\\') last = p;
    const wchar_t* name = last ? last + 1 : wpath;
    wchar_t exename[256] = {0};
    wcsncpy_s(exename, 256, name, _TRUNCATE);
    wchar_t* dot = wcsrchr(exename, L'.');
    if (dot) *dot = L'\0';

    // 建 crashdumps 目录（已存在则忽略）
    wchar_t dumpDir[MAX_PATH + 16];
    wcsncpy_s(dumpDir, MAX_PATH + 16, dir, _TRUNCATE);
    wcscat_s(dumpDir, MAX_PATH + 16, L"\\crashdumps");
    CreateDirectoryW(dumpDir, nullptr);

    wchar_t ts[32]; fileTimestamp(ts, 32);
    wchar_t dmpPath[MAX_PATH + 32];
    _snwprintf_s(dmpPath, MAX_PATH + 32, _TRUNCATE, L"%s\\%s_%s_%08X.dmp",
        dumpDir, exename, ts, code);

    // 先写元数据（轻量必成）
    wchar_t baseNoExt[MAX_PATH + 32];
    wcsncpy_s(baseNoExt, MAX_PATH + 32, dmpPath, _TRUNCATE);
    size_t L = wcslen(baseNoExt);
    if (L > 4 && _wcsicmp(baseNoExt + L - 4, L".dmp") == 0) baseNoExt[L - 4] = L'\0';
    writeMeta(baseNoExt, reason, code, addr);

    // 再尝试写 dmp
    if (g_miniDumpWriteDump) {
        HANDLE h = CreateFileW(dmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithProcessThreadData);
            g_miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, type,
                ep ? &mei : nullptr, nullptr, nullptr);
            CloseHandle(h);
        }
    }
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* ep) {
    g_reason = "unhandled_exception";
    capture(g_reason, ep);
    return EXCEPTION_EXECUTE_HANDLER; // 终止进程
}

void terminateHandler() {
    g_reason = "std_terminate";
    capture(g_reason, nullptr);
    abort();
}
void purecallHandler() {
    g_reason = "purecall";
    capture(g_reason, nullptr);
    abort();
}
void invalidParamHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    g_reason = "invalid_parameter";
    capture(g_reason, nullptr);
    abort();
}

} // namespace

namespace nl::crash {

void install() {
    static bool done = false;
    if (done) return;
    done = true;

    // 预取 dbghelp（崩溃时避免加载）
    if (!g_dbgHelp) {
        g_dbgHelp = LoadLibraryW(L"dbghelp.dll");
        if (g_dbgHelp)
            g_miniDumpWriteDump = (MiniDumpWriteDumpFn)GetProcAddress(g_dbgHelp, "MiniDumpWriteDump");
    }

    // 抑制系统 GPF / 错误框，避免无人值守或测试时卡住等待用户
    ::SetErrorMode(::SetErrorMode(0) | SEM_FAILCRITICALERRORS |
                   SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    SetUnhandledExceptionFilter(unhandledFilter);
    std::set_terminate(terminateHandler);
    _set_purecall_handler(purecallHandler);
    _set_invalid_parameter_handler(invalidParamHandler);
}

void trigger_test_crash() {
    volatile int* p = nullptr;
    *p = 0xDEAD; // 故意空指针写，触发 EXCEPTION_ACCESS_VIOLATION
}

} // namespace nl::crash
