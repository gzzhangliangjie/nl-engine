#pragma once
// M-J5a：显式前置依赖 DAG 的薄封装（底层 = vendored Taskflow 3.11.0，MIT / header-only / C++17）。
//
// 为什么有这一层（2026-11-09 用户拍板）：关联系统优先用成熟库，手写预算留给 nanite / lumen；
// 但业务代码不直接暴露 tf:: 类型，换库时只改 core/src/jobgraph.cpp。
// 依据：UE TGraphTask::CreateTask(Prerequisites) 的显式前置依赖 / Carrot Async::Counter 的依赖计数。
//
// 与老 jobs::ThreadPool 的关系（并存，不是替代）：
//   - ThreadPool：数据并行 parallelFor + 同步 fork-join scope，无依赖图 —— M-J0~J3 的消费者一行不动。
//   - Graph：任务之间有「A 必须先于 B」时用，底层 work-stealing，依赖没齐的任务不入调度。
//
// 确定性纪律（项目硬判据）：work-stealing 不承诺无依赖任务之间的执行顺序，
// 因此任务只写自己的槽位，结果归并只按 add() 的登记序（id 升序），不按完成序。
// 静态图成环会挂死，Taskflow 不检测 —— 本封装用 Kahn 拓扑排序拒环。

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace nl::core {
namespace jobgraph {

class Graph {
public:
    using Body = std::function<void()>;

    // 登记一个任务，返回登记序 id（0,1,2...）。结果归并只按此序。
    // 空 body 视为非法（返回 size() 并置内部错误？不抛）——见 run() 的 error。
    std::size_t add(std::string name, Body body);

    // from 必须先于 to。id 越界 / 自环 / 重复边返回 false（图不变，不抛）。
    bool precede(std::size_t from, std::size_t to);

    // 执行整图并等待完成。threadCount==0 取 hardware_concurrency。
    // 返回 false = 非法图（空 body / 成环 / 边越界）或任务抛异常，error 写 ASCII 原因。
    bool run(unsigned threadCount, std::string* error) const;

    // Kahn 拓扑序（同入度层按登记序出队，确定性）；成环返回空 vector 并置 error。
    std::vector<std::size_t> topoOrder(std::string* error) const;

    std::size_t size() const { return nodes_.size(); }
    std::size_t edgeCount() const { return edges_.size(); }
    const std::string& name(std::size_t id) const;

    // DOT 文本（按登记序输出节点与边，逐字节确定），供证据图 / 调试。
    std::string dumpDot() const;

private:
    struct Node {
        std::string name;
        Body body;
        std::vector<std::size_t> successors;
        std::size_t indegree = 0;
    };
    struct Edge { std::size_t from, to; };
    std::vector<Node> nodes_;
    std::vector<Edge> edges_;
};

} // namespace jobgraph
} // namespace nl::core
