#pragma once

// P8 M-SC10：脚本属性面板（--prop-panel 时 viewer 挂入；null = 完全不画，
// 既有基线一像素不动——NavPanel 同款先例）。ImGui 符号只在
// renderer/src/prop_panel.cpp；本头文件零第三方符号、零 core 依赖（kind 码
// 与 Bindings.hpp kPropKind* 数值对齐，镜像而非 include——renderer 不依赖
// script 模块，viewer 同步侧负责拷贝转换）。
//
// 数据纪律（M-T5 SignalPanel 教训 + NavPanel 双向通道同款）：
//   - State 是 HudFrame 值成员：viewer 持**帧外持久局部 State**，帧前整份拷入
//     hud.propPanelState、帧后整份拷回（drawWindow 的控件编辑落在 hud 副本上，
//     不拷回 = 编辑丢失）。
//   - 真源分工：描述表（nl::script::scriptProps，C# 推 POD）+ 值经 ScriptHost
//     propGet/propSet 通道往返脚本静态字段；viewer 按描述表 gen 代数重建行
//     （热更 reload → 新表新 gen → 面板自动跟随），dirty 行由 viewer 消费
//     propSet 后清边沿。

namespace nl::renderer {

struct PropPanel {
    static constexpr int kMaxRows = 64;   // 面板行上限（描述表 256 超出截断显示）
    static constexpr int kNameCap = 63;   // Bindings.hpp kPropNameCap 镜像
    static constexpr int kStringCap = 255;// Bindings.hpp kPropStringCap 镜像

    enum Kind : int { kFloat = 0, kInt = 1, kBool = 2, kString = 3 };

    struct Row {
        int kind = 0;
        char name[kNameCap + 1] = {};
        float min = 0.f, max = 0.f;   // min==max = 无 Range（退直输控件）
        float f = 0.f;
        int i = 0;
        bool b = false;
        char s[kStringCap + 1] = {};
        bool dirty = false;           // 面板改过（viewer 消费 propSet 后清零）
    };

    // 全 POD 状态（viewer 帧外持久持有；HudFrame 值成员整份拷入/拷回）。
    struct State {
        bool active = false;          // 通道接通且描述表非空（画窗体的总闸）
        bool channelAttached = false; // ScriptHost::hasProps()（状态行文案区分）
        int rowCount = 0;             // 实际行数（<=kMaxRows）
        int totalProps = 0;           // 描述表全长
        unsigned gen = 0;             // 现役描述表代数（重建信号）
        char status[128] = {};        // 通道/表状态行
        Row rows[kMaxRows];
    };

    // hud.cpp 每帧调用：画 "script props" 窗口（状态行 + 行区 480 滚动子区，
    // HUD 高纪律 <=560；控件 = Slider/InputFloat、Slider/InputInt、Checkbox、
    // InputText；编辑置 Row::dirty 边沿，viewer 消费）。
    void drawWindow(State& st);
};

}  // namespace nl::renderer
