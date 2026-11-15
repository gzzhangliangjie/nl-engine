# ---- P8 M-P0：phys-tools —— 物理最小世界无头验收工具（A 类轨道）----
# 退出码：0=PASS / 1=断言失败 / 2=用法错误（对齐 nanite-scene / 展示台 expectExit）。
# 只链 nl_core（nl/physics PIMPL 把 Jolt 头挡在 cpp 内），零图形 API。
# 2026-11-10 自根 CMakeLists.txt 拆出（target + ctest 用例一块搬）。
add_executable(phys-tools main.cpp)
target_link_libraries(phys-tools PRIVATE nl_core)

if(MSVC)
    target_compile_options(phys-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(phys-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- P8 M-P0：物理最小世界（A 类工具版验收，零渲染依赖）----
add_test(NAME p0-phys-selftest COMMAND phys-tools selftest)
set_tests_properties(p0-phys-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "phys-p0 selftest: PASS")
add_test(NAME p0-phys-drop COMMAND phys-tools simulate --frames 240)
set_tests_properties(p0-phys-drop PROPERTIES
    PASS_REGULAR_EXPRESSION "nl-physics M-P0 drop trajectory")
add_test(NAME p0-phys-bad-config COMMAND phys-tools simulate --restitution 1.5)
set_tests_properties(p0-phys-bad-config PROPERTIES WILL_FAIL TRUE)

# ---- P8 M-P1：碰撞层矩阵 + 体关联（A 类工具版验收）----
add_test(NAME p1-phys-layers COMMAND phys-tools layers --frames 180)
set_tests_properties(p1-phys-layers PROPERTIES
    PASS_REGULAR_EXPRESSION "nl-physics M-P1 collision layer matrix probe")
add_test(NAME p1-phys-negative COMMAND phys-tools layers --frames 0)
set_tests_properties(p1-phys-negative PROPERTIES WILL_FAIL TRUE)

# ---- P8 M-P2：堆叠稳定 / 休眠岛（A 类工具版验收）----
add_test(NAME p2-phys-stack COMMAND phys-tools stack --frames 180)
set_tests_properties(p2-phys-stack PROPERTIES
    PASS_REGULAR_EXPRESSION "nl-physics M-P2 stacking stability")
add_test(NAME p2-phys-negative COMMAND phys-tools stack --frames 0)
set_tests_properties(p2-phys-negative PROPERTIES WILL_FAIL TRUE)

# ---- P8 M-P3：碰撞形状调试线框流（A 类工具版验收，Jolt DebugRenderer 直出）----
add_test(NAME p3-phys-debug COMMAND phys-tools debug --frames 240
         --out ${CMAKE_CURRENT_BINARY_DIR}/phys-p3-debug.tsv)
set_tests_properties(p3-phys-debug PROPERTIES
    PASS_REGULAR_EXPRESSION "wrote .*phys-p3-debug\\.tsv")
add_test(NAME p3-phys-debug-check
         COMMAND phys-tools check-debug ${CMAKE_CURRENT_BINARY_DIR}/phys-p3-debug.tsv)
set_tests_properties(p3-phys-debug-check PROPERTIES
    DEPENDS p3-phys-debug
    PASS_REGULAR_EXPRESSION "phys-p3 check-debug: PASS")
add_test(NAME p3-phys-negative COMMAND phys-tools debug --frames 0)
set_tests_properties(p3-phys-negative PROPERTIES WILL_FAIL TRUE)

# ---- P8 M-P4：角色走查（CharacterVirtual，CharacterMovement 式扫掠/地面检测/斜坡台阶）----
add_test(NAME p4-phys-character COMMAND phys-tools character --frames 1200
         --out ${CMAKE_CURRENT_BINARY_DIR}/phys-p4-character.tsv)
set_tests_properties(p4-phys-character PROPERTIES
    PASS_REGULAR_EXPRESSION "wrote .*phys-p4-character\\.tsv")
add_test(NAME p4-phys-character-check
         COMMAND phys-tools check-character ${CMAKE_CURRENT_BINARY_DIR}/phys-p4-character.tsv)
set_tests_properties(p4-phys-character-check PROPERTIES
    DEPENDS p4-phys-character
    PASS_REGULAR_EXPRESSION "phys-p4 check-character: PASS")
add_test(NAME p4-phys-negative COMMAND phys-tools character --frames 0)
set_tests_properties(p4-phys-negative PROPERTIES WILL_FAIL TRUE)

# ---- P8 M-J5c: Jolt JobSystem adapter (pool-backed parallel physics) ----
add_test(NAME p-j5c-check COMMAND phys-tools check-jobs --threads 4 --frames 240)
set_tests_properties(p-j5c-check PROPERTIES PASS_REGULAR_EXPRESSION "phys-j5c check-jobs: PASS")
add_test(NAME p-j5c-negative COMMAND phys-tools simulate --jobs -1 --frames 240)
set_tests_properties(p-j5c-negative PROPERTIES WILL_FAIL TRUE)
# ---- P8 M-D0: contact event bridge (A-class tool acceptance, zero graphics) ----
add_test(NAME d0-contact-trace COMMAND phys-tools contact-trace --frames 240
         --out ${CMAKE_CURRENT_BINARY_DIR}/phys-d0-contacts.tsv)
set_tests_properties(d0-contact-trace PROPERTIES
    PASS_REGULAR_EXPRESSION "wrote .*phys-d0-contacts\\.tsv")
add_test(NAME d0-contact-check
         COMMAND phys-tools contact-check ${CMAKE_CURRENT_BINARY_DIR}/phys-d0-contacts.tsv)
set_tests_properties(d0-contact-check PROPERTIES
    DEPENDS d0-contact-trace
    PASS_REGULAR_EXPRESSION "phys-d0 contact-check: PASS")
add_test(NAME d0-contact-negative COMMAND phys-tools contact-trace --frames 0)
set_tests_properties(d0-contact-negative PROPERTIES WILL_FAIL TRUE)

# ---- P8 M-D1: ECS consumption of the M-D0 contact stream (A-class tool acceptance) ----
add_test(NAME d1-ecs-trace COMMAND phys-tools ecs-trace --frames 240
         --out ${CMAKE_CURRENT_BINARY_DIR}/phys-d1-ecs.tsv)
set_tests_properties(d1-ecs-trace PROPERTIES
    PASS_REGULAR_EXPRESSION "wrote .*phys-d1-ecs\\.tsv")
add_test(NAME d1-ecs-check
         COMMAND phys-tools ecs-check ${CMAKE_CURRENT_BINARY_DIR}/phys-d1-ecs.tsv)
set_tests_properties(d1-ecs-check PROPERTIES
    DEPENDS d1-ecs-trace
    PASS_REGULAR_EXPRESSION "phys-d1 ecs-check: PASS")
add_test(NAME d1-ecs-negative COMMAND phys-tools ecs-trace --frames 0)
set_tests_properties(d1-ecs-negative PROPERTIES WILL_FAIL TRUE)

# ---- P8 M-D2: headless VehicleConstraint drive (A-class tool acceptance) ----
add_test(NAME d2-drive-trace COMMAND phys-tools drive-trace --frames 480
         --out ${CMAKE_CURRENT_BINARY_DIR}/phys-d2-drive.tsv)
set_tests_properties(d2-drive-trace PROPERTIES
    PASS_REGULAR_EXPRESSION "wrote .*phys-d2-drive\\.tsv")
add_test(NAME d2-drive-check
         COMMAND phys-tools drive-check ${CMAKE_CURRENT_BINARY_DIR}/phys-d2-drive.tsv)
set_tests_properties(d2-drive-check PROPERTIES
    DEPENDS d2-drive-trace
    PASS_REGULAR_EXPRESSION "phys-d2 drive-check: PASS")
add_test(NAME d2-drive-negative COMMAND phys-tools drive-trace --frames 0)
set_tests_properties(d2-drive-negative PROPERTIES WILL_FAIL TRUE)

# ---- M-PQ0: general physics query API (A-class tool acceptance) ----
add_test(NAME pq0-query-trace COMMAND phys-tools query-trace
         --out ${CMAKE_CURRENT_BINARY_DIR}/phys-pq0-query.tsv)
set_tests_properties(pq0-query-trace PROPERTIES
    PASS_REGULAR_EXPRESSION "wrote .*phys-pq0-query\.tsv")
add_test(NAME pq0-query-check
         COMMAND phys-tools query-check ${CMAKE_CURRENT_BINARY_DIR}/phys-pq0-query.tsv)
set_tests_properties(pq0-query-check PROPERTIES
    DEPENDS pq0-query-trace
    PASS_REGULAR_EXPRESSION "phys-pq0 query-check: PASS")
add_test(NAME pq0-query-negative COMMAND phys-tools query-trace --out)
set_tests_properties(pq0-query-negative PROPERTIES WILL_FAIL TRUE)
