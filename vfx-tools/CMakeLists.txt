# ---- PT M-PT1：vfx-tools —— Effekseer .efkefc → .nfx 离线转换器（B 类轨道）----
# 退出码：0=成功 / 1=断言失败 / 2=用法或子集外模块显式报错（对齐展示台 expectExit）。
# 链 nl_core + nl_effekseer（vendored 解析层）；运行时不链 Effekseer。
# 2026-11-10 feat/particles 合并时自根 CMakeLists.txt 拆出（对齐 2026-11-10 工具拆分口径）。
add_executable(vfx-tools main.cpp)
target_link_libraries(vfx-tools PRIVATE nl_core nl_effekseer)

if(MSVC)
    target_compile_options(vfx-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(vfx-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# PT M-PT1：vfx-tools 机械验收（selftest + 真实样例转换正负向）
add_test(NAME p-mpt1-selftest
         COMMAND vfx-tools selftest)
set_tests_properties(p-mpt1-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "nfx selftest: PASS")
add_test(NAME p-mpt1-convert-positive
         COMMAND vfx-tools convert ${NL_ROOT}/assets/efx/CullingSprite.efkefc
                 ${CMAKE_CURRENT_BINARY_DIR}/p-mpt1-culling.nfx)
set_tests_properties(p-mpt1-convert-positive PROPERTIES
    PASS_REGULAR_EXPRESSION "converted: CullingSprite")
