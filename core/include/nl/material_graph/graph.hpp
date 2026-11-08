#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

namespace nl::material_graph {

// 材质图端口 / 节点承载的语义类型。B 路「自建轻量材质图」标签，
// 仅作编译期 / 生成期分类，不引入任何图形后端依赖。
enum class ValueType : uint8_t {
    Invalid = 0,
    Float,
    Float2,
    Float3,
    Float4,
    Color3,
    Bool,
    Texture2D,
    Material,
};

const char* valueTypeName(ValueType t);
bool parseValueType(const std::string& s, ValueType& out);

struct Pin {
    int         id = -1;
    std::string name;
    ValueType   type = ValueType::Invalid;
    bool        isOutput = false;
};

struct Node {
    int id = -1;
    std::string type;  // "ConstantFloat" / "Multiply" / "Output" ...
    float x = 0.0f, y = 0.0f;  // 编辑器布局坐标，无渲染语义
    std::vector<Pin> pins;
    std::unordered_map<std::string, std::string> props;  // 节点属性（如 ConstantFloat.value）
};

struct Edge {
    int id = -1;
    int fromNode = -1, fromPin = -1;
    int toNode = -1,   toPin = -1;
};

// 材质图：节点 + 连线的纯数据模型 + 稳定序列化。
// A 类轨道，零图形 API、零第三方依赖。
class MaterialGraph {
public:
    int addNode(std::string type, float x = 0.0f, float y = 0.0f);
    int addPin(int node, std::string name, ValueType type, bool isOutput);
    int connect(int fromNode, int fromPin, int toNode, int toPin);  // 成功返回 edge id，非法/冲突返回 -1
    bool setProp(int node, std::string key, std::string value);
    bool removeNode(int node);
    bool removeEdge(int edge);

    const Node* findNode(int id) const;
    const Pin*  findPin(int node, int pin) const;

    // 合法性：引用的节点/端口存在；每个输入端口至多一条入边；连线两端类型一致。
    bool validate(std::string* err = nullptr) const;

    // 稳定、无外部依赖的文本格式（material_graph v1）。
    bool serialize(std::ostream& os) const;
    static bool deserialize(std::istream& is, MaterialGraph& out, std::string* err = nullptr);

    const std::vector<Node>& nodes() const { return nodes_; }
    const std::vector<Edge>& edges() const { return edges_; }

    int nodeCount() const { return static_cast<int>(nodes_.size()); }
    int edgeCount() const { return static_cast<int>(edges_.size()); }

private:
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
    int nextNodeId_ = 1;
    int nextPinId_  = 1;
    int nextEdgeId_ = 1;
};

}  // namespace nl::material_graph
