# ---- P8 M-I0：input-tools —— 输入状态层无头验收工具（A 类轨道）----
# 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐 phys-tools / 展示台 expectExit）。
# 只链 nl_core，注入路径（无窗口）跑全部数值锚；零图形 API。
# glfw 链接说明：input.hpp 公开 API 里用 GLFW 键码常量（P8-INPUT-PLAN §3 口径），
# 与 nl_renderer 链 glfw 的理由同款（console_ui.cpp 吃键值常量）。
# 2026-11-10 自根 CMakeLists.txt 拆出（target + ctest 用例一块搬）。
add_executable(input-tools main.cpp)
target_link_libraries(input-tools PRIVATE nl_core glfw)

if(MSVC)
    target_compile_options(input-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(input-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# P8 M-I0：输入状态层机械单测（边沿三态 / 组合 / REPEAT / 鼠标 / 越界 / mods / 确定性）
add_test(NAME mi0-input-selftest COMMAND input-tools selftest)
add_test(NAME mi0-input-usage-negative COMMAND input-tools selftest bogus)
set_tests_properties(mi0-input-usage-negative PROPERTIES WILL_FAIL TRUE)
# P8 M-I1：映射层机械单测（Carrot 解析规则 / 组开关冻结 / 序列确定性）
add_test(NAME mi1-input-actions-selftest COMMAND input-tools selftest actions)
add_test(NAME mi1-input-actions-det-selftest COMMAND input-tools selftest actions-det)
