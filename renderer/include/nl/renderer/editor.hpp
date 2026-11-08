#pragma once

// T1 支线：交互式可视化节点编辑器（imgui-node-editor v0.9.4 + vendored ImGui）。
// 图形 API / ImGui / node-editor 符号只在 renderer/——公开接口与 HudFrame 一样
// 只暴露指针与 std 类型，viewer/core 不接触任何 ImGui 类型。
// 编辑器窗口由 hud.cpp 在 NewFrame..Render 区间内调用 drawWindow()。

#include <memory>
#include <string>

namespace nl::renderer {

class NodeEditor {
public:
    // graphPath：material_graph v1 文本文件（material-graph-tools build 产出）。
    // 总是返回对象；加载失败时 valid()==false，原因在 lastError()。
    static std::unique_ptr<NodeEditor> create(const char* graphPath,
                                              std::string* err = nullptr);
    ~NodeEditor();
    NodeEditor(const NodeEditor&) = delete;
    NodeEditor& operator=(const NodeEditor&) = delete;

    bool valid() const;
    const std::string& lastError() const;
    const std::string& graphPath() const;

    // hud.cpp 每帧调用：画编辑器窗口（画布 + 工具栏 + 状态条）。
    // 内部维护编辑器上下文；Save 把节点坐标与图写回 graphPath；
    // Compile 走 core::material_graph::compileCheck（emit + slangc 四路）。
    void drawWindow();

private:
    NodeEditor();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nl::renderer
