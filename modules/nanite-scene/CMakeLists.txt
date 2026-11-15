# ---- M-E0：nanite-scene —— ECS-lite 场景（nl/scene）自测工具，A 类并行轨道 ----
# 退出码约定：0=PASS / 1=断言失败 / 2=用法错误（与展示台 expectExit 0/1/2 对齐）。
# 2026-11-10 自根 CMakeLists.txt 拆出（target + ctest 用例一块搬）。
add_executable(nanite-scene main.cpp)
target_link_libraries(nanite-scene PRIVATE nl_core)

if(MSVC)
    target_compile_options(nanite-scene PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(nanite-scene PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- M-E0：ECS-lite World/Entity 核心机械单测（A 类轨道，P8-ECS-PLAN.md 判据）----
# 不进渲染回归总闸（bytes/near），走工具版交代。
add_test(NAME e0-ecs-core-selftest COMMAND nanite-scene selftest --suite ecs-core)
set_tests_properties(e0-ecs-core-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "ecs-core selftest: PASS")
# 负向：未知 suite = 用法错误退出码 2
add_test(NAME e0-ecs-unknown-suite COMMAND nanite-scene selftest --suite bogus)
set_tests_properties(e0-ecs-unknown-suite PROPERTIES WILL_FAIL TRUE)

# ---- M-E1：组件 + 层级机械单测（数值锚 = 手算对照，P8-ECS-PLAN.md 判据）----
add_test(NAME e1-ecs-hier-selftest COMMAND nanite-scene selftest --suite ecs-hier)
set_tests_properties(e1-ecs-hier-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "ecs-hier selftest: PASS")

# ---- M-E2：System + 场景 roundtrip（P8-ECS-PLAN.md 判据）----
add_test(NAME e2-ecs-system-selftest COMMAND nanite-scene selftest --suite ecs-system)
set_tests_properties(e2-ecs-system-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "ecs-system selftest: PASS")
add_test(NAME e2-ecs-serial-selftest COMMAND nanite-scene selftest --suite ecs-serial)
set_tests_properties(e2-ecs-serial-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "ecs-serial selftest: PASS")
# 负向：非法 JSON / 未知组件名 = 显式退出码 2（M-E2 判据原样落地）
add_test(NAME e2-ecs-badjson-exit2 COMMAND nanite-scene deserialise-check badjson)
set_tests_properties(e2-ecs-badjson-exit2 PROPERTIES
    PASS_REGULAR_EXPRESSION "rejected")
add_test(NAME e2-ecs-unknown-comp-exit2 COMMAND nanite-scene deserialise-check unknown)
set_tests_properties(e2-ecs-unknown-comp-exit2 PROPERTIES
    PASS_REGULAR_EXPRESSION "rejected")
add_test(NAME e2-ecs-good-scene COMMAND nanite-scene deserialise-check good)
set_tests_properties(e2-ecs-good-scene PROPERTIES
    PASS_REGULAR_EXPRESSION "PASS")
