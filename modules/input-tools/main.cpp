// input-tools — P8 M-I0/M-I1 headless input acceptance tool (A-class track).
// Only links nl_core (nl::core::InputState injection path + ActionMap mapping
// layer); zero graphics API, no GPU, no kStage, no rendering regression. No
// real window: all anchors go through the synthetic-event injection path,
// which shares the same update kernel as the future window path.
//
// Exit code contract (aligned with phys-tools / anim-tools, showcase expectExit):
//   0 = PASS            1 = assertion / runtime failure            2 = usage error
//
// Subcommands:
//   selftest [section]     mechanical anchors; section omitted = run all
//                          (edges / combos / repeat / mouse / oob / mods /
//                          determinism / actions / actions-det); unknown
//                          section = usage error (exit 2)
//   trace [options]        dump the 12-frame synthetic drive sequence as TSV
//                          (--frames N, default 12; real tool output, feeds
//                          the evidence renderer) [M-I0 state layer]
//   check <tsv>            re-verify a dumped trace with the same anchors
//                          (row count + f0/f1/f3/f10/f11 key-state anchors)
//   act-trace [options]    dump the 12-frame driving-map action sequence as
//                          TSV (throttle/steer/handbrake/look/pause; includes
//                          ActionSet switching at f8/f9) [M-I1 mapping layer]
//   act-check <tsv>        re-verify a dumped act-trace (12 rows + 8 anchors:
//                          f0/f2/f4/f6/f8/f9/f10 + row count)
//
// Evidence rule: no GPU screenshots — tool-card evidence images are generated
// from trace/act-trace TSV by tools/make-input-m{0,1}-evidence.py (real
// output, no hand-drawn diagrams).

#include "nl/core/input.hpp"
#include "nl/core/action_map.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <process.h> // _getpid

namespace {

int  g_failures = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  ok %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

// 记录一帧的完整查询快照（确定性对拍的最小单元）。
struct FrameSnapshot {
    bool wDown, wPressed, wReleased;
    bool spaceDown, spacePressed;
    bool lmbDown, lmbPressed, lmbReleased;
    float dx, dy, sx, sy;
    bool ctrl, shift, alt;
    bool operator==(const FrameSnapshot& o) const {
        return wDown == o.wDown && wPressed == o.wPressed && wReleased == o.wReleased &&
               spaceDown == o.spaceDown && spacePressed == o.spacePressed &&
               lmbDown == o.lmbDown && lmbPressed == o.lmbPressed && lmbReleased == o.lmbReleased &&
               dx == o.dx && dy == o.dy && sx == o.sx && sy == o.sy &&
               ctrl == o.ctrl && shift == o.shift && alt == o.alt;
    }
};

FrameSnapshot snapshot(const nl::core::InputState& in) {
    const auto& c = in;
    return {c.isDown(GLFW_KEY_W), c.justPressed(GLFW_KEY_W), c.justReleased(GLFW_KEY_W),
            c.isDown(GLFW_KEY_SPACE), c.justPressed(GLFW_KEY_SPACE),
            c.mouseIsDown(GLFW_MOUSE_BUTTON_LEFT), c.mouseJustPressed(GLFW_MOUSE_BUTTON_LEFT),
            c.mouseJustReleased(GLFW_MOUSE_BUTTON_LEFT),
            c.mouseDelta().x, c.mouseDelta().y,
            c.scrollDelta().x, c.scrollDelta().y,
            c.ctrlDown(), c.shiftDown(), c.altDown()};
}

// ---- s1：边沿三态节奏（bevy ButtonInput 口径锚） ----
void selftestEdges() {
    std::printf("[edges]\n");
    nl::core::InputState in;
    // 键位表锚：物理键码钉死（GLFW 版本 ABI；上游 bump 撞表长在此炸出）。
    check(GLFW_KEY_W == 87, "GLFW_KEY_W == 87");
    check(GLFW_KEY_SPACE == 32, "GLFW_KEY_SPACE == 32");
    check(GLFW_KEY_ESCAPE == 256, "GLFW_KEY_ESCAPE == 256");
    check(GLFW_MOUSE_BUTTON_LAST == 7, "GLFW_MOUSE_BUTTON_LAST == 7");
    check(nl::core::kGlfwKeyTableSize == GLFW_KEY_LAST + 1, "key table size == GLFW_KEY_LAST+1");

    check(!in.isDown(GLFW_KEY_W) && !in.justPressed(GLFW_KEY_W), "frame0: W idle");
    in.feedKey(GLFW_KEY_W, true);
    check(in.isDown(GLFW_KEY_W) && in.justPressed(GLFW_KEY_W) && !in.justReleased(GLFW_KEY_W),
          "frame1: press -> down+pressed");
    in.endFrame();
    check(in.isDown(GLFW_KEY_W) && !in.justPressed(GLFW_KEY_W) && !in.justReleased(GLFW_KEY_W),
          "frame2 after endFrame: down kept, edge cleared");
    in.feedKey(GLFW_KEY_W, false);
    check(!in.isDown(GLFW_KEY_W) && in.justReleased(GLFW_KEY_W) && !in.justPressed(GLFW_KEY_W),
          "frame2: release -> released only");
    in.endFrame();
    check(!in.isDown(GLFW_KEY_W) && !in.justPressed(GLFW_KEY_W) && !in.justReleased(GLFW_KEY_W),
          "frame3 after endFrame: all clear");
}

// ---- s2：同帧 press/release 组合（事件顺序累积语义） ----
void selftestCombos() {
    std::printf("[combos]\n");
    // press -> release 同帧：终态 down=false，press 边沿被 release 抹掉，released=true。
    nl::core::InputState in;
    in.feedKey(GLFW_KEY_W, true);
    in.feedKey(GLFW_KEY_W, false);
    check(!in.isDown(GLFW_KEY_W) && !in.justPressed(GLFW_KEY_W) && in.justReleased(GLFW_KEY_W),
          "same-frame press->release: released wins");
    in.endFrame();
    check(!in.isDown(GLFW_KEY_W), "combos: clean after endFrame");

    // release -> press 同帧（release 丢帧后补按）：终态 down=true，released 被抹掉。
    nl::core::InputState in2;
    in2.feedKey(GLFW_KEY_SPACE, false);
    in2.feedKey(GLFW_KEY_SPACE, true);
    check(in2.isDown(GLFW_KEY_SPACE) && in2.justPressed(GLFW_KEY_SPACE) && !in2.justReleased(GLFW_KEY_SPACE),
          "same-frame release->press: pressed wins");
}

// ---- s3：REPEAT 语义（down 已真时再 feed down 不重触发 justPressed） ----
void selftestRepeat() {
    std::printf("[repeat]\n");
    nl::core::InputState in;
    in.feedKey(GLFW_KEY_W, true);
    in.feedKey(GLFW_KEY_W, true);   // 模拟 GLFW REPEAT（down 重复）
    in.feedKey(GLFW_KEY_W, true);
    check(in.justPressed(GLFW_KEY_W), "repeat: justPressed exactly once this frame");
    check(in.isDown(GLFW_KEY_W), "repeat: down stays");
    in.endFrame();
    in.feedKey(GLFW_KEY_W, true);   // 下一帧的 REPEAT
    check(!in.justPressed(GLFW_KEY_W) && in.isDown(GLFW_KEY_W),
          "next-frame repeat: no new justPressed");
}

// ---- s4：鼠标全量（按键三态 + delta/scroll 增量累积 + endFrame 清增量） ----
void selftestMouse() {
    std::printf("[mouse]\n");
    nl::core::InputState in;
    in.feedMouseButton(GLFW_MOUSE_BUTTON_LEFT, true);
    in.feedMouseButton(GLFW_MOUSE_BUTTON_RIGHT, true);
    check(in.mouseIsDown(GLFW_MOUSE_BUTTON_LEFT) && in.mouseJustPressed(GLFW_MOUSE_BUTTON_LEFT),
          "mouse: left down+pressed");
    check(in.mouseIsDown(GLFW_MOUSE_BUTTON_RIGHT), "mouse: right down");
    in.feedMouseButton(GLFW_MOUSE_BUTTON_LEFT, false);
    check(in.mouseJustReleased(GLFW_MOUSE_BUTTON_LEFT) && !in.mouseIsDown(GLFW_MOUSE_BUTTON_LEFT),
          "mouse: left release edge");
    check(in.mouseIsDown(GLFW_MOUSE_BUTTON_RIGHT), "mouse: right unaffected by left release");
    in.feedMouseDelta(3.5f, -2.0f);
    in.feedMouseDelta(1.0f, 1.0f);
    in.feedScroll(0.0f, 2.0f);
    check(in.mouseDelta().x == 4.5f && in.mouseDelta().y == -1.0f, "mouse: delta accumulates");
    check(in.scrollDelta().y == 2.0f, "mouse: scroll accumulates");
    in.feedCursor(128.0f, 256.0f);
    check(in.cursorPosition().x == 128.0f && in.cursorPosition().y == 256.0f && in.hasCursor(),
          "mouse: cursor position");
    in.endFrame();
    check(in.mouseDelta().x == 0 && in.scrollDelta().y == 0, "mouse: increments cleared at endFrame");
    check(in.mouseIsDown(GLFW_MOUSE_BUTTON_RIGHT), "mouse: right down kept across endFrame");
    check(!in.mouseJustPressed(GLFW_MOUSE_BUTTON_RIGHT), "mouse: edge cleared at endFrame");
}

// ---- s5：越界防御（显式丢弃，不崩不误伤合法键） ----
void selftestOob() {
    std::printf("[oob]\n");
    nl::core::InputState in;
    in.feedKey(-1, true);
    in.feedKey(GLFW_KEY_LAST + 1, true);
    in.feedKey(nl::core::kGlfwKeyTableSize + 100, true);
    in.feedMouseButton(-1, true);
    in.feedMouseButton(GLFW_MOUSE_BUTTON_LAST + 1, true);
    check(!in.isDown(-1) && !in.isDown(GLFW_KEY_LAST + 1), "oob key queries false");
    check(!in.mouseIsDown(-1) && !in.mouseIsDown(GLFW_MOUSE_BUTTON_LAST + 1), "oob mouse queries false");
    check(!in.isDown(GLFW_KEY_W), "oob feed did not corrupt table");
    in.endFrame();  // 越界注入后的 endFrame 不崩即锚的一部分
    check(true, "oob endFrame survived");
}

// ---- s6：修饰键（注入入口 + endFrame 跨帧保留） ----
void selftestMods() {
    std::printf("[mods]\n");
    nl::core::InputState in;
    in.feedMods(true, false, true);
    check(in.ctrlDown() && !in.shiftDown() && in.altDown(), "mods: ctrl+alt");
    in.endFrame();
    check(in.ctrlDown() && in.altDown(), "mods: kept across endFrame (persistent state)");
    in.feedMods(false, false, false);
    check(!in.ctrlDown() && !in.shiftDown() && !in.altDown(), "mods: cleared");
}

// ---- s7：确定性（同一注入序列重放两次，逐帧快照逐位一致） ----
void selftestDeterminism() {
    std::printf("[determinism]\n");
    auto runSequence = []() {
        nl::core::InputState in;
        std::vector<FrameSnapshot> frames;
        // 12 帧驾驶手感序列：油门按住、手刹点按、左键点按、鼠标拖动、滚轮。
        for (int f = 0; f < 12; ++f) {
            if (f == 0) { in.feedKey(GLFW_KEY_W, true); in.feedMods(false, false, false); }
            if (f == 3) in.feedMouseButton(GLFW_MOUSE_BUTTON_LEFT, true);
            if (f == 5) in.feedMouseButton(GLFW_MOUSE_BUTTON_LEFT, false);
            if (f == 6) in.feedKey(GLFW_KEY_SPACE, true);
            if (f == 8) in.feedKey(GLFW_KEY_SPACE, false);
            if (f == 10) in.feedKey(GLFW_KEY_W, false);
            in.feedMouseDelta(0.5f * (f % 4), -0.25f * (f % 3));
            in.feedScroll(0.0f, (f == 2) ? 1.0f : 0.0f);
            frames.push_back(snapshot(in));
            in.endFrame();
        }
        return frames;
    };
    const auto a = runSequence();
    const auto b = runSequence();
    bool identical = (a.size() == b.size());
    for (size_t i = 0; identical && i < a.size(); ++i) identical = (a[i] == b[i]);
    check(identical, "same sequence replays bit-identical across 12 frames");
    // 序列内容锚：帧 0 W 刚按下、帧 1 只剩持续态、帧 3 左键边沿、帧 10 W 释放。
    check(a[0].wPressed && a[0].wDown, "seq anchor: f0 W pressed+down");
    check(a[1].wDown && !a[1].wPressed, "seq anchor: f1 W down only");
    check(a[3].lmbPressed, "seq anchor: f3 LMB edge");
    check(a[10].wReleased && !a[10].wDown, "seq anchor: f10 W released");
    check(a[11].dx == 1.5f && a[11].dy == -0.5f, "seq anchor: f11 delta values (0.5*(11%4), -0.25*(11%3))");
}

struct Section { const char* name; void (*fn)(); };

// ---- s8：映射层（Carrot 解析规则 + ActionSet 组开关冻结语义） ----
void selftestActions() {
    std::printf("[actions]\n");
    // 驾驶映射表键码锚（键码数值钉死，GLFW 版本 ABI 口径同 [edges] 节）。
    check(GLFW_KEY_A == 65 && GLFW_KEY_S == 83 && GLFW_KEY_D == 68 && GLFW_KEY_C == 67,
          "keycodes: A=65 S=83 D=68 C=67");

    nl::core::ActionMap map;
    const nl::core::DrivingActions d = nl::core::buildDrivingActionMap(map);

    // 预声明口径（Carrot ActionSet：先定义后用；名字/组可查）。
    check(map.find("throttle") == d.throttle && map.find("steer") == d.steer,
          "define/find: named actions resolvable");
    check(map.find("nope") == nl::core::ActionMap::kInvalidAction,
          "find: unknown action -> kInvalidAction");
    check(map.findSet("driving") == d.drivingSet && map.findSet("ui") == d.uiSet,
          "define/find: sets resolvable");
    check(map.isActive(d.drivingSet) && !map.isActive(d.uiSet),
          "factory state: driving on, ui off");

    // Float = max-abs（Carrot README 口径）：W/S 轴。
    nl::core::InputState in;
    in.feedKey(GLFW_KEY_W, true);
    map.update(in);
    check(map.floatValue(d.throttle) == 1.0f, "throttle: W -> +1 (Float=max-abs)");
    check(map.floatDelta(d.throttle) == 1.0f, "throttle: delta = value - prev");
    in.endFrame();
    in.feedKey(GLFW_KEY_S, true);   // W+S：|+1|==|-1| 平局，严格大于比较取先定义的 W
    map.update(in);
    check(map.floatValue(d.throttle) == 1.0f, "throttle: W+S tie -> first-defined W (+1)");
    in.endFrame();
    in.feedKey(GLFW_KEY_W, false);  // 只剩 S
    map.update(in);
    check(map.floatValue(d.throttle) == -1.0f, "throttle: S-only -> -1");
    check(map.floatValue(d.steer) == 0.0f, "steer: idle = 0");
    in.endFrame();
    in.feedKey(GLFW_KEY_D, true);
    map.update(in);
    check(map.floatValue(d.steer) == 1.0f, "steer: D -> +1");
    in.endFrame();

    // Bool 边沿（Carrot wasJustPressed/Released 语义：value vs previousValue）。
    in.feedKey(GLFW_KEY_SPACE, true);
    map.update(in);
    check(map.boolValue(d.handbrake) && map.boolJustPressed(d.handbrake),
          "handbrake: press -> value + justPressed");
    in.endFrame();
    map.update(in);
    check(map.boolValue(d.handbrake) && !map.boolJustPressed(d.handbrake),
          "handbrake: held -> edge frozen (prev=value shift)");
    in.endFrame();
    in.feedKey(GLFW_KEY_SPACE, false);
    map.update(in);
    check(!map.boolValue(d.handbrake) && map.boolJustReleased(d.handbrake),
          "handbrake: release -> justReleased");

    // camToggle（C 键，Bool OR 单绑定）。
    in.feedKey(GLFW_KEY_C, true);
    map.update(in);
    check(map.boolJustPressed(d.camToggle), "cam_toggle: C justPressed");
    in.endFrame();
    in.feedKey(GLFW_KEY_C, false);

    // Vec2 additive（Carrot changeVec2Input 口径）：鼠标增量轴。
    in.feedMouseDelta(2.0f, -1.0f);
    map.update(in);
    check(map.vec2Value(d.look).x == 2.0f && map.vec2Value(d.look).y == -1.0f,
          "look: mouse delta axes -> vec2 (additive)");
    in.endFrame();
    map.update(in);
    check(map.vec2Value(d.look).x == 0.0f && map.vec2Delta(d.look).x == -2.0f,
          "look: endFrame cleared source -> value 0, delta -2");

    // Bool<-Float 阈值 0.5（Carrot gamepad 轴口径；滚轮浮点源）。
    const nl::core::ActionMap::ActionId zoom = map.define("zoom", nl::core::ActionMap::Type::Bool);
    map.bindScroll(zoom, true);
    in.feedScroll(0.0f, 0.3f);
    map.update(in);
    check(!map.boolValue(zoom), "zoom: |0.3| < 0.5 -> false");
    in.endFrame();
    in.feedScroll(0.0f, 1.0f);
    map.update(in);
    check(map.boolValue(zoom) && map.boolJustPressed(zoom),
          "zoom: |1.0| >= 0.5 -> true (threshold 0.5)");
    in.endFrame();

    // 组开关冻结（本层有意偏离 Carrot：关停 = 清零不保旧值，见 action_map.hpp）。
    in.feedKey(GLFW_KEY_ESCAPE, true);
    map.update(in);
    check(!map.boolValue(d.pause) && !map.boolJustPressed(d.pause),
          "set freeze: ui off -> pause zeroed despite Esc down");
    in.endFrame();
    map.setActive(d.uiSet, true);
    map.update(in);
    check(map.boolValue(d.pause) && map.boolJustPressed(d.pause),
          "set switch on: pause value + justPressed");
    check(map.floatValue(d.throttle) == -1.0f,
          "set switch on: driving set untouched (S still held)");
    map.setActive(d.drivingSet, false);
    in.feedKey(GLFW_KEY_W, true);
    map.update(in);
    check(map.floatValue(d.throttle) == 0.0f && map.floatValue(d.steer) == 0.0f,
          "set freeze: driving off -> throttle/steer zeroed");
    check(map.boolValue(zoom) == false,
          "unassigned action: zoom stays governed by input regardless of sets");
    in.endFrame();
    map.setActive(d.uiSet, false);
    map.update(in);
    check(!map.boolValue(d.pause), "set freeze: ui off again -> pause zeroed");
}

// ---- s9：映射层确定性（驾驶序列重放逐位一致 + 序列内容锚） ----
namespace {

struct ActionFrame {
    float th, st, lx, ly;
    int   hb, hbP, pause, pauseP;
    bool operator==(const ActionFrame& o) const {
        return th == o.th && st == o.st && lx == o.lx && ly == o.ly &&
               hb == o.hb && hbP == o.hbP && pause == o.pause && pauseP == o.pauseP;
    }
};

// 12 帧驾驶序列：W 进、D 转向+滚轮、S 对踩（平局锚）、手刹、ui 组开关、
// 全松。喂事件 -> map.update -> 快照 -> in.endFrame（映射层帧节奏口径）。
void driveActionSequence(nl::core::ActionMap& map, nl::core::DrivingActions d,
                         nl::core::InputState& in, std::vector<ActionFrame>& out) {
    for (int f = 0; f < 12; ++f) {
        if (f == 0)  in.feedKey(GLFW_KEY_W, true);
        if (f == 2)  { in.feedKey(GLFW_KEY_D, true); in.feedScroll(0.0f, 1.0f); }
        if (f == 4)  in.feedKey(GLFW_KEY_S, true);
        if (f == 6)  in.feedKey(GLFW_KEY_SPACE, true);
        if (f == 7)  in.feedKey(GLFW_KEY_ESCAPE, true);
        if (f == 8)  map.setActive(d.uiSet, true);
        if (f == 9)  { map.setActive(d.uiSet, false); in.feedKey(GLFW_KEY_SPACE, false); }
        if (f == 10) { in.feedKey(GLFW_KEY_W, false); in.feedKey(GLFW_KEY_S, false);
                       in.feedKey(GLFW_KEY_D, false); in.feedKey(GLFW_KEY_ESCAPE, false); }
        in.feedMouseDelta(0.5f * (f % 4), -0.25f * (f % 3));
        map.update(in);
        ActionFrame fr{};
        fr.th = map.floatValue(d.throttle);
        fr.st = map.floatValue(d.steer);
        fr.lx = map.vec2Value(d.look).x;
        fr.ly = map.vec2Value(d.look).y;
        fr.hb    = map.boolValue(d.handbrake) ? 1 : 0;
        fr.hbP   = map.boolJustPressed(d.handbrake) ? 1 : 0;
        fr.pause = map.boolValue(d.pause) ? 1 : 0;
        fr.pauseP= map.boolJustPressed(d.pause) ? 1 : 0;
        out.push_back(fr);
        in.endFrame();
    }
}

} // namespace

void selftestActionsDet() {
    std::printf("[actions-det]\n");
    auto run = []() {
        nl::core::ActionMap map;
        const nl::core::DrivingActions d = nl::core::buildDrivingActionMap(map);
        nl::core::InputState in;
        std::vector<ActionFrame> fs;
        driveActionSequence(map, d, in, fs);
        return fs;
    };
    const std::vector<ActionFrame> a = run();
    const std::vector<ActionFrame> b = run();
    bool identical = (a.size() == b.size());
    for (size_t i = 0; identical && i < a.size(); ++i) identical = (a[i] == b[i]);
    check(identical, "action sequence replays bit-identical across 12 frames");
    // 序列内容锚：Float 轴 / 平局 / 手刹边沿 / 组开关清零 / 全松回零。
    check(a[0].th == 1.0f, "seq anchor: f0 throttle +1");
    check(a[2].st == 1.0f, "seq anchor: f2 steer +1");
    check(a[4].th == 1.0f, "seq anchor: f4 W+S tie -> W first-defined");
    check(a[6].hb == 1 && a[6].hbP == 1, "seq anchor: f6 handbrake press edge");
    check(a[8].pause == 1 && a[8].pauseP == 1, "seq anchor: f8 ui on -> pause edge");
    check(a[9].pause == 0 && a[9].pauseP == 0, "seq anchor: f9 ui off -> pause zeroed");
    check(a[10].th == 0.0f && a[10].st == 0.0f && a[10].hb == 0,
          "seq anchor: f10 all released -> zeros");
    check(a[11].lx == 1.5f && a[11].ly == -0.5f, "seq anchor: f11 look (0.5*(11%4), -0.25*(11%3))");
}

// ---- trace：确定性 12 帧序列 → TSV（evidence 管线数据源，真实工具输出） ----
int runTrace(int argc, char** argv) {
    int frames = 12;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames") {
            if (i + 1 >= argc) { std::printf("ERROR: --frames needs a value\n"); return 1; }
            frames = std::atoi(argv[++i]);
            if (frames <= 0 || frames > 1000) { std::printf("ERROR: bad --frames %s\n", argv[i]); return 1; }
        } else {
            std::printf("ERROR: unknown trace option '%s'\n", a.c_str());
            return 1;
        }
    }
    nl::core::InputState in;
    std::printf("frame\tw_down\tw_pressed\tw_released\tspace_down\tspace_pressed\t"
                "lmb_down\tlmb_pressed\tlmb_released\tdx\tdy\tscroll\tctrl\tshift\talt\n");
    for (int f = 0; f < frames; ++f) {
        if (f == 0)  { in.feedKey(GLFW_KEY_W, true); in.feedMods(false, false, false); }
        if (f == 3)  in.feedMouseButton(GLFW_MOUSE_BUTTON_LEFT, true);
        if (f == 5)  in.feedMouseButton(GLFW_MOUSE_BUTTON_LEFT, false);
        if (f == 6)  in.feedKey(GLFW_KEY_SPACE, true);
        if (f == 8)  in.feedKey(GLFW_KEY_SPACE, false);
        if (f == 10) in.feedKey(GLFW_KEY_W, false);
        in.feedMouseDelta(0.5f * (f % 4), -0.25f * (f % 3));
        in.feedScroll(0.0f, (f == 2) ? 1.0f : 0.0f);
        std::printf("%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.3f\t%.3f\t%.3f\t%d\t%d\t%d\n",
                    f,
                    in.isDown(GLFW_KEY_W) ? 1 : 0,
                    in.justPressed(GLFW_KEY_W) ? 1 : 0,
                    in.justReleased(GLFW_KEY_W) ? 1 : 0,
                    in.isDown(GLFW_KEY_SPACE) ? 1 : 0,
                    in.justPressed(GLFW_KEY_SPACE) ? 1 : 0,
                    in.mouseIsDown(GLFW_MOUSE_BUTTON_LEFT) ? 1 : 0,
                    in.mouseJustPressed(GLFW_MOUSE_BUTTON_LEFT) ? 1 : 0,
                    in.mouseJustReleased(GLFW_MOUSE_BUTTON_LEFT) ? 1 : 0,
                    in.mouseDelta().x, in.mouseDelta().y, in.scrollDelta().y,
                    in.ctrlDown() ? 1 : 0, in.shiftDown() ? 1 : 0, in.altDown() ? 1 : 0);
        in.endFrame();
    }
    return 0;
}

// ---- check：对 dump 出来的 trace 重验同一组锚（锚必须与 selftest 同源） ----
int runCheck(int argc, char** argv) {
    if (argc < 1) { std::printf("ERROR: check needs a <tsv> path\n"); return 1; }
    std::FILE* fp = std::fopen(argv[0], "r");
    if (!fp) { std::printf("ERROR: cannot open '%s'\n", argv[0]); return 1; }
    char line[512];
    int rows = 0;
    bool ok = true;
    // 逐行解析：frame \t 14 个字段。锚直接对 TSV 行断言（不重新跑序列——
    // check 的语义是「这份 dump 本身满足锚」，与 selftest 的确定性对拍互补）。
    struct Row { int f; int wDown, wPressed, wReleased, spaceDown, spacePressed,
                     lmbDown, lmbPressed, lmbReleased; double dx, dy, sc; int ctrl, shift, alt; };
    std::vector<Row> rows_v;
    while (std::fgets(line, sizeof(line), fp)) {
        Row r{};
        const int n = std::sscanf(line, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%lf\t%lf\t%lf\t%d\t%d\t%d",
                                  &r.f, &r.wDown, &r.wPressed, &r.wReleased, &r.spaceDown, &r.spacePressed,
                                  &r.lmbDown, &r.lmbPressed, &r.lmbReleased,
                                  &r.dx, &r.dy, &r.sc, &r.ctrl, &r.shift, &r.alt);
        if (n == 15 && rows_v.empty() && r.f == 0 && std::strstr(line, "frame\t") == line) continue; // header
        if (n == 15) { rows_v.push_back(r); ++rows; }
        else if (std::strstr(line, "frame\t") != line) { ok = false; }
    }
    std::fclose(fp);
    auto at = [&](int f) -> const Row* {
        for (const Row& r : rows_v) if (r.f == f) return &r;
        return nullptr;
    };
    check(rows == 12, "check: 12 rows");
    if (const Row* r0 = at(0))  check(r0->wPressed == 1 && r0->wDown == 1, "check: f0 W pressed+down"); else ok = false;
    if (const Row* r1 = at(1))  check(r1->wDown == 1 && r1->wPressed == 0, "check: f1 W down only"); else ok = false;
    if (const Row* r3 = at(3))  check(r3->lmbPressed == 1, "check: f3 LMB edge"); else ok = false;
    if (const Row* r10 = at(10)) check(r10->wReleased == 1 && r10->wDown == 0, "check: f10 W released"); else ok = false;
    if (const Row* r11 = at(11)) check(r11->dx > 1.49 && r11->dx < 1.51 && r11->dy < -0.49 && r11->dy > -0.51,
                                       "check: f11 delta values"); else ok = false;
    if (!ok || g_failures != 0) { std::printf("check FAIL\n"); return 1; }
    std::printf("check PASS (12 rows, 5 anchors)\n");
    return 0;
}

// ---- s10：映射文件 + 运行时重绑定 + 手柄源（M-I5，P8-INPUT-PLAN §7.2）----
// 锚：JSON 全量校验装载（坏文件不动 map）/ 手柄按钮-轴两源经 ActionMap 解析 /
// rebind 生效点 = 下一次 update（Carrot 冻结语义的有意偏离，逐位锚钉死）。
void selftestActionsFile() {
    std::printf("[actions-file]\n");
    namespace fs = std::filesystem;

    // 写临时映射文件（手柄 A=0 → fire(Bool)；左摇杆X → steer(Float)；组合 sets）
    const fs::path dir = fs::temp_directory_path() /
                         ("nl-input-actions-" + std::to_string(_getpid()));
    fs::create_directories(dir);
    const fs::path file = dir / "drive.actions.json";
    {
        std::ofstream f(file, std::ios::binary);
        f << "{\n"
          << "  \"sets\": [\"driving\"],\n"
          << "  \"actions\": [\n"
          << "    {\"name\": \"fire\", \"type\": \"bool\", \"sets\": [\"driving\"],\n"
          << "     \"binds\": [{\"src\": \"pad\", \"id\": 0}]},\n"
          << "    {\"name\": \"steer\", \"type\": \"float\", \"sets\": [\"driving\"],\n"
          << "     \"binds\": [{\"src\": \"padAxis\", \"id\": 0, \"scale\": 1.0}]},\n"
          << "    {\"name\": \"throttle\", \"type\": \"float\",\n"
          << "     \"binds\": [{\"src\": \"key\", \"id\": 87, \"scale\": 1.0}]}\n"
          << "  ]\n"
          << "}\n";
    }

    // 正向：装载 + 组开关 + 手柄两源解析
    {
        nl::core::ActionMap map;
        std::string err;
        check(map.loadFromFile(file.string().c_str(), err), "file: load ok");
        const auto fire = map.find("fire");
        const auto steer = map.find("steer");
        const auto throttle = map.find("throttle");
        check(fire != nl::core::ActionMap::kInvalidAction, "file: fire defined");
        check(steer != nl::core::ActionMap::kInvalidAction, "file: steer defined");
        check(throttle != nl::core::ActionMap::kInvalidAction, "file: throttle defined");
        check(map.type(fire) == nl::core::ActionMap::Type::Bool, "file: fire is Bool");
        check(map.type(steer) == nl::core::ActionMap::Type::Float, "file: steer is Float");

        const auto driving = map.findSet("driving");
        check(driving != nl::core::ActionMap::kInvalidSet, "file: driving set defined");
        map.setActive(driving, true);

        nl::core::InputState in;
        in.feedGamepadButton(0, true);      // A 按下 → fire 真（Bool 源）
        in.feedGamepadAxis(0, -0.5f);       // 左摇杆 X = -0.5 → steer = -0.5（轴源带 scale）
        in.feedKey(87, true);               // W → throttle = +1（键源）
        map.update(in);
        check(map.boolValue(fire) && map.boolJustPressed(fire), "file: pad button -> fire pressed");
        check(std::fabs(map.floatValue(steer) + 0.5f) < 1e-6f, "file: pad axis -> steer -0.5");
        check(std::fabs(map.floatValue(throttle) - 1.0f) < 1e-6f, "file: key -> throttle +1");

        // 组关停 = 清零（M-I1 语义在文件装载面同样成立）
        map.setActive(driving, false);
        in.endFrame();
        in.feedGamepadButton(0, true);
        map.update(in);
        check(!map.boolValue(fire) && !map.boolJustPressed(fire), "file: set-off clears fire");
    }

    // 负向：坏 src / 坏 type / 引用未声明组 —— map 不动（fail-closed 口径）
    {
        nl::core::ActionMap map;
        std::string err;
        const fs::path bad1 = dir / "bad-src.json";
        { std::ofstream f(bad1, std::ios::binary); f << "{\"actions\":[{\"name\":\"x\",\"type\":\"bool\",\"binds\":[{\"src\":\"warp\",\"id\":1}]}]}"; }
        check(!map.loadFromFile(bad1.string().c_str(), err) && err.find("unknown src") != std::string::npos,
              "file: unknown src rejected with reason");
        const fs::path bad2 = dir / "bad-set.json";
        { std::ofstream f(bad2, std::ios::binary); f << "{\"sets\":[\"a\"],\"actions\":[{\"name\":\"x\",\"type\":\"bool\",\"sets\":[\"b\"],\"binds\":[{\"src\":\"key\",\"id\":1}]}]}"; }
        check(!map.loadFromFile(bad2.string().c_str(), err) && err.find("unknown set") != std::string::npos,
              "file: unknown set reference rejected");
        const auto none = map.find("x");
        check(none == nl::core::ActionMap::kInvalidAction, "file: failed load leaves map untouched");
    }

    // rebind：生效点 = 下一次 update（throttle W→S 改绑后值翻符号）
    {
        nl::core::ActionMap map;
        const nl::core::DrivingActions d = nl::core::buildDrivingActionMap(map);
        nl::core::InputState in;
        in.feedKey(87, true);
        map.update(in);
        check(std::fabs(map.floatValue(d.throttle) - 1.0f) < 1e-6f, "rebind: pre W=+1");

        nl::core::ActionMap::Binding b;
        b.source = nl::core::ActionMap::Source::Key;
        b.index = 83;            // S
        b.scale = 1.0f;
        std::vector<nl::core::ActionMap::Binding> binds{b};
        check(map.rebind(d.throttle, binds), "rebind: replace ok");
        check(std::fabs(map.floatValue(d.throttle) - 1.0f) < 1e-6f, "rebind: frozen until next update");
        in.endFrame();
        in.feedKey(87, false);
        in.feedKey(83, true);
        map.update(in);
        check(std::fabs(map.floatValue(d.throttle) - 1.0f) < 1e-6f, "rebind: S now drives throttle");
    }

    // 手柄直查防御面（越界按钮/轴 = false/0 不崩）
    {
        nl::core::InputState in;
        in.feedGamepadButton(999, true);
        in.feedGamepadAxis(999, 1.0f);
        check(!in.gamepadIsDown(999) && in.gamepadAxis(999) == 0.0f && !in.gamepadJustPressed(0),
              "gamepad: oob feed no-op, queries defensive");
        in.feedGamepadButton(3, true);
        check(in.gamepadIsDown(3) && in.gamepadJustPressed(3), "gamepad: Y(3) down+pressed");
        in.endFrame();
        check(in.gamepadIsDown(3) && !in.gamepadJustPressed(3), "gamepad: edge cleared by endFrame");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
}

int runSelftest(int argc, char** argv) {
    static const Section sections[] = {
        {"edges",       selftestEdges},
        {"combos",      selftestCombos},
        {"repeat",      selftestRepeat},
        {"mouse",       selftestMouse},
        {"oob",         selftestOob},
        {"mods",        selftestMods},
        {"determinism", selftestDeterminism},
        {"actions",     selftestActions},
        {"actions-det", selftestActionsDet},
        {"actions-file", selftestActionsFile},
    };
    const char* only = (argc >= 1) ? argv[0] : nullptr;
    bool ranAny = false;
    for (const Section& s : sections) {
        if (only && std::strcmp(only, s.name) != 0) continue;
        ranAny = true;
        s.fn();
    }
    if (!ranAny) {
        std::printf("ERROR: unknown selftest section '%s'\n", only ? only : "");
        std::printf("usage: input-tools selftest [edges|combos|repeat|mouse|oob|mods|determinism|actions|actions-det|actions-file]\n");
        return 2;
    }
    if (g_failures != 0) {
        std::printf("selftest FAIL (%d failure(s))\n", g_failures);
        return 1;
    }
    std::printf("selftest PASS (%zu sections)\n", sizeof(sections) / sizeof(sections[0]));
    return 0;
}

// ---- act-trace：12 帧驾驶映射序列 → TSV（M-I1 evidence 管线数据源） ----
int runActTrace(int argc, char** argv) {
    int frames = 12;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames") {
            if (i + 1 >= argc) { std::printf("ERROR: --frames needs a value\n"); return 1; }
            frames = std::atoi(argv[++i]);
            if (frames <= 0 || frames > 1000) { std::printf("ERROR: bad --frames %s\n", argv[i]); return 1; }
        } else {
            std::printf("ERROR: unknown act-trace option '%s'\n", a.c_str());
            return 1;
        }
    }
    nl::core::ActionMap map;
    const nl::core::DrivingActions d = nl::core::buildDrivingActionMap(map);
    nl::core::InputState in;
    std::vector<ActionFrame> fs;
    driveActionSequence(map, d, in, fs);
    std::printf("frame\tthrottle\tsteer\tlook_x\tlook_y\thandbrake\thb_pressed\tpause\tpause_pressed\n");
    for (int f = 0; f < frames && f < static_cast<int>(fs.size()); ++f) {
        const ActionFrame& fr = fs[f];
        // +0.0f 归一化：IEEE 负零（-0.25*0 = -0.0f）在 %.3f 下打成 "-0.000"，TSV 不留负零。
        std::printf("%d\t%.3f\t%.3f\t%.3f\t%.3f\t%d\t%d\t%d\t%d\n",
                    f, fr.th + 0.0f, fr.st + 0.0f, fr.lx + 0.0f, fr.ly + 0.0f, fr.hb, fr.hbP, fr.pause, fr.pauseP);
    }
    return 0;
}

// ---- act-check：对 dump 出来的 act-trace 重验同组锚（与 selftest 同源） ----
int runActCheck(int argc, char** argv) {
    if (argc < 1) { std::printf("ERROR: act-check needs a <tsv> path\n"); return 1; }
    std::FILE* fp = std::fopen(argv[0], "r");
    if (!fp) { std::printf("ERROR: cannot open '%s'\n", argv[0]); return 1; }
    char line[512];
    int rows = 0;
    bool ok = true;
    struct Row { int f; double th, st, lx, ly; int hb, hbP, pause, pauseP; };
    std::vector<Row> rows_v;
    while (std::fgets(line, sizeof(line), fp)) {
        Row r{};
        const int n = std::sscanf(line, "%d\t%lf\t%lf\t%lf\t%lf\t%d\t%d\t%d\t%d",
                                  &r.f, &r.th, &r.st, &r.lx, &r.ly, &r.hb, &r.hbP, &r.pause, &r.pauseP);
        if (n == 9 && rows_v.empty() && r.f == 0 && std::strstr(line, "frame\t") == line) continue; // header
        if (n == 9) { rows_v.push_back(r); ++rows; }
        else if (std::strstr(line, "frame\t") != line) { ok = false; }
    }
    std::fclose(fp);
    auto at = [&](int f) -> const Row* {
        for (const Row& r : rows_v) if (r.f == f) return &r;
        return nullptr;
    };
    auto near = [](double v, double expect) { return v > expect - 1e-3 && v < expect + 1e-3; };
    check(rows == 12, "act-check: 12 rows");
    if (const Row* r = at(0))  check(near(r->th, 1.0), "act-check: f0 throttle +1"); else ok = false;
    if (const Row* r = at(2))  check(near(r->st, 1.0) && near(r->lx, 1.0) && near(r->ly, -0.5),
                                     "act-check: f2 steer +1, look (1.0,-0.5)"); else ok = false;
    if (const Row* r = at(4))  check(near(r->th, 1.0), "act-check: f4 W+S tie -> W"); else ok = false;
    if (const Row* r = at(6))  check(r->hb == 1 && r->hbP == 1, "act-check: f6 handbrake press edge"); else ok = false;
    if (const Row* r = at(8))  check(r->pause == 1 && r->pauseP == 1, "act-check: f8 ui on -> pause edge"); else ok = false;
    if (const Row* r = at(9))  check(r->pause == 0 && r->pauseP == 0, "act-check: f9 ui off -> pause zeroed"); else ok = false;
    if (const Row* r = at(10)) check(near(r->th, 0.0) && near(r->st, 0.0) && r->hb == 0,
                                     "act-check: f10 all released -> zeros"); else ok = false;
    if (const Row* r = at(11)) check(near(r->lx, 1.5) && near(r->ly, -0.5),
                                     "act-check: f11 look values"); else ok = false;
    if (!ok || g_failures != 0) { std::printf("act-check FAIL\n"); return 1; }
    std::printf("act-check PASS (12 rows, 9 anchors)\n");
    return 0;
}

void printUsage() {
    std::printf(
        "input-tools — P8 M-I0/M-I1 input acceptance tool (state layer + action mapping)\n"
        "usage:\n"
        "  input-tools selftest [section]   run mechanical anchors (default: all)\n"
        "  input-tools trace [--frames N]   dump the 12-frame state-layer drive sequence as TSV\n"
        "  input-tools check <tsv>          re-verify a dumped trace with the same anchors\n"
        "  input-tools act-trace [--frames N]  dump the 12-frame driving-map action sequence as TSV\n"
        "  input-tools act-check <tsv>      re-verify a dumped act-trace with the same anchors\n"
        "sections: edges combos repeat mouse oob mods determinism actions actions-det\n"
        "exit codes: 0=PASS 1=failure 2=usage error\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "selftest")  return runSelftest(argc - 2, argv + 2);
    if (cmd == "trace")     return runTrace(argc - 2, argv + 2);
    if (cmd == "check")     return runCheck(argc - 2, argv + 2);
    if (cmd == "act-trace") return runActTrace(argc - 2, argv + 2);
    if (cmd == "act-check") return runActCheck(argc - 2, argv + 2);
    printUsage();
    return 2;
}
