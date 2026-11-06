#include "nl/core/modes.hpp"

#include <cstring>

namespace nl::core {
namespace {

constexpr const char* kModeNames[] = {
    "normal",
};

static_assert(sizeof(kModeNames) / sizeof(kModeNames[0]) ==
                  static_cast<int>(ViewMode::Count),
              "ViewMode and kModeNames must stay in sync");

} // namespace

const char* modeName(ViewMode mode) {
    const int i = static_cast<int>(mode);
    if (i < 0 || i >= modeCount()) return nullptr;
    return kModeNames[i];
}

const char* modeNameAt(int index) {
    if (index < 0 || index >= modeCount()) return nullptr;
    return kModeNames[index];
}

int modeCount() { return static_cast<int>(ViewMode::Count); }

bool parseViewMode(const char* text, ViewMode& out) {
    if (!text) return false;
    for (int i = 0; i < modeCount(); ++i) {
        if (std::strcmp(text, kModeNames[i]) == 0) {
            out = static_cast<ViewMode>(i);
            return true;
        }
    }
    return false;
}

bool modeSelfTest() {
    if (modeCount() < 1) return false;
    for (int i = 0; i < modeCount(); ++i) {
        const char* name = modeNameAt(i);
        if (!name || !*name) return false;
        ViewMode parsed = ViewMode::Count;
        if (!parseViewMode(name, parsed)) return false;
        if (static_cast<int>(parsed) != i) return false;
        if (std::strcmp(modeName(parsed), name) != 0) return false;
        for (int j = 0; j < i; ++j)
            if (std::strcmp(modeNameAt(j), name) == 0) return false;  // 名字必须唯一
    }
    // S0 只有 normal：随便一个未来模式名此刻必须被拒绝（防止分发器静默吞掉拼错）。
    ViewMode parsed = ViewMode::Normal;
    if (parseViewMode("cluster", parsed)) return false;
    if (parseViewMode("", parsed)) return false;
    if (parseViewMode(nullptr, parsed)) return false;
    if (modeName(ViewMode::Count) != nullptr) return false;
    return true;
}

} // namespace nl::core
