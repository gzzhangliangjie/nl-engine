// M-SC1：console 绑定面实现。口径/ABI/内存所有权见 nl/script/Bindings.hpp 文件头。
//
// 落点：nl_core（core/console 与 core/script 同库，绑定胶水不引新 lib）。
// extern "C" 函数只被 C# 经函数指针调用，不在任何 C++ 头暴露（ApiTable 是唯一入口）。

#include "nl/script/Bindings.hpp"

#include "nl/core/console.hpp"

#include <objbase.h>
#include <string>
#include <vector>

#include <windows.h>

namespace nl::script {

namespace {

// ---- C++ → C# 返回值静态缓冲（下一次调用前有效；console 单线程约定下无竞态）----

std::string& ownedBuffer() {
    static std::string s;
    return s;
}

// ---- 托管命令回调 ABI（C# 侧实现）----
// int fn(int argc, const char** argv, const char** outText) -> 0=ok / 1=失败
using ManagedCommandFn = int(__cdecl*)(int, const char**, const char**);

// SEH guard：托管异常在互操作边界捕获（__try 函数体禁 C++ 对象，独立纯 POD 函数）。
int guardedCommandCall(ManagedCommandFn fn, int argc, const char** argv,
                       const char** outText, std::uint32_t* exceptionCode) {
    __try {
        *outText = nullptr;   // 先清：托管侧若中途炸，C++ 侧不读到野指针
        *exceptionCode = 0;
        return fn(argc, argv, outText);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exceptionCode = static_cast<std::uint32_t>(GetExceptionCode());
        return -1;   // 哨兵：与托管侧 0/1 区分
    }
}

} // namespace

// ---- extern "C" 绑定函数（只经 ApiTable 函数指针被 C# 调用）----

namespace {

using nl::core::console::Console;
using nl::core::console::CVar;

extern "C" int nl_cvarGet(const char* name, const char** outValue) {
    if (!name || !outValue) return 0;
    *outValue = nullptr;
    CVar* cv = Console::get().findCVar(name);
    if (!cv) return 0;
    ownedBuffer() = cv->value().toString();
    *outValue = ownedBuffer().c_str();
    // 类型码与 CVarType 枚举对齐（Bool=1 起，0 留给"未注册"）
    return static_cast<int>(cv->type()) + 1;
}

extern "C" int nl_cvarSet(const char* name, const char* value) {
    if (!name || !value) return 2;
    CVar* cv = Console::get().findCVar(name);
    if (!cv) return 1;
    return cv->setFromString(value) ? 0 : 2;
}

extern "C" const char* nl_cvarList() {
    // 与 installBuiltins 的 cvar.list 同格式（allEntries 全量视图）
    std::string out;
    for (const auto& e : Console::get().allEntries()) {
        if (!e.isCVar) continue;
        const CVar* cv = Console::get().findCVar(e.name);
        out += "\n  " + e.name + " = " + (cv ? cv->value().toString() : std::string("?")) +
               "  (" + (cv ? nl::core::console::cvarTypeName(cv->type()) : "?") + ")  " + e.help;
    }
    ownedBuffer() = std::move(out);
    return ownedBuffer().c_str();
}

extern "C" int nl_consoleExec(const char* line, const char** outOutput) {
    if (!line || !outOutput) return 2;
    const auto r = Console::get().exec(line);
    *outOutput = nullptr;
    ownedBuffer() = r.output;
    // 返回值在缓冲填充后取：exec 内部也可能动 ownedBuffer 吗？不会——本文件是唯一
    // 使用者；但 exec 的输出经 finish 进 console 环形日志，ownedBuffer 只在这里写。
    *outOutput = ownedBuffer().c_str();
    return r.exitCode();
}

extern "C" int nl_consoleRegister(const char* name, const char* help, void* fn) {
    if (!name || !help || !fn) return 1;
    if (Console::get().findCVar(name)) return 1;   // 不许遮蔽 CVar（统一注册表先查 CVar）
    ManagedCommandFn managed = reinterpret_cast<ManagedCommandFn>(fn);
    std::string nameCopy = name;
    Console::get().registerCommand(name, help,
        [managed, nameCopy](const std::vector<std::string>& args) -> nl::core::console::CommandResult {
            // argv 生命周期：C++ 字符串在调用期间存活（lambda 参数持有）
            std::vector<const char*> argv;
            argv.reserve(args.size());
            for (const auto& a : args) argv.push_back(a.c_str());

            const char* text = nullptr;
            std::uint32_t excCode = 0;
            const int rc = guardedCommandCall(managed, static_cast<int>(argv.size()),
                                              argv.data(), &text, &excCode);

            // 托管侧 outText = Marshal.AllocHGlobal（CoTaskMem）→ C++ 拷完 CoTaskMemFree
            std::string textCopy;
            if (text) {
                textCopy = text;
                ::CoTaskMemFree(const_cast<char*>(text));
            }

            if (rc == -1) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "%08x", excCode);
                return {false, std::string("managed exception at command boundary: SEH code=0x") + buf};
            }
            return {rc == 0, std::move(textCopy)};
        });
    return 0;
}

extern "C" int nl_consoleUnregister(const char* name) {
    if (!name) return 1;
    return Console::get().unregisterCommand(name) ? 0 : 1;
}

} // namespace

const ApiTable* consoleApiTable() {
    static const ApiTable table{
        reinterpret_cast<void*>(&nl_cvarGet),
        reinterpret_cast<void*>(&nl_cvarSet),
        reinterpret_cast<void*>(&nl_cvarList),
        reinterpret_cast<void*>(&nl_consoleExec),
        reinterpret_cast<void*>(&nl_consoleRegister),
        reinterpret_cast<void*>(&nl_consoleUnregister),
    };
    return &table;
}

} // namespace nl::script
