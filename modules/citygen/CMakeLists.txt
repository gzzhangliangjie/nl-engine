# ---- M-CG0：citygen —— 自研城市生成库 + CLI 壳（游戏③广州地图，feat/citygen）----
# 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐 city-tools / 展示台 expectExit）。
# 纯 CPU 离线、零图形 API、零引擎依赖；解析 = vendored nlohmann/json 3.10.4 单头
# （thirdparty/nlohmann/VENDOR.md，MIT）。依据 docs/citygen/CG0-PLAN.md。
# 库形态（2026-11-15 citygen 库化拍板）：include/src 分离，CLI 只是壳；
# nlohmann 类型不进公开头（选型闸第 5 条隔离）。
add_library(citygen_core STATIC
    src/osm.cpp
    src/roadnet.cpp)
target_include_directories(citygen_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${THIRDPARTY})  # <nlohmann/json.hpp> → thirdparty/nlohmann/json.hpp
target_compile_features(citygen_core PUBLIC cxx_std_17)

add_executable(citygen main.cpp)
target_link_libraries(citygen PRIVATE citygen_core)

if(MSVC)
    target_compile_options(citygen_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(citygen PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(citygen PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- M-CG0.0：解析/bbox/坐标 selftest（验收判据①解析基线）----
add_test(NAME cg0-selftest COMMAND citygen selftest)
set_tests_properties(cg0-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "citygen selftest: PASS")

# ---- M-CG0.0：真实 Overpass 分片样本解析锚（fixture 由 tile-3-2 珠江新城裁剪，
# 生成脚本留档 test/gen_fixture.py；计数锚以实测为准，改动 fixture 必须同步）----
add_test(NAME cg0-sample
    COMMAND citygen stats --input ${CMAKE_CURRENT_SOURCE_DIR}/test/zjx-sample.json
            --bbox 23.108,113.316,23.130,113.342)
set_tests_properties(cg0-sample PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test
    PASS_REGULAR_EXPRESSION "ways=[0-9]+ droppedNoGeometry=[0-9]+ droppedNonWay=[0-9]+")

# ---- M-CG0.0：负向（非法输入显式退出 2）----
add_test(NAME cg0-negative COMMAND citygen stats --input nonexistent.json)
set_tests_properties(cg0-negative PROPERTIES WILL_FAIL TRUE)

# ---- M-CG0.1：真实样本 → roadnet.json（fixture 同 CG0.0，计数锚以实测为准）----
add_test(NAME cg0-convert
    COMMAND citygen convert --input ${CMAKE_CURRENT_SOURCE_DIR}/test/zjx-sample.json
            --bbox 23.108,113.316,23.130,113.342
            --out ${CMAKE_CURRENT_SOURCE_DIR}/test/zjx-roadnet.json)
set_tests_properties(cg0-convert PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/test
    PASS_REGULAR_EXPRESSION "nodes=[0-9]+ edges=[0-9]+")
