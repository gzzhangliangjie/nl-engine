// input-tools — P8 M-I0 headless input acceptance tool (A-class parallel track).
// Only links nl_core (nl::core::InputState injection path); zero graphics API,
// no GPU, no kStage, no rendering regression. No real window: all anchors go
// through the synthetic-event injection path, which shares the same update
// kernel (applyKey/applyMouseButton) as the future window path.
//
// Exit code contract (aligned with phys-tools / anim-tools, showcase expectExit):
//   0 = PASS            1 = assertion / runtime failure            2 = usage error
//
// Subcommands:
//   selftest [section]     mechanical anchors; section omitted = run all
//                          (edges / combos / repeat / mouse / oob / mods /
//                          determinism); unknown section = usage error (exit 2)
//   trace [options]        dump the 12-frame synthetic drive sequence as TSV
//                          (--frames N, default 12; real tool output, feeds
//                          the evidence renderer)
//   check <tsv>            re-verify a dumped trace with the same anchors
//                          (row count + f0/f1/f3/f10/f11 key-state anchors)
//
// Evidence rule: no GPU screenshots for M-I0 — the tool-card evidence image is
// generated from trace TSV by tools/make-input-m0-evidence.py (real output,
// no hand-drawn diagrams).

#include "nl/core/input.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

int runSelftest(int argc, char** argv) {
    static const Section sections[] = {
        {"edges",       selftestEdges},
        {"combos",      selftestCombos},
        {"repeat",      selftestRepeat},
        {"mouse",       selftestMouse},
        {"oob",         selftestOob},
        {"mods",        selftestMods},
        {"determinism", selftestDeterminism},
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
        std::printf("usage: input-tools selftest [edges|combos|repeat|mouse|oob|mods|determinism]\n");
        return 2;
    }
    if (g_failures != 0) {
        std::printf("selftest FAIL (%d failure(s))\n", g_failures);
        return 1;
    }
    std::printf("selftest PASS (7 sections)\n");
    return 0;
}

void printUsage() {
    std::printf(
        "input-tools — P8 M-I0 input state layer acceptance tool\n"
        "usage:\n"
        "  input-tools selftest [section]   run mechanical anchors (default: all)\n"
        "  input-tools trace [--frames N]   dump the 12-frame synthetic drive sequence as TSV\n"
        "  input-tools check <tsv>          re-verify a dumped trace with the same anchors\n"
        "sections: edges combos repeat mouse oob mods determinism\n"
        "exit codes: 0=PASS 1=failure 2=usage error\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "selftest") return runSelftest(argc - 2, argv + 2);
    if (cmd == "trace")    return runTrace(argc - 2, argv + 2);
    if (cmd == "check")    return runCheck(argc - 2, argv + 2);
    printUsage();
    return 2;
}
