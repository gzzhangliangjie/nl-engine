#pragma once

// P8 M-I2：input-hud 调试面板的帧数据（A 类轨道，docs/p8/P8-INPUT-PLAN.md §M-I2）。
//
// 与 HudFrame 同一条数据纪律：viewer / core 不接触 ImGui 符号，本 POD 是
// 唯一交换面——viewer 每帧首填「显示快照」（注入路径 InputState/ActionMap 的
// 冻结值），hud.build() 的 NewFrame..Render 区间调 drawInputDebugUi() 消费，
// 并把「注入请求」（checkbox 双向态）写回，viewer 下一帧首转成 feed 边沿。
//
// ⚠️ 单消费者铁律（input.hpp 文件头）：viewer 绝不调 updateFromWindow——
// 本面板的一切输入都来自注入 API（ImGui 按钮 / --input-demo 脚本），现有
// 游戏热键与 ImGui 键盘通道零触碰。
//
// 面板字符串全 ASCII（HUD 字符串约定）；截图回归下数值全部来自确定性脚本，
// 无时序样本，基线可逐字节复现。

namespace nl::renderer {

struct InputDebugUi {
    // ---- viewer -> HUD（显示快照；viewer 帧首在 endFrame 之前填）----
    bool show = false;            // 仅 --mode input-hud 置位；false = 零布局（既有基线保证）

    // 键态矩阵（注入路径三态快照；固定 8 行）
    enum { kMaxRows = 8 };
    struct KeyRow {
        const char* label = "";   // ASCII
        bool down = false;        // 持续态
        bool pressed = false;     // 本帧边沿（endFrame 清零前采样）
        bool released = false;
    };
    KeyRow rows[kMaxRows];
    int  rowCount = 0;

    // 动作值快照（驾驶映射表 buildDrivingActionMap 的冻结值）
    float throttle = 0.0f;        // [-1,+1]（W/S）
    float steer    = 0.0f;        // [-1,+1]（A/D）
    float lookX    = 0.0f;        // 鼠标增量轴 x（endFrame 前采样）
    float lookY    = 0.0f;
    float handbrake = 0.0f;       // Bool 动作按 0/1 画条
    bool  handbrakePressed  = false;
    bool  handbrakeReleased = false;
    float pause = 0.0f;           // Bool（ui 组；组关停时恒 0——清零语义演示）
    bool  pausePressed  = false;
    bool  pauseReleased = false;
    bool  drivingSetActive = true;   // 驾驶组激活态（出厂 true）
    bool  uiSetActive      = false;  // UI 组激活态（出厂 false；viewer 在 input-hud 模式显式开）

    // ---- HUD -> viewer（注入请求；checkbox 双向态 = 期望按下态，viewer 帧首转 feed 边沿）----
    bool reqW = false, reqA = false, reqS = false, reqD = false;
    bool reqSpace = false, reqEsc = false, reqLmb = false;
    bool reqUiSetActive = true;   // UI 组激活期望态（与 uiSetActive 快照独立——上一帧结果）
};

// ImGui 面板绘制（input_debug.cpp；hud.build() 内 NewFrame..Render 区间调用，
// 位置在 console / audio 调试窗口之后——同款「增量兄弟窗口」落位）。
void drawInputDebugUi(InputDebugUi& ui);

} // namespace nl::renderer
