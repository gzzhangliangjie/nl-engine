#pragma once

// P8 M-N2：NavMesh 调参 / 五阶段可视化面板（--nav-panel 时 viewer 挂入；
// null = 完全不画，既有基线一像素不动——NodeEditor 同款先例）。
// 图形 API / ImGui 符号只在 renderer/src/nav_panel.cpp；nl/nav 的 DebugPrims
// 是纯 POD 图元流（零第三方符号），本头文件用 void* 持其指针避免引 core 头。
//
// 数据纪律（HudFrame 双向通道同款，与 cull/color/lod 一致）：
//   - cvar 是唯一真源：viewer 每帧把 nav.* cvar 现值拷入 State 参数字段
//     （panel 显示），panel 控件写回 State 后置 paramsChanged，viewer 检测
//     差异 setFloat 写回 cvar（onChanged 链触发）。
//   - 重烘焙 = panel Bake 按钮置 bakeRequested，viewer 消费后清零（UE 显式
//     rebuild 口径；不搞拖滑条每帧全量重烘）。
//   - 阶段切换 = State.stage（panel 下拉写回），viewer 检测变化后重新收集
//     DebugPrims 喂回 prims 指针。

#include <memory>

namespace nl::renderer {

class NavPanel {
public:
    // 全 POD 状态（viewer 每帧刷新/消费）。
    struct State {
        // ---- viewer -> panel（显示数据）----
        bool        baked = false;   // 有可显示产物（prims 非空）
        const void* prims = nullptr; // const nl::nav::DebugPrims*（viewer 持有存储）
        float       bmin[3] = {0.f, 0.f, 0.f};
        float       bmax[3] = {0.f, 0.f, 0.f};   // 画布投影 bounds（stats 口径）
        char        status[160] = {};            // 状态行（bake 结果 / 错误）
        // ---- panel <-> viewer（双向）----
        int   stage = 0;            // 当前阶段 0..6（DebugStage 枚举序）
        bool  stageDirty = false;   // 面板下拉本帧真实改动（边沿；POD 每帧重置，
                                    //   viewer 靠它区分「面板写回」与「POD 归零」）
        bool  bakeRequested = false; // Bake 按钮（panel 置位，viewer 清零）
        bool  paramsChanged = false; // 面板改过参数（viewer 写回 cvar 后清零）
        // 烘焙参数（cvar nav.* 镜像；口径见 nl::nav::BuildParams）
        float cellSize      = 0.30f;
        float cellHeight    = 0.20f;
        float slope         = 45.f;
        float agentHeight   = 2.f;
        float agentRadius   = 0.5f;
        float agentClimb    = 0.4f;
        float regionMin     = 2.f;
        float regionMerge   = 6.f;
        float edgeMaxLen    = 12.f;
        float edgeMaxError  = 1.3f;
        float detailDist    = 6.f;
        float detailMaxError= 1.f;
        int   vertsPerPoly  = 6;
    };

    static std::unique_ptr<NavPanel> create();
    ~NavPanel();
    NavPanel(const NavPanel&) = delete;
    NavPanel& operator=(const NavPanel&) = delete;

    // hud.cpp 每帧调用：画面板窗口（阶段下拉 + 参数区 + 俯视画布 + 状态条）。
    void drawWindow(State& state);

private:
    NavPanel();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nl::renderer
