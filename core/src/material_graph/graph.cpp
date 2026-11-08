// B 路「自建轻量材质图」第 1 步（PARALLEL-TRACKS.md §2.1）：节点图数据结构 + 稳定序列化。
// 纯数据模型，零图形 API、零第三方依赖（A 类轨道）。
// 序列化目标是「无外部依赖、可 diff、跨版本向前兼容」的文本格式 material_graph v1。

#include "nl/material_graph/graph.hpp"

#include <algorithm>
#include <cstdio>
#include <istream>
#include <ostream>
#include <sstream>

namespace nl::material_graph {

// ---- ValueType 文本映射 ----
const char* valueTypeName(ValueType t) {
    switch (t) {
        case ValueType::Float:    return "float";
        case ValueType::Float2:   return "float2";
        case ValueType::Float3:   return "float3";
        case ValueType::Float4:   return "float4";
        case ValueType::Color3:   return "color3";
        case ValueType::Bool:     return "bool";
        case ValueType::Texture2D:return "texture2d";
        case ValueType::Material: return "material";
        case ValueType::Invalid:
        default:                  return "invalid";
    }
}

bool parseValueType(const std::string& s, ValueType& out) {
    // 大小写敏感，与 valueTypeName 严格互逆。
    static const std::pair<const char*, ValueType> table[] = {
        {"float",     ValueType::Float},
        {"float2",    ValueType::Float2},
        {"float3",    ValueType::Float3},
        {"float4",    ValueType::Float4},
        {"color3",    ValueType::Color3},
        {"bool",      ValueType::Bool},
        {"texture2d", ValueType::Texture2D},
        {"material",  ValueType::Material},
        {"invalid",   ValueType::Invalid},
    };
    for (const auto& e : table) {
        if (s == e.first) { out = e.second; return true; }
    }
    return false;
}

namespace {
// 浮点稳定格式：%.6g 足够编辑器坐标精度，且同一值序列化前后一致（确定性）。
std::string fmtFloat(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}
float parseFloat(const std::string& s, float def) {
    try {
        return static_cast<float>(std::stof(s));
    } catch (...) {
        return def;
    }
}
}  // namespace

// ---- 节点 / 引脚 / 连线 构造 ----
int MaterialGraph::addNode(std::string type, float x, float y) {
    Node n;
    n.id = nextNodeId_++;
    n.type = std::move(type);
    n.x = x;
    n.y = y;
    nodes_.push_back(std::move(n));
    return n.id;
}

int MaterialGraph::addPin(int node, std::string name, ValueType type, bool isOutput) {
    Node* n = const_cast<Node*>(findNode(node));
    if (!n) return -1;
    Pin p;
    p.id = nextPinId_++;
    p.name = std::move(name);
    p.type = type;
    p.isOutput = isOutput;
    n->pins.push_back(std::move(p));
    return p.id;
}

int MaterialGraph::connect(int fromNode, int fromPin, int toNode, int toPin) {
    if (!findPin(fromNode, fromPin) || !findPin(toNode, toPin)) return -1;
    const Pin* fp = findPin(fromNode, fromPin);
    const Pin* tp = findPin(toNode, toPin);
    if (!fp->isOutput || tp->isOutput) return -1;        // 必须 输出→输入
    if (fp->type != tp->type) return -1;                 // 类型必须一致
    // 每个输入端口至多一条入边
    for (const auto& e : edges_) {
        if (e.toNode == toNode && e.toPin == toPin) return -1;
    }
    Edge e;
    e.id = nextEdgeId_++;
    e.fromNode = fromNode; e.fromPin = fromPin;
    e.toNode = toNode;   e.toPin = toPin;
    edges_.push_back(std::move(e));
    return e.id;
}

bool MaterialGraph::setProp(int node, std::string key, std::string value) {
    Node* n = const_cast<Node*>(findNode(node));
    if (!n) return false;
    n->props[key] = std::move(value);
    return true;
}

bool MaterialGraph::setNodePos(int node, float x, float y) {
    Node* n = const_cast<Node*>(findNode(node));
    if (!n) return false;
    n->x = x;
    n->y = y;
    return true;
}

bool MaterialGraph::removeNode(int node) {
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
                           [node](const Node& n) { return n.id == node; });
    if (it == nodes_.end()) return false;
    nodes_.erase(it);
    // 级联删除涉及该节点的连线
    edges_.erase(
        std::remove_if(edges_.begin(), edges_.end(),
                      [node](const Edge& e) {
                          return e.fromNode == node || e.toNode == node;
                      }),
        edges_.end());
    return true;
}

bool MaterialGraph::removeEdge(int edge) {
    auto it = std::find_if(edges_.begin(), edges_.end(),
                           [edge](const Edge& e) { return e.id == edge; });
    if (it == edges_.end()) return false;
    edges_.erase(it);
    return true;
}

const Node* MaterialGraph::findNode(int id) const {
    for (const auto& n : nodes_) if (n.id == id) return &n;
    return nullptr;
}

const Pin* MaterialGraph::findPin(int node, int pin) const {
    const Node* n = findNode(node);
    if (!n) return nullptr;
    for (const auto& p : n->pins) if (p.id == pin) return &p;
    return nullptr;
}

// ---- 合法性校验 ----
bool MaterialGraph::validate(std::string* err) const {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    // 连线引用的节点 / 引脚必须存在
    for (const auto& e : edges_) {
        if (!findPin(e.fromNode, e.fromPin)) return fail("edge references missing from-pin");
        if (!findPin(e.toNode, e.toPin))    return fail("edge references missing to-pin");
        const Pin* fp = findPin(e.fromNode, e.fromPin);
        const Pin* tp = findPin(e.toNode, e.toPin);
        if (!fp->isOutput) return fail("edge source is not an output pin");
        if (tp->isOutput)  return fail("edge target is not an input pin");
        if (fp->type != tp->type) return fail("edge type mismatch");
    }
    // 每个输入端口至多一条入边（connect 已挡，这里兜底）
    for (const auto& n : nodes_) {
        for (const auto& p : n.pins) {
            if (p.isOutput) continue;
            int incoming = 0;
            for (const auto& e : edges_)
                if (e.toNode == n.id && e.toPin == p.id) ++incoming;
            if (incoming > 1) return fail("input pin has multiple incoming edges");
        }
    }
    if (err) *err = "";
    return true;
}

// ---- 序列化：material_graph v1 ----
// 行标签：header / node / pin / prop / edge，全部按 nodeId 显式标注，解析顺序无关。
//   node <id> <type> <x> <y>
//   pin  <nodeId> <pinId> <name> <typeName> <isOutput>
//   prop <nodeId> <key> <value...>          # value 为整行余下内容，允许含空格
//   edge <id> <fromNode> <fromPin> <toNode> <toPin>
bool MaterialGraph::serialize(std::ostream& os) const {
    os << "material_graph v1\n";
    os << "# nodes\n";
    for (const auto& n : nodes_) {
        os << "node " << n.id << ' ' << n.type << ' '
           << fmtFloat(n.x) << ' ' << fmtFloat(n.y) << '\n';
    }
    os << "# pins\n";
    for (const auto& n : nodes_) {
        for (const auto& p : n.pins) {
            os << "pin " << n.id << ' ' << p.id << ' ' << p.name << ' '
               << valueTypeName(p.type) << ' ' << (p.isOutput ? 1 : 0) << '\n';
        }
    }
    os << "# props\n";
    for (const auto& n : nodes_) {
        for (const auto& kv : n.props) {
            os << "prop " << n.id << ' ' << kv.first << ' ' << kv.second << '\n';
        }
    }
    os << "# edges\n";
    for (const auto& e : edges_) {
        os << "edge " << e.id << ' ' << e.fromNode << ' ' << e.fromPin << ' '
           << e.toNode << ' ' << e.toPin << '\n';
    }
    return static_cast<bool>(os);
}

bool MaterialGraph::deserialize(std::istream& is, MaterialGraph& out, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    out = MaterialGraph{};
    std::string line;
    bool haveHeader = false;
    while (std::getline(is, line)) {
        // 跳过空行与注释
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string tag;
        if (!(ss >> tag)) continue;
        if (tag == "material_graph") {
            std::string ver;
            if (!(ss >> ver)) return fail("missing version in header");
            if (ver != "v1") return fail("unsupported version: " + ver);
            haveHeader = true;
            continue;
        }
        if (!haveHeader) return fail("missing material_graph header");
        if (tag == "node") {
            int id; std::string type; float x, y;
            if (!(ss >> id >> type >> x >> y)) return fail("malformed node line");
            Node n; n.id = id; n.type = type; n.x = x; n.y = y;
            out.nodes_.push_back(std::move(n));
            out.nextNodeId_ = std::max(out.nextNodeId_, id + 1);
        } else if (tag == "pin") {
            int nodeId, pinId; std::string name, typeName; int isOut;
            if (!(ss >> nodeId >> pinId >> name >> typeName >> isOut))
                return fail("malformed pin line");
            ValueType vt;
            if (!parseValueType(typeName, vt)) return fail("unknown value type: " + typeName);
            Node* n = const_cast<Node*>(out.findNode(nodeId));
            if (!n) return fail("pin references missing node");
            Pin p; p.id = pinId; p.name = name; p.type = vt; p.isOutput = (isOut != 0);
            n->pins.push_back(std::move(p));
            out.nextPinId_ = std::max(out.nextPinId_, pinId + 1);
        } else if (tag == "prop") {
            int nodeId; std::string key;
            if (!(ss >> nodeId >> key)) return fail("malformed prop line");
            // value 为整行余下内容（允许含空格）
            std::string value;
            std::getline(ss, value);
            // getline 会保留前导空格，剥掉第一个空格分隔符留下的那个
            if (!value.empty() && value[0] == ' ') value.erase(0, 1);
            Node* n = const_cast<Node*>(out.findNode(nodeId));
            if (!n) return fail("prop references missing node");
            n->props[key] = value;
        } else if (tag == "edge") {
            int id, fn, fp, tn, tp;
            if (!(ss >> id >> fn >> fp >> tn >> tp)) return fail("malformed edge line");
            Edge e; e.id = id; e.fromNode = fn; e.fromPin = fp; e.toNode = tn; e.toPin = tp;
            out.edges_.push_back(std::move(e));
            out.nextEdgeId_ = std::max(out.nextEdgeId_, id + 1);
        } else {
            return fail("unknown tag: " + tag);
        }
    }
    if (!haveHeader) return fail("empty or headerless input");
    return true;
}

}  // namespace nl::material_graph
