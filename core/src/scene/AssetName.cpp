// M-SC7：资产文件名白名单单源实现（语义 = renderer.cpp 原版 validAssetFileName；
// 收编说明见 AssetName.hpp 头注）。
#include "nl/scene/AssetName.hpp"

#include <cstring>
#include <string>

namespace nl::scene {

bool validAssetFileName(const char* name, const char* ext) {
    if (!name || !*name) return false;
    const std::string s(name);
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return false;
    if (s.find("..") != std::string::npos) return false;
    const size_t extLen = std::strlen(ext);
    if (s.size() < extLen + 1 || s.compare(s.size() - extLen, extLen, ext) != 0) return false;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7E) return false;   // 可打印 ASCII，不含空格
    }
    return true;
}

} // namespace nl::scene
