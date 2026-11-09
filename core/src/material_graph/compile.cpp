// M-J5a：editor 与 CLI 共用的图编译实现（唯一一份）。
// 旧实现有两处重复：本文件的 _mg_ed_* 与 material-graph-tools/main.cpp 的 _mg_hot_*，
// 现统一到 recompileGraph()，工具侧只剩参数组装。
// 4 路 slangc 用 nl::core::jobgraph 的 fan-in DAG（底层 vendored Taskflow）并行；
// 结果按登记序归并、逐路命令回显也按登记序打印，保证并行不改输出。

#include "nl/material_graph/compile.hpp"

#include "nl/core/jobgraph.hpp"
#include "nl/material_graph/emit.hpp"
#include "nl/material_graph/graph.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace nl::material_graph {

namespace {

// 仅当路径含空格时加引号（system() 引号陷阱）。
std::string q(const std::string& p) {
    if (p.find(' ') != std::string::npos) return "\"" + p + "\"";
    return p;
}

std::string findSlangc() {
    const char* bin = std::getenv("NL_SLANG_BIN");
    if (bin && *bin) return std::string(bin) + "\\slangc.exe";
    // M-J5a 清理：旧默认指向已拆除的 nl-engine-material-graph worktree（必然找不到）。
    // 改指主树；各 worktree 的 thirdparty/slang/dist 是共享 junction，路径始终有效。
    return "E:\\UE_STUDY_2026\\nl-engine\\thirdparty\\slang\\dist\\bin\\slangc.exe";
}

std::string findDxc() {
    const char* d = std::getenv("NL_DXC_DIR");
    if (d && *d) return std::string(d);
    return "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64";
}

// 四路登记序固定（与改造前串行版逐字节对齐）：ps.spv / vs.spv / ps.dxil / vs.dxil。
struct Lane {
    const char* file;
    const char* target;
    const char* entry;
    const char* stage;
};
const Lane kLanes[4] = {
    {"ps.spv",  "spirv", "psMain", "fragment"},
    {"vs.spv",  "spirv", "vsMain", "vertex"},
    {"ps.dxil", "dxil",  "psMain", "fragment"},
    {"vs.dxil", "dxil",  "vsMain", "vertex"},
};

struct LaneOutcome {
    bool ok = false;
    std::string text;   // 汇入 report 的片段（旧口径：target/entry + " OK  " / " FAIL "）
    std::string log;    // verbose 回显（汇总 job 按登记序打印）
};

LaneOutcome runLane(const CompileOptions& opt,
                    const std::string& slangc, const std::string& dxc,
                    const std::string& inSlang, const Lane& lane) {
    const std::string out = opt.stem + lane.file;
    std::string cmd = q(slangc) + " " + q(inSlang) + " -target " + lane.target
                    + " -entry " + lane.entry + " -stage " + lane.stage;
    if (std::string(lane.target) == "spirv")
        cmd += " -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0";
    else
        cmd += " -dxc-path " + q(dxc);
    cmd += " -o " + q(out);

    LaneOutcome r;
    if (opt.verbose) {
        r.log += std::string("  > slangc ") + lane.target + "/" + lane.entry
               + "/" + lane.stage + "\n";
    }
    const int rc = std::system(cmd.c_str());   // 并发的是 4 个独立进程，各自写各自的输出文件
    r.ok = (rc == 0);
    if (opt.verbose && rc != 0) {
        r.log += "    slangc failed (rc=" + std::to_string(rc) + ")\n";
    }
    r.text = std::string(lane.target) + "/" + lane.entry
           + (rc == 0 ? " OK  " : " FAIL ");
    return r;
}

void cleanup(const CompileOptions& opt) {
    std::remove(opt.tmpSlang.c_str());
    if (opt.keepOutputs) return;
    for (const Lane& lane : kLanes) std::remove((opt.stem + lane.file).c_str());
}

}  // namespace

bool recompileGraph(const MaterialGraph& g, const CompileOptions& opt,
                    RecompileReport* rep, std::string* report, std::string* error) {
    if (report) report->clear();
    if (error) error->clear();

    std::string err;
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        if (report) *report = msg;
        return false;
    };
    if (!g.validate(&err)) return fail(std::string("validate FAIL: ") + err);
    {
        std::ofstream os(opt.tmpSlang, std::ios::binary);
        if (!emitSlang(g, os, &err)) return fail(std::string("emit FAIL: ") + err);
    }

    const std::string slangc = opt.slangc.empty() ? findSlangc() : opt.slangc;
    const std::string dxc    = opt.dxcDir.empty() ? findDxc() : opt.dxcDir;

    // 每路只写自己的槽位：DAG 的 work-stealing 不承诺完成顺序，归并一律按登记序。
    std::vector<LaneOutcome> outcomes(4);
    const std::size_t kFanIn = 4;

    if (opt.threads == 1) {
        // 串行参考路径：与改造前逐字节同构，供 MD5 / 文本对拍。
        for (std::size_t i = 0; i < 4; ++i)
            outcomes[i] = runLane(opt, slangc, dxc, opt.tmpSlang, kLanes[i]);
    } else {
        nl::core::jobgraph::Graph graph;
        for (std::size_t i = 0; i < 4; ++i) {
            graph.add(std::string(kLanes[i].target) + "/" + kLanes[i].entry,
                      [&, i] { outcomes[i] = runLane(opt, slangc, dxc, opt.tmpSlang, kLanes[i]); });
        }
        // 汇总 job：4 个编译 job 全部完成后才执行（显式 fan-in 边）。
        graph.add("fan-in/merge", [&] {
            std::string merged;
            for (std::size_t i = 0; i < 4; ++i) {
                if (opt.verbose && !outcomes[i].log.empty())
                    std::fputs(outcomes[i].log.c_str(), stdout);
                merged += outcomes[i].text;
            }
            if (report) *report = merged;
        });
        for (std::size_t i = 0; i < 4; ++i) {
            if (!graph.precede(i, kFanIn)) {
                cleanup(opt);
                return fail("jobgraph FAIL: fan-in edge rejected");
            }
        }
        std::string graphErr;
        if (!graph.run(opt.threads, &graphErr)) {
            cleanup(opt);
            return fail(std::string("jobgraph FAIL: ") + graphErr);
        }
    }

    if (opt.threads == 1) {
        std::string merged;
        for (std::size_t i = 0; i < 4; ++i) {
            if (opt.verbose && !outcomes[i].log.empty())
                std::fputs(outcomes[i].log.c_str(), stdout);
            merged += outcomes[i].text;
        }
        if (report) *report = merged;
    }

    RecompileReport r;
    r.psSpirv = outcomes[0].ok;
    r.vsSpirv = outcomes[1].ok;
    r.psDxil  = outcomes[2].ok;
    r.vsDxil  = outcomes[3].ok;
    if (rep) *rep = r;

    cleanup(opt);
    return r.ok();
}

bool compileCheck(const MaterialGraph& g, std::string* report) {
    CompileOptions opt;   // editor 口径：_mg_ed_* 文件名、自动定位工具链、DAG 并行
    return recompileGraph(g, opt, nullptr, report, nullptr);
}

}  // namespace nl::material_graph
