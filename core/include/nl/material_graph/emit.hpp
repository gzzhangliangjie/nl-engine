#pragma once

#include <ostream>
#include <string>

#include "nl/material_graph/graph.hpp"

namespace nl::material_graph {

// B3：把 MaterialGraph 编译成符合本项目绑定约定的 .slang。
// 约定（见 renderer/shaders/model.slang + docs/SHADER-CONVENTIONS.md）：
//   - 命名入口 vsMain / psMain；shader 内只写 register(b0/t0/s0)，SPIR-V 绑定平移交给 slangc 命令行。
//   - cbuffer SceneConstants（g_mvp / g_model / g_lightDir / g_options）= 固定管线材质常量。
// 首版内置节点：ConstantFloat / Multiply / Output(baseColor: float)。
// 纯函数 + 确定性：同一图两次 emit 字节一致。零图形 API、零第三方依赖。
bool emitSlang(const MaterialGraph& g, std::ostream& os, std::string* err = nullptr);

}  // namespace nl::material_graph
