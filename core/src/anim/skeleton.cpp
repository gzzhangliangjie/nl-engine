// M-A4b：骨架校验与 FK（口径见 skeleton.hpp 文件头）。

#include "nl/anim/skeleton.hpp"

#include <set>

namespace nl::anim {
using namespace nl::math;   // 数学函数本体在 nl/math/math.hpp（2026-11-09 上移）

std::size_t findBoneIndex(const Skeleton& skeleton, const std::string& bone) {
    for (std::size_t i = 0; i < skeleton.boneNames.size(); ++i)
        if (skeleton.boneNames[i] == bone) return i;
    return static_cast<std::size_t>(-1);
}

std::string validateSkeleton(const Skeleton& skeleton) {
    const std::size_t n = skeleton.boneCount();
    if (n == 0) return "skeleton: must have at least one bone";
    if (skeleton.parentIndices.size() != n || skeleton.restPose.size() != n)
        return "skeleton: names/parents/restPose size mismatch";
    std::set<std::string> seen;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string& name = skeleton.boneNames[i];
        if (name.empty()) return "skeleton: bone name must not be empty";
        if (!seen.insert(name).second) return "skeleton: duplicate bone name " + name;
        const int p = skeleton.parentIndices[i];
        if (p == -1) continue;
        if (p < -1 || static_cast<std::size_t>(p) >= i)
            return "skeleton: parent index out of topological order";
    }
    return {};
}

bool localToModel(const std::vector<BonePose>& local, const Skeleton& skeleton,
                  std::vector<BonePose>& out) {
    if (local.size() != skeleton.boneCount()) return false;
    const std::vector<BonePose> src = local;   // 拷贝隔离：out 允许与 local 同容器
    out.resize(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        const int p = skeleton.parentIndices[i];
        if (p == -1) {
            out[i] = src[i];
            continue;
        }
        const BonePose& parent = out[static_cast<std::size_t>(p)];
        out[i].rotation = mulQuat(parent.rotation, src[i].rotation);
        out[i].translation = parent.translation +
                             rotVecByQuat(parent.rotation, src[i].translation);
    }
    return true;
}

} // namespace nl::anim
