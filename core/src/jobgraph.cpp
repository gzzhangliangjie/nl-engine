// M-J5a：显式依赖 DAG 薄封装实现。tf:: 只在本 TU 出现，业务代码零泄漏。
// 版本 / 许可证 / 风险核查见 thirdparty/taskflow/VENDOR.md。

#include "nl/core/jobgraph.hpp"

#include <taskflow/taskflow.hpp>

#include <algorithm>
#include <exception>
#include <sstream>
#include <thread>
#include <vector>

namespace nl::core {
namespace jobgraph {

namespace {

const std::string kEmptyName;

} // namespace

std::size_t Graph::add(std::string name, Body body) {
    Node n;
    n.name = std::move(name);
    n.body = std::move(body);
    nodes_.push_back(std::move(n));
    return nodes_.size() - 1;
}

bool Graph::precede(std::size_t from, std::size_t to) {
    if (from >= nodes_.size() || to >= nodes_.size() || from == to) return false;
    // 重复边直接拒绝：indegree 加两次会让 Kahn 永远消不掉，且图语义上无意义。
    for (const Edge& e : edges_) {
        if (e.from == from && e.to == to) return false;
    }
    edges_.push_back({from, to});
    nodes_[from].successors.push_back(to);
    ++nodes_[to].indegree;
    return true;
}

const std::string& Graph::name(std::size_t id) const {
    return id < nodes_.size() ? nodes_[id].name : kEmptyName;
}

std::vector<std::size_t> Graph::topoOrder(std::string* error) const {
    std::vector<std::size_t> order;
    if (nodes_.empty()) return order;   // 空图合法（run 直接成功）

    std::vector<std::size_t> indeg(nodes_.size());
    for (std::size_t i = 0; i < nodes_.size(); ++i) indeg[i] = nodes_[i].indegree;

    // 就绪队列按登记序扫描，保证同一 DAG 的拓扑序逐次一致（确定性）。
    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < indeg.size(); ++i)
        if (indeg[i] == 0) ready.push_back(i);

    while (!ready.empty()) {
        std::size_t cur = ready.front();
        ready.erase(ready.begin());
        order.push_back(cur);
        for (std::size_t nxt : nodes_[cur].successors) {
            if (--indeg[nxt] == 0) ready.push_back(nxt);
        }
        // 新就绪节点按 id 排序插入位置：扫描式实现下 ready 天然按父序，
        // 为严格确定（父 id 大 / 小混合）统一排序。
        std::sort(ready.begin(), ready.end());
    }

    if (order.size() != nodes_.size()) {
        if (error) *error = "cycle detected: task graph is not a DAG";
        return {};
    }
    return order;
}

bool Graph::run(unsigned threadCount, std::string* error) const {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (!nodes_[i].body) {
            if (error) *error = "empty body at task " + std::to_string(i);
            return false;
        }
    }
    std::vector<std::size_t> order = topoOrder(error);
    if (nodes_.empty()) return true;
    if (order.empty()) return false;    // 成环，error 已写

    const unsigned n = threadCount ? threadCount : std::thread::hardware_concurrency();
    tf::Executor executor(n ? n : 1u);
    tf::Taskflow flow;

    // 按登记序 emplace，保证 tf 节点编号与项目 id 一一对应。
    // 异常在封装层自行捕获：Taskflow 把异常挂在 topology 上（v3.11.0 executor.hpp:1863/1871），
    // 单 task 的 has_exception_ptr() 不保证置位；自捕获还能按登记序取「第一个」异常，确定性更好。
    std::vector<std::exception_ptr> errors(nodes_.size());
    std::vector<tf::Task> tasks;
    tasks.reserve(nodes_.size());
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        Body body = nodes_[i].body;   // 拷贝闭包；任务只写自己的槽位
        tasks.push_back(flow.emplace([body, i, &errors] {
            try { body(); }
            catch (...) { errors[i] = std::current_exception(); }
        }));
    }
    for (const Edge& e : edges_) tasks[e.from].precede(tasks[e.to]);

    executor.run(flow).wait();

    for (std::size_t i = 0; i < errors.size(); ++i) {
        if (errors[i]) {
            if (error) *error = "exception in task " + std::to_string(i)
                              + " (" + nodes_[i].name + ")";
            return false;
        }
    }
    return true;
}

std::string Graph::dumpDot() const {
    std::ostringstream os;
    os << "digraph jobgraph {\n";
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        os << "  t" << i << " [label=\"" << nodes_[i].name << "\"];\n";
    }
    for (const Edge& e : edges_) {
        os << "  t" << e.from << " -> t" << e.to << ";\n";
    }
    os << "}\n";
    return os.str();
}

} // namespace jobgraph
} // namespace nl::core
