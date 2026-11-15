# ---- B 路材质图工具（A 类轨道，工具版交代）----
# 第 2 步种子：节点图数据模型 + 稳定序列化。只链 nl_core（日志），零图形 API。
# 2026-11-10 自根 CMakeLists.txt 拆出（target + ctest 用例一块搬）。
add_executable(material-graph-tools main.cpp)
target_link_libraries(material-graph-tools PRIVATE nl_core)

if(MSVC)
    target_compile_options(material-graph-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(material-graph-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- B 路材质图：节点图数据模型 + 序列化机械单测（A 类轨道第 2 步验收）----
# 不进渲染回归总闸（bytes/near），走工具版交代。
add_test(NAME b2-graph-selftest COMMAND material-graph-tools selftest)
set_tests_properties(b2-graph-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "material-graph selftest: PASS")

# ---- B 路材质图 B3：图 -> Slang 代码生成 + 双后端编译验收（A 类轨道）----
# 走工具版交代：emit 示例图 -> slangc 编译 SPIR-V + DXIL 各 vs/ps 入口。
add_test(NAME b3-graph-genslang COMMAND material-graph-tools check)
set_tests_properties(b3-graph-genslang PROPERTIES
    PASS_REGULAR_EXPRESSION "genslang   : OK"
    ENVIRONMENT "NL_SLANG_BIN=${NL_SLANG_BIN};NL_DXC_DIR=${NL_DXC_DIR}")

# ---- B 路材质图 B4：热重载（改图即重编译）----
# watch --once 对 fixture 图做单次 recompile（emit + 双后端编译），验收热重载链路本身；
# 与 check 同源，只是走 watch 的加载 + 轮询首轮路径。不进渲染总闸，走工具版交代。
add_test(NAME b4-graph-hot-reload
         COMMAND material-graph-tools watch
                 ${NL_ROOT}/modules/material-graph-tools/sample.mg --once)
# M-MOD0 迁移遗留根修（M-W2A 顺手，2026-11-15）：模块目录已从根级
# material-graph-tools/ 迁入 modules/，本用例 test path 未跟 → cannot stat 假挂。
set_tests_properties(b4-graph-hot-reload PROPERTIES
    PASS_REGULAR_EXPRESSION "OK"
    DEPENDS b3-graph-genslang
    ENVIRONMENT "NL_SLANG_BIN=${NL_SLANG_BIN};NL_DXC_DIR=${NL_DXC_DIR}")
