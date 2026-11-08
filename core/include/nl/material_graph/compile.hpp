#pragma once

#include <string>

namespace nl::material_graph {

class MaterialGraph;

// T1：图编译验收（emit -> slangc 双后端 x vs/ps 四路）。纯进程调用，零图形 API——
// 图形符号只在 renderer/ 的纪律不受影响。editor 与 CLI 工具共用同一套判据口径。
// slangc / dxc 定位：env NL_SLANG_BIN / NL_DXC_DIR 优先，退回本机默认路径。
// report 返回逐路结果文本（ASCII）。任意一路 exit != 0 即返回 false。
bool compileCheck(const MaterialGraph& g, std::string* report = nullptr);

}  // namespace nl::material_graph
