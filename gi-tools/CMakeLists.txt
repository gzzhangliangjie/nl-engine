# ---- S11 M11.0：gi-tools —— 世界缓存 CPU 仿真与无头验收工具（主线）----
# 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐 nav-tools / 展示台 expectExit）。
# 只链 nl_core，零图形 API；WorldCacheSim 与 renderer/shaders/world_cache.slang
# 是同一套 hashmap 语义的 CPU/GPU 双实现（Q11-0 拍板方案 A）。
add_executable(gi-tools main.cpp)
target_link_libraries(gi-tools PRIVATE nl_core)

if(MSVC)
    target_compile_options(gi-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(gi-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- S11 M11.0: world cache CPU simulation (ctest) ----
add_test(NAME s11-gi-selftest COMMAND gi-tools selftest cachesim)
set_tests_properties(s11-gi-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "gi-tools cachesim selftest: PASS")
add_test(NAME s11-gi-selftest-negative COMMAND gi-tools selftest bogus)
set_tests_properties(s11-gi-selftest-negative PROPERTIES WILL_FAIL TRUE)
add_test(NAME s11-gi-cachesim
         COMMAND gi-tools cachesim --frames 120
                 --png ${CMAKE_CURRENT_BINARY_DIR}/s11-cachesim.png --size 512)
set_tests_properties(s11-gi-cachesim PROPERTIES
    PASS_REGULAR_EXPRESSION "cells        : [1-9]")
add_test(NAME s11-gi-cachesim-usage COMMAND gi-tools cachesim --bogus)
set_tests_properties(s11-gi-cachesim-usage PROPERTIES WILL_FAIL TRUE)
add_test(NAME s11-gi-cachesim-badframes COMMAND gi-tools cachesim --frames 0)
set_tests_properties(s11-gi-cachesim-badframes PROPERTIES WILL_FAIL TRUE)
