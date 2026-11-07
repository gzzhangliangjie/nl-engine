#pragma once

// M0.5：viewer 的模式分发器在 S0 定型，S1~S13 每加一个里程碑就在这里加一项。
// core 不认识任何图形 API / ImGui 类型；模式只是「这个名字对应哪种运行方式」。
// 新增模式的顺序：1) 在 ViewMode 里加枚举  2) kModeNames 里加同名字符串
//                 3) viewer/main.cpp 的 runMode() 里加分支  4) example/ 里加用例。

namespace nl::core {

enum class ViewMode {
    Normal = 0,   // M0.5 唯一的模式：普通渲染（带贴图 Suzanne + 轨道相机 + HUD）
    Cluster = 1,  // M1.3：加载 .nmesh，每簇随机着色（S1 切簇的可视化验收）
    Lod = 2,      // M2.3：加载 <stem>.lodN.nmesh 的分层资产，HUD 滑条切层（S2 无缝验收）
    DagTop = 3,   // M3.2：渲染 DAG 顶层（最粗层）+ 任选组下钻一层（S5 组原子 cut 预演）
    // S1 起依次追加：Cull, Dag, Hiz, Visbuf, Rt, Shadow,
    //                Reference, Probe, ProbeViz, Cache, CacheViz, Filter, Final
    Count
};

// 返回以 \0 结尾的名字；越界返回 nullptr。
const char* modeName(ViewMode mode);
const char* modeNameAt(int index);
int modeCount();

// 严格匹配（大小写敏感）；未知名字返回 false 且不改 out。
bool parseViewMode(const char* text, ViewMode& out);

// 无窗口自检：名字唯一、往返一致、未知模式被拒绝。
bool modeSelfTest();

} // namespace nl::core
