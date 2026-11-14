// 发版打包路径解析（B1 拍板 2026-11-15）。设计口径见 paths.hpp 头注释。
// 只依赖 Win32 + stdlib，不碰任何图形 API 头（core 纪律）。

#include "nl/core/paths.hpp"

#include <filesystem>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace nl::core::paths {
namespace {

// 目录存在且至少含一个普通文件才采信（防空目录静默劫持解析）。
bool dirHasFiles(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return false;
    for (fs::directory_iterator it(p, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file(ec)) return true;
    }
    return false;
}

fs::path exeRelative(const char* rel) {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(buf).parent_path() / rel;
#else
    (void)rel;
    return {};
#endif
}

// 候选有效则返回规范化绝对路径，否则返回空串（调用方回退编译期注入）。
std::string probe(const char* rel, bool needFiles) {
    fs::path cand = exeRelative(rel);
    if (cand.empty()) return {};
    std::error_code ec;
    if (needFiles ? dirHasFiles(cand) : fs::is_directory(cand, ec)) {
        fs::path canon = fs::weakly_canonical(cand, ec);
        return ec ? cand.string() : canon.string();
    }
    return {};
}

} // namespace

const std::string& exeDir() {
    static const std::string dir = [] {
        fs::path p = exeRelative(".");
        return p.parent_path().string();
    }();
    return dir;
}

const std::string& shaderDir() {
    static const std::string dir = [] {
#ifndef NL_BACKEND_NAME
#error "NL_BACKEND_NAME must be injected (DX12/VULKAN) by root CMakeLists"
#endif
        std::string probed = probe("../../shaders/" NL_BACKEND_NAME, true);
        if (!probed.empty()) return probed;
#ifdef NL_SHADER_DIR
        return std::string(NL_SHADER_DIR);
#else
        return std::string();
#endif
    }();
    return dir;
}

const std::string& assetDir() {
    static const std::string dir = [] {
        std::string probed = probe("../../assets/models", false);
        if (!probed.empty()) return probed;
#ifdef NL_ASSET_DIR
        return std::string(NL_ASSET_DIR);
#else
        return std::string();
#endif
    }();
    return dir;
}

const std::string& meshletDir() {
    static const std::string dir = [] {
        std::string probed = probe("../../assets/meshlets", false);
        if (!probed.empty()) return probed;
#ifdef NL_MESHLET_DIR
        return std::string(NL_MESHLET_DIR);
#else
        return std::string();
#endif
    }();
    return dir;
}

} // namespace nl::core::paths
