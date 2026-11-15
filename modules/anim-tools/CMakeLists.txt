# ---- M-A1：anim-tools —— 动作系统（A 类并行轨道）自测工具，只链 nl_core ----
# 退出码约定：0=PASS / 1=断言失败 / 2=用法错误（与展示台 expectExit 0/1/2 对齐）。
# 2026-11-10 自根 CMakeLists.txt 拆出（target + ctest 用例一块搬）。
add_executable(anim-tools main.cpp)
target_link_libraries(anim-tools PRIVATE nl_core)

if(MSVC)
    target_compile_options(anim-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(anim-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- M-A 轨道（A 类并行：动作系统，工具版交代，不进渲染回归总闸） ----
# M-A1：关键帧采样机械单测（lerp/slerp 锚点、clamp、Step、常量、wrap、validate 负向、确定性）
add_test(NAME ma1-anim-curve-selftest COMMAND anim-tools selftest curve)
set_tests_properties(ma1-anim-curve-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "anim curve selftest: PASS")
# M-A1：负向——未知 selftest 用例显式拒绝（exit 2）
add_test(NAME ma1-anim-unknown-case COMMAND anim-tools selftest bogus)
set_tests_properties(ma1-anim-unknown-case PROPERTIES WILL_FAIL TRUE)
# M-A2：线性/加法混合机械单测（lerp 锚点 w=0/0.5/1、加法 quat 相对叠加语义、骨架不一致拒绝）
add_test(NAME ma2-anim-blend-selftest COMMAND anim-tools selftest blend)
set_tests_properties(ma2-anim-blend-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "anim blend selftest: PASS")
# M-A3：状态机机械单测（优先级取首个、crossfade 权重曲线、UE 回退缩时+姿态连续、
# 10 步确定性场景逐值断言、validate 负向、两次运行一致）
add_test(NAME ma3-anim-statemachine-selftest COMMAND anim-tools selftest statemachine)
set_tests_properties(ma3-anim-statemachine-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "anim statemachine selftest: PASS")
# M-A4：1D blend space 机械单测（段权重锚点、域外 clamp、单 sample 退化、
# validate 负向、参数域扫掠两次运行逐位一致）
add_test(NAME ma4-anim-blendspace-selftest COMMAND anim-tools selftest blendspace)
set_tests_properties(ma4-anim-blendspace-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "anim blendspace selftest: PASS")
# M-A4b：骨架 FK + rigged glTF 入口机械单测（FK 手算锚点、sway t=0.5 世界位置、
# wrap 循环、两次加载逐位一致、缺文件/scale/拓扑序负向）
add_test(NAME ma4b-anim-skeleton-selftest
         COMMAND anim-tools selftest skeleton
                 ${NL_ROOT}/assets/models/RiggedChain.glb)
set_tests_properties(ma4b-anim-skeleton-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "anim skeleton selftest: PASS")

# ---- M-A5：蒙皮确定性自测（刚体/LBS 手算锚点 + CesiumMan rest 调色板≈identity）----
add_test(NAME ma5-anim-skinning-selftest
         COMMAND anim-tools selftest skinning
                 ${NL_ROOT}/assets/models/rigged/CesiumMan.glb)
set_tests_properties(ma5-anim-skinning-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "anim skinning selftest: PASS")
