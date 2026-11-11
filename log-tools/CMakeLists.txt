# ---- A 类日志轨道 M-LOG0：log-tools —— nl::core::log 兼容层无头验收工具 ----
# 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐展示台 expectExit 约定）。
# 只链 nl_core（spdlog 被兼容层挡在 log.cpp 内），零图形 API。
add_executable(log-tools main.cpp)
target_link_libraries(log-tools PRIVATE nl_core)

if(MSVC)
    target_compile_options(log-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(log-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- M-LOG0：兼容层语义保形 + 三路 sink 自证（A 类工具版验收）----
add_test(NAME log-m0-selftest COMMAND log-tools selftest)
set_tests_properties(log-m0-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "log selftest: PASS")
# 负向：非法过滤指令必须显式拒绝（exit 1），不许静默吞。
add_test(NAME log-m1-bad-filter COMMAND log-tools selftest --negative-only)
set_tests_properties(log-m1-bad-filter PROPERTIES
    PASS_REGULAR_EXPRESSION "log selftest: PASS")
add_test(NAME log-m0-negative COMMAND log-tools bogus-command)
set_tests_properties(log-m0-negative PROPERTIES WILL_FAIL TRUE)
