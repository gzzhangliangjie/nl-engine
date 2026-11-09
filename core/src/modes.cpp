#include "nl/core/modes.hpp"

#include <cstring>

namespace nl::core {
namespace {

constexpr const char* kModeNames[] = {
    "normal",
    "cluster",   // M1.3：.nmesh 每簇随机着色
    "lod",       // M2.3：<stem>.lodN.nmesh 逐层切换
    "dag-top",   // M3.2：DAG 顶层 + 任选组下钻一层（cut 预演）
    "cull",      // M4.0：vertex pulling 渲染（单 drawIndexedIndirect，全簇全画）
    "dag",       // M5.1：GPU DAG 遍历选层（compute 显式栈，误差阈值 cut）
    "skin",      // M-A5：CPU 蒙皮展示（rigged GLB 逐帧 LBS）
    "hiz",       // M6.0：Hi-Z 深度预通道 + min 金字塔 + 层级可视化
    "visbuf",    // M7.0：u64 visibility buffer 打包写入 + 簇/三角可视化
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
    // 已有 normal / cluster / lod / dag-top / cull / dag；尚未实现的模式名此刻
    // 必须被拒绝（防分发器静默吞掉拼错的名字）。
    ViewMode parsed = ViewMode::Normal;
    if (parseViewMode("dagtop", parsed)) return false;
    if (parseViewMode("", parsed)) return false;
    if (parseViewMode(nullptr, parsed)) return false;
    if (modeName(ViewMode::Count) != nullptr) return false;
    return true;
}

} // namespace nl::core
