// nl-script —— C# hosting（nl::script）自测工具，A 类并行轨道（feat/script worktree）。
// 只依赖 core（nl/script Vm），零图形 API 符号；接线仿 nanite-scene 先例。
//
// 退出码约定（与 nanite-tools / 展示台 expectExit 0/1/2 对齐）：
//   0 = 用例全过 / 脚本运行成功（打印 "PASS" / "run: ret=<n>"）
//   1 = 断言失败 / dotnet build 失败 / 托管异常（SEH 边界捕获转 Result）
//   2 = 用法错误（未知子命令 / 缺参数）
//
// 子命令（docs/p8/P8-SCRIPT-PLAN.md M-SC0 验收判据）：
//   check  <projectDir>                        只编译（dotnet build），不装载运行时
//   run    <projectDir> [--args <s>] [--type <asm-qualified>] 编译+装载+调入口
//   selftest <projectDir>                      全量机械断言（M-SC0 验收）：
//     s1 Vm::create(runtimeconfig) 装载运行时（官方 3 导出口径）
//     s2 getEntryPoint 默认签名入口（[UnmanagedCallersOnly]，UNMANAGEDCALLERSONLY_METHOD）
//     s3 调用返回值锚：args="ok" -> 42 / 空参数 -> -1 / "echo" -> 0
//     s4 托管异常边界捕获：args="throw" -> SEH 转错误码，不崩进程
//     s5 热重编行为更新：拷贝工程改 42->43，dotnet build -p:AssemblyName 换名重编，
//        同进程装载新 DLL -> 43（「脚本改动经 dotnet build 重编后行为更新」验收）
//     s6 console 绑定（M-SC1，需第 2 个参数给出 console 示例工程目录）：
//        s6a C# 改 cvar 触发 C++ onChanged / s6b C# 注册命令经 console exec 调通 /
//        s6c 错误码逐一对齐（0/1/2/3）/ s6d 托管异常命令 SEH 边界 /
//        s6e 双向读回 + cvar.list / s6f 注销命令解绑（M-SC3 预演）
//     s7 ECS 数据面绑定（M-SC2，需第 3 个参数给出 ecs 示例工程目录）：
//        s7a C# spawn+挂 Transform+改字段 -> C++ 读回一致 /
//        s7b C++ 经绑定表改字段 -> C# 读回锚值 /
//        s7c 失效实体全操作拒绝（NoEntity，不崩）/
//        s7d 延迟删除 + flush 后 exists 假 + 绑定表 TransformGet 拒绝 /
//        s7e 父子关系经绑定表 roundtrip（getChildIDs 视图一致）/
//        s7f JSON roundtrip 不受脚本侧改动影响（字节一致）/
//        s7g 解绑 World 后导出一律 NoWorld（M-SC3 热重载窗口兜底）
//     s8 脚本热更（M-SC3，需第 4 个参数给出 bootstrap 工程目录）：
//        s8a bootstrap 装载 + console.reload-script 注册（幂等 init）/
//        s8b 首次装载（LoadFromStream collectible ALC）+ Entry 重绑定 /
//        s8c 改脚本源 -> dotnet build 同名覆盖 -> ScriptHost::reloadScript /
//        s8d 行为换新（echo 前缀 v1 -> [v2]）/ s8e C++ 状态无损（cvar 值/实体/
//            内建命令不动 + 脚本命令表重登记）/
//        s8f 连续 reload x3 无泄漏（GetProcessHandleCount 漂移阈值）/
//        s8g unloadScript weakref 校验（完全卸载）/ s8h 卸载后命令 UnknownCommand
//            + C++ 状态仍可用（ECS 导出/内建命令）
//     s9 Time/Input/Log 绑定（M-SC5，需第 5 个参数给出 timeinput 示例工程目录）：
//        s9a C# 读时钟锚（tick x3）+ SetState 控制读回 + 哨兵语义（-1 不变）/
//        s9a ActionFind 三型冻结值（W/D 喂键 update 后 Bool/Float/Vec2）+
//            NoAction 负向（不存在 / 类型不符）+ KeyIsDown（含越界键码防御）/
//        s9a Log.Write 通道 "script" + level 越界 BadArg + SetFilter 生效与
//            非法指令显式报错（error 带原因）/
//        s9b 解绑路径：timeSetClock/inputSetState/inputSetActionMap(nullptr) ->
//            NoClock/NoSource（不隐式建表拍板），Log 全局面不受影响
//     s10 Camera 绑定（M-SC6，需第 6 个参数给出 camera 示例工程目录）：
//        s10a 假适配源罐头读回锚（GetPose eye/yaw/pitch/fov/mode）+ SetPose rc
//            与 C++ 捕获核 {4,5,6,45,15} + BadArg（eye null）+ fov 写/clamp 读回
//            （500 -> 120，clamp [20,120] 与 viewer 适配源同构）/
//        s10b track 模式（mode=3）：GetPose 照常 + SetPose ModeUnsupported（轨迹
//            数据主权）+ fov 照常写（模式统一生效）/
//        s10c 解绑路径：cameraSetSource(nullptr) -> NoSource / GetFov NaN
//            （f32 返回无 rc 通道账本特例）
//
// 用法示例：
//   nl-script selftest script/examples/hello script/examples/console script/examples/ecs script/bootstrap script/examples/timeinput script/examples/camera
//   nl-script run script/examples/hello --args ok
//   nl-script run script/examples/console --api
//   nl-script run script/examples/ecs --api
//   nl-script check script/examples/hello

#include "nl/core/log.hpp"
#include "nl/core/game_clock.hpp"   // s9 Time 导出验收（M-SC5）
#include "nl/core/input.hpp"        // s9 Input 导出验收（InputState 三态表）
#include "nl/core/action_map.hpp"   // s9 ActionMap（注意：不在 input.hpp——动作映射独立头）
#include "nl/core/console.hpp"
#include "nl/physics/World.hpp"     // s12 载具域验收（M-SC8：DebugScene 训练场）
#include "nl/scene/Component.hpp"
#include "nl/scene/World.hpp"
#include "nl/script/Vm.hpp"
#include "nl/script/Bindings.hpp"
#include "nl/script/ScriptHost.hpp"

#include <chrono>
#include <cmath>       // s11 std::fabs（M-SC7 终态核容差）
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>
#include <process.h>   // _getpid（selftest temp 目录加 PID 后缀——regression 并行跑
                       // 同 case 双后端实例，固定目录名互踩 = m-sc3 间歇 FAIL 毒源）

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

// SEH guard（与 core/src/script/Vm.cpp 同口径；工具侧直调入口用）。
// __try 函数体里不许有需要展开的 C++ 对象，故独立成纯 POD 函数。
int guardedCall(nl::script::EntryPointFn fn, void* args, std::int32_t sizeBytes,
                std::int32_t* ret, std::uint32_t* exceptionCode) {
    __try {
        *ret = fn(args, sizeBytes);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exceptionCode = static_cast<std::uint32_t>(GetExceptionCode());
        return 1;
    }
}

void expectTrue(bool ok, const char* what) {
    if (ok) {
        std::printf("  ok  %s\n", what);
        return;
    }
    std::printf("  FAIL %s\n", what);
    ++g_failures;
}

void expectInt(long long got, long long want, const char* what) {
    if (got == want) {
        std::printf("  ok  %s (got %lld)\n", what, got);
        return;
    }
    std::printf("  FAIL %s: got %lld want %lld\n", what, got, want);
    ++g_failures;
}

// 定位 build 产物：bin/Debug/net8.0/*.runtimeconfig.json。
// preferStem 非空时按程序集名精确匹配（热重编 -p:AssemblyName 换名场景），
// 否则取 mtime 最新的一个（run/check 通用场景）。
struct ScriptOutput {
    fs::path runtimeConfig;
    fs::path assemblyDll;
    std::string assemblyName;
};

std::optional<ScriptOutput> locateOutput(const fs::path& projectDir, const std::string& preferStem) {
    const fs::path outDir = projectDir / "bin" / "Debug" / "net8.0";
    std::error_code ec;
    if (!fs::exists(outDir, ec)) return std::nullopt;

    std::optional<ScriptOutput> best;
    bool haveBest = false;
    fs::file_time_type bestTime{};
    for (const auto& entry : fs::directory_iterator(outDir, ec)) {
        // 注意：extension() 只取最后一个点（".json"），runtimeconfig 判断用文件名 ends_with
        const std::string fname = entry.path().filename().string();
        if (fname.size() <= strlen(".runtimeconfig.json") ||
            fname.compare(fname.size() - strlen(".runtimeconfig.json"), strlen(".runtimeconfig.json"),
                          ".runtimeconfig.json") != 0)
            continue;
        ScriptOutput out;
        out.runtimeConfig = entry.path();
        out.assemblyName = fname.substr(0, fname.size() - strlen(".runtimeconfig.json"));
        // 注意不能 replace_extension（会把最后的 ".json" 换掉）：DLL = 同目录 / 程序集名 + ".dll"
        out.assemblyDll = entry.path().parent_path() / (out.assemblyName + ".dll");
        if (!fs::exists(out.assemblyDll, ec)) continue;
        if (!preferStem.empty() && out.assemblyName != preferStem) continue;
        auto t = fs::last_write_time(entry.path(), ec);
        if (!preferStem.empty()) return out; // 精确匹配优先，命中即返回
        if (!haveBest || t > bestTime) {
            best = out;
            bestTime = t;
            haveBest = true;
        }
    }
    return best;
}

// dotnet build（构建期编译，plan 口径：不做 Roslyn 运行时编译）。
// DOTNET_CLI_UI_LANGUAGE=en：CLI 摘要（已成功生成/警告/错误）统一英文——
// M-SC0 踩坑 1 复发修正：中文摘要在证据图 Consolas 下成方框。
bool dotnetBuild(const std::string& projectDir, const std::string& extraArgs) {
    const std::string cmd = "set DOTNET_CLI_UI_LANGUAGE=en&& dotnet build \"" + projectDir +
                            "\" -nologo -v q " + extraArgs;
    std::printf("  $ %s\n", cmd.c_str());
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::printf("  dotnet build failed (exit %d)\n", rc);
        return false;
    }
    return true;
}

int cmdCheck(const fs::path& projectDir) {
    if (!dotnetBuild(projectDir.string(), {})) return 1;
    if (!locateOutput(projectDir, {})) {
        std::printf("check: FAIL no *.runtimeconfig.json found under bin/Debug/net8.0\n");
        return 1;
    }
    std::printf("check: PASS %s\n", projectDir.string().c_str());
    return 0;
}

int cmdRun(const fs::path& projectDir, const std::string& args, const std::string& typeOverride,
           bool passApiTable) {
    if (!dotnetBuild(projectDir.string(), {})) return 1;
    auto out = locateOutput(projectDir, {});
    if (!out) {
        std::printf("run: FAIL no build output found\n");
        return 1;
    }

    nl::script::Result result;
    auto vm = nl::script::Vm::create(out->runtimeConfig.string(), &result);
    if (!vm) {
        std::printf("run: FAIL vm init: code=%d %s\n", result.code, result.message.c_str());
        return 1;
    }

    // 类型名默认约定：RootNamespace.<工程目录名首字母大写>Script, AssemblyName
    // （csproj 双锚：hello->HelloScript / console->ConsoleScript / ecs->EcsScript）。
    std::string stem = projectDir.filename().string();
    if (!stem.empty()) stem[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(stem[0])));
    const std::string typeName =
        typeOverride.empty() ? ("NlScript.Examples." + stem + "Script, " + out->assemblyName) : typeOverride;

    nl::script::EntryPointFn entry = nullptr;
    result = vm->getEntryPoint(out->assemblyDll.string(), typeName, "EntryNative", &entry);
    if (!result.ok) {
        std::printf("run: FAIL getEntryPoint: code=%d %s\n", result.code, result.message.c_str());
        return 1;
    }

    // 绑定脚本（M-SC1/M-SC2）约定 args = nl::script::ApiTable*；普通脚本 args = 命令行串。
    if (passApiTable) {
        // 宿主侧预注册（生产宿主注册自家 cvar / 绑 World 的口径；脚本由此可 get/set/exec/spawn）。
        auto& console = nl::core::console::Console::get();
        console.installBuiltins();
        console.registerInt("script.count", 0, "demo cvar bound to script (M-SC1)");
        console.registerFloat("script.rate", 1.5, "demo float cvar (M-SC1)");
        static nl::scene::World world;   // 绑定脚本生命周期 = 本工具进程
        nl::script::ecsSetWorld(&world);
    }
    void* argPtr = passApiTable ? const_cast<void*>(static_cast<const void*>(nl::script::consoleApiTable()))
                                : (args.empty() ? nullptr : const_cast<char*>(args.c_str()));
    const std::int32_t argSize = passApiTable ? 0 : static_cast<std::int32_t>(args.size());
    std::int32_t ret = 0;
    std::uint32_t excCode = 0;
    if (guardedCall(entry, argPtr, argSize, &ret, &excCode) != 0) {
        std::printf("run: FAIL managed exception at boundary: SEH code=0x%08x\n", excCode);
        return 1;
    }
    std::printf("run: ret=%d\n", ret);
    return 0;
}

// ---- selftest ----

int cmdSelftest(const fs::path& projectDir, const fs::path& consoleDir, const fs::path& ecsDir,
                const fs::path& bootstrapDir, const fs::path& timeDir, const fs::path& camDir,
                const fs::path& modelDir, const fs::path& driveDir, const fs::path& propsDir,
                const fs::path& mouseDir, const fs::path& gamepadDir) {
    std::printf("nl-script selftest: project=%s\n", projectDir.string().c_str());

    // 编译示例工程（热重编测试的工作目录也由此保证存在）
    if (!dotnetBuild(projectDir.string(), {})) return 1;

    auto base = locateOutput(projectDir, {});
    expectTrue(base.has_value(), "s0 locate HelloScript build output (runtimeconfig + dll)");
    if (!base) return 1;

    // s1 装载运行时（官方 3 导出口径：init_for_runtime_config -> delegate -> [close]）
    nl::script::Result result;
    auto vm = nl::script::Vm::create(base->runtimeConfig.string(), &result);
    expectTrue(vm != nullptr, "s1 Vm::create via runtimeconfig.json (hostfxr 3-export flow)");
    if (!vm) {
        std::printf("  init detail: code=%d %s\n", result.code, result.message.c_str());
        return 1;
    }

    // s2 取默认签名入口（[UnmanagedCallersOnly] + UNMANAGEDCALLERSONLY_METHOD）
    const std::string typeName = "NlScript.Examples.HelloScript, " + base->assemblyName;
    nl::script::EntryPointFn entry = nullptr;
    result = vm->getEntryPoint(base->assemblyDll.string(), typeName, "EntryNative", &entry);
    expectTrue(result.ok && entry != nullptr, "s2 getEntryPoint Entry (default signature, UnmanagedCallersOnly)");
    if (!result.ok) {
        std::printf("  entry detail: code=%d %s\n", result.code, result.message.c_str());
        return 1;
    }

    // s3 返回值锚
    auto call = [&](const char* mode) {
        std::int32_t ret = 0;
        std::uint32_t exc = 0;
        std::int32_t size = mode ? static_cast<std::int32_t>(std::strlen(mode)) : 0;
        const int hit = guardedCall(entry, mode ? const_cast<char*>(mode) : nullptr, size, &ret, &exc);
        return std::pair<int, std::int32_t>{hit, hit == 0 ? ret : -1000 - static_cast<std::int32_t>(exc)};
    };
    expectInt(call("ok").second, 42, "s3a args=\"ok\" -> 42");
    expectInt(call("").second, -1, "s3b empty args -> -1");
    expectInt(call("echo").second, 0, "s3c args=\"echo\" -> 0 (script-api Version printed above)");
    expectInt(call("unknown-mode").second, -1, "s3d unknown mode -> -1");

    // s4 托管异常边界捕获：进程不崩，SEH 码转错误
    {
        auto [hit, val] = call("throw");
        expectTrue(hit != 0, "s4a managed exception caught at SEH boundary (process alive)");
        std::printf("  exception code: 0x%08x (CLR managed exception 0xe0434352)\n",
                    static_cast<unsigned>(-1000 - val));
    }
    // 边界之后入口仍可调用（异常未污染状态）
    expectInt(call("ok").second, 42, "s4b entry callable again after exception");

    // s5 热重编行为更新：拷贝工程 -> 改 42->43 -> 换名重编 -> 同进程装载新 DLL
    {
        std::error_code ec;
        const fs::path hotRoot = fs::temp_directory_path(ec) /
            ("nl-script-selftest-hot-" + std::to_string(_getpid()));
        fs::remove_all(hotRoot, ec);
        fs::create_directories(hotRoot, ec);
        // 只拷工程源文件（csproj + .cs），跳过 bin/obj：带 obj 的 NuGet 缓存会让
        // restore 撞出 "Ambiguous project name 'HelloScriptHot'"（M-SC0 踩坑）
        fs::create_directories(hotRoot / "hello", ec);
        for (const auto& entry : fs::directory_iterator(projectDir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            fs::copy(entry.path(), hotRoot / "hello" / entry.path().filename(), ec);
        }
        expectTrue(!ec, "s5a copy example project to temp dir");

        // ProjectReference 是仓库相对路径，拷到 temp 后断链 → 改写成绝对路径
        const fs::path apiCsproj = projectDir / "../../script-api/NlScriptApi.csproj";
        const fs::path hotCsproj = hotRoot / "hello" / "HelloScript.csproj";
        std::ifstream cin_(hotCsproj, std::ios::binary);
        std::string csprojText((std::istreambuf_iterator<char>(cin_)), std::istreambuf_iterator<char>());
        cin_.close();
        const std::size_t refPos = csprojText.find("../../script-api/NlScriptApi.csproj");
        expectTrue(refPos != std::string::npos, "s5a2 locate ProjectReference in copied csproj");
        if (refPos != std::string::npos) {
            csprojText.replace(refPos, strlen("../../script-api/NlScriptApi.csproj"),
                               apiCsproj.lexically_normal().string());
            std::ofstream cout_(hotCsproj, std::ios::binary | std::ios::trunc);
            cout_ << csprojText;
            cout_.close();
        }
        // AssemblyName 必须在 csproj 里改，不能用 -p:AssemblyName 全局属性：
        // 全局属性会连带被 ProjectReference 的 script-api 一起改名 →
        // NuGet restore "Ambiguous project name"（M-SC0 踩坑）
        {
            std::ifstream cin2(hotCsproj, std::ios::binary);
            std::string t2((std::istreambuf_iterator<char>(cin2)), std::istreambuf_iterator<char>());
            cin2.close();
            const std::size_t ap = t2.find("<AssemblyName>HelloScript</AssemblyName>");
            expectTrue(ap != std::string::npos, "s5a3 set hot AssemblyName in copied csproj");
            if (ap != std::string::npos) {
                t2.replace(ap, strlen("<AssemblyName>HelloScript</AssemblyName>"),
                           "<AssemblyName>HelloScriptHot</AssemblyName>");
                std::ofstream cout2(hotCsproj, std::ios::binary | std::ios::trunc);
                cout2 << t2;
            }
        }

        // 源码改动：42 -> 43（脚本改动）
        const fs::path cs = hotRoot / "hello" / "HelloScript.cs";
        std::ifstream in(cs, std::ios::binary);
        std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const std::size_t pos = src.find("return 42;");
        expectTrue(pos != std::string::npos, "s5b locate anchor \"return 42;\" in copied source");
        if (pos != std::string::npos) {
            src.replace(pos, 10, "return 43;");
            std::ofstream out(cs, std::ios::binary | std::ios::trunc);
            out << src;
            out.close();

            // dotnet build 重编（csproj 内已换 AssemblyName，避免同标识程序集复用旧映像）
            expectTrue(dotnetBuild((hotRoot / "hello").string(), {}),
                       "s5c dotnet build rebuilt script (HelloScriptHot)");
            auto hot = locateOutput(hotRoot / "hello", "HelloScriptHot");
            expectTrue(hot.has_value(), "s5d locate rebuilt output");
            if (hot) {
                nl::script::EntryPointFn hotEntry = nullptr;
                result = vm->getEntryPoint(hot->assemblyDll.string(),
                                           "NlScript.Examples.HelloScript, HelloScriptHot",
                                           "EntryNative", &hotEntry);
                expectTrue(result.ok, "s5e load rebuilt assembly in same process (no host restart)");
                if (result.ok) {
                    std::int32_t ret = 0;
                    std::uint32_t exc = 0;
                    guardedCall(hotEntry, const_cast<char*>("ok"), 2, &ret, &exc);
                    expectInt(ret, 43, "s5f rebuilt script behavior updated: \"ok\" -> 43");
                }
            }
        }
        fs::remove_all(hotRoot, ec);
    }

    // ---- s6 console 绑定（M-SC1；consoleDir 为空 = 上游分支未合入本轮范围，跳过）----
    if (!consoleDir.empty()) {
        if (!dotnetBuild(consoleDir.string(), {})) {
            expectTrue(false, "s6a dotnet build console example");
            std::printf("nl-script selftest: FAIL (console binding build)\n");
            return 1;
        }
        auto cso = locateOutput(consoleDir, {});
        expectTrue(cso.has_value(), "s6a locate ConsoleScript build output");
        if (cso) {
            const std::string csType = "NlScript.Examples.ConsoleScript, " + cso->assemblyName;
            // 绑定入口取用帮手（args = ApiTable*）
            auto callEntry = [&](nl::script::EntryPointFn fn) {
                std::int32_t ret = 0;
                std::uint32_t exc = 0;
                const void* table = nl::script::consoleApiTable();
                const int hit = guardedCall(fn, const_cast<void*>(table), 0, &ret, &exc);
                return std::pair<int, std::int32_t>{hit, hit == 0 ? ret : -2000 - static_cast<std::int32_t>(exc)};
            };

            // C++ 侧注册脚本绑定的 cvar（onChanged 钩子记录，验收「C# 改 cvar 触发 onChanged」）
            auto& console = nl::core::console::Console::get();
            console.installBuiltins();   // 生产宿主口径：内建命令集（cvar.set / cvar.list / help）
            int onChangedCount = 0;
            std::string onChangedValue;
            nl::core::console::CVar& countCvar = nl::core::console::Console::get().registerInt(
                "script.count", 0, "cvar bound to script (M-SC1 selftest)");
            countCvar.setOnChanged([&](const nl::core::console::CVar& c) {
                ++onChangedCount;
                onChangedValue = c.value().toString();
            });
            nl::core::console::Console::get().registerFloat(
                "script.rate", 1.5, "float cvar for readback (M-SC1 selftest)");

            nl::script::EntryPointFn setEntry = nullptr, regEntry = nullptr, errEntry = nullptr,
                                     throwEntry = nullptr, readEntry = nullptr, unregEntry = nullptr;
            result = vm->getEntryPoint(cso->assemblyDll.string(), csType, "SetCvar", &setEntry);
            result = vm->getEntryPoint(cso->assemblyDll.string(), csType, "RegisterCmd", &regEntry);
            result = vm->getEntryPoint(cso->assemblyDll.string(), csType, "ErrorCodes", &errEntry);
            result = vm->getEntryPoint(cso->assemblyDll.string(), csType, "ThrowCmd", &throwEntry);
            result = vm->getEntryPoint(cso->assemblyDll.string(), csType, "Readback", &readEntry);
            result = vm->getEntryPoint(cso->assemblyDll.string(), csType, "UnregisterCmd", &unregEntry);
            expectTrue(result.ok && unregEntry != nullptr, "s6a locate console binding entries (UnmanagedCallersOnly)");
            if (!result.ok) {
                std::printf("  entry detail: code=%d %s\n", result.code, result.message.c_str());
            } else {
                // s6a：C# 改 cvar -> onChanged 触发 + CvarSet 码 0
                {
                    auto [hit, ret] = callEntry(setEntry);
                    expectInt(hit == 0 ? ret : -2000, 0, "s6a C# CvarSet script.count=77 -> 0");
                    expectInt(onChangedCount, 1, "s6a C++ onChanged hook fired exactly once");
                    expectTrue(onChangedValue == "77", "s6a onChanged captured value 77");
                }
                // s6b：C# 注册命令 -> console exec 调通
                {
                    expectInt(callEntry(regEntry).second, 0, "s6b C# ConsoleRegister script.echo -> 0");
                    const auto r = nl::core::console::Console::get().exec("script.echo hello world");
                    expectTrue(r.ok() && r.output.find("hello world") != std::string::npos,
                               "s6b console exec of C#-registered command returns echo text");
                }
                // s6c：错误码逐一对齐（C# 视角触发 0/1/2/3，返回位掩码 31）
                expectInt(callEntry(errEntry).second, 31,
                          "s6c ExecCode alignment 0/1/2/3 across binding (mask 31)");
                // s6d：托管异常命令 -> SEH 边界捕获 -> CommandFailed，进程存活
                {
                    expectInt(callEntry(throwEntry).second, 0, "s6d C# ConsoleRegisterRaw throwcmd -> 0");
                    const auto r = nl::core::console::Console::get().exec("script.throwcmd");
                    expectTrue(r.code == nl::core::console::ExecCode::CommandFailed &&
                               r.output.find("SEH code=0xe0434352") != std::string::npos,
                               "s6d managed exception in command -> CommandFailed + SEH captured");
                    const auto r2 = nl::core::console::Console::get().exec("script.echo alive");
                    expectTrue(r2.ok() && r2.output.find("alive") != std::string::npos,
                               "s6d console alive and commands still callable after exception");
                }
                // s6e：双向读回（C++ 类型化赋值 -> C# 读；cvar.list 全量视图）
                expectInt(callEntry(readEntry).second, 3,
                          "s6e C++-set float read from C# + cvar.list contains script cvar");
                // s6f：注销命令 -> 解绑（M-SC3 热重载预演）
                {
                    expectInt(callEntry(unregEntry).second, 0, "s6f C# ConsoleUnregister script.echo -> 0");
                    const auto r = nl::core::console::Console::get().exec("script.echo gone");
                    expectTrue(r.code == nl::core::console::ExecCode::UnknownCommand,
                               "s6f exec after unregister -> UnknownCommand");
                }
            }
        }
    }

    // ---- s7 ECS 数据面绑定（M-SC2；ecsDir 为空 = 跳过，同 s6 口径）----
    if (!ecsDir.empty()) {
        if (!dotnetBuild(ecsDir.string(), {})) {
            expectTrue(false, "s7a dotnet build ecs example");
            std::printf("nl-script selftest: FAIL (ecs binding build)\n");
            return 1;
        }
        auto eso = locateOutput(ecsDir, {});
        expectTrue(eso.has_value(), "s7a locate EcsScript build output");
        if (eso) {
            const std::string esType = "NlScript.Examples.EcsScript, " + eso->assemblyName;
            nl::script::EntryPointFn spawnEntry = nullptr, readEntry = nullptr,
                                     probeEntry = nullptr, removeEntry = nullptr;
            result = vm->getEntryPoint(eso->assemblyDll.string(), esType, "SpawnSetup", &spawnEntry);
            result = vm->getEntryPoint(eso->assemblyDll.string(), esType, "ReadVerify", &readEntry);
            result = vm->getEntryPoint(eso->assemblyDll.string(), esType, "InvalidProbe", &probeEntry);
            result = vm->getEntryPoint(eso->assemblyDll.string(), esType, "RemoveSpawned", &removeEntry);
            expectTrue(result.ok && removeEntry != nullptr, "s7a locate ecs binding entries (UnmanagedCallersOnly)");
            if (!result.ok) {
                std::printf("  entry detail: code=%d %s\n", result.code, result.message.c_str());
            } else {
                const nl::script::ApiTable* table = nl::script::consoleApiTable();
                // 绑定表导出的强类型视图（C++ 侧经同一张表调用 = 与 C# 完全同路径）
                using SpawnFnT = int(__cdecl*)(const char*, std::uint64_t*);
                using RemoveFnT = int(__cdecl*)(std::uint64_t);
                using FlushFnT = int(__cdecl*)();
                using ExistsFnT = int(__cdecl*)(std::uint64_t, int*);
                using SetParentFnT = int(__cdecl*)(std::uint64_t, std::uint64_t);
                using GetParentFnT = int(__cdecl*)(std::uint64_t, std::uint64_t*);
                using AddTFnT = int(__cdecl*)(std::uint64_t);
                using TGetFnT = int(__cdecl*)(std::uint64_t, float*, float*, float*);
                using TSetFnT = int(__cdecl*)(std::uint64_t, const float*, const float*, const float*);
                using FindFnT = int(__cdecl*)(const char*, std::uint64_t*);
                const auto ecsSpawn = reinterpret_cast<SpawnFnT>(table->ecsSpawn);
                const auto ecsRemove = reinterpret_cast<RemoveFnT>(table->ecsRemove);
                const auto ecsFlush = reinterpret_cast<FlushFnT>(table->ecsFlush);
                const auto ecsExists = reinterpret_cast<ExistsFnT>(table->ecsExists);
                const auto ecsSetParent = reinterpret_cast<SetParentFnT>(table->ecsSetParent);
                const auto ecsGetParent = reinterpret_cast<GetParentFnT>(table->ecsGetParent);
                const auto ecsAddTransform = reinterpret_cast<AddTFnT>(table->ecsAddTransform);
                const auto ecsTransformGet = reinterpret_cast<TGetFnT>(table->ecsTransformGet);
                const auto ecsTransformSet = reinterpret_cast<TSetFnT>(table->ecsTransformSet);
                const auto ecsFindByName = reinterpret_cast<FindFnT>(table->ecsFindByName);

                auto callEntry = [&](nl::script::EntryPointFn fn) {
                    std::int32_t ret = 0;
                    std::uint32_t exc = 0;
                    const int hit = guardedCall(fn, const_cast<void*>(static_cast<const void*>(table)),
                                                0, &ret, &exc);
                    return std::pair<int, std::int32_t>{hit, hit == 0 ? ret : -2000 - static_cast<std::int32_t>(exc)};
                };

                // 每轮新 World + 绑定（selftest 隔离；生产宿主同款时序：绑 World -> 调入口）
                static nl::scene::World world;
                nl::script::ecsSetWorld(&world);

                // C++ 侧预置 cppEnt（经绑定表走 = 与 C# 同路径）：挂 Transform + 锚值
                {
                    std::uint64_t cppEnt = 0;
                    expectInt(ecsSpawn("cppEnt", &cppEnt), 0, "s7-pre C++ spawn cppEnt via table");
                    expectInt(ecsAddTransform(cppEnt), 0, "s7-pre C++ addTransform cppEnt");
                    const float pos[3] = {10.f, -20.f, 30.f};
                    const float rot[4] = {0.f, 0.f, 0.7071067811865476f, 0.7071067811865476f};
                    const float scl[3] = {0.5f, 1.5f, 3.f};
                    expectInt(ecsTransformSet(cppEnt, pos, rot, scl), 0, "s7-pre C++ set anchor TRS");
                }

                // s7a：C# spawn + 挂 Transform + 改字段 -> C++ 读回一致
                {
                    auto [hit, ret] = callEntry(spawnEntry);
                    expectInt(hit == 0 ? ret : -2000, 0, "s7a C# SpawnSetup -> 0");

                    // 宿主 tick 边界 flush（Carrot 口径：spawn 排队、flush 后进全局表，
                    // findEntityByName 走全局表——脚本 spawn 完宿主 flush 再查，同生产时序）
                    expectInt(ecsFlush(), 0, "s7a host flush after script spawn");

                    std::uint64_t root = 0, leaf = 0;
                    expectInt(ecsFindByName("scriptRoot", &root), 0, "s7a C++ find scriptRoot");
                    expectInt(ecsFindByName("scriptLeaf", &leaf), 0, "s7a C++ find scriptLeaf");
                    expectTrue(root != 0 && leaf != 0 && root != leaf, "s7a both entities exist with distinct ids");

                    std::uint64_t parent = 0;
                    expectInt(ecsGetParent(leaf, &parent), 0, "s7a getparent leaf -> ok");
                    expectTrue(parent == root, "s7a leaf parent == root id");

                    float pos[3] = {}, rot[4] = {}, scl[3] = {};
                    expectInt(ecsTransformGet(root, pos, rot, scl), 0, "s7a transformGet scriptRoot -> 0");
                    expectTrue(pos[0] == 1.f && pos[1] == 2.f && pos[2] == 3.f, "s7a pos anchor (1,2,3) exact");
                    expectTrue(rot[0] == 0.5f && rot[1] == 0.5f && rot[2] == 0.5f && rot[3] == 0.5f,
                               "s7a quat anchor (0.5,0.5,0.5,0.5) exact");
                    expectTrue(scl[0] == 2.f && scl[1] == 2.f && scl[2] == 2.f, "s7a scale anchor (2,2,2) exact");
                    expectInt(ecsTransformGet(leaf, pos, rot, scl), 0, "s7a transformGet scriptLeaf -> 0");
                    expectTrue(pos[0] == -1.f && pos[1] == -2.f && pos[2] == -3.f, "s7a leaf pos anchor (-1,-2,-3) exact");
                }

                // s7b：C++ 改字段（预置锚）-> C# 读回一致
                expectInt(callEntry(readEntry).second, 0, "s7b C# ReadVerify cppEnt anchors -> 0");

                // s7c：失效实体全操作拒绝（NoEntity，不崩）
                expectInt(callEntry(probeEntry).second, 0, "s7c C# InvalidProbe all rejected -> 0");

                // s7d：延迟删除 + flush 后 exists 假 + 绑定表 TransformGet 拒绝
                {
                    std::uint64_t leaf = 0;
                    ecsFindByName("scriptLeaf", &leaf);
                    expectInt(callEntry(removeEntry).second, 0, "s7d C# RemoveSpawned(scriptLeaf) -> 0");
                    int alive = 1;
                    ecsExists(leaf, &alive);
                    expectTrue(alive == 1, "s7d deferred: still exists before flush (Carrot asymmetry)");
                    expectInt(ecsFlush(), 0, "s7d flush -> 0");
                    ecsExists(leaf, &alive);
                    expectTrue(alive == 0, "s7d after flush exists -> false");
                    float pos[3] = {}, rot[4] = {}, scl[3] = {};
                    expectInt(ecsTransformGet(leaf, pos, rot, scl), nl::script::kEcsNoEntity,
                              "s7d TransformGet on flushed entity -> NoEntity (safe reject)");
                }

                // s7e：父子关系经绑定表 roundtrip（解除 + 重挂 + 子视图一致）
                {
                    std::uint64_t root = 0;
                    ecsFindByName("scriptRoot", &root);
                    expectInt(ecsSetParent(root, 0), 0, "s7e clear parent -> 0");
                    std::uint64_t parent = 12345;
                    expectInt(ecsGetParent(root, &parent), 0, "s7e getparent root -> 0");
                    expectTrue(parent == 0, "s7e root parent cleared");
                    std::uint64_t cppEnt = 0;
                    ecsFindByName("cppEnt", &cppEnt);
                    expectInt(ecsSetParent(root, cppEnt), 0, "s7e reparent root -> cppEnt");
                    ecsGetParent(root, &parent);
                    expectTrue(parent == cppEnt, "s7e root parent == cppEnt");
                    const auto kids = world.getChildIDs(cppEnt);
                    bool found = false;
                    for (const auto k : kids) found |= (k == root);
                    expectTrue(found, "s7e getChildIDs(cppEnt) view contains root");
                }

                // s7f：JSON roundtrip 不受脚本侧改动影响（序列化字节一致）
                {
                    const auto a = world.serialise().dump();
                    nl::scene::World clone;
                    clone.getComponentLibrary().registerComponent<nl::scene::TransformComponent>();
                    std::string err;
                    expectTrue(clone.deserialise(nlohmann::json::parse(a), err),
                               "s7f deserialise world snapshot -> true");
                    if (!err.empty()) std::printf("  s7f detail: %s\n", err.c_str());
                    expectTrue(clone.serialise().dump() == a, "s7f serialise roundtrip bytes equal");
                }

                // s7g：解绑 World 后导出一律 NoWorld（M-SC3 热重载窗口兜底）
                {
                    nl::script::ecsSetWorld(nullptr);
                    std::uint64_t id = 0;
                    expectInt(ecsSpawn("nope", &id), nl::script::kEcsNoWorld,
                              "s7g spawn after unbind -> NoWorld");
                    expectInt(ecsFlush(), nl::script::kEcsNoWorld, "s7g flush after unbind -> NoWorld");
                    int alive = 0;
                    expectInt(ecsExists(1, &alive), nl::script::kEcsNoWorld, "s7g exists after unbind -> NoWorld");
                }
            }
        }
    }

    // ---- s8 脚本热更（M-SC3；bootstrapDir 为空 = 跳过，同 s6/s7 口径）----
    if (!bootstrapDir.empty()) {
        // 前置：bootstrap 编译 + ScriptHost::init（幂等；含 console.reload-script 注册）
        if (!dotnetBuild(bootstrapDir.string(), {})) {
            expectTrue(false, "s8-pre dotnet build bootstrap");
            std::printf("nl-script selftest: FAIL (bootstrap build)\n");
            return 1;
        }
        auto bso = locateOutput(bootstrapDir, "NlScriptBootstrap");
        expectTrue(bso.has_value(), "s8-pre locate NlScriptBootstrap build output");
        if (bso) {
            nl::script::Result ir = nl::script::ScriptHost::get().init(vm.get(), bso->assemblyDll.string());
            expectTrue(ir.ok, "s8a ScriptHost::init (Vm::loadBootstrap 3 function pointers, idempotent)");
            if (!ir.ok) {
                std::printf("  init detail: code=%d %s\n", ir.code, ir.message.c_str());
            } else {
                // console.reload-script 已注册：无脚本时 exec 返回 CommandFailed + 提示文本
                {
                    const auto r = nl::core::console::Console::get().exec("console.reload-script");
                    expectTrue(r.code == nl::core::console::ExecCode::CommandFailed &&
                               r.output.find("no script loaded") != std::string::npos,
                               "s8a2 console.reload-script registered (no-script case -> CommandFailed hint)");
                }

                // 热拷贝 console 工程到 temp（只源文件，ProjectReference 改绝对路径；
                // 不改 AssemblyName —— 本里程碑验收「同名 DLL 覆盖」工作流，与 s5 换名口径对照）
                std::error_code ec;
                const fs::path hotRoot = fs::temp_directory_path(ec) /
                    ("nl-script-selftest-hot-s8-" + std::to_string(_getpid()));
                fs::remove_all(hotRoot, ec);
                fs::create_directories(hotRoot / "console", ec);
                for (const auto& entry : fs::directory_iterator(consoleDir, ec)) {
                    if (!entry.is_regular_file(ec)) continue;
                    fs::copy(entry.path(), hotRoot / "console" / entry.path().filename(), ec);
                }
                expectTrue(!ec, "s8b copy console project to temp (sources only)");
                const fs::path apiCsproj = consoleDir / "../../script-api/NlScriptApi.csproj";
                const fs::path hotCsproj = hotRoot / "console" / "ConsoleScript.csproj";
                {
                    std::ifstream cin_(hotCsproj, std::ios::binary);
                    std::string csprojText((std::istreambuf_iterator<char>(cin_)),
                                           std::istreambuf_iterator<char>());
                    cin_.close();
                    const std::size_t refPos = csprojText.find("../../script-api/NlScriptApi.csproj");
                    expectTrue(refPos != std::string::npos, "s8b2 locate ProjectReference in copied csproj");
                    if (refPos != std::string::npos) {
                        csprojText.replace(refPos, strlen("../../script-api/NlScriptApi.csproj"),
                                           apiCsproj.lexically_normal().string());
                        std::ofstream cout_(hotCsproj, std::ios::binary | std::ios::trunc);
                        cout_ << csprojText;
                        cout_.close();
                    }
                }

                const fs::path hotConsole = hotRoot / "console";
                expectTrue(dotnetBuild(hotConsole.string(), {}), "s8b3 dotnet build hot console (v1)");
                auto hot = locateOutput(hotConsole, {});
                expectTrue(hot.has_value(), "s8b4 locate v1 build output");
                if (hot) {
                    // 首次装载：LoadFromStream 进 collectible ALC + Entry 重绑定
                    //（Entry 注册 script.echo + 设 script.count=77）
                    nl::script::Result load = nl::script::ScriptHost::get().loadScript(
                        hot->assemblyDll.string(), "NlScript.Examples.ConsoleScript");
                    expectTrue(load.ok, "s8b5 first load via collectible ALC + Entry rebinding");
                    if (!load.ok) std::printf("  load detail: code=%d %s\n", load.code, load.message.c_str());

                    // s8b 判据：v1 行为锚（echo 前缀 = "script.echo: "）+ 脚本命令登记
                    const auto r1 = nl::core::console::Console::get().exec("script.echo v1 hello");
                    expectTrue(r1.ok() && r1.output.find("script.echo: v1 hello") != std::string::npos &&
                               r1.output.find("[v2]") == std::string::npos,
                               "s8b6 v1 echo anchor (prefix \"script.echo: \")");
                    const auto regNames = nl::script::scriptRegisteredCommands();
                    bool hasEcho = false;
                    for (const auto& n : regNames) hasEcho |= (n == "script.echo");
                    expectTrue(hasEcho, "s8b7 script command registered in C++ ledger");

                    // C++ 状态锚（reload 不许动）：script.rate 由 C++ 设 2.5（脚本不碰）+
                    // 实体 cppEnt（挂 Transform 锚值；World 绑定后 ECS 导出全程可用）
                    static nl::scene::World world8;   // 绑定脚本生命周期 = 本工具进程
                    nl::script::ecsSetWorld(&world8);
                    auto& console8 = nl::core::console::Console::get();
                    auto* rateCvar = console8.findCVar("script.rate");
                    expectTrue(rateCvar != nullptr,
                               "s8-pre3 script.rate cvar present (registered by s6 host preset)");
                    if (rateCvar) rateCvar->setFloat(2.5);
                    std::uint64_t cppEnt8 = 0;
                    {
                        using SpawnFnT = int(__cdecl*)(const char*, std::uint64_t*);
                        using AddTFnT = int(__cdecl*)(std::uint64_t);
                        using TSetFnT = int(__cdecl*)(std::uint64_t, const float*, const float*, const float*);
                        const nl::script::ApiTable* table = nl::script::consoleApiTable();
                        const auto sp = reinterpret_cast<SpawnFnT>(table->ecsSpawn);
                        const auto at = reinterpret_cast<AddTFnT>(table->ecsAddTransform);
                        const auto ts = reinterpret_cast<TSetFnT>(table->ecsTransformSet);
                        sp("cppEntHot", &cppEnt8);
                        at(cppEnt8);
                        const float pos[3] = {7.f, 8.f, 9.f};
                        ts(cppEnt8, pos, nullptr, nullptr);
                    }
                    expectTrue(cppEnt8 != 0, "s8-pre2 C++ state anchor: cppEntHot spawned with transform");

                    // s8c：改脚本源（echo 前缀 v1 -> [v2]）-> dotnet build 同名覆盖
                    //（LoadFromStream 不锁文件，构建可写——本里程碑关键通路）
                    const fs::path cs = hotConsole / "ConsoleScript.cs";
                    std::ifstream in(cs, std::ios::binary);
                    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    in.close();
                    const std::size_t pos = src.find("\"script.echo: \" + string.Join(\" \", args)");
                    expectTrue(pos != std::string::npos, "s8c locate echo prefix anchor in copied source");
                    if (pos != std::string::npos) {
                        // 全量替换：前缀在 RegisterCmd 与 Entry 的 lambda 各出现一处——
                        // reload 后新 Entry 重注册用的是 Entry 里那份，只改第一处会让 s8d 行为锚失效
                        const std::string from = "\"script.echo: \" + string.Join(\" \", args)";
                        const std::string to = "\"[v2] script.echo: \" + string.Join(\" \", args)";
                        std::size_t at = 0;
                        int replaced = 0;
                        while ((at = src.find(from, at)) != std::string::npos) {
                            src.replace(at, from.size(), to);
                            at += to.size();
                            ++replaced;
                        }
                        expectTrue(replaced >= 2, "s8c2 echo prefix replaced in both lambdas");
                        std::ofstream out(cs, std::ios::binary | std::ios::trunc);
                        out << src;
                        out.close();

                        expectTrue(dotnetBuild(hotConsole.string(), {}),
                                   "s8c2 dotnet build overwrite same DLL name (file not locked by LoadFromStream)");

                        // ScriptHost::reloadScript：解绑 -> unload（weakref 校验）-> 重装载 -> Entry 重注册
                        std::string info;
                        const nl::script::Result rr = nl::script::ScriptHost::get().reloadScript(&info);
                        expectTrue(rr.ok, "s8c3 ScriptHost::reloadScript (unbind -> unload -> load -> rebind)");
                        if (!rr.ok) {
                            std::printf("  reload detail: code=%d %s\n", rr.code, rr.message.c_str());
                        } else {
                            std::printf("  %s\n", info.c_str());
                        }

                        // s8d：行为换新（新映像 echo 前缀 [v2]）
                        const auto r2 = nl::core::console::Console::get().exec("script.echo v2 reload");
                        expectTrue(r2.ok() && r2.output.find("[v2] script.echo: v2 reload") != std::string::npos,
                                   "s8d behavior updated after reload (echo prefix [v2])");

                        // s8e：C++ 状态无损（cvar 值 / 实体树 / 内建命令 / 命令登记表）
                        expectTrue(console8.findCVar("script.rate") &&
                                       console8.findCVar("script.rate")->value().toString() == "2.5",
                                   "s8e C++ cvar state intact (script.rate == 2.5)");
                        {
                            using TGetFnT = int(__cdecl*)(std::uint64_t, float*, float*, float*);
                            const nl::script::ApiTable* table = nl::script::consoleApiTable();
                            const auto tg = reinterpret_cast<TGetFnT>(table->ecsTransformGet);
                            float p[3] = {}, q[4] = {}, s[3] = {};
                            expectInt(tg(cppEnt8, p, q, s), 0, "s8e2 entity alive after reload");
                            expectTrue(p[0] == 7.f && p[1] == 8.f && p[2] == 9.f,
                                       "s8e3 entity transform bits intact (7,8,9)");
                        }
                        {
                            const auto r3 = console8.exec("cvar.set script.rate 2.6");
                            expectTrue(r3.ok(), "s8e4 builtin cvar.set intact after reload");
                            console8.findCVar("script.rate")->setFloat(2.5);   // 恢复锚值
                        }
                        bool regAgain = false;
                        for (const auto& n : nl::script::scriptRegisteredCommands())
                            regAgain |= (n == "script.echo");
                        expectTrue(regAgain, "s8e5 script command re-registered by new Entry (ledger)");

                        // s8f：连续 reload x3 无泄漏（同 DLL 换载链路；句柄计数漂移阈值 32）
                        {
                            DWORD h0 = 0, h1 = 0;
                            GetProcessHandleCount(GetCurrentProcess(), &h0);
                            bool allOk = true;
                            for (int i = 0; i < 3 && allOk; ++i) {
                                std::string infoN;
                                allOk = nl::script::ScriptHost::get().reloadScript(&infoN).ok;
                            }
                            expectTrue(allOk, "s8f3 consecutive reload x3 all ok");
                            GetProcessHandleCount(GetCurrentProcess(), &h1);
                            const long long drift = static_cast<long long>(h1) - static_cast<long long>(h0);
                            std::printf("  handle drift over 3 reloads: %lld (h0=%lu h1=%lu)\n",
                                        drift, static_cast<unsigned long>(h0), static_cast<unsigned long>(h1));
                            expectTrue(drift <= 32 && drift >= -32,
                                       "s8f4 no handle leak over consecutive reloads (|drift| <= 32)");
                        }

                        // s8g：显式卸载 + weakref 校验（GC 排空后完全卸载）
                        {
                            bool fully = false;
                            const nl::script::Result ur = nl::script::ScriptHost::get().unloadScript(&fully);
                            expectTrue(ur.ok && fully,
                                       "s8g unloadScript -> weakref dead (ALC fully collected)");
                        }
                        // s8h：卸载后脚本命令 UnknownCommand + C++ 状态仍可用
                        {
                            const auto r4 = nl::core::console::Console::get().exec("script.echo gone");
                            expectTrue(r4.code == nl::core::console::ExecCode::UnknownCommand,
                                       "s8h script command gone after unload -> UnknownCommand");
                            using SpawnFnT = int(__cdecl*)(const char*, std::uint64_t*);
                            const nl::script::ApiTable* table = nl::script::consoleApiTable();
                            const auto sp = reinterpret_cast<SpawnFnT>(table->ecsSpawn);
                            std::uint64_t id = 0;
                            expectInt(sp("afterUnload", &id), 0, "s8h2 ECS export still works (World bound in C++)");
                            expectTrue(console8.findCVar("script.rate") &&
                                           console8.findCVar("script.rate")->value().toString() == "2.5",
                                       "s8h3 C++ cvar intact after unload");
                        }
                    }
                    fs::remove_all(hotRoot, ec);
                }
            }
        }
    }

    // ---- s9 Time / Input / Log 绑定（M-SC5；timeDir 为空 = 跳过，同 s6/s7/s8 口径）----
    if (!timeDir.empty()) {
        if (!dotnetBuild(timeDir.string(), {})) {
            expectTrue(false, "s9-pre dotnet build timeinput example");
            std::printf("nl-script selftest: FAIL (time/input/log binding build)\n");
            return 1;
        }
        auto tso = locateOutput(timeDir, {});
        expectTrue(tso.has_value(), "s9-pre locate TimeInputScript build output");
        if (tso) {
            const std::string tiType = "NlScript.Examples.TimeInputScript, " + tso->assemblyName;
            nl::script::EntryPointFn boundEntry = nullptr, unboundEntry = nullptr;
            result = vm->getEntryPoint(tso->assemblyDll.string(), tiType, "BoundProbe", &boundEntry);
            result = vm->getEntryPoint(tso->assemblyDll.string(), tiType, "UnboundProbe", &unboundEntry);
            expectTrue(result.ok && unboundEntry != nullptr, "s9-pre locate binding entries (UnmanagedCallersOnly)");
            if (!result.ok) {
                std::printf("  entry detail: code=%d %s\n", result.code, result.message.c_str());
            } else {
                const nl::script::ApiTable* table = nl::script::consoleApiTable();
                auto callEntry9 = [&](nl::script::EntryPointFn fn) {
                    std::int32_t ret = 0;
                    std::uint32_t exc = 0;
                    const int hit = guardedCall(fn, const_cast<void*>(static_cast<const void*>(table)),
                                                0, &ret, &exc);
                    return std::pair<int, std::int32_t>{hit, hit == 0 ? ret : -2000 - static_cast<std::int32_t>(exc)};
                };

                // 宿主活实例（static：selftest 进程生命周期；生产宿主同款时序：
                // 建实例 -> 喂状态 -> 绑定 -> 调脚本入口。ActionMap 未绑 = NoSource
                // 的负向语义在 s9b 解绑路径验收——不隐式建表拍板口径）。
                static nl::core::GameClock clock9;
                static nl::core::InputState input9;
                static nl::core::ActionMap map9;
                // 动作面（键码 = M-I0 selftest 锚定数值：W=87 S=83 A=65 D=68）
                const auto aBool9 = map9.define("s9bool", nl::core::ActionMap::Type::Bool);
                map9.bindKey(aBool9, 87);
                const auto aFloat9 = map9.define("s9float", nl::core::ActionMap::Type::Float);
                map9.bindKey(aFloat9, 87, 1.0f);
                map9.bindKey(aFloat9, 83, -1.0f);
                const auto aVec9 = map9.define("s9vec2", nl::core::ActionMap::Type::Vec2);
                map9.bindAxisKey(aVec9, 68, 0, 1.0f);
                map9.bindAxisKey(aVec9, 65, 0, -1.0f);
                input9.feedKey(87, true);
                input9.feedKey(68, true);
                map9.update(input9);   // 冻结（ActionMap 帧节奏口径；脚本读冻结值）
                nl::script::timeSetClock(&clock9);
                nl::script::inputSetState(&input9);
                nl::script::inputSetActionMap(&map9);

                // 时钟锚：tick(16.6667ms) x3 → tickCount=3 / simTime=0.05（double 确定性）
                for (int i = 0; i < 3; ++i) clock9.tick(16.6667, [](double, std::uint64_t) {});

                // s9a：绑定路径全断言（C# 读锚 + 控制 + NoAction/BadFilter 负向）
                {
                    auto [hit, ret] = callEntry9(boundEntry);
                    expectInt(hit == 0 ? ret : -2000, 0, "s9a C# BoundProbe -> 0");
                    if (hit != 0 || ret != 0)
                        std::printf("  s9a detail: hit=%d ret=%d\n", hit, ret);
                }

                // C++ 侧终态核（脚本 SetState(0,-1)：unpause + 倍率 -1 哨兵保持 2.5）
                expectTrue(!clock9.paused(), "s9a2 clock unpaused after script SetState(0,-1)");
                expectTrue(clock9.timeScale() == 2.5, "s9a2 timeScale kept 2.5 (-1 sentinel)");

                // setFilter 全局生效核（C# SetFilter("nl:warn") → 全局缺省 Warn）+ 还原
                expectTrue(nl::core::log::level() == nl::core::log::Level::Warn,
                           "s9a3 C# SetFilter(\"nl:warn\") -> global default Warn");
                expectTrue(nl::core::log::filterInstruction() == "nl:warn",
                           "s9a3 filterInstruction readback norm form");
                nl::core::log::setFilter("nl:info");   // 还原（进程卫生，s9b 前复位）

                // s9b：解绑路径（nullptr = NoClock/NoSource；Log 全局面不受影响）
                nl::script::timeSetClock(nullptr);
                nl::script::inputSetState(nullptr);
                nl::script::inputSetActionMap(nullptr);
                {
                    auto [hit, ret] = callEntry9(unboundEntry);
                    expectInt(hit == 0 ? ret : -2000, 0, "s9b C# UnboundProbe -> 0");
                }
            }
        }
    }

    // ---- s10 Camera 绑定（M-SC6；camDir 为空 = 跳过，同 s6~s9 口径）----
    if (!camDir.empty()) {
        if (!dotnetBuild(camDir.string(), {})) {
            expectTrue(false, "s10-pre dotnet build camera example");
            std::printf("nl-script selftest: FAIL (camera binding build)\n");
            return 1;
        }
        auto cso = locateOutput(camDir, {});
        expectTrue(cso.has_value(), "s10-pre locate CameraScript build output");
        if (cso) {
            const std::string camType = "NlScript.Examples.CameraScript, " + cso->assemblyName;
            nl::script::EntryPointFn camBound = nullptr, camTrack = nullptr, camUnbound = nullptr;
            result = vm->getEntryPoint(cso->assemblyDll.string(), camType, "BoundProbe", &camBound);
            result = vm->getEntryPoint(cso->assemblyDll.string(), camType, "TrackProbe", &camTrack);
            result = vm->getEntryPoint(cso->assemblyDll.string(), camType, "UnboundProbe", &camUnbound);
            expectTrue(result.ok && camUnbound != nullptr, "s10-pre locate camera entries (UnmanagedCallersOnly)");
            if (!result.ok) {
                std::printf("  entry detail: code=%d %s\n", result.code, result.message.c_str());
            } else {
                const nl::script::ApiTable* table = nl::script::consoleApiTable();
                auto callEntry10 = [&](nl::script::EntryPointFn fn) {
                    std::int32_t ret = 0;
                    std::uint32_t exc = 0;
                    const int hit = guardedCall(fn, const_cast<void*>(static_cast<const void*>(table)),
                                                0, &ret, &exc);
                    return std::pair<int, std::int32_t>{hit, hit == 0 ? ret : -2000 - static_cast<std::int32_t>(exc)};
                };

                // 假适配源（语义与 viewer 适配源同构：fov clamp [20,120] / track 拒写 /
                // 罐头读回）+ 捕获槽（C++ 侧终态核：脚本写进适配源的值逐位核回）。
                static struct FakeCamSource {
                    float eye[3] = {1.5f, 2.5f, 3.5f};
                    float yaw = 30.0f, pitch = -10.0f, fov = 60.0f;
                    int mode = 0;
                    float lastSetEye[3] = {0.0f, 0.0f, 0.0f};
                    float lastYaw = 0.0f, lastPitch = 0.0f;
                    static void GetPose(void* ctx, float outEye[3], float* oy, float* op,
                                        float* of, int* om) {
                        auto& s = *static_cast<FakeCamSource*>(ctx);
                        outEye[0] = s.eye[0]; outEye[1] = s.eye[1]; outEye[2] = s.eye[2];
                        if (oy) *oy = s.yaw;
                        if (op) *op = s.pitch;
                        if (of) *of = s.fov;
                        if (om) *om = s.mode;
                    }
                    static int SetPose(void* ctx, const float e[3], float y, float p) {
                        auto& s = *static_cast<FakeCamSource*>(ctx);
                        if (s.mode == 3) return nl::script::kCamModeUnsupported;
                        s.lastSetEye[0] = e[0]; s.lastSetEye[1] = e[1]; s.lastSetEye[2] = e[2];
                        s.lastYaw = y; s.lastPitch = p;
                        return nl::script::kCamOk;
                    }
                    static float GetFov(void* ctx) { return static_cast<FakeCamSource*>(ctx)->fov; }
                    static void SetFov(void* ctx, float f) {
                        auto& s = *static_cast<FakeCamSource*>(ctx);
                        s.fov = f < 20.0f ? 20.0f : (f > 120.0f ? 120.0f : f);
                    }
                } fakeCam;
                nl::script::CameraSource camSrc{&fakeCam, &FakeCamSource::GetPose,
                                                &FakeCamSource::SetPose, &FakeCamSource::GetFov,
                                                &FakeCamSource::SetFov};
                nl::script::cameraSetSource(&camSrc);

                // s10a：绑定路径全断言（罐头读回 + 写路径 rc + fov clamp 读回链）
                {
                    auto [hit, ret] = callEntry10(camBound);
                    expectInt(hit == 0 ? ret : -2000, 0, "s10a C# BoundProbe -> 0");
                    if (hit != 0 || ret != 0)
                        std::printf("  s10a detail: hit=%d ret=%d\n", hit, ret);
                }
                // C++ 侧终态核：脚本 SetPose({4,5,6},45,15) 逐位落进适配源捕获槽
                expectTrue(fakeCam.lastSetEye[0] == 4.0f && fakeCam.lastSetEye[1] == 5.0f &&
                               fakeCam.lastSetEye[2] == 6.0f,
                           "s10a C++ capture: script SetPose eye {4,5,6}");
                expectTrue(fakeCam.lastYaw == 45.0f && fakeCam.lastPitch == 15.0f,
                           "s10a C++ capture: yaw 45 / pitch 15");
                expectTrue(fakeCam.fov == 120.0f, "s10a C++ capture: SetFov(500) clamped -> 120");

                // s10b：track 模式（mode=3）——读照常 / 写位姿 ModeUnsupported / fov 照常
                fakeCam.mode = 3;
                {
                    auto [hit, ret] = callEntry10(camTrack);
                    expectInt(hit == 0 ? ret : -2000, 0, "s10b C# TrackProbe -> 0");
                }
                expectTrue(fakeCam.fov == 60.0f, "s10b C++ capture: SetFov(60) ok in track (mode-uniform)");

                // s10c：解绑路径（nullptr = NoSource / GetFov NaN）
                fakeCam.mode = 0;
                nl::script::cameraSetSource(nullptr);
                {
                    auto [hit, ret] = callEntry10(camUnbound);
                    expectInt(hit == 0 ? ret : -2000, 0, "s10c C# UnboundProbe -> 0");
                }
            }
        }
    }

    // ---- s11 模型挂载（M-SC7；modelDir 为空 = 跳过，同 s6~s10 口径）----
    if (!modelDir.empty()) {
        if (!dotnetBuild(modelDir.string(), {})) {
            expectTrue(false, "s11-pre dotnet build model example");
            std::printf("nl-script selftest: FAIL (model binding build)\n");
            return 1;
        }
        auto mso = locateOutput(modelDir, {});
        expectTrue(mso.has_value(), "s11-pre locate ModelScript build output");
        if (mso) {
            const std::string modelType = "NlScript.Examples.ModelScript, " + mso->assemblyName;
            nl::script::EntryPointFn mBound = nullptr, mError = nullptr,
                                     mUnbound = nullptr, mDetach = nullptr;
            result = vm->getEntryPoint(mso->assemblyDll.string(), modelType, "BoundProbe", &mBound);
            result = vm->getEntryPoint(mso->assemblyDll.string(), modelType, "ErrorProbe", &mError);
            result = vm->getEntryPoint(mso->assemblyDll.string(), modelType, "UnboundProbe", &mUnbound);
            result = vm->getEntryPoint(mso->assemblyDll.string(), modelType, "DetachProbe", &mDetach);
            expectTrue(result.ok && mDetach != nullptr, "s11-pre locate model entries (UnmanagedCallersOnly)");
            if (!result.ok) {
                std::printf("  entry detail: code=%d %s\n", result.code, result.message.c_str());
            } else {
                const nl::script::ApiTable* table = nl::script::consoleApiTable();
                auto callEntry11 = [&](nl::script::EntryPointFn fn) {
                    std::int32_t ret = 0;
                    std::uint32_t exc = 0;
                    const int hit = guardedCall(fn, const_cast<void*>(static_cast<const void*>(table)),
                                                0, &ret, &exc);
                    return std::pair<int, std::int32_t>{hit, hit == 0 ? ret : -2000 - static_cast<std::int32_t>(exc)};
                };

                // 罐头 World（绑定脚本生命周期；与 s8 world8 独立——本段实体名空间自洽）
                static nl::scene::World world11;
                nl::script::ecsSetWorld(&world11);

                // s11a：挂载 + 读回 + 留态（C++ 终态核：ModelComponent 落进罐头 World）
                {
                    auto [hit, ret] = callEntry11(mBound);
                    expectInt(hit == 0 ? ret : -2000, 0, "s11a C# BoundProbe -> 0");
                    if (hit != 0 || ret != 0)
                        std::printf("  s11a detail: hit=%d ret=%d\n", hit, ret);
                }
                // newEntity 延迟落表（entitiesToAdd_ 队列，tick/flush 才进 entities_）
                // ——罐头 World 没 tick，C++ findEntityByName / C# FindByName 都扫
                // entities_，不 flush 就永远看不到。宿主显式 flush（ecsFlush 语义）。
                world11.flushEntityCreationAndRemoval();
                const nl::scene::ModelComponent* probeModel = nullptr;
                if (const auto e = world11.findEntityByName("probeModel"))
                    probeModel = world11.getComponent<nl::scene::ModelComponent>(e->getID());
                expectTrue(probeModel != nullptr && probeModel->meshFile == "cube.glb",
                           "s11a C++ capture: probeModel ModelComponent meshFile=cube.glb");
                expectTrue(probeModel != nullptr &&
                           std::fabs(probeModel->tint.x - 1.0f) < 1e-5f &&
                           std::fabs(probeModel->tint.y - 1.0f) < 1e-5f &&
                           std::fabs(probeModel->tint.z - 1.0f) < 1e-5f &&
                           std::fabs(probeModel->roughness - 1.0f) < 1e-5f &&
                           std::fabs(probeModel->metallic - 0.0f) < 1e-5f,
                           "s11a C++ capture: default tint(1,1,1)/roughness 1/metallic 0");

                // s11b：错误码逐一对齐（NoEntity=2 / BadArg=4 / BadAsset=6 /
                // NoTransform=5 / Component=3——重复挂拒绝不改状态）
                {
                    auto [hit, ret] = callEntry11(mError);
                    expectInt(hit == 0 ? ret : -2000, 0, "s11b C# ErrorProbe -> 0");
                    if (hit != 0 || ret != 0)
                        std::printf("  s11b detail: hit=%d ret=%d\n", hit, ret);
                }

                // s11c：解绑路径（nullptr = NoWorld 三导出对齐，不崩）→ 重绑
                nl::script::ecsSetWorld(nullptr);
                {
                    auto [hit, ret] = callEntry11(mUnbound);
                    expectInt(hit == 0 ? ret : -2000, 0, "s11c C# UnboundProbe -> 0");
                }
                nl::script::ecsSetWorld(&world11);

                // s11d：摘除路径 + C++ 终态核（Model 摘净、Transform 留守）
                {
                    auto [hit, ret] = callEntry11(mDetach);
                    expectInt(hit == 0 ? ret : -2000, 0, "s11d C# DetachProbe -> 0");
                }
                bool modelGone = true, transformKept = false;
                if (const auto e = world11.findEntityByName("probeModel")) {
                    modelGone = world11.getComponent<nl::scene::ModelComponent>(e->getID()) == nullptr;
                    transformKept = world11.getComponent<nl::scene::TransformComponent>(e->getID()) != nullptr;
                }
                expectTrue(modelGone, "s11d C++ capture: probeModel ModelComponent removed");
                expectTrue(transformKept, "s11d C++ capture: TransformComponent kept after detach");
            }
        }
    }

    // ---- s12 载具域（M-SC8；driveDir 为空 = 跳过，同 s6~s11 口径）----
    // 操作目标 = 真训练场：nl::physics::DebugScene build(2)（Jolt 已链 nl_core），
    // driveSetScene 直持指针（拍板 Q-SC8-1 A'）；C++ 终态核与脚本共用同一实例。
    if (!driveDir.empty()) {
        if (!dotnetBuild(driveDir.string(), {})) {
            expectTrue(false, "s12-pre dotnet build drive example");
            std::printf("nl-script selftest: FAIL (drive binding build)\n");
            return 1;
        }
        auto dso = locateOutput(driveDir, {});
        expectTrue(dso.has_value(), "s12-pre locate DriveScript build output");
        if (dso) {
            const std::string driveType = "NlScript.Examples.DriveScript, " + dso->assemblyName;
            nl::script::EntryPointFn dBound = nullptr, dUnbound = nullptr;
            result = vm->getEntryPoint(dso->assemblyDll.string(), driveType, "BoundProbe", &dBound);
            result = vm->getEntryPoint(dso->assemblyDll.string(), driveType, "UnboundProbe", &dUnbound);
            expectTrue(result.ok && dUnbound != nullptr,
                       "s12-pre locate drive entries (UnmanagedCallersOnly)");
            if (!result.ok) {
                std::printf("  entry detail: code=%d %s\n", result.code, result.message.c_str());
            } else {
                const nl::script::ApiTable* table = nl::script::consoleApiTable();
                auto callEntry12 = [&](nl::script::EntryPointFn fn) {
                    std::int32_t ret = 0;
                    std::uint32_t exc = 0;
                    const int hit = guardedCall(fn, const_cast<void*>(static_cast<const void*>(table)),
                                                0, &ret, &exc);
                    return std::pair<int, std::int32_t>{hit, hit == 0 ? ret : -2000 - static_cast<std::int32_t>(exc)};
                };

                // 真训练场（scenario 2：slot0 = 既有单车）；C++ 终态核同实例。
                nl::physics::DebugScene scene12;
                std::string err12;
                expectTrue(scene12.build(2, 12345, &err12), "s12-pre DebugScene build(2)");
                nl::script::driveSetScene(&scene12);

                // s12a：正向全链（加车 slot1 / 四输入 / FullPose 含 out-NULL 路径 /
                // teleport 逐位读回 / tombstone 移除后三导出 NoSlot——脚本内断言）
                {
                    auto [hit, ret] = callEntry12(dBound);
                    expectInt(hit == 0 ? ret : -2000, 0, "s12a C# BoundProbe -> 0");
                    if (hit != 0 || ret != 0)
                        std::printf("  s12a detail: hit=%d ret=%d\n", hit, ret);
                }
                // C++ 终态核：slot0 既有单车健在（tombstone 不波及）；脚本车已移除
                expectTrue(scene12.vehicleChassisState(0).valid,
                           "s12a C++ capture: slot0 (built-in car) still valid");
                expectTrue(!scene12.vehicleChassisState(1).valid,
                           "s12a C++ capture: script car tombstoned (slot1 invalid)");

                // s12b：未绑路径（driveSetScene(nullptr) → 五导出全 NoScene）
                nl::script::driveSetScene(nullptr);
                {
                    auto [hit, ret] = callEntry12(dUnbound);
                    expectInt(hit == 0 ? ret : -2000, 0, "s12b C# UnboundProbe -> 0");
                    if (hit != 0 || ret != 0)
                        std::printf("  s12b detail: hit=%d ret=%d\n", hit, ret);
                }
            }
        }
    }

    // ---- s13 属性面板（M-SC10；propsDir 为空 = 跳过，同 s6~s12 口径）----
    // 载体 = 入口脚本类 [Prop] 静态字段（拍板 Q-SC10-1 A）；描述表 = C# 反射推
    // POD（propsPushBegin/propsDescAdd，gen 计数）；值通道 = BootTickPtr 协议
    // 复用（PropGet/PropSet 可选契约，ScriptHost 生命周期纪律同 tickFn_）。
    if (!propsDir.empty()) {
        if (!dotnetBuild(propsDir.string(), {})) {
            expectTrue(false, "s13-pre dotnet build props example");
            std::printf("nl-script selftest: FAIL (props binding build)\n");
            return 1;
        }
        auto pso = locateOutput(propsDir, {});
        expectTrue(pso.has_value(), "s13-pre locate PropsScript build output");
        if (pso) {
            auto& host13 = nl::script::ScriptHost::get();

            // s13-pre：装载前表态（进程内无 props 推送）
            expectTrue(nl::script::scriptProps().empty(),
                       "s13-pre descriptor table empty before load");
            expectTrue(!host13.hasProps(),
                       "s13-pre props channel not attached before load");

            // 热更段热拷贝工程（s8b 同款：只拷源文件 + csproj 引用绝对化）
            std::error_code ec;
            const fs::path hotPropsRoot = fs::temp_directory_path(ec) /
                ("nl-script-selftest-props-" + std::to_string(_getpid()));
            fs::remove_all(hotPropsRoot, ec);
            fs::create_directories(hotPropsRoot / "props", ec);
            for (const auto& entry : fs::directory_iterator(propsDir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                fs::copy(entry.path(), hotPropsRoot / "props" / entry.path().filename(), ec);
            }
            expectTrue(!ec, "s13-pre copy props project to temp (sources only)");
            const fs::path apiCsproj13 = propsDir / "../../script-api/NlScriptApi.csproj";
            const fs::path hotPropsCsproj = hotPropsRoot / "props" / "PropsScript.csproj";
            {
                std::ifstream cin13(hotPropsCsproj, std::ios::binary);
                std::string csproj13((std::istreambuf_iterator<char>(cin13)),
                                     std::istreambuf_iterator<char>());
                cin13.close();
                const std::size_t refPos13 =
                    csproj13.find("../../script-api/NlScriptApi.csproj");
                expectTrue(refPos13 != std::string::npos,
                           "s13-pre locate ProjectReference in copied csproj");
                if (refPos13 != std::string::npos) {
                    csproj13.replace(refPos13, strlen("../../script-api/NlScriptApi.csproj"),
                                     apiCsproj13.lexically_normal().string());
                    std::ofstream cout13(hotPropsCsproj, std::ios::binary | std::ios::trunc);
                    cout13 << csproj13;
                    cout13.close();
                }
            }
            const fs::path hotProps = hotPropsRoot / "props";
            expectTrue(dotnetBuild(hotProps.string(), {}), "s13-pre dotnet build hot props (v1)");
            auto hotPso = locateOutput(hotProps, {});
            expectTrue(hotPso.has_value(), "s13-pre locate hot PropsScript v1 output");

            if (hotPso) {
                const std::string propsType =
                    "NlScript.Examples.PropsScript, " + hotPso->assemblyName;

                // s13a：装载（Entry 推表）→ 描述表 POD 断言 + 通道接通
                expectTrue(host13.loadScript(hotPso->assemblyDll.string(), propsType).ok,
                           "s13a loadScript (Entry pushes [Prop] descriptor table)");
                const auto& tbl13 = nl::script::scriptProps();
                expectTrue(tbl13.size() == 4,
                           "s13a descriptor table has 4 rows ([Prop] static fields)");
                expectTrue(nl::script::scriptPropsGen() >= 1,
                           "s13a descriptor gen bumped by pushBegin");
                expectTrue(host13.hasProps(),
                           "s13a props channel attached (PropGet/PropSet resolved)");
                if (tbl13.size() == 4) {
                    const auto& d0 = tbl13[0];
                    expectTrue(d0.kind == nl::script::kPropKindFloat &&
                                   std::string(d0.name) == "Speed" &&
                                   d0.min == 0.f && d0.max == 10.f && d0.def == 3.5f,
                               "s13a row0 Speed kind/Range/default bits");
                    const auto& d1 = tbl13[1];
                    expectTrue(d1.kind == nl::script::kPropKindInt &&
                                   std::string(d1.name) == "Count" &&
                                   d1.min == 0.f && d1.max == 100.f && d1.def == 7.f,
                               "s13a row1 Count kind/Range/default bits");
                    const auto& d2 = tbl13[2];
                    expectTrue(d2.kind == nl::script::kPropKindBool &&
                                   std::string(d2.name) == "Enabled" && d2.def == 1.f,
                               "s13a row2 Enabled kind/default bits");
                    const auto& d3 = tbl13[3];
                    expectTrue(d3.kind == nl::script::kPropKindString &&
                                   std::string(d3.name) == "Label",
                               "s13a row3 Label kind bits");
                }

                // s13a2：值通道往返（float/int/bool/string；C# 静态字段为真源）
                {
                    std::uint8_t buf8[8] = {};
                    float fv = 0.f;
                    expectTrue(host13.propGet(0, buf8, sizeof buf8).ok, "s13a2 propGet float ok");
                    std::memcpy(&fv, buf8 + 4, 4);
                    expectTrue(fv == 3.5f, "s13a2 Speed initial value 3.5 via channel");

                    fv = 7.25f;
                    std::memcpy(buf8 + 4, &fv, 4);
                    expectTrue(host13.propSet(0, buf8, sizeof buf8).ok, "s13a2 propSet float ok");
                    std::memset(buf8, 0, sizeof buf8);
                    expectTrue(host13.propGet(0, buf8, sizeof buf8).ok, "s13a2 propGet float after set");
                    std::memcpy(&fv, buf8 + 4, 4);
                    expectTrue(fv == 7.25f, "s13a2 Speed roundtrip 7.25");

                    int iv = 42;
                    std::memcpy(buf8 + 4, &iv, 4);
                    expectTrue(host13.propSet(1, buf8, sizeof buf8).ok, "s13a2 propSet int ok");
                    // C# 真源核：ReadField 探针经 bootstrap BootCall 读静态字段
                    //（collectible 正本——禁 getEntryPoint 直拉脚本 dll，M-GL4 口径）。
                    std::int32_t fieldVal = 0;
                    {
                        struct ReadArgs { std::int32_t idx; std::int32_t pad; void* outRaw; }
                        ra{1, 0, &fieldVal};   // ABI：idx@0 / outRaw@8（PropsScript.cs 同文锚）
                        std::int32_t retR = 0;
                        expectTrue(vm->scriptCall(propsType, "ReadField",
                                                  &ra, sizeof ra, &retR).ok &&
                                   retR == 0 && fieldVal == 42,
                                   "s13a2 C# Count static field == 42 (cross-source via ReadField)");
                    }
                    std::memcpy(buf8 + 4, &(iv = 0), 4);
                    expectTrue(host13.propSet(2, buf8, sizeof buf8).ok, "s13a2 propSet bool 0 ok");
                    expectTrue(host13.propGet(2, buf8, sizeof buf8).ok, "s13a2 propGet bool ok");
                    int rawInt13 = 0;
                    std::memcpy(&rawInt13, buf8 + 4, 4);
                    expectTrue(rawInt13 == 0, "s13a2 Enabled roundtrip false");

                    char sbuf[260] = {};
                    std::memcpy(sbuf + 4, "hola props", 11);
                    expectTrue(host13.propSet(3, sbuf, sizeof sbuf).ok, "s13a2 propSet string ok");
                    std::memset(sbuf, 0, sizeof sbuf);
                    expectTrue(host13.propGet(3, sbuf, sizeof sbuf).ok, "s13a2 propGet string ok");
                    expectTrue(std::string(sbuf + 4) == "hola props",
                               "s13a2 Label roundtrip UTF8 bytes");
                }

                // s13b：负向（idx 越界 → 脚本侧 2 → rc 8；坏缓冲 → C++ 侧 BadArg 2）
                {
                    std::uint8_t buf8[8] = {};
                    const auto rOob = host13.propGet(99, buf8, sizeof buf8);
                    expectTrue(!rOob.ok && rOob.code == 8,
                               "s13b propGet idx out of range -> rc 8 (script-side 2)");
                    const auto rBad = host13.propSet(0, buf8, 4);
                    expectTrue(!rBad.ok && rBad.code == 2,
                               "s13b propSet bad buffer size -> rc 2 (C++-side BadArg)");
                }

                // s13c：热更跟随（改源加一行 [Prop] 字段 → 重编同名覆盖 → reload）
                {
                    const fs::path cs13 = hotProps / "PropsScript.cs";
                    std::ifstream in13(cs13, std::ios::binary);
                    std::string src13((std::istreambuf_iterator<char>(in13)),
                                      std::istreambuf_iterator<char>());
                    in13.close();
                    const std::string anchor13 = "[Prop(\"Label\")] public static string Label";
                    const std::size_t pos13 = src13.find(anchor13);
                    expectTrue(pos13 != std::string::npos,
                               "s13c locate Label anchor in copied source");
                    if (pos13 != std::string::npos) {
                        src13.insert(pos13,
                                     "[Prop(\"Extra\", 0, 50)] public static int Extra = 5;\n        ");
                        std::ofstream out13(cs13, std::ios::binary | std::ios::trunc);
                        out13 << src13;
                        out13.close();
                        expectTrue(dotnetBuild(hotProps.string(), {}),
                                   "s13c dotnet build overwrite (Extra field added)");
                        std::string info13;
                        expectTrue(host13.reloadScript(&info13).ok,
                                   "s13c reloadScript (table + channel re-resolved)");
                        if (!host13.hasProps())
                            std::printf("  s13c detail: %s\n", info13.c_str());
                        expectTrue(nl::script::scriptProps().size() == 5,
                                   "s13c descriptor table follows reload (4 -> 5)");
                        bool extraFound = false;
                        for (const auto& d : nl::script::scriptProps())
                            extraFound |= std::string(d.name) == "Extra" &&
                                          d.kind == nl::script::kPropKindInt &&
                                          d.max == 50.f;
                        expectTrue(extraFound, "s13c Extra row present (kind int, max 50)");
                        expectTrue(host13.hasProps(),
                                   "s13c channel re-attached after reload");
                    }
                }

                // s13d：泄露段（万次级 string 串往返 + GC.Collect 后句柄漂移阈值；
                // s8f 手法扩展——生命周期账本硬规则②新域必带）
                {
                    // 泄露段必须打在 **string 行** 上——reload 后行序随 MetadataToken
                    // 变化（s13c 把 Extra 插在 Label 声明前 → row3=Extra(int)），
                    // 硬编码 idx 会把 4 字节 string 比特写进 int 字段（实测坑）。
                    int labelIdx13 = -1;
                    const auto& tblL = nl::script::scriptProps();
                    for (int r = 0; r < static_cast<int>(tblL.size()); ++r)
                        if (tblL[r].kind == nl::script::kPropKindString) labelIdx13 = r;
                    expectTrue(labelIdx13 >= 0, "s13d string row present in reloaded table");
                    DWORD h013 = 0, h113 = 0;
                    GetProcessHandleCount(GetCurrentProcess(), &h013);
                    bool loopOk = labelIdx13 >= 0;
                    char sbuf13[260] = {};
                    for (int i = 0; i < 10000 && loopOk; ++i) {
                        std::snprintf(sbuf13 + 4, sizeof sbuf13 - 4, "leak-%d", i);
                        loopOk = host13.propSet(labelIdx13, sbuf13, sizeof sbuf13).ok;
                        if (!loopOk) break;
                        std::memset(sbuf13, 0, sizeof sbuf13);
                        loopOk = host13.propGet(labelIdx13, sbuf13, sizeof sbuf13).ok &&
                                 std::memcmp(sbuf13 + 4, "leak-", 5) == 0;
                    }
                    expectTrue(loopOk, "s13d 10k string propGet/propSet roundtrips all ok");
                    std::int32_t retGc = 0;
                    expectTrue(vm->scriptCall(propsType, "GCCollect", nullptr, 0, &retGc).ok &&
                               retGc == 0,
                               "s13d managed GC.Collect via probe");
                    GetProcessHandleCount(GetCurrentProcess(), &h113);
                    const long long drift13 =
                        static_cast<long long>(h113) - static_cast<long long>(h013);
                    expectTrue(drift13 < 32,
                               "s13d handle drift < 32 after 10k string roundtrips + GC");
                    if (drift13 >= 32)
                        std::printf("  s13d drift=%lld\n", drift13);
                }

                // s13e：卸载（通道前置空 = 铁律；描述表 POD 留存 C++ 侧）
                {
                    bool fully13 = false;
                    expectTrue(host13.unloadScript(&fully13).ok && fully13,
                               "s13e unloadScript (weakref fully unloaded)");
                    expectTrue(!host13.hasProps(),
                               "s13e props channel nulled before unload (lifecycle rule)");
                    expectTrue(nl::script::scriptPropsGen() >= 2,
                               "s13e descriptor table POD persists (C++-owned)");
                }
            }
            fs::remove_all(hotPropsRoot, ec);
        }
    }

    // ---- s14 鼠标裸查询域（M-I3；mouseDir 为空 = 跳过，同 s6~s13 口径）----
    // 注入路径验收：feedMouseButton/feedMouseDelta/feedScroll/feedCursor 合成序列
    // → C# Mouse* 导出逐位锚（M-I0 注入契约同款：注入与窗口路径同一更新内核）。
    if (!mouseDir.empty()) {
        if (!dotnetBuild(mouseDir.string(), {})) {
            expectTrue(false, "s14-pre dotnet build mouse example");
            std::printf("nl-script selftest: FAIL (mouse binding build)\n");
            return 1;
        }
        auto mso = locateOutput(mouseDir, {});
        expectTrue(mso.has_value(), "s14-pre locate MouseScript build output");
        if (mso) {
            const std::string mouseType = "NlScript.Examples.MouseScript, " + mso->assemblyName;
            nl::script::EntryPointFn mouseBound = nullptr, mouseUnbound = nullptr;
            result = vm->getEntryPoint(mso->assemblyDll.string(), mouseType, "BoundProbe", &mouseBound);
            result = vm->getEntryPoint(mso->assemblyDll.string(), mouseType, "UnboundProbe", &mouseUnbound);
            expectTrue(result.ok && mouseUnbound != nullptr, "s14-pre locate mouse entries (UnmanagedCallersOnly)");
            if (!result.ok) {
                std::printf("  entry detail: code=%d %s\n", result.code, result.message.c_str());
            } else {
                const nl::script::ApiTable* table = nl::script::consoleApiTable();
                auto callEntry14 = [&](nl::script::EntryPointFn fn) {
                    std::int32_t ret = 0;
                    std::uint32_t exc = 0;
                    const int hit = guardedCall(fn, const_cast<void*>(static_cast<const void*>(table)),
                                                0, &ret, &exc);
                    return std::pair<int, std::int32_t>{hit, hit == 0 ? ret : -2000 - static_cast<std::int32_t>(exc)};
                };

                // 宿主活实例（M-I0 注入锚数值：左键 0 / 右键 1；增量 3.5/-2.0/1.25 全精确浮点）
                static nl::core::InputState input14;
                input14.feedMouseButton(0, true);           // 左键 down + justPressed
                input14.feedMouseButton(1, true);           // 右键先按下
                input14.feedMouseButton(1, false);          // 再释放 → justReleased（down=false）
                input14.feedMouseDelta(3.5f, -2.0f);
                input14.feedScroll(0.0f, 1.25f);
                input14.feedCursor(640.0f, 360.0f, true);
                nl::script::inputSetState(&input14);

                {
                    auto [hit, ret] = callEntry14(mouseBound);
                    expectInt(hit == 0 ? ret : -2000, 0, "s14a C# BoundProbe -> 0");
                    if (hit != 0 || ret != 0)
                        std::printf("  s14a detail: hit=%d ret=%d\n", hit, ret);
                }

                // s14b：解绑路径（nullptr = NoSource 全族）
                nl::script::inputSetState(nullptr);
                {
                    auto [hit, ret] = callEntry14(mouseUnbound);
                    expectInt(hit == 0 ? ret : -2000, 0, "s14b C# UnboundProbe -> 0");
                }
            }
        }
    }

    // ---- s15 手柄 / 字符域（M-I5；gamepadDir 为空 = 跳过，同 s6~s14 口径）----
    // 注入路径验收：feedGamepadButton/feedGamepadAxis 合成序列 → C# 手柄导出锚；
    // TextPop 在 selftest 无 Window → 全程 NoSource（viewer scene 模式 inputSetWindow
    // 接线的正路径由 viewer 手工冒烟背书，窗口级 consumeTextChars 正路径 M-C1 已覆盖）。
    if (!gamepadDir.empty()) {
        if (!dotnetBuild(gamepadDir.string(), {})) {
            expectTrue(false, "s15-pre dotnet build gamepad example");
            std::printf("nl-script selftest: FAIL (gamepad binding build)\n");
            return 1;
        }
        auto gso = locateOutput(gamepadDir, {});
        expectTrue(gso.has_value(), "s15-pre locate GamepadScript build output");
        if (gso) {
            const std::string gamepadType = "NlScript.Examples.GamepadScript, " + gso->assemblyName;
            nl::script::EntryPointFn padBound = nullptr, padUnbound = nullptr;
            result = vm->getEntryPoint(gso->assemblyDll.string(), gamepadType, "BoundProbe", &padBound);
            result = vm->getEntryPoint(gso->assemblyDll.string(), gamepadType, "UnboundProbe", &padUnbound);
            expectTrue(result.ok && padUnbound != nullptr, "s15-pre locate gamepad entries (UnmanagedCallersOnly)");
            if (!result.ok) {
                std::printf("  entry detail: code=%d %s\n", result.code, result.message.c_str());
            } else {
                const nl::script::ApiTable* table = nl::script::consoleApiTable();
                auto callEntry15 = [&](nl::script::EntryPointFn fn) {
                    std::int32_t ret = 0;
                    std::uint32_t exc = 0;
                    const int hit = guardedCall(fn, const_cast<void*>(static_cast<const void*>(table)),
                                                0, &ret, &exc);
                    return std::pair<int, std::int32_t>{hit, hit == 0 ? ret : -2000 - static_cast<std::int32_t>(exc)};
                };

                // 宿主活实例（注入锚数值：A=0 / B=1；轴 0=左摇杆X / 4=左扳机）
                static nl::core::InputState input15;
                input15.feedGamepadButton(0, true);          // A down + justPressed
                input15.feedGamepadButton(1, true);          // B 先按下
                input15.feedGamepadButton(1, false);         // 再释放 → justReleased
                input15.feedGamepadAxis(0, 0.75f);
                input15.feedGamepadAxis(4, 0.5f);
                nl::script::inputSetState(&input15);
                // inputSetWindow 不调（selftest 无窗口；TextPop 负向 = NoSource）。

                {
                    auto [hit, ret] = callEntry15(padBound);
                    expectInt(hit == 0 ? ret : -2000, 0, "s15a C# BoundProbe -> 0");
                    if (hit != 0 || ret != 0)
                        std::printf("  s15a detail: hit=%d ret=%d\n", hit, ret);
                }

                // s15b：解绑路径（nullptr = NoSource 全族）
                nl::script::inputSetState(nullptr);
                {
                    auto [hit, ret] = callEntry15(padUnbound);
                    expectInt(hit == 0 ? ret : -2000, 0, "s15b C# UnboundProbe -> 0");
                }
            }
        }
    }

    std::printf("nl-script selftest: %s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}

void usage() {
    std::printf(
        "nl-script -- C# hosting selftest tool (nl::script, A-class track)\n"
        "usage:\n"
        "  nl-script check <projectDir>                       compile only (dotnet build)\n"
        "  nl-script run <projectDir> [--args <s>] [--api] [--type <ns.Class, Assembly>]\n"
        "             --api passes ApiTable* to Entry (console-binding scripts)\n"
        "  nl-script selftest <projectDir> [consoleProjectDir] [ecsProjectDir] [bootstrapDir]\n"
        "                    [timeinputProjectDir] [cameraProjectDir] [modelProjectDir]\n"
        "                    [driveProjectDir] [propsProjectDir] [mouseProjectDir] [gamepadProjectDir]  full machine-checked acceptance\n"
        "exit codes: 0=PASS / 1=fail / 2=usage\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];

    if (cmd == "check") {
        if (argc < 3) {
            usage();
            return 2;
        }
        return cmdCheck(fs::weakly_canonical(argv[2]));
    }
    if (cmd == "run") {
        if (argc < 3) {
            usage();
            return 2;
        }
        std::string args, typeOverride;
        bool passApiTable = false;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--args" && i + 1 < argc) {
                args = argv[++i];
            } else if (std::string(argv[i]) == "--api") {
                passApiTable = true;
            } else if (std::string(argv[i]) == "--type" && i + 1 < argc) {
                typeOverride = argv[++i];
            } else {
                usage();
                return 2;
            }
        }
        return cmdRun(fs::weakly_canonical(argv[2]), args, typeOverride, passApiTable);
    }
    if (cmd == "selftest") {
        if (argc < 3) {
            usage();
            return 2;
        }
        const fs::path consoleDir = argc >= 4 ? fs::weakly_canonical(argv[3]) : fs::path();
        const fs::path ecsDir = argc >= 5 ? fs::weakly_canonical(argv[4]) : fs::path();
        const fs::path bootstrapDir = argc >= 6 ? fs::weakly_canonical(argv[5]) : fs::path();
        const fs::path timeDir = argc >= 7 ? fs::weakly_canonical(argv[6]) : fs::path();
        const fs::path camDir = argc >= 8 ? fs::weakly_canonical(argv[7]) : fs::path();
        const fs::path modelDir = argc >= 9 ? fs::weakly_canonical(argv[8]) : fs::path();
        const fs::path driveDir = argc >= 10 ? fs::weakly_canonical(argv[9]) : fs::path();
        const fs::path propsDir = argc >= 11 ? fs::weakly_canonical(argv[10]) : fs::path();
        const fs::path mouseDir = argc >= 12 ? fs::weakly_canonical(argv[11]) : fs::path();
        const fs::path gamepadDir = argc >= 13 ? fs::weakly_canonical(argv[12]) : fs::path();
        return cmdSelftest(fs::weakly_canonical(argv[2]), consoleDir, ecsDir, bootstrapDir, timeDir, camDir, modelDir, driveDir, propsDir, mouseDir, gamepadDir);
    }
    usage();
    return 2;
}
