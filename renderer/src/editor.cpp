#include "nl/renderer/editor.hpp"

#include "nl/material_graph/compile.hpp"
#include "nl/material_graph/graph.hpp"

#include <imgui.h>
#include <imgui_node_editor.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace ed = ax::NodeEditor;

namespace nl::renderer {

namespace {

// 节点配色（ASCII，与 CLI render 证据图同一套语义）。
ImVec4 typeColor(const std::string& type) {
    if (type == "ConstantFloat") return ImVec4(0.30f, 0.50f, 0.70f, 1.0f);
    if (type == "Multiply")      return ImVec4(0.30f, 0.60f, 0.40f, 1.0f);
    if (type == "Add")           return ImVec4(0.75f, 0.55f, 0.25f, 1.0f);
    if (type == "Subtract")      return ImVec4(0.40f, 0.45f, 0.70f, 1.0f);
    if (type == "Divide")        return ImVec4(0.55f, 0.40f, 0.30f, 1.0f);
    if (type == "Power")         return ImVec4(0.65f, 0.35f, 0.55f, 1.0f);
    if (type == "Lerp")          return ImVec4(0.25f, 0.55f, 0.55f, 1.0f);
    if (type == "OneMinus")      return ImVec4(0.50f, 0.50f, 0.30f, 1.0f);
    if (type == "Abs")           return ImVec4(0.45f, 0.45f, 0.60f, 1.0f);
    if (type == "Clamp")         return ImVec4(0.30f, 0.55f, 0.65f, 1.0f);
    if (type == "Output")        return ImVec4(0.70f, 0.35f, 0.35f, 1.0f);
    return ImVec4(0.35f, 0.35f, 0.45f, 1.0f);
}

// 引脚反查（引脚 id 全图唯一）：引脚存在性/方向/类型校验与 connect 约束同源。
struct PinRef {
    int node = -1;
    bool valid = false;
    bool isOutput = false;
    nl::material_graph::ValueType type = nl::material_graph::ValueType::Invalid;
    std::string name;
};
PinRef findPin(const nl::material_graph::MaterialGraph& g, int pinId) {
    for (const auto& n : g.nodes())
        for (const auto& p : n.pins)
            if (p.id == pinId)
                return PinRef{n.id, true, p.isOutput, p.type, p.name};
    return PinRef{};
}

// 全部 10 种节点类型（封顶版节点库）：二值运算 a/b、三输入 Lerp a/b/t、
// 一元 x、Clamp 的 min/max 走属性。返回新节点 id（供建节点时定位画布坐标）。
int addTypedNode(nl::material_graph::MaterialGraph& g, const std::string& type) {
    using namespace nl::material_graph;
    int n = g.addNode(type);
    if (type == "ConstantFloat") {
        g.setProp(n, "value", "0.5");
        g.addPin(n, "out", ValueType::Float, true);
    } else if (type == "Lerp") {
        g.addPin(n, "a", ValueType::Float, false);
        g.addPin(n, "b", ValueType::Float, false);
        g.addPin(n, "t", ValueType::Float, false);
        g.addPin(n, "out", ValueType::Float, true);
    } else if (type == "OneMinus" || type == "Abs" || type == "Clamp") {
        g.addPin(n, "x", ValueType::Float, false);
        g.addPin(n, "out", ValueType::Float, true);
        if (type == "Clamp") { g.setProp(n, "min", "0"); g.setProp(n, "max", "1"); }
    } else if (type == "Output") {
        g.addPin(n, "baseColor", ValueType::Float, false);
    } else {  // Multiply / Add / Subtract / Divide / Power：二值 float
        g.addPin(n, "a", ValueType::Float, false);
        g.addPin(n, "b", ValueType::Float, false);
        g.addPin(n, "out", ValueType::Float, true);
    }
    return n;
}

}  // namespace

struct NodeEditor::Impl {
    std::string path;
    std::string lastError;
    nl::material_graph::MaterialGraph graph;
    ed::EditorContext* ctx = nullptr;
    std::string status = "ready";

    ~Impl() {
        if (ctx) ed::DestroyEditor(ctx);
    }

    void drawWindow() {
        ImGui::SetNextWindowSize(ImVec2(780.0f, 660.0f), ImGuiCond_FirstUseEver);
        ImGui::Begin("Material Graph Editor (T1)");
        ed::SetCurrentEditor(ctx);   // 整个窗口体都在编辑器作用域内（Save 要回收坐标）

        // ---- 工具栏：加节点 / 保存 / 编译（T1 拆解 3/4/5 步；Ctrl+S 亦可存）----
        if (ImGui::Button("+ ConstantFloat")) { addTypedNode(graph, "ConstantFloat"); status = "node added"; }
        ImGui::SameLine();
        if (ImGui::Button("+ Multiply")) { addTypedNode(graph, "Multiply"); status = "node added"; }
        ImGui::SameLine();
        if (ImGui::Button("+ Add")) { addTypedNode(graph, "Add"); status = "node added"; }
        ImGui::SameLine();
        if (ImGui::Button("+ Output")) { addTypedNode(graph, "Output"); status = "node added"; }
        ImGui::SameLine();
        if (ImGui::Button("Save")) save();
        ImGui::SameLine();
        if (ImGui::Button("Compile")) {
            std::string rep;
            bool ok = nl::material_graph::compileCheck(graph, &rep);
            status = ok ? ("compile OK: " + rep) : ("compile FAIL: " + rep);
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
            && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
            save();
        }

        // ---- 节点画布（host child 预留底部 separator + status + 提示三行；库对
        //      size<=0 的处理是"占满剩余"，负值预留不生效，必须外层包 child）----
        const float bottomBarH = ImGui::GetTextLineHeightWithSpacing() * 3.0f + 14.0f;
        ImGui::BeginChild("canvas_host", ImVec2(0.0f, -bottomBarH));
        ed::Begin("canvas");
        for (const auto& n : graph.nodes()) {
            ed::BeginNode(static_cast<std::uintptr_t>(n.id));
            ImGui::PushStyleColor(ImGuiCol_Header, typeColor(n.type));
            ImGui::TextUnformatted(n.type.c_str());
            ImGui::PopStyleColor();
            for (const auto& p : n.pins) {          // 输入列在左
                if (p.isOutput) continue;
                ed::BeginPin(static_cast<std::uintptr_t>(p.id), ed::PinKind::Input);
                ed::PinPivotAlignment(ImVec2(0.0f, 0.5f));
                ImGui::Text(">%s %s", p.name.c_str(),
                            nl::material_graph::valueTypeName(p.type));
                ed::EndPin();
            }
            if (n.type == "ConstantFloat") {        // 属性即时编辑（写回 props）
                // uniform 开关：勾上后进 cbuffer MaterialParams(b1)（B5 运行时绑定）
                bool asUniform = false;
                auto itU = n.props.find("uniform");
                asUniform = (itU != n.props.end()
                             && (itU->second == "1" || itU->second == "true"));
                if (ImGui::Checkbox("uniform", &asUniform)) {
                    if (asUniform) {
                        graph.setProp(n.id, "uniform", "1");
                        if (n.props.find("uname") == n.props.end())
                            graph.setProp(n.id, "uname", "uParam" + std::to_string(n.id));
                    } else {
                        graph.setProp(n.id, "uniform", "0");
                    }
                    status = "prop changed";
                }
                if (asUniform) {
                    char buf[64] = {};
                    auto un = n.props.find("uname");
                    if (un != n.props.end())
                        std::snprintf(buf, sizeof(buf), "%s", un->second.c_str());
                    ImGui::SetNextItemWidth(110.0f);
                    if (ImGui::InputText("uname", buf, sizeof(buf))) {
                        graph.setProp(n.id, "uname", buf);
                        status = "prop changed";
                    }
                } else {
                    float v = 0.0f;
                    auto it = n.props.find("value");
                    if (it != n.props.end()) v = std::strtof(it->second.c_str(), nullptr);
                    ImGui::SetNextItemWidth(110.0f);    // 不占满整行，out 引脚保持在同一列高度
                    if (ImGui::InputFloat("value", &v, 0.0f, 0.0f, "%.3f")) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%.3f", v);
                        graph.setProp(n.id, "value", buf);
                        status = "prop changed";
                    }
                }
            }
            for (const auto& p : n.pins) {          // 输出列在右
                if (!p.isOutput) continue;
                ed::BeginPin(static_cast<std::uintptr_t>(p.id), ed::PinKind::Output);
                ed::PinPivotAlignment(ImVec2(1.0f, 0.5f));
                ImGui::Text("%s %s>", p.name.c_str(),
                            nl::material_graph::valueTypeName(p.type));
                ed::EndPin();
            }
            ed::EndNode();
        }
        for (const auto& e : graph.edges()) {
            ed::Link(static_cast<std::uintptr_t>(e.id),
                     static_cast<std::uintptr_t>(e.fromPin),
                     static_cast<std::uintptr_t>(e.toPin));
        }
        if (graph.nodes().empty())
            ImGui::TextUnformatted("(empty graph) - use toolbar to add nodes");

        // ---- 右键菜单：空白处 = 加节点（落在鼠标处），节点上 = 删除 ----
        const ed::NodeId hovered = ed::GetHoveredNode();
        if (ImGui::IsMouseReleased(1))
            ImGui::OpenPopup(hovered ? "node_menu" : "canvas_menu");
        if (ImGui::BeginPopup("canvas_menu")) {
            const ImVec2 at = ed::ScreenToCanvas(ImGui::GetMousePosOnOpeningCurrentPopup());
            const char* types[] = {"ConstantFloat", "Multiply", "Add", "Subtract",
                                   "Divide", "Power", "Lerp", "OneMinus", "Abs",
                                   "Clamp", "Output"};
            for (const char* t : types)
                if (ImGui::MenuItem(t)) {
                    const int id = addTypedNode(graph, t);
                    ed::SetNodePosition(static_cast<std::uintptr_t>(id), at);
                    status = "node added";
                }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("node_menu")) {
            if (ImGui::MenuItem("Delete node")) {
                graph.removeNode(static_cast<int>(hovered.Get()));
                status = "node deleted (edges cascaded)";
            }
            ImGui::EndPopup();
        }
        ed::End();
        ImGui::EndChild();

        // ---- 连线创建：拖拽引脚 -> 校验（存在/一进一出/类型一致）-> connect ----
        if (ed::BeginCreate()) {
            ed::PinId startPin, endPin;
            if (ed::QueryNewLink(&startPin, &endPin)) {
                const PinRef a = findPin(graph, static_cast<int>(startPin.Get()));
                const PinRef b = findPin(graph, static_cast<int>(endPin.Get()));
                const bool ok = a.valid && b.valid && (a.isOutput != b.isOutput)
                                && (a.type == b.type);
                if (ok) {
                    if (ed::AcceptNewItem()) {
                        // 方向归一（from = 输出端）后落 graph.connect。
                        const PinRef& out = a.isOutput ? a : b;
                        const PinRef& in  = a.isOutput ? b : a;
                        const int outPin  = a.isOutput ? static_cast<int>(startPin.Get())
                                                       : static_cast<int>(endPin.Get());
                        const int inPin   = a.isOutput ? static_cast<int>(endPin.Get())
                                                       : static_cast<int>(startPin.Get());
                        const int id = graph.connect(out.node, outPin, in.node, inPin);
                        status = (id >= 0) ? "link created"
                                           : "connect rejected (duplicate input)";
                    }
                } else {
                    ed::RejectNewItem();
                    if (a.valid && b.valid && a.isOutput == b.isOutput)
                        status = "link rejected: same direction";
                    else if (a.valid && b.valid && a.type != b.type)
                        status = "link rejected: type mismatch";
                }
            }
        }
        ed::EndCreate();

        // ---- 删除：选中连线 Del 删除；选中节点 Del 级联删节点及其连线 ----
        if (ed::BeginDelete()) {
            ed::LinkId deleted;
            while (ed::QueryDeletedLink(&deleted)) {
                if (ed::AcceptDeletedItem()) {
                    graph.removeEdge(static_cast<int>(deleted.Get()));
                    status = "link deleted";
                }
            }
            ed::NodeId deletedNode;
            while (ed::QueryDeletedNode(&deletedNode)) {
                if (ed::AcceptDeletedItem()) {
                    graph.removeNode(static_cast<int>(deletedNode.Get()));
                    status = "node deleted (edges cascaded)";
                }
            }
        }
        ed::EndDelete();

        ImGui::Separator();
        ImGui::TextWrapped("status: %s", status.c_str());
        ImGui::TextWrapped("drag pin->pin = link | drag title = move | wheel = zoom | Del on link/node = delete | right-click = add/delete node | Ctrl+S = save");

        ed::SetCurrentEditor(nullptr);
        ImGui::End();
    }

    void save() {
        // 编辑器上下文里的拖拽坐标 -> graph（Node.x/y 是序列化的布局真值）。
        ed::SetCurrentEditor(ctx);
        for (const auto& n : graph.nodes()) {
            const ImVec2 p = ed::GetNodePosition(static_cast<std::uintptr_t>(n.id));
            graph.setNodePos(n.id, p.x, p.y);
        }
        ed::SetCurrentEditor(nullptr);
        std::ofstream os(path, std::ios::binary);
        if (!graph.serialize(os)) { status = "save FAIL"; return; }
        status = "saved";
    }
};

NodeEditor::NodeEditor() = default;
NodeEditor::~NodeEditor() = default;

std::unique_ptr<NodeEditor> NodeEditor::create(const char* graphPath, std::string* err) {
    std::unique_ptr<NodeEditor> self(new NodeEditor());
    self->impl_ = std::make_unique<Impl>();
    Impl& im = *self->impl_;
    im.path = graphPath;
    std::ifstream is(im.path, std::ios::binary);
    if (!is) {
        im.lastError = "cannot open " + im.path;
        if (err) *err = im.lastError;
        return self;
    }
    if (!nl::material_graph::MaterialGraph::deserialize(is, im.graph, &im.lastError)) {
        if (err) *err = im.lastError;
        return self;
    }
    std::string verr;
    if (!im.graph.validate(&verr)) {
        im.lastError = "graph invalid: " + verr;
        if (err) *err = im.lastError;
        return self;
    }
    im.ctx = ed::CreateEditor(nullptr);
    // 序列化保存的布局坐标（Node.x/y）作为画布初始位置——.mg 的布局真值一以贯之。
    ed::SetCurrentEditor(im.ctx);
    for (const auto& n : im.graph.nodes())
        ed::SetNodePosition(static_cast<std::uintptr_t>(n.id), ImVec2(n.x, n.y));
    ed::SetCurrentEditor(nullptr);
    im.status = "loaded " + std::to_string(im.graph.nodeCount()) + " nodes, "
              + std::to_string(im.graph.edgeCount()) + " edges";
    return self;
}

bool NodeEditor::valid() const { return impl_->ctx != nullptr; }
const std::string& NodeEditor::lastError() const { return impl_->lastError; }
const std::string& NodeEditor::graphPath() const { return impl_->path; }
void NodeEditor::drawWindow() { impl_->drawWindow(); }

}  // namespace nl::renderer
