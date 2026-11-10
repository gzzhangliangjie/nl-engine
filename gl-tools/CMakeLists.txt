# ---- P8 M-GL0：gl-tools —— 游戏循环（双帧模型 GameClock）自测工具（A 类轨道）----
# 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐 phys-tools / 展示台 expectExit）。
# 只链 nl_core（nl/core GameClock 纯逻辑零依赖），零图形 API。
# 2026-11-10 自根 CMakeLists.txt 拆出（target + ctest 用例一块搬）。
add_executable(gl-tools main.cpp)
target_link_libraries(gl-tools PRIVATE nl_core)

if(MSVC)
    target_compile_options(gl-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(gl-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- P8 M-GL0: game loop (dual-frame GameClock) ----
add_test(NAME gl0-selftest COMMAND gl-tools selftest)
set_tests_properties(gl0-selftest PROPERTIES PASS_REGULAR_EXPRESSION "gl-m0 selftest: PASS")
add_test(NAME gl0-trace COMMAND gl-tools trace --frames 240 --dt 16.7)
set_tests_properties(gl0-trace PROPERTIES PASS_REGULAR_EXPRESSION "gl-m0 trace: PASS")
add_test(NAME gl0-negative COMMAND gl-tools trace --dt abc)
set_tests_properties(gl0-negative PROPERTIES WILL_FAIL TRUE)
