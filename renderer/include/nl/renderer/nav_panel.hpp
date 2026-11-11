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

        // ---- M-N3 补充：动态障碍交互（用户点名解禁的冻结面）----
        // 双模式：nav.tiled=0 保持 M-N2 Solo Mesh（五阶段可视化零回归）；
        // nav.tiled=1 时 bake 走 bakeTiled()（v2 + dtTileCache），才具备加障碍能力。
        // 与 build params 同款纪律：面板控件写回置边沿，viewer 写 cvar（唯一真源），
        // 显示侧由 viewer 每帧无条件回填（HudFrame POD 每帧归零）。
        bool  tiledMode     = false;   // cvar nav.tiled 镜像（bake 分流开关）
        bool  tiledModeDirty= false;   // 面板勾选本帧真实改动（边沿）
        bool  obsPanelOpen  = false;   // cvar nav.obsPanel：证据脚本展开折叠头
                                       //（默认 0 = M-N2 首屏一行不动）
        bool  tiled         = false;   // viewer 回填：当前 navMesh 是 v2 tiled
        bool  obsParamsChanged = false; // 障碍参数滑条改动（边沿）
        bool  addObstacleRequested   = false;
        bool  clearObstaclesRequested= false;
        float obsX      = -0.35f;      // 圆柱中心 x（cvar nav.obsX 镜像）
        float obsZ      = -1.70f;      // 圆柱中心 z（cvar nav.obsZ 镜像）
        float obsRadius = 0.5f;        // cvar nav.obsRadius
        float obsHeight = 3.f;         // cvar nav.obsHeight；y 下界由 viewer 固定
                                       // 为 -1（M-N3 铁律：必须跨过 tile 高度域下界）
        int   obstacleCount = 0;       // viewer 回填（dtTileCache 已处理障碍数）
        char  pathStatus[64] = {};     // viewer 回填（路径查询状态 + 折线点数）
        // 画布叠加（viewer 回填；定长 POD，避免 State 内出现 STL）
        int   obsDrawCount = 0;
        float obsDraw[8][3] = {};      // 每项 x, z, radius
        int   pathPointCount = 0;
        float pathPoints[32][2] = {};  // 每项 x, z
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
