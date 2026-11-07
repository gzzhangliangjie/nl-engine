// B 路材质图工具（PARALLEL-TRACKS.md §2.1 第 2 步种子，A 类轨道工具版交代）：
//   - 节点图数据模型的确定性机械单测（construct / validate / 序列化 roundtrip / 负向）
//   - 从文本图加载并 dump，以及用代码装配一个示例图写盘（供后续「图→Slang」步骤消费）
// 只依赖 core（日志），零图形 API —— 与 nanite-tools 的纪律一致。

#include "nl/material_graph/graph.hpp"
#include "nl/material_graph/emit.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

namespace {

void printUsage() {
    std::printf(
        "material-graph-tools - nl-engine material graph (B-route)\n"
        "usage:\n"
        "  material-graph-tools selftest            deterministic unit tests (model + serialize)\n"
        "  material-graph-tools build <out.mg>      assemble a sample graph and write it\n"
        "  material-graph-tools dump <in.mg>        load and pretty-print a graph\n"
        "  material-graph-tools gen <in.mg> <out.slang>   graph -> Slang (B3)\n"
        "  material-graph-tools check               emit sample -> slangc spirv+dxil x vs/ps\n"
        "  material-graph-tools render <in.mg> <out.svg>  graph -> SVG evidence image (deterministic)\n"
        "  material-graph-tools watch <in.mg> [--slang DIR] [--dxc DIR]\n"
        "                              [--interval MS] [--once]   hot-reload recompile (B4)\n");
}

// 用代码装配一个最小有意义图：两个常量 + 乘法 → 输出（供后续代码生成消费）
nl::material_graph::MaterialGraph buildSample() {
    using namespace nl::material_graph;
    MaterialGraph g;
    int nA = g.addNode("ConstantFloat", 0.0f, 0.0f);
    g.setProp(nA, "value", "0.8");
    int pA = g.addPin(nA, "out", ValueType::Float, true);

    int nB = g.addNode("ConstantFloat", 0.0f, 120.0f);
    g.setProp(nB, "value", "0.5");
    int pB = g.addPin(nB, "out", ValueType::Float, true);

    int nMul = g.addNode("Multiply", 200.0f, 60.0f);
    int pMulA = g.addPin(nMul, "a", ValueType::Float, false);
    int pMulB = g.addPin(nMul, "b", ValueType::Float, false);
    int pMulO = g.addPin(nMul, "out", ValueType::Float, true);
    g.connect(nA, pA, nMul, pMulA);
    g.connect(nB, pB, nMul, pMulB);

    int nOut = g.addNode("Output", 400.0f, 60.0f);
    int pOutIn = g.addPin(nOut, "baseColor", ValueType::Float, false);
    g.connect(nMul, pMulO, nOut, pOutIn);
    return g;
}

int cmdBuild(int argc, char** argv) {
    if (argc < 1) {
        std::fprintf(stderr, "build: missing <out.mg>\n");
        return 1;
    }
    nl::material_graph::MaterialGraph g = buildSample();
    std::string err;
    if (!g.validate(&err)) {
        std::fprintf(stderr, "build: graph invalid: %s\n", err.c_str());
        return 1;
    }
    std::ofstream os(argv[0], std::ios::binary);
    if (!os) { std::fprintf(stderr, "build: cannot open %s\n", argv[0]); return 1; }
    if (!g.serialize(os)) { std::fprintf(stderr, "build: serialize failed\n"); return 1; }
    std::printf("build      : %s\n", argv[0]);
    std::printf("nodes      : %d\n", g.nodeCount());
    std::printf("edges      : %d\n", g.edgeCount());
    return 0;
}

int cmdDump(int argc, char** argv) {
    if (argc < 1) {
        std::fprintf(stderr, "dump: missing <in.mg>\n");
        return 1;
    }
    std::ifstream is(argv[0], std::ios::binary);
    if (!is) { std::fprintf(stderr, "dump: cannot open %s\n", argv[0]); return 1; }
    nl::material_graph::MaterialGraph g;
    std::string err;
    if (!nl::material_graph::MaterialGraph::deserialize(is, g, &err)) {
        std::fprintf(stderr, "dump: %s\n", err.c_str());
        return 1;
    }
    std::printf("file       : %s\n", argv[0]);
    std::printf("nodes      : %d\n", g.nodeCount());
    std::printf("edges      : %d\n", g.edgeCount());
    for (const auto& n : g.nodes()) {
        std::printf("  node %d %s @(%.2f,%.2f)\n", n.id, n.type.c_str(), n.x, n.y);
        for (const auto& kv : n.props)
            std::printf("    prop %s = %s\n", kv.first.c_str(), kv.second.c_str());
        for (const auto& p : n.pins)
            std::printf("    pin %d %s %s %s\n", p.id, p.name.c_str(),
                        nl::material_graph::valueTypeName(p.type),
                        p.isOutput ? "out" : "in");
    }
    std::string verr;
    if (!g.validate(&verr)) { std::printf("VALIDATE   : FAIL (%s)\n", verr.c_str()); return 1; }
    std::printf("VALIDATE   : PASS\n");
    return 0;
}

// ---- gen / check：B3 图 -> Slang 代码生成 + 双后端编译验收 ----
namespace {
// 定位 slangc：env NL_SLANG_BIN（目录）优先，否则退回本机已知路径（与 CMake 同源）。
std::string findSlangc() {
    const char* bin = std::getenv("NL_SLANG_BIN");
    if (bin && *bin) return std::string(bin) + "\\slangc.exe";
    return "E:\\UE_STUDY_2026\\nl-engine-material-graph\\thirdparty\\slang\\dist\\bin\\slangc.exe";
}
std::string findDxc() {
    const char* d = std::getenv("NL_DXC_DIR");
    if (d && *d) return std::string(d);
    return "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64";
}
// 仅当路径含空格时加引号（避免 system() 把无空格的可执行路径用引号包住导致 cmd 解析失败）。
std::string q(const std::string& p) {
    if (p.find(' ') != std::string::npos) return "\"" + p + "\"";
    return p;
}
// 跑一条 slangc；exit 0 才返回 true。SPIR-V 走统一绑定平移，DXIL 走 dxc-path。
bool runSlangc(const std::string& slangc, const std::string& dxcDir,
               const std::string& inSlang, const std::string& out,
               const char* target, const char* entry, const char* stage) {
    std::string cmd = q(slangc) + " " + q(inSlang) + " -target " + target
                    + " -entry " + entry + " -stage " + stage;
    if (std::string(target) == "spirv")
        cmd += " -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0";
    else
        cmd += " -dxc-path " + q(dxcDir);
    cmd += " -o " + q(out);
    std::printf("  > slangc %s/%s/%s\n", target, entry, stage);
    int rc = std::system(cmd.c_str());
    if (rc != 0) { std::printf("    slangc failed (rc=%d)\n", rc); return false; }
    return true;
}
}  // namespace

int cmdGen(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "gen: missing <in.mg> <out.slang>\n"); return 1; }
    std::ifstream is(argv[0], std::ios::binary);
    if (!is) { std::fprintf(stderr, "gen: cannot open %s\n", argv[0]); return 1; }
    nl::material_graph::MaterialGraph g;
    std::string err;
    if (!nl::material_graph::MaterialGraph::deserialize(is, g, &err)) {
        std::fprintf(stderr, "gen: %s\n", err.c_str()); return 1;
    }
    if (!g.validate(&err)) { std::fprintf(stderr, "gen: graph invalid: %s\n", err.c_str()); return 1; }
    std::ofstream os(argv[1], std::ios::binary);
    if (!os) { std::fprintf(stderr, "gen: cannot open %s\n", argv[1]); return 1; }
    if (!nl::material_graph::emitSlang(g, os, &err)) {
        std::fprintf(stderr, "gen: emit failed: %s\n", err.c_str()); return 1;
    }
    std::printf("gen        : %s -> %s\n", argv[0], argv[1]);
    return 0;
}

// B4：热重载核心。把图 emit + slangc 双后端 x (vs/ps)，集中汇报每次结果。
// recompileGraph 是 B3 验收链路的复用点：check / watch 都走它。
struct RecompileReport {
    bool psSpirv = false, vsSpirv = false, psDxil = false, vsDxil = false;
    bool ok() const { return psSpirv && vsSpirv && psDxil && vsDxil; }
};
bool recompileGraph(const nl::material_graph::MaterialGraph& g,
                    const std::string& slangc, const std::string& dxcDir,
                    RecompileReport* rep) {
    std::string err;
    if (!g.validate(&err)) { std::printf("  validate FAIL: %s\n", err.c_str()); return false; }
    std::string tmp = "_mg_hot.slang";
    {
        std::ofstream os(tmp, std::ios::binary);
        if (!nl::material_graph::emitSlang(g, os, &err)) {
            std::printf("  emit FAIL: %s\n", err.c_str()); return false;
        }
    }
    RecompileReport r;
    r.psSpirv = runSlangc(slangc, dxcDir, tmp, "_mg_hot.ps.spv",  "spirv", "psMain", "fragment");
    r.vsSpirv = runSlangc(slangc, dxcDir, tmp, "_mg_hot.vs.spv",  "spirv", "vsMain", "vertex");
    r.psDxil = runSlangc(slangc, dxcDir, tmp, "_mg_hot.ps.dxil", "dxil",  "psMain", "fragment");
    r.vsDxil = runSlangc(slangc, dxcDir, tmp, "_mg_hot.vs.dxil", "dxil",  "vsMain", "vertex");
    std::remove(tmp.c_str());
    std::remove("_mg_hot.ps.spv");   std::remove("_mg_hot.vs.spv");
    std::remove("_mg_hot.ps.dxil");  std::remove("_mg_hot.vs.dxil");
    if (rep) *rep = r;
    return r.ok();
}

// check：内部装配示例图 -> 重编译（emit + 双后端编译）。B3 验收入口，B4 复用 recompileGraph。
int cmdCheck() {
    nl::material_graph::MaterialGraph g = buildSample();
    std::string err;
    if (!g.validate(&err)) { std::fprintf(stderr, "check: graph invalid: %s\n", err.c_str()); return 1; }
    RecompileReport rep;
    bool ok = recompileGraph(g, findSlangc(), findDxc(), &rep);
    if (ok) { std::printf("genslang   : OK\n"); return 0; }
    std::printf("genslang   : FAIL\n");
    return 1;
}

// B4：热重载。轮询 .mg 文件修改时间，变更即重新 emit + 编译双后端；
// 首轮先编译一次作基线。--once 只编译一次（供 ctest 非交互验收）。
int cmdWatch(int argc, char** argv) {
    // 长进程输出走管道时默认全缓冲，强制无缓冲以便实时看到每次重编译结果。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string inMg, slangBin, dxcDir;
    long intervalMs = 500;
    bool once = false;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--slang" && i + 1 < argc) slangBin = argv[++i];
        else if (a == "--dxc" && i + 1 < argc) dxcDir = argv[++i];
        else if (a == "--interval" && i + 1 < argc) intervalMs = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--once") once = true;
        else if (inMg.empty()) inMg = a;
        else { std::fprintf(stderr, "watch: unexpected arg %s\n", a.c_str()); return 1; }
    }
    if (inMg.empty()) { std::fprintf(stderr, "watch: missing <in.mg>\n"); return 1; }
    std::string slangc = slangBin.empty() ? findSlangc() : slangBin + "\\slangc.exe";
    std::string dxc = dxcDir.empty() ? findDxc() : dxcDir;

    auto stamp = []() -> std::string {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        return buf;
    };

    std::error_code ec;
    std::filesystem::file_time_type last = std::filesystem::last_write_time(inMg, ec);
    if (ec) { std::fprintf(stderr, "watch: cannot stat %s\n", inMg.c_str()); return 1; }
    bool first = true;
    for (;;) {
        auto now = std::filesystem::last_write_time(inMg, ec);
        if (ec) {
            std::printf("%s  ERR (stat)\n", stamp().c_str());
            if (once) return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
            continue;
        }
        if (first || now != last) {
            first = false; last = now;
            std::ifstream is(inMg, std::ios::binary);
            nl::material_graph::MaterialGraph g;
            std::string err;
            if (!nl::material_graph::MaterialGraph::deserialize(is, g, &err)) {
                std::printf("%s  LOAD FAIL (%s)\n", stamp().c_str(), err.c_str());
                if (once) return 1;
                std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
                continue;
            }
            RecompileReport rep;
            bool ok = recompileGraph(g, slangc, dxc, &rep);
            if (ok) std::printf("%s  OK\n", stamp().c_str());
            else std::printf("%s  FAIL (psS=%d vsS=%d psD=%d vsD=%d)\n", stamp().c_str(),
                             rep.psSpirv, rep.vsSpirv, rep.psDxil, rep.vsDxil);
            if (once) return ok ? 0 : 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }
}

// ---- render：从 .mg 真实数据生成 SVG 证据图（确定性布局，非手绘）----
// A 类纪律：工具不产 GPU 截图时，证据图必须从工具真实输出生成。
// 布局：不信任文件内坐标也可用？——直接用序列化时保存的编辑器坐标（Node.x/y，
// buildSample 已赋值），这是图的真实状态；画布尺寸按内容自适应。
namespace {
// 引脚类型 -> 圆点填充色（标签语义可视化，与 ValueType 一一对应）
const char* pinColor(nl::material_graph::ValueType t) {
    using nl::material_graph::ValueType;
    switch (t) {
        case ValueType::Float:    return "#7ec8ff";
        case ValueType::Float2:   return "#7effc8";
        case ValueType::Float3:   return "#ffe27e";
        case ValueType::Float4:   return "#ff9e7e";
        case ValueType::Color3:   return "#ff7ec8";
        case ValueType::Bool:     return "#d0d0d0";
        case ValueType::Texture2D:return "#c87eff";
        case ValueType::Material: return "#ff6060";
        default:                  return "#666666";
    }
}
}  // namespace

int cmdRender(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "render: missing <in.mg> <out.svg>\n"); return 1; }
    std::ifstream is(argv[0], std::ios::binary);
    if (!is) { std::fprintf(stderr, "render: cannot open %s\n", argv[0]); return 1; }
    nl::material_graph::MaterialGraph g;
    std::string err;
    if (!nl::material_graph::MaterialGraph::deserialize(is, g, &err)) {
        std::fprintf(stderr, "render: %s\n", err.c_str()); return 1;
    }
    if (!g.validate(&err)) { std::fprintf(stderr, "render: graph invalid: %s\n", err.c_str()); return 1; }

    using nl::material_graph::Node;
    using nl::material_graph::Pin;
    const float NW = 190.0f;          // 节点框宽
    const float TITLE_H = 26.0f;      // 标题栏高
    const float PIN_H = 22.0f;        // 每行引脚高
    const float PAD_B = 10.0f;        // 底部留白
    const float MARGIN = 40.0f;

    // 画布尺寸：按所有节点包围盒自适应（坐标可能是负数时整体平移）
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (const auto& n : g.nodes()) {
        float inN = 0, outN = 0;
        for (const auto& p : n.pins) (p.isOutput ? outN : inN) += 1;
        float h = TITLE_H + (inN > outN ? inN : outN) * PIN_H + PAD_B;
        minX = (n.x < minX) ? n.x : minX;  minY = (n.y < minY) ? n.y : minY;
        maxX = (n.x + NW > maxX) ? n.x + NW : maxX;  maxY = (n.y + h > maxY) ? n.y + h : maxY;
    }
    float ox = MARGIN - minX, oy = MARGIN - minY;  // 整体平移量
    float W = (maxX - minX) + 2 * MARGIN, H = (maxY - minY) + 2 * MARGIN + 28.0f; // 底部一行元信息

    // 引脚在节点内的行号（输入列 / 输出列各自计数）
    std::ostringstream svg;
    svg << std::fixed << std::setprecision(1);
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << W << "\" height=\"" << H
        << "\" viewBox=\"0 0 " << W << " " << H << "\" font-family=\"Consolas,monospace\">\n";
    svg << "<rect width=\"100%\" height=\"100%\" fill=\"#1e1e28\"/>\n";
    svg << "<text x=\"" << (MARGIN) << "\" y=\"24\" font-size=\"14\" fill=\"#9aa0b4\">"
        << "material_graph v1  |  " << g.nodeCount() << " nodes, " << g.edgeCount()
        << " edges  |  generated by material-graph-tools render</text>\n";

    // 先画连线（在节点下层）。贝塞尔控制点水平外伸，视觉上像标准节点编辑器。
    svg << "<g>\n";
    for (const auto& e : g.edges()) {
        const Node* sn = g.findNode(e.fromNode);
        const Node* dn = g.findNode(e.toNode);
        const Pin* sp = g.findPin(e.fromNode, e.fromPin);
        const Pin* dp = g.findPin(e.toNode, e.toPin);
        if (!sn || !dn || !sp || !dp) continue;
        // 输出行号：该节点所有输出引脚中此引脚的序号
        int so = 0, doo = 0;
        for (const auto& p : sn->pins) { if (p.isOutput) { if (p.id == sp->id) break; ++so; } }
        for (const auto& p : dn->pins) { if (!p.isOutput) { if (p.id == dp->id) break; ++doo; } }
        float x1 = sn->x + ox + NW, y1 = sn->y + oy + TITLE_H + so * PIN_H + PIN_H * 0.5f;
        float x2 = dn->x + ox,      y2 = dn->y + oy + TITLE_H + doo * PIN_H + PIN_H * 0.5f;
        float c = (x2 - x1) * 0.5f; if (c < 40.0f) c = 40.0f;
        svg << "<path d=\"M " << x1 << " " << y1
            << " C " << (x1 + c) << " " << y1 << ", " << (x2 - c) << " " << y2 << ", "
            << x2 << " " << y2 << "\" fill=\"none\" stroke=\"#8892b0\" stroke-width=\"2\"/>\n";
        // 箭头小三角（终点）
        svg << "<path d=\"M " << x2 << " " << y2 << " l -7 -4 l 0 8 z\" fill=\"#8892b0\"/>\n";
    }
    svg << "</g>\n";

    // 节点框 + 标题 + 引脚 + 属性值
    for (const auto& n : g.nodes()) {
        int inN = 0, outN = 0;
        for (const auto& p : n.pins) (p.isOutput ? outN : inN) += 1;
        float h = TITLE_H + (inN > outN ? inN : outN) * PIN_H + PAD_B;
        float x = n.x + ox, y = n.y + oy;
        svg << "<g>\n";
        svg << "<rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << NW << "\" height=\"" << h
            << "\" rx=\"6\" fill=\"#2b2b3a\" stroke=\"#555a78\" stroke-width=\"1.5\"/>\n";
        svg << "<rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << NW << "\" height=\"" << TITLE_H
            << "\" rx=\"6\" fill=\"#3a3f5c\"/>\n";
        svg << "<text x=\"" << (x + NW * 0.5f) << "\" y=\"" << (y + 17.5f)
            << "\" font-size=\"13\" fill=\"#e8eaf2\" text-anchor=\"middle\" font-weight=\"bold\">"
            << n.type << "</text>\n";
        int ii = 0, oo = 0;
        for (const auto& p : n.pins) {
            float py = y + TITLE_H + (p.isOutput ? oo : ii) * PIN_H + PIN_H * 0.5f;
            float cx = p.isOutput ? (x + NW) : x;
            svg << "<circle cx=\"" << cx << "\" cy=\"" << py << "\" r=\"4.5\" fill=\""
                << pinColor(p.type) << "\" stroke=\"#1e1e28\" stroke-width=\"1\"/>\n";
            svg << "<text x=\"" << (p.isOutput ? (x + NW - 12.0f) : (x + 12.0f)) << "\" y=\""
                << (py + 4.0f) << "\" font-size=\"11\" fill=\"#b8bdd0\" text-anchor=\""
                << (p.isOutput ? "end" : "start") << "\">" << p.name
                << " <tspan fill=\"#6f7590\">" << nl::material_graph::valueTypeName(p.type)
                << "</tspan></text>\n";
            if (p.isOutput) ++oo; else ++ii;
        }
        // 属性值显示在引脚区之后
        float ay = y + TITLE_H + (inN > outN ? inN : outN) * PIN_H + 4.0f;
        for (const auto& kv : n.props) {
            svg << "<text x=\"" << (x + 12.0f) << "\" y=\"" << (ay + 10.0f)
                << "\" font-size=\"11\" fill=\"#8fd18f\">" << kv.first << " = "
                << kv.second << "</text>\n";
            ay += 14.0f;
        }
        svg << "</g>\n";
    }
    svg << "</svg>\n";

    std::ofstream os(argv[1], std::ios::binary);
    if (!os) { std::fprintf(stderr, "render: cannot open %s\n", argv[1]); return 1; }
    os << svg.str();
    std::printf("render     : %s -> %s (%d nodes, %d edges)\n", argv[0], argv[1], g.nodeCount(), g.edgeCount());
    return 0;
}

// ---- selftest：节点图数据模型 + 序列化（第 2 步验收核心）----
int selftestGraph() {
    using namespace nl::material_graph;
    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        if (!cond) { std::printf("  FAIL: %s\n", msg); ++failures; }
    };

    // 1) 构造 + validate 通过
    MaterialGraph g = buildSample();
    std::string err;
    check(g.nodeCount() == 4, "expected 4 nodes");
    check(g.edgeCount() == 3, "expected 3 edges");
    check(g.validate(&err), "sample graph should validate");

    // 2) 序列化确定性：两次写出字节一致
    std::ostringstream s1, s2;
    check(g.serialize(s1), "serialize #1");
    check(g.serialize(s2), "serialize #2");
    check(s1.str() == s2.str(), "serialize must be deterministic");

    // 3) roundtrip：反序列化后结构一致
    std::istringstream in(s1.str());
    MaterialGraph g2;
    check(MaterialGraph::deserialize(in, g2, &err), "deserialize roundtrip");
    check(g2.nodeCount() == g.nodeCount(), "roundtrip node count");
    check(g2.edgeCount() == g.edgeCount(), "roundtrip edge count");
    // 重新序列化应与原串一致（幂等）
    std::ostringstream s3;
    check(g2.serialize(s3), "re-serialize");
    check(s3.str() == s1.str(), "roundtrip must be byte-identical");

    // 4) 负向：类型不匹配的连线被拒
    MaterialGraph bad;
    int nF = bad.addNode("ConstantFloat");
    int pF = bad.addPin(nF, "out", ValueType::Float, true);
    int nT = bad.addNode("Output");
    int pT = bad.addPin(nT, "baseColor", ValueType::Float3, false);
    check(bad.connect(nF, pF, nT, pT) == -1, "type-mismatch connect must be rejected");

    // 5) 负向：同一输入引脚第二条入边被拒
    MaterialGraph dup;
    int a = dup.addNode("ConstantFloat");
    int pa = dup.addPin(a, "out", ValueType::Float, true);
    int b = dup.addNode("ConstantFloat");
    int pb = dup.addPin(b, "out", ValueType::Float, true);
    int m = dup.addNode("Output");
    int pm = dup.addPin(m, "in", ValueType::Float, false);
    check(dup.connect(a, pa, m, pm) >= 0, "first edge ok");
    check(dup.connect(b, pb, m, pm) == -1, "duplicate input edge must be rejected");

    // 6) 负向：缺 header 的反序列化失败
    std::istringstream broken("node 1 Foo 0 0\n");
    MaterialGraph g3;
    check(!MaterialGraph::deserialize(broken, g3, nullptr),
          "headerless input must be rejected");

    if (failures == 0) {
        std::printf("material-graph selftest: PASS\n");
        return 0;
    }
    std::printf("material-graph selftest: FAIL (%d case(s))\n", failures);
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { printUsage(); return 1; }
    const std::string cmd = argv[1];
    if (cmd == "selftest") return selftestGraph();
    if (cmd == "build")    return cmdBuild(argc - 2, argv + 2);
    if (cmd == "dump")     return cmdDump(argc - 2, argv + 2);
    if (cmd == "gen")      return cmdGen(argc - 2, argv + 2);
    if (cmd == "check")    return cmdCheck();
    if (cmd == "render")   return cmdRender(argc - 2, argv + 2);
    if (cmd == "watch")    return cmdWatch(argc - 2, argv + 2);
    std::fprintf(stderr, "unknown subcommand: %s\n", cmd.c_str());
    printUsage();
    return 1;
}
