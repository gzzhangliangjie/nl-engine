# ---- M-SP0：sprite-tools —— 2D 精灵 core 侧无头验收工具（B 类穿插轨）----
# 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐 phys-tools / input-tools）。
# 只链 nl_core：atlas / flipbook / batch 全是纯 POD + CPU 逻辑，零图形 API。
add_executable(sprite-tools main.cpp)
target_link_libraries(sprite-tools PRIVATE nl_core)

if(MSVC)
    target_compile_options(sprite-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(sprite-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set(SP0_TSV_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# 机械单测：atlas UV / flipbook 时间线 / batch 稳定排序 + 分桶，一次全跑。
add_test(NAME sp0-sprite-selftest COMMAND sprite-tools selftest)
add_test(NAME sp0-selftest-negative COMMAND sprite-tools selftest bogus)
set_tests_properties(sp0-selftest-negative PROPERTIES WILL_FAIL TRUE)

# atlas：4x4/2x3/1x1/0x4 四种网格 × Throw/Repeat/Clamp 三种寻址。
add_test(NAME sp0-atlas-trace COMMAND sprite-tools atlas-trace
         --out ${SP0_TSV_DIR}/sp0-atlas.tsv)
add_test(NAME sp0-atlas-check COMMAND sprite-tools atlas-check
         ${SP0_TSV_DIR}/sp0-atlas.tsv)

# flipbook：loop / 非 loop finished latch / reverse / pause / stop。
add_test(NAME sp0-flipbook-trace COMMAND sprite-tools flipbook-trace
         --out ${SP0_TSV_DIR}/sp0-flipbook.tsv)
add_test(NAME sp0-flipbook-check COMMAND sprite-tools flipbook-check
         ${SP0_TSV_DIR}/sp0-flipbook.tsv)

# batch：乱序 layer/textureId + invalid id，验稳定排序与桶覆盖。
add_test(NAME sp0-batch-trace COMMAND sprite-tools batch-trace
         --out ${SP0_TSV_DIR}/sp0-batch.tsv)
add_test(NAME sp0-batch-check COMMAND sprite-tools batch-check
         ${SP0_TSV_DIR}/sp0-batch.tsv)

# 负例：--out 缺路径必须 exit 2。
add_test(NAME sp0-trace-negative COMMAND sprite-tools atlas-trace --out)
set_tests_properties(sp0-trace-negative PROPERTIES WILL_FAIL TRUE)
