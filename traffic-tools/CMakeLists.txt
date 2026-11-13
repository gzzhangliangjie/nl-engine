# ---- P8 M-T0.0：traffic-tools —— 交通 lane graph 无头验收工具（A 类轨道 feat/worldgen）----
# 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐 nav-tools / city-tools / 展示台 expectExit）。
# 只链 nl_core（nl/traffic 零图形依赖）；拍板与口径 docs/worldgen/TRAFFIC-PLAN.md。
add_executable(traffic-tools main.cpp)
target_link_libraries(traffic-tools PRIVATE nl_core)

if(MSVC)
    target_compile_options(traffic-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(traffic-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- M-T0.0：lane graph 数值锚 selftest（车道数网格公式 / 偏移几何 / 连接闭合 /
# 禁掉头 / 双跑 dump 逐字节一致 / 负向显式报错；TRAFFIC-PLAN §3 M-T0.0 验收判据①）----
add_test(NAME m-t0-traffic-selftest COMMAND traffic-tools selftest)
set_tests_properties(m-t0-traffic-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "traffic selftest: PASS")

# ---- M-T0.0：hermetic 夹具落盘（build 正向用例数据源，DEPENDS 链）----
add_test(NAME m-t0-traffic-fixture COMMAND traffic-tools selftest
         --emit-fixture ${CMAKE_CURRENT_BINARY_DIR}/t0-fixture-roadnet.json)
set_tests_properties(m-t0-traffic-fixture PROPERTIES
    PASS_REGULAR_EXPRESSION "wrote .*t0-fixture-roadnet\\.json")

# ---- M-T0.0：build 正向（方格夹具 -> 8 lanes / 8 links / 80m + 确定性 dump 落盘）----
add_test(NAME m-t0-traffic-build COMMAND traffic-tools build
         ${CMAKE_CURRENT_BINARY_DIR}/t0-fixture-roadnet.json
         --dump ${CMAKE_CURRENT_BINARY_DIR}/t0-lane-graph.json)
set_tests_properties(m-t0-traffic-build PROPERTIES
    DEPENDS m-t0-traffic-fixture
    PASS_REGULAR_EXPRESSION "lanes       : 8")

# ---- M-T0.0：负向（缺文件显式退出 1）----
add_test(NAME m-t0-traffic-negative COMMAND traffic-tools build
         ${CMAKE_CURRENT_BINARY_DIR}/t0-missing-roadnet.json)
set_tests_properties(m-t0-traffic-negative PROPERTIES WILL_FAIL TRUE)
