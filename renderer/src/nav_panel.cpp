// P8 M-N2：NavPanel 实现。ImGui 符号封在本 TU（editor.cpp 同款纪律）；
// nl/nav DebugPrims POD 图元流消费端——ImDrawList 直绘（x→右、z→下俯视
// 正交投影，与 nav-tools wire / debug-stage 同一投影口径），零新 NVRHI
// 管线、零纹理上传。
//
// 图元颜色：上游 duRGBA 布局 r | g<<8 | b<<16 | a<<24 → IM_COL32 同序直转；
// 线宽/点径取 Group.size（上游 lineWidth 语义）。

#include "nl/renderer/nav_panel.hpp"

#include "nl/nav/Nav.hpp"

#include <imgui.h>

#include <cstdio>

namespace nl::renderer {

struct NavPanel::Impl {
    // 画布交互态（拖拽平移 / 滚轮缩放的增量视图——只影响显示，不回写数据）
    float panX = 0.f, panY = 0.f;
    float zoom = 1.f;
};

NavPanel::NavPanel() : impl_(new Impl()) {}
NavPanel::~NavPanel() = default;

std::unique_ptr<NavPanel> NavPanel::create() {
    return std::unique_ptr<NavPanel>(new NavPanel());
}

void NavPanel::drawWindow(State& st) {
    // 证据脚本展开障碍区时把窗口调高，并让 build params 收起，给俯视画布
    // 留出空间；默认（nav.obsPanel=0）仍严格是 M-N2 的 460x600 布局。
    ImGui::SetNextWindowSize(ImVec2(460.f, st.obsPanelOpen ? 700.f : 600.f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("NavMesh Debug")) {
        ImGui::End();
        return;
    }

    // ---- 阶段下拉（nl/nav 单一命名源；stageDirty 边沿 = 面板真实改动，
    // viewer 据此写回 cvar 并重收集图元）----
    int stage = st.stage;
    if (ImGui::BeginCombo("stage", nl::nav::debugStageName(
            static_cast<nl::nav::DebugStage>(st.stage)))) {
        for (int s = 0; s <= 6; ++s) {
            const bool sel = (s == st.stage);
            if (ImGui::Selectable(nl::nav::debugStageName(
                    static_cast<nl::nav::DebugStage>(s)), sel)) {
                stage = s;
                st.stageDirty = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    st.stage = stage;

    // ---- Bake 按钮 + 状态行 ----
    if (ImGui::Button("bake")) st.bakeRequested = true;
    ImGui::SameLine();
    ImGui::TextUnformatted(st.status);

    // ---- 参数区（cvar 镜像；控件写回置 paramsChanged，viewer 同步 cvar）----
    if (st.obsPanelOpen) ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("build params",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        auto f = [&](const char* label, float& v, float mn, float mx) {
            if (ImGui::SliderFloat(label, &v, mn, mx)) st.paramsChanged = true;
        };
        f("cellSize",    st.cellSize,      0.05f, 1.5f);
        f("cellHeight",  st.cellHeight,    0.05f, 1.0f);
        f("slope",       st.slope,         5.f,   80.f);
        f("agentHeight", st.agentHeight,   0.3f,  5.f);
        f("agentRadius", st.agentRadius,   0.0f,  2.f);
        f("agentClimb",  st.agentClimb,    0.05f, 1.5f);
        f("regionMin",   st.regionMin,     0.f,   20.f);
        f("regionMerge", st.regionMerge,   0.f,   40.f);
        f("edgeMaxLen",  st.edgeMaxLen,    2.f,   60.f);
        f("edgeMaxError",st.edgeMaxError,  0.5f,  5.f);
        f("detailDist",  st.detailDist,    0.f,   30.f);
        f("detailMaxErr",st.detailMaxError,0.1f,  5.f);
        if (ImGui::SliderInt("vertsPerPoly", &st.vertsPerPoly, 3, 6))
            st.paramsChanged = true;
    }

    // ---- M-N3 补充：动态障碍（默认折叠，M-N2 首屏布局只多一行 header）----
    // tiled 模式下 rc 中间产物不保留，五阶段只有 NavMesh 可用，viewer 会
    // 把 stage 钉到 6 并在状态行提示；障碍按钮只在 tiled bake 成功后可用。
    // ImGuiCond_Once：只在窗口首次出现时按脚本意图展开，之后用户手动
    // 折叠/展开不被每帧覆盖（与 cvar 唯一真源不冲突——这是纯显示初值）。
    if (st.obsPanelOpen) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("dynamic obstacles (tiled)")) {
        if (ImGui::Checkbox("tiled bake (v2)", &st.tiledMode))
            st.tiledModeDirty = true;
        auto of = [&](const char* label, float& v, float mn, float mx) {
            if (ImGui::SliderFloat(label, &v, mn, mx)) st.obsParamsChanged = true;
        };
        of("obsX",      st.obsX,      -4.f,  4.f);
        of("obsZ",      st.obsZ,      -4.f,  4.f);
        of("obsRadius", st.obsRadius, 0.1f,  2.f);
        of("obsHeight", st.obsHeight, 0.5f,  6.f);
        if (!st.tiled) {
            ImGui::TextColored(ImVec4(0.55f, 0.25f, 0.25f, 1.f),
                               "check 'tiled bake' then press 'bake' first");
        } else {
            if (ImGui::Button("add obstacle")) st.addObstacleRequested = true;
            ImGui::SameLine();
            if (ImGui::Button("clear obstacles"))
                st.clearObstaclesRequested = true;
        }
        ImGui::Text("obstacles: %d   path: %s", st.obstacleCount, st.pathStatus);
    }

    // ---- 俯视画布（x→右、z→下；与 nav-tools 同投影口径）----
    ImGui::Separator();
    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSz  = ImGui::GetContentRegionAvail();
    if (canvasSz.x > 4.f && canvasSz.y > 4.f)
        ImGui::InvisibleButton("nav_canvas", canvasSz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 min = canvasPos;
    const ImVec2 max = ImVec2(canvasPos.x + canvasSz.x,
                              canvasPos.y + canvasSz.y);
    dl->AddRectFilled(min, max, IM_COL32(247, 247, 247, 255));
    dl->PushClipRect(min, max, true);

    const nl::nav::DebugPrims* prims =
        st.baked ? static_cast<const nl::nav::DebugPrims*>(st.prims) : nullptr;
    if (!prims || !st.baked) {
        dl->AddText(ImVec2(min.x + 12.f, min.y + 12.f), IM_COL32(120, 120, 120, 255),
                    "no bake yet (press 'bake')");
        dl->PopClipRect();
        ImGui::End();
        return;
    }

    // 投影：bounds 等比适配画布（24px 边距），等价 nav-tools makePrimCanvas
    const float spanX = st.bmax[0] - st.bmin[0];
    const float spanZ = st.bmax[2] - st.bmin[2];
    const float span  = spanX > spanZ ? spanX : spanZ;
    if (span <= 0.f) {
        dl->AddText(ImVec2(min.x + 12.f, min.y + 12.f), IM_COL32(160, 60, 60, 255),
                    "degenerate bounds");
        dl->PopClipRect();
        ImGui::End();
        return;
    }
    const float scale = (std::min(canvasSz.x, canvasSz.y) - 48.f) / span *
                        impl_->zoom;
    const float cx    = (st.bmin[0] + st.bmax[0]) * 0.5f;
    const float cz    = (st.bmin[2] + st.bmax[2]) * 0.5f;
    const ImVec2 ctr  = ImVec2((min.x + max.x) * 0.5f + impl_->panX,
                               (min.y + max.y) * 0.5f + impl_->panY);
    auto toScreen = [&](float x, float z, ImVec2& out) {
        out.x = ctr.x + (x - cx) * scale;
        out.y = ctr.y + (z - cz) * scale;  // 俯视：z→下（与 nav-tools 同口径）
    };

    for (const nl::nav::DebugPrims::Group& g : prims->groups) {
        const int n = g.vertEnd - g.vertBegin;
        if (n <= 0) continue;
        const ImU32 col0 = IM_COL32(prims->verts[g.vertBegin].col & 0xFF,
                                    (prims->verts[g.vertBegin].col >> 8) & 0xFF,
                                    (prims->verts[g.vertBegin].col >> 16) & 0xFF,
                                    (prims->verts[g.vertBegin].col >> 24) & 0xFF);
        if (g.prim == nl::nav::DebugPrims::POINTS) {
            for (int v = g.vertBegin; v < g.vertEnd; ++v) {
                ImVec2 p;
                toScreen(prims->verts[v].pos[0], prims->verts[v].pos[2], p);
                dl->AddCircleFilled(p, 2.0f * impl_->zoom,
                                    IM_COL32(prims->verts[v].col & 0xFF,
                                             (prims->verts[v].col >> 8) & 0xFF,
                                             (prims->verts[v].col >> 16) & 0xFF,
                                             (prims->verts[v].col >> 24) & 0xFF));
            }
        } else if (g.prim == nl::nav::DebugPrims::LINES) {
            const float w = g.size > 0.f ? g.size : 1.f;
            for (int v = g.vertBegin; v + 1 < g.vertEnd; v += 2) {
                ImVec2 a, b;
                toScreen(prims->verts[v].pos[0], prims->verts[v].pos[2], a);
                toScreen(prims->verts[v + 1].pos[0],
                         prims->verts[v + 1].pos[2], b);
                dl->AddLine(a, b,
                            IM_COL32(prims->verts[v].col & 0xFF,
                                     (prims->verts[v].col >> 8) & 0xFF,
                                     (prims->verts[v].col >> 16) & 0xFF,
                                     (prims->verts[v].col >> 24) & 0xFF),
                            w);
            }
        } else if (g.prim == nl::nav::DebugPrims::TRIS) {
            for (int v = g.vertBegin; v + 2 < g.vertEnd; v += 3) {
                ImVec2 a, b, c;
                toScreen(prims->verts[v].pos[0],     prims->verts[v].pos[2], a);
                toScreen(prims->verts[v + 1].pos[0], prims->verts[v + 1].pos[2], b);
                toScreen(prims->verts[v + 2].pos[0], prims->verts[v + 2].pos[2], c);
                dl->AddTriangleFilled(a, b, c,
                                      IM_COL32(prims->verts[v].col & 0xFF,
                                               (prims->verts[v].col >> 8) & 0xFF,
                                               (prims->verts[v].col >> 16) & 0xFF,
                                               (prims->verts[v].col >> 24) & 0xFF));
            }
        } else if (g.prim == nl::nav::DebugPrims::QUADS) {
            for (int v = g.vertBegin; v + 3 < g.vertEnd; v += 4) {
                ImVec2 a, b, c, d;
                toScreen(prims->verts[v].pos[0],     prims->verts[v].pos[2], a);
                toScreen(prims->verts[v + 1].pos[0], prims->verts[v + 1].pos[2], b);
                toScreen(prims->verts[v + 2].pos[0], prims->verts[v + 2].pos[2], c);
                toScreen(prims->verts[v + 3].pos[0], prims->verts[v + 3].pos[2], d);
                dl->AddTriangleFilled(a, b, c, col0);
                dl->AddTriangleFilled(a, c, d, col0);
            }
        }
    }

    // ---- M-N3 补充：动态障碍 + 路径折线叠加（viewer 回填的 POD 数据）----
    // 画在 NavMesh 图元之上：半透明红圆 = 圆柱障碍俯视投影，蓝折线 =
    // findPath 当前结果。障碍生效时折线消失 / 退化即为闭环的可视证据。
    const int nObs = st.obsDrawCount > 8 ? 8 : st.obsDrawCount;
    for (int i = 0; i < nObs; ++i) {
        ImVec2 c;
        toScreen(st.obsDraw[i][0], st.obsDraw[i][1], c);
        const float r = st.obsDraw[i][2] * scale;
        if (r <= 0.f) continue;
        dl->AddCircleFilled(c, r, IM_COL32(220, 60, 60, 80));
        dl->AddCircle(c, r, IM_COL32(190, 40, 40, 255), 0, 2.f);
    }
    const int nPts = st.pathPointCount > 32 ? 32 : st.pathPointCount;
    if (nPts >= 2) {
        ImVec2 pts[32];
        for (int i = 0; i < nPts; ++i)
            toScreen(st.pathPoints[i][0], st.pathPoints[i][1], pts[i]);
        dl->AddPolyline(pts, nPts, IM_COL32(30, 90, 220, 255), false, 2.5f);
        dl->AddCircleFilled(pts[0], 3.5f, IM_COL32(30, 90, 220, 255));
        dl->AddCircleFilled(pts[nPts - 1], 3.5f, IM_COL32(30, 160, 60, 255));
    }
    dl->PopClipRect();
    ImGui::End();
}

}  // namespace nl::renderer
