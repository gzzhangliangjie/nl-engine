#pragma once
// 材质图编译验收的**唯一入口**（M-J5a 起）：
//   - editor Compile（renderer/src/editor.cpp）走 compileCheck()
//   - CLI check / watch（material-graph-tools）走 recompileGraph()
// 两者共用同一份 emit + 4 路 slangc 实现，不再各写一份（B4-PLAN 曾声称无重复实现，实际有两份）。
// 纯进程调用，零图形 API —— 图形符号只在 renderer/ 的纪律不受影响。
//
// 4 路编译默认走 nl::core::jobgraph 的 fan-in DAG（4 个编译 job → 1 个汇总 job），
// 底层是 vendored Taskflow 3.11.0。threads==1 走串行参考路径，用于逐字节对拍。
// 结果归并按登记序，不按完成序；任何输出与串行版不一致即 FAIL。

#include <string>

namespace nl::material_graph {

class MaterialGraph;

// 四路各自的成败（顺序固定：psSpirv / vsSpirv / psDxil / vsDxil）。
struct RecompileReport {
    bool psSpirv = false, vsSpirv = false, psDxil = false, vsDxil = false;
    bool ok() const { return psSpirv && vsSpirv && psDxil && vsDxil; }
};

struct CompileOptions {
    // 产物前缀（含分隔符）。editor 用 "_mg_ed_"，CLI 用 "_mg_hot."，保持历史文件名不变。
    std::string stem = "_mg_ed_";
    std::string tmpSlang = "_mg_ed_check.slang";
    // 空 = 按 env NL_SLANG_BIN / NL_DXC_DIR 自动定位，再退回本机默认路径。
    std::string slangc;
    std::string dxcDir;
    // 0 = hardware_concurrency；1 = 纯串行参考（不经 DAG）。
    unsigned threads = 0;
    // 逐路命令回显（按登记序打印，保证并行下输出也确定）。
    bool verbose = false;
    // 保留 4 路产物供 MD5 对拍；默认全部清理。
    bool keepOutputs = false;
};

// emit -> 4 路 slangc -> 汇总清理。返回 false = validate / emit / DAG / 编译任一路失败。
// report 为逐路结果文本（ASCII，按登记序），error 为失败原因；两者均可为 nullptr。
bool recompileGraph(const MaterialGraph& g, const CompileOptions& opt,
                    RecompileReport* rep, std::string* report, std::string* error);

// editor 侧兼容入口（等价于 opt 全默认的 recompileGraph）。
bool compileCheck(const MaterialGraph& g, std::string* report = nullptr);

}  // namespace nl::material_graph
