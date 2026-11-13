# ---- W0 M-W0.0：city-tools —— 大世界生成离线工具（A 类轨道 feat/worldgen）----
# 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐 phys-tools / 展示台 expectExit）。
# 纯 CPU、零图形 API、零引擎依赖（仅 vendored stb_image_write.h 出 PNG，
# renderer/src/png_writer.cpp 同源先例）。依据 docs/worldgen/W0-PLAN.md（W3-A
# 全自研最小算法拍板）；selftest 锁 seed 确定性（同 seed 逐字节、异 seed 必异）。
add_executable(city-tools main.cpp)
target_include_directories(city-tools PRIVATE ${THIRDPARTY}/stb)

if(MSVC)
    target_compile_options(city-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(city-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- M-W0.0：seed 对称确定性 selftest（验收判据①）----
add_test(NAME w0-city-selftest COMMAND city-tools selftest)
set_tests_properties(w0-city-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "citygen selftest: PASS")

# ---- M-W0.0：空白城俯视 PNG 生成（验收判据②证据源）----
add_test(NAME w0-city-generate COMMAND city-tools generate --seed 12345 --size 6
         --out ${CMAKE_CURRENT_BINARY_DIR}/w0-city --stats)
set_tests_properties(w0-city-generate PROPERTIES
    PASS_REGULAR_EXPRESSION "wrote .*w0-city.city_top\\.png")

# ---- M-W0.0：负向（size 越界显式退出 2）----
add_test(NAME w0-city-negative COMMAND city-tools generate --size 0
         --out ${CMAKE_CURRENT_BINARY_DIR}/w0-city-bad)
set_tests_properties(w0-city-negative PROPERTIES WILL_FAIL TRUE)

# ---- M-W0.1：roadnet.json 契约产物（节点/有向边网格公式锚）----
add_test(NAME w0-roadnet COMMAND city-tools generate --seed 12345 --size 6
         --out ${CMAKE_CURRENT_BINARY_DIR}/w0-roadnet --stats)
set_tests_properties(w0-roadnet PROPERTIES
    PASS_REGULAR_EXPRESSION "wrote .*w0-roadnet.roadnet\\.json \\(nodes=49 edges=168 directed\\)")
