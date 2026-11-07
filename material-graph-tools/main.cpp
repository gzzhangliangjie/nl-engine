// B 路材质图工具（PARALLEL-TRACKS.md §2.1 第 2 步种子，A 类轨道工具版交代）：
//   - 节点图数据模型的确定性机械单测（construct / validate / 序列化 roundtrip / 负向）
//   - 从文本图加载并 dump，以及用代码装配一个示例图写盘（供后续「图→Slang」步骤消费）
// 只依赖 core（日志），零图形 API —— 与 nanite-tools 的纪律一致。

#include "nl/material_graph/graph.hpp"
#include "nl/material_graph/emit.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

void printUsage() {
    std::printf(
        "material-graph-tools - nl-engine material graph (B-route)\n"
        "usage:\n"
        "  material-graph-tools selftest            deterministic unit tests (model + serialize)\n"
        "  material-graph-tools build <out.mg>      assemble a sample graph and write it\n"
        "  material-graph-tools dump <in.mg>        load and pretty-print a graph\n"
        "  material-graph-tools gen <in.mg> <out.slang>   graph -> Slang (B3)\n"
        "  material-graph-tools check               emit sample -> slangc spirv+dxil x vs/ps\n");
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

// check：内部装配示例图 -> emit -> slangc 双后端 x (vs/ps) 编译
int cmdCheck() {
    nl::material_graph::MaterialGraph g = buildSample();
    std::string err;
    if (!g.validate(&err)) { std::fprintf(stderr, "check: graph invalid: %s\n", err.c_str()); return 1; }
    std::string slangc = findSlangc();
    std::string dxcDir = findDxc();
    std::string tmpSlang = "_mg_check.slang";
    {
        std::ofstream os(tmpSlang, std::ios::binary);
        if (!nl::material_graph::emitSlang(g, os, &err)) {
            std::fprintf(stderr, "check: emit failed: %s\n", err.c_str()); return 1;
        }
    }
    std::printf("check      : emitting + compiling (spirv/dxil x vs/ps)\n");
    bool ok = true;
    ok &= runSlangc(slangc, dxcDir, tmpSlang, "_mg_check.ps.spv",  "spirv", "psMain", "fragment");
    ok &= runSlangc(slangc, dxcDir, tmpSlang, "_mg_check.vs.spv",  "spirv", "vsMain", "vertex");
    ok &= runSlangc(slangc, dxcDir, tmpSlang, "_mg_check.ps.dxil", "dxil",  "psMain", "fragment");
    ok &= runSlangc(slangc, dxcDir, tmpSlang, "_mg_check.vs.dxil", "dxil",  "vsMain", "vertex");
    std::remove(tmpSlang.c_str());
    std::remove("_mg_check.ps.spv");   std::remove("_mg_check.vs.spv");
    std::remove("_mg_check.ps.dxil");  std::remove("_mg_check.vs.dxil");
    if (ok) { std::printf("genslang   : OK\n"); return 0; }
    std::printf("genslang   : FAIL\n");
    return 1;
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
    std::fprintf(stderr, "unknown subcommand: %s\n", cmd.c_str());
    printUsage();
    return 1;
}
