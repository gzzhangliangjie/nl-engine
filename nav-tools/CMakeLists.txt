# ---- P8 M-N0：nav-tools —— 寻路烘焙/查询无头验收工具（A 类轨道）----
# 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐 phys-tools / 展示台 expectExit）。
# 只链 nl_core（nl/nav PIMPL 把 Recast/Detour 头挡在 cpp 内），零图形 API。
# 2026-11-10 自根 CMakeLists.txt 拆出（target + ctest 用例一块搬）。
add_executable(nav-tools main.cpp)
target_link_libraries(nav-tools PRIVATE nl_core)

if(MSVC)
    target_compile_options(nav-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(nav-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- P8 M-N0: nav-tools (Recast/Detour bake/dump, A-class track) ----
add_test(NAME m-n0-nav-selftest COMMAND nav-tools selftest)
set_tests_properties(m-n0-nav-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "nav selftest: PASS")
add_test(NAME m-n0-nav-bake
         COMMAND nav-tools bake ${NL_ROOT}/assets/models/HizScene.glb
                 ${CMAKE_CURRENT_BINARY_DIR}/nav-m0-hiz.nnav
                 --cell-size 0.1 --agent-radius 0.2)
set_tests_properties(m-n0-nav-bake PROPERTIES
    PASS_REGULAR_EXPRESSION "saved       :")
add_test(NAME m-n0-nav-dump
         COMMAND nav-tools dump ${CMAKE_CURRENT_BINARY_DIR}/nav-m0-hiz.nnav)
set_tests_properties(m-n0-nav-dump PROPERTIES
    DEPENDS m-n0-nav-bake
    PASS_REGULAR_EXPRESSION "polys       : [1-9]")
add_test(NAME m-n0-nav-negative COMMAND nav-tools dump ${NL_ROOT}/assets/models/Suzanne.glb)
set_tests_properties(m-n0-nav-negative PROPERTIES WILL_FAIL TRUE)
