#include "nl/material_graph/compile.hpp"

#include "nl/material_graph/emit.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace nl::material_graph {

namespace {

// 与 material-graph-tools 同源：仅当路径含空格时加引号（system() 引号陷阱）。
std::string q(const std::string& p) {
    if (p.find(' ') != std::string::npos) return "\"" + p + "\"";
    return p;
}

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

bool runOne(const std::string& slangc, const std::string& dxcDir,
            const char* inSlang, const char* out,
            const char* target, const char* entry, const char* stage,
            std::string& report) {
    std::string cmd = q(slangc) + " " + q(inSlang) + " -target " + target
                    + " -entry " + entry + " -stage " + stage;
    if (std::string(target) == "spirv")
        cmd += " -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0";
    else
        cmd += " -dxc-path " + q(dxcDir);
    cmd += " -o " + q(out);
    int rc = std::system(cmd.c_str());
    report += target;
    report += "/";
    report += entry;
    report += (rc == 0 ? " OK  " : " FAIL ");
    return rc == 0;
}

}  // namespace

bool compileCheck(const MaterialGraph& g, std::string* report) {
    std::string rep, err;
    if (!g.validate(&err)) {
        if (report) *report = std::string("validate FAIL: ") + err;
        return false;
    }
    const char* tmp = "_mg_ed_check.slang";
    {
        std::ofstream os(tmp, std::ios::binary);
        if (!emitSlang(g, os, &err)) {
            if (report) *report = std::string("emit FAIL: ") + err;
            return false;
        }
    }
    std::string slangc = findSlangc();
    std::string dxc = findDxc();
    bool ok = true;
    ok &= runOne(slangc, dxc, tmp, "_mg_ed_ps.spv",  "spirv", "psMain", "fragment", rep);
    ok &= runOne(slangc, dxc, tmp, "_mg_ed_vs.spv",  "spirv", "vsMain", "vertex",   rep);
    ok &= runOne(slangc, dxc, tmp, "_mg_ed_ps.dxil", "dxil",  "psMain", "fragment", rep);
    ok &= runOne(slangc, dxc, tmp, "_mg_ed_vs.dxil", "dxil",  "vsMain", "vertex",   rep);
    std::remove(tmp);
    std::remove("_mg_ed_ps.spv");  std::remove("_mg_ed_vs.spv");
    std::remove("_mg_ed_ps.dxil"); std::remove("_mg_ed_vs.dxil");
    if (report) *report = rep;
    return ok;
}

}  // namespace nl::material_graph
