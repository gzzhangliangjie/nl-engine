// B3/T1-M3：MaterialGraph -> .slang 代码生成（确定性、零依赖）。
// 从 Output 节点向上游 DAG 递归求值，生成像素阶段表达式；
// 顶点阶段复用 model.slang 的 passthrough 签名（position/normal/uv）。
// uniform 曝光：ConstantFloat 带 uniform=1 属性时进 cbuffer MaterialParams
// (register(b1))，表达式引用 uniform 名（运行时绑定接 PBR pass 属 B5）。

#include "nl/material_graph/emit.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace nl::material_graph {

namespace {

// 把属性字符串规整成 Slang 浮点字面量（保证带小数点与 f 后缀）。
std::string toFloatLiteral(const std::string& s) {
    // 已经是数字？补 .0 与 f。
    bool isNum = !s.empty();
    bool hasDot = false;
    for (char c : s) {
        if (c == '.') hasDot = true;
        else if (!((c >= '0' && c <= '9') || c == '-' || c == '+' ||
                   c == 'e' || c == 'E')) { isNum = false; break; }
    }
    if (!isNum) return "0.0f";   // 非数字（如空属性）退化为 0
    std::string out = s;
    if (!hasDot) out += ".0";
    if (out.back() != 'f' && out.back() != 'F') out += "f";
    return out;
}

// uniform 名 -> 合法标识符（非法字符替换 '_'，数字开头补 'u' 前缀）。
std::string sanitizeIdent(const std::string& s) {
    std::string out;
    for (char c : s)
        out += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') ? c : '_';
    if (out.empty()) out = "uParam";
    if (out[0] >= '0' && out[0] <= '9') out = "u" + out;
    return out;
}

// ConstantFloat 的 uniform 名（缺省按节点 id 派生，确定性且全图唯一）。
std::string uniformName(const Node* n) {
    auto it = n->props.find("uname");
    if (it != n->props.end() && !it->second.empty())
        return sanitizeIdent(it->second);
    return "uParam" + std::to_string(n->id);
}

bool isUniform(const Node* n) {
    auto it = n->props.find("uniform");
    return it != n->props.end() && (it->second == "1" || it->second == "true");
}

// 递归生成某输入引脚的表达式。memo 记忆化：一个上游输出被多条边引用时
// 复用同一表达式（此前用 int visited 集合，二次引用会把内部 key 泄漏进
// 生成代码——B3 潜伏 bug，封顶版示例图一节点双引用后暴露）。
std::string emitPinExpr(const MaterialGraph& g, int nodeId, int pinId,
                        std::map<std::string, std::string>& memo) {
    const Node* n = g.findNode(nodeId);
    const Pin* p = g.findPin(nodeId, pinId);
    if (!n || !p) return "0.0f";
    if (p->isOutput) {
        // 输出引脚：按节点类型生成该节点的值
        const std::string key = std::to_string(nodeId) + ":" + std::to_string(pinId);
        auto hit = memo.find(key);
        if (hit != memo.end()) return hit->second;
        std::string expr;
        // 输入引脚按声明序收集 + 上游表达式取值（各节点类型共用）。
        std::vector<int> inPins;
        for (const auto& pin : n->pins)
            if (!pin.isOutput) inPins.push_back(pin.id);
        auto up = [&](int inPin) -> std::string {
            for (const auto& e : g.edges())
                if (e.toNode == nodeId && e.toPin == inPin)
                    return emitPinExpr(g, e.fromNode, e.fromPin, memo);
            return "0.0f";
        };
        auto A = [&]() { return inPins.size() > 0 ? up(inPins[0]) : "0.0f"; };
        auto B = [&]() { return inPins.size() > 1 ? up(inPins[1]) : "0.0f"; };
        auto C = [&]() { return inPins.size() > 2 ? up(inPins[2]) : "0.0f"; };

        const std::string& t = n->type;
        if (t == "ConstantFloat") {
            if (isUniform(n))
                expr = uniformName(n);   // uniform 曝光：引用 MaterialParams 成员
            else {
                auto it = n->props.find("value");
                expr = toFloatLiteral(it != n->props.end() ? it->second : "");
            }
        } else if (t == "Multiply")  expr = "(" + A() + " * " + B() + ")";
        else if (t == "Add")         expr = "(" + A() + " + " + B() + ")";
        else if (t == "Subtract")    expr = "(" + A() + " - " + B() + ")";
        else if (t == "Divide")      expr = "(" + A() + " / " + B() + ")";
        else if (t == "Power")       expr = "pow(" + A() + ", " + B() + ")";
        else if (t == "Lerp")        expr = "lerp(" + A() + ", " + B() + ", " + C() + ")";
        else if (t == "OneMinus")    expr = "(1.0f - " + A() + ")";
        else if (t == "Abs")         expr = "abs(" + A() + ")";
        else if (t == "Clamp") {
            // min/max 走属性（不占引脚），缺省 0/1。
            auto mn = n->props.find("min");
            auto mx = n->props.find("max");
            expr = "clamp(" + A() + ", "
                 + toFloatLiteral(mn != n->props.end() ? mn->second : "0") + ", "
                 + toFloatLiteral(mx != n->props.end() ? mx->second : "1") + ")";
        } else {
            expr = "0.0f";   // 未知节点类型：占位（emitSlang 会在校验阶段报错）
        }
        memo[key] = expr;
        return expr;
    }
    // 输入引脚：沿入边取上游输出表达式
    for (const auto& e : g.edges()) {
        if (e.toNode == nodeId && e.toPin == pinId)
            return emitPinExpr(g, e.fromNode, e.fromPin, memo);
    }
    return "0.0f";   // 未连接输入：退化为 0
}

// 收集全图 uniform 名（排序去重 -> cbuffer 成员顺序确定）。
std::vector<std::string> collectUniforms(const MaterialGraph& g) {
    std::set<std::string> names;
    for (const auto& n : g.nodes())
        if (n.type == "ConstantFloat" && isUniform(&n))
            names.insert(uniformName(&n));
    return std::vector<std::string>(names.begin(), names.end());
}

}  // namespace

bool emitSlang(const MaterialGraph& g, std::ostream& os, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };

    // 必须有且一个 Output 节点
    const Node* outNode = nullptr;
    for (const auto& n : g.nodes()) {
        if (n.type == "Output") {
            if (outNode) return fail("multiple Output nodes");
            outNode = &n;
        }
    }
    if (!outNode) return fail("missing Output node");

    // 找 Output 的 baseColor 输入引脚（类型 float 的输入）
    int basePin = -1;
    for (const auto& p : outNode->pins) {
        if (!p.isOutput && p.type == ValueType::Float) { basePin = p.id; break; }
    }
    if (basePin < 0) return fail("Output has no float input pin");

    const std::vector<std::string> uniforms = collectUniforms(g);

    // ---- 头部 + 固定管线材质常量（cbuffer SceneConstants）----
    os << "// Generated by material-graph-tools (B3): MaterialGraph -> Slang\n";
    os << "// Resource registers shared by both targets (model.slang convention):\n";
    os << "//   b0 = SceneConstants, b1 = MaterialParams (graph uniforms),\n";
    os << "//   t0 = base color texture, s0 = sampler\n\n";
    os << "cbuffer SceneConstants : register(b0)\n{\n";
    os << "    float4x4 g_mvp;\n";
    os << "    float4x4 g_model;\n";
    os << "    float4   g_lightDir;\n";
    os << "    float4   g_options;\n};\n\n";
    if (!uniforms.empty()) {
        os << "// Graph uniforms (ConstantFloat with uniform=1); runtime binding lands at B5.\n";
        os << "cbuffer MaterialParams : register(b1)\n{\n";
        for (const auto& u : uniforms)
            os << "    float " << u << ";\n";
        os << "};\n\n";
    }

    // ---- 顶点阶段：passthrough（与 model.slang vsMain 同构）----
    os << "struct VSInput\n{\n";
    os << "    float3 position : POSITION;\n";
    os << "    float3 normal   : NORMAL;\n";
    os << "    float2 uv       : TEXCOORD;\n};\n\n";
    os << "struct VSOutput\n{\n";
    os << "    float4 position : SV_Position;\n";
    os << "    float3 normal   : NORMAL;\n";
    os << "    float2 uv       : TEXCOORD;\n};\n\n";
    os << "VSOutput vsMain(VSInput input)\n{\n";
    os << "    VSOutput output;\n";
    os << "    output.position = mul(g_mvp, float4(input.position, 1.0f));\n";
    os << "    output.normal   = mul((float3x3)g_model, input.normal);\n";
    os << "    output.uv       = input.uv;\n";
    os << "    return output;\n}\n\n";

    // ---- 像素阶段：从 Output 上游求值 ----
    std::map<std::string, std::string> memo;
    std::string baseExpr = emitPinExpr(g, outNode->id, basePin, memo);
    os << "struct PSInput\n{\n";
    os << "    float4 position : SV_Position;\n";
    os << "    float3 normal   : NORMAL;\n";
    os << "    float2 uv       : TEXCOORD;\n};\n\n";
    os << "float4 psMain(PSInput input) : SV_Target\n{\n";
    os << "    float baseColor = " << baseExpr << ";\n";
    os << "    return float4(baseColor, baseColor, baseColor, 1.0f);\n";
    os << "}\n";

    if (err) *err = "";
    return static_cast<bool>(os);
}

}  // namespace nl::material_graph
