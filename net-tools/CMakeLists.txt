# ---- NET M-NET0：net-tools —— 传输层无头验收工具（A 类轨道 net）----
# 退出码：0=PASS / 1=错误（建链失败等）/ 2=用法错误或超时（对齐 nav-tools / 展示台 expectExit）。
# 只链 nl_core（nl/net PIMPL 把 ENet 头挡在 core/src/net/Net.cpp 单 TU），零图形 API。
add_executable(net-tools main.cpp)
target_link_libraries(net-tools PRIVATE nl_core)

if(MSVC)
    target_compile_options(net-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(net-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- M-NET0 ctest（ctest 新增后须双后端重新 configure 才写入，见 AGENTS.md 工程纪律）----
# 单进程回环 echo：127.0.0.1 双 host 单线程泵，全量回显断言（确定性）
add_test(NAME net-m0-echo COMMAND net-tools echo)
set_tests_properties(net-m0-echo PROPERTIES
    PASS_REGULAR_EXPRESSION "echo        : OK")
# 机械单测（16 可靠 + 32 不可靠，往返字节级）
add_test(NAME net-m0-selftest COMMAND net-tools selftest)
set_tests_properties(net-m0-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "selftest net: PASS")
# 版本锚（vendored 溯源复核）
add_test(NAME net-m0-version COMMAND net-tools version)
set_tests_properties(net-m0-version PROPERTIES
    PASS_REGULAR_EXPRESSION "enet        : 1.3.18")
# 负向：未知子命令显式拒绝（exit 2）
add_test(NAME net-m0-usage-negative COMMAND net-tools bogus)
set_tests_properties(net-m0-usage-negative PROPERTIES WILL_FAIL TRUE)
# 负向：客户端连无监听端口（握手超时拒绝，exit 1）
add_test(NAME net-m0-connect-refused
         COMMAND net-tools echo-client 127.0.0.1 29199 --packets 4 --timeout-ms 1500)
set_tests_properties(net-m0-connect-refused PROPERTIES
    PASS_REGULAR_EXPRESSION "connect     : FAILED"
    WILL_FAIL TRUE)
