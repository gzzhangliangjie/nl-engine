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
    Cull = 4,     // M4.0：vertex pulling 渲染（单 drawIndexedIndirect，全簇全画验证通路；M4.1 接 GPU 剔除）
    Dag = 5,      // M5.1：GPU DAG 遍历选层（compute 显式栈，输出复用 M4 indirect 通路）
    Skin = 6,     // M-A5：CPU 蒙皮展示——rigged GLB（skin+动画）逐帧 LBS，看角色动起来
    Hiz = 7,      // M6.0：Hi-Z 深度预通道 + 手写 min 金字塔（dag 遍历 + 金字塔 + 层级可视化）
    Scene = 8,    // M-E1b.0：ECS 场景（--scene 读 World JSON，N 物体 M 方向灯实例化）
    Visbuf = 9,   // M7.0：visibility buffer 打包写入（u64 簇/三角/深度）+ 簇/三角可视化
    Pulling = 10, // M7.1：vis buffer 材质反解（全屏 pass 顶点拉取 + 解析式 barycentrics + 材质着色）
    InputHud = 11, // P8 M-I2：输入调试面板（注入路径驱动的键态矩阵 + 动作值条 + ImGui 注入按钮）
    Rt = 12,      // M8.2：Ray Query 调试射线闭环（compute 每像素相机射线打 TLAS，
                  //  距离灰度 / id 伪彩两档；BLAS/TLAS 走 initDag 资产链）
    Shadow = 13,  // M9.0：compute 直光对照（M9.0 阶段不发射线；M9.1 起同 mode
                  //  默认每像素 1 条 shadow ray）。Q4 方案 A：属性即取即算
    Reference = 14, // M9.2：离线参考模式（渐进式多帧累积的直光真值图，裁判 M9.1 阴影射线）
    ProbePlace = 15, // M10.0：屏幕探针 cascade 布点（velocity prepass + 布点 compute
                     //  + 探针点可视化；S10-PLAN Q10-1/Q10-2 拍板 UE 口径）
    ProbeViz = 16,   // M10.1：探针辐照度可视化（管线同 ProbePlace + 探针追踪 compute，
                     //  点色 = 八面体图按视线方向采样；S10-PLAN Q12-1/2/3 拍板）
    // S1 起依次追加：Probe, Cache, CacheViz, Filter, Final
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
