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
# M-N3：E1 场景 .scene 里的 meshFile 是 basename，需拼 assets/models 绝对路径。
target_compile_definitions(nav-tools PRIVATE
    NL_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets/models")

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

# ---- P8 M-N1: Detour query (findPath/findStraightPath + negative semantics) ----
add_test(NAME m-n1-nav-selftest COMMAND nav-tools selftest --verbose)
set_tests_properties(m-n1-nav-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "nav selftest: PASS")
add_test(NAME m-n1-nav-bake
         COMMAND nav-tools bake ${NL_ROOT}/assets/models/HizScene.glb
                 ${CMAKE_CURRENT_BINARY_DIR}/nav-n1-hiz.nnav
                 --cell-size 0.1 --agent-radius 0.2)
set_tests_properties(m-n1-nav-bake PROPERTIES
    PASS_REGULAR_EXPRESSION "saved       :")
add_test(NAME m-n1-nav-query-ok
         COMMAND nav-tools query ${CMAKE_CURRENT_BINARY_DIR}/nav-n1-hiz.nnav
                 --start -1.9,0,-2.1 --end -1.0,0,-1.0)
set_tests_properties(m-n1-nav-query-ok PROPERTIES
    DEPENDS m-n1-nav-bake
    PASS_REGULAR_EXPRESSION "status      : OK")
add_test(NAME m-n1-nav-query-unreachable
         COMMAND nav-tools query ${CMAKE_CURRENT_BINARY_DIR}/nav-n1-hiz.nnav
                 --start -1.8,0,-2.0 --end 100,0,100)
set_tests_properties(m-n1-nav-query-unreachable PROPERTIES
    DEPENDS m-n1-nav-bake
    WILL_FAIL TRUE)
add_test(NAME m-n1-nav-query-usage
         COMMAND nav-tools query ${CMAKE_CURRENT_BINARY_DIR}/nav-n1-hiz.nnav)
set_tests_properties(m-n1-nav-query-usage PROPERTIES
    WILL_FAIL TRUE)

# ---- P8 M-N2: five-stage debug visualization (keepDebug + DebugPrims) ----
foreach(stage IN ITEMS voxels regions contours polymesh navmesh)
    add_test(NAME m-n2-nav-debug-${stage}
             COMMAND nav-tools debug-stage
                     ${NL_ROOT}/assets/models/HizScene.glb
                     ${CMAKE_CURRENT_BINARY_DIR}/nav-n2-${stage}.png
                     --stage ${stage} --size 768
                     --cell-size 0.1 --agent-radius 0.2)
    set_tests_properties(m-n2-nav-debug-${stage} PROPERTIES
        PASS_REGULAR_EXPRESSION "stage       : ${stage}")
endforeach()
add_test(NAME m-n2-nav-debug-usage
         COMMAND nav-tools debug-stage ${NL_ROOT}/assets/models/HizScene.glb
                 ${CMAKE_CURRENT_BINARY_DIR}/nav-n2-bad.png --stage bogus)
set_tests_properties(m-n2-nav-debug-usage PROPERTIES WILL_FAIL TRUE)

# ---- P8 M-N3: DetourTileCache tiled bake + .nnav v2 + dynamic obstacles ----
# 方案 A（tiled 增量重烘）+ A-1（同一容器 v2，v1 兼容）；A 类轨道不进渲染回归。
add_test(NAME m-n3-nav-selftest COMMAND nav-tools selftest --verbose)
set_tests_properties(m-n3-nav-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "nav selftest: PASS")
add_test(NAME m-n3-nav-tilebake
         COMMAND nav-tools tilebake ${NL_ROOT}/assets/models/HizScene.glb
                 ${CMAKE_CURRENT_BINARY_DIR}/nav-n3-hiz.nnav
                 --tile-size 32 --cell-size 0.1 --agent-radius 0.2)
set_tests_properties(m-n3-nav-tilebake PROPERTIES
    PASS_REGULAR_EXPRESSION "version     : 2")
add_test(NAME m-n3-nav-dump-v2
         COMMAND nav-tools dump ${CMAKE_CURRENT_BINARY_DIR}/nav-n3-hiz.nnav)
set_tests_properties(m-n3-nav-dump-v2 PROPERTIES
    DEPENDS m-n3-nav-tilebake
    PASS_REGULAR_EXPRESSION "polys       : [1-9]")
add_test(NAME m-n3-nav-obstacle
         COMMAND nav-tools obstacle ${CMAKE_CURRENT_BINARY_DIR}/nav-n3-hiz.nnav
                 --pos -0.35,-1,-1.70 --radius 0.5 --height 3
                 --start -1.9,0,-2.1 --end -1.0,0,-1.0)
set_tests_properties(m-n3-nav-obstacle PROPERTIES
    DEPENDS m-n3-nav-tilebake
    PASS_REGULAR_EXPRESSION "result      : PASS")
# E1 场景：专门造的含可走面场景（3 Model entity，含 parent 层级复合）
add_test(NAME m-n3-nav-scene
         COMMAND nav-tools tilebake ${NL_ROOT}/example/nav-n3/nav-n3.scene
                 ${CMAKE_CURRENT_BINARY_DIR}/nav-n3-scene.nnav
                 --tile-size 32 --cell-size 0.1 --agent-radius 0.2)
set_tests_properties(m-n3-nav-scene PROPERTIES
    PASS_REGULAR_EXPRESSION "entities    : 3")
# 50 物体规模锚：现成 E1 scale 场景（曲面模型无可走面，tiles=0 属几何语义）
add_test(NAME m-n3-nav-scene-50
         COMMAND nav-tools tilebake ${NL_ROOT}/example/ecs-e1b/ecs-e1b-scale.scene
                 ${CMAKE_CURRENT_BINARY_DIR}/nav-n3-scale.nnav --tile-size 64)
set_tests_properties(m-n3-nav-scene-50 PROPERTIES
    PASS_REGULAR_EXPRESSION "entities    : 50")
add_test(NAME m-n3-nav-tilebake-usage
         COMMAND nav-tools tilebake ${NL_ROOT}/assets/models/HizScene.glb
                 ${CMAKE_CURRENT_BINARY_DIR}/nav-n3-bad.nnav --tile-size 1)
set_tests_properties(m-n3-nav-tilebake-usage PROPERTIES WILL_FAIL TRUE)
add_test(NAME m-n3-nav-obstacle-usage
         COMMAND nav-tools obstacle ${CMAKE_CURRENT_BINARY_DIR}/nav-n3-hiz.nnav)
set_tests_properties(m-n3-nav-obstacle-usage PROPERTIES
    DEPENDS m-n3-nav-tilebake
    WILL_FAIL TRUE)
# v1 solo 容器必须被 obstacle 明确拒绝（不静默当 tiled 处理）
add_test(NAME m-n3-nav-obstacle-v1
         COMMAND nav-tools obstacle ${CMAKE_CURRENT_BINARY_DIR}/nav-n1-hiz.nnav
                 --pos -0.35,-1,-1.70 --start -1.9,0,-2.1 --end -1.0,0,-1.0)
set_tests_properties(m-n3-nav-obstacle-v1 PROPERTIES
    DEPENDS m-n1-nav-bake
    WILL_FAIL TRUE)

# ---- P8 M-N4: DetourCrowd fixed-step multi-agent trace ----
# 开工闸拍板 Q1=A（工具版）/ Q2=A-1（.nnav 不升 v3）/ Q3=A（固定 dt + 帧数 + 顺序）。
add_test(NAME m-n4-nav-selftest COMMAND nav-tools selftest --verbose)
set_tests_properties(m-n4-nav-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "nav selftest: PASS")
add_test(NAME m-n4-nav-crowd-arena
         COMMAND nav-tools crowd --arena --frames 600
                 --trace ${CMAKE_CURRENT_BINARY_DIR}/nav-n4-arena.ntraj
                 --png ${CMAKE_CURRENT_BINARY_DIR}/nav-n4-arena.png --size 768)
set_tests_properties(m-n4-nav-crowd-arena PROPERTIES
    PASS_REGULAR_EXPRESSION "result       : PASS")
# A-1：v1 solo 与 v2 tiled 容器都能被 Crowd 直接消费（.nnav 格式一字不改）
add_test(NAME m-n4-nav-crowd-v1
         COMMAND nav-tools crowd ${CMAKE_CURRENT_BINARY_DIR}/nav-n1-hiz.nnav
                 --spawn -1.9,0.2,-2.1:-1.0,0.2,-1.0 --frames 60)
set_tests_properties(m-n4-nav-crowd-v1 PROPERTIES
    DEPENDS m-n1-nav-bake
    PASS_REGULAR_EXPRESSION "mode         : solo \\(v1\\)")
add_test(NAME m-n4-nav-crowd-v2
         COMMAND nav-tools crowd ${CMAKE_CURRENT_BINARY_DIR}/nav-n3-hiz.nnav
                 --spawn -1.9,0.2,-2.1:-1.0,0.2,-1.0 --frames 60)
set_tests_properties(m-n4-nav-crowd-v2 PROPERTIES
    DEPENDS m-n3-nav-tilebake
    PASS_REGULAR_EXPRESSION "mode         : tiled \\(v2\\)")
# 负向：缺参数 / 零步长 / 网格外 spawn / 非 .nnav 输入，全部明确拒绝
add_test(NAME m-n4-nav-crowd-usage COMMAND nav-tools crowd)
set_tests_properties(m-n4-nav-crowd-usage PROPERTIES WILL_FAIL TRUE)
add_test(NAME m-n4-nav-crowd-dt
         COMMAND nav-tools crowd --arena --dt 0)
set_tests_properties(m-n4-nav-crowd-dt PROPERTIES WILL_FAIL TRUE)
add_test(NAME m-n4-nav-crowd-offmesh
         COMMAND nav-tools crowd --arena --spawn 100,0,100:-8,0.2,-8 --frames 30)
set_tests_properties(m-n4-nav-crowd-offmesh PROPERTIES WILL_FAIL TRUE)
add_test(NAME m-n4-nav-crowd-bad-nav
         COMMAND nav-tools crowd ${NL_ROOT}/assets/models/Suzanne.glb
                 --spawn -8,0.2,-8:8,0.2,8 --frames 30)
set_tests_properties(m-n4-nav-crowd-bad-nav PROPERTIES WILL_FAIL TRUE)

# ---- P8 M-N5: off-mesh connection (bake-time injection + .nnav v3 + jump flags) ----
# 开工闸拍板 Q5-1..Q5-5：烘焙期注入 / v3 描述段（v1-v2 零回归）/ 只做 Solo /
# DT_STRAIGHTPATH_OFFMESH_CONNECTION 透出 + excludeOffMesh / 连通性反转 + 确定性。
add_test(NAME m-n5-nav-selftest COMMAND nav-tools selftest --verbose)
set_tests_properties(m-n5-nav-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "nav selftest: PASS")
add_test(NAME m-n5-nav-offmesh
         COMMAND nav-tools offmesh
                 --nav ${CMAKE_CURRENT_BINARY_DIR}/nav-n5-main.nnav
                 --png ${CMAKE_CURRENT_BINARY_DIR}/nav-n5-main.png --size 640)
set_tests_properties(m-n5-nav-offmesh PROPERTIES
    PASS_REGULAR_EXPRESSION "result      : PASS")
add_test(NAME m-n5-nav-offmesh-no-link
         COMMAND nav-tools offmesh --no-link
                 --nav ${CMAKE_CURRENT_BINARY_DIR}/nav-n5-nolink.nnav)
set_tests_properties(m-n5-nav-offmesh-no-link PROPERTIES
    PASS_REGULAR_EXPRESSION "status      : UNREACHABLE")
# UE NavLinkFlag 口径：excludeOffMesh 精确排除链接 poly
add_test(NAME m-n5-nav-offmesh-exclude
         COMMAND nav-tools offmesh --exclude
                 --nav ${CMAKE_CURRENT_BINARY_DIR}/nav-n5-exclude.nnav)
set_tests_properties(m-n5-nav-offmesh-exclude PROPERTIES
    PASS_REGULAR_EXPRESSION "status      : UNREACHABLE")
add_test(NAME m-n5-nav-offmesh-one-way
         COMMAND nav-tools offmesh --one-way
                 --nav ${CMAKE_CURRENT_BINARY_DIR}/nav-n5-oneway.nnav)
set_tests_properties(m-n5-nav-offmesh-one-way PROPERTIES
    PASS_REGULAR_EXPRESSION "status      : OK")
add_test(NAME m-n5-nav-offmesh-one-way-reverse
         COMMAND nav-tools offmesh --one-way --reverse
                 --nav ${CMAKE_CURRENT_BINARY_DIR}/nav-n5-reverse.nnav)
set_tests_properties(m-n5-nav-offmesh-one-way-reverse PROPERTIES
    PASS_REGULAR_EXPRESSION "status      : UNREACHABLE")
# 负向：非法半径 / bbox 外端点（封装层显式拒绝，不静默吞）/ 用法错误
add_test(NAME m-n5-nav-offmesh-radius
         COMMAND nav-tools offmesh --radius 0
                 --nav ${CMAKE_CURRENT_BINARY_DIR}/nav-n5-radius.nnav)
set_tests_properties(m-n5-nav-offmesh-radius PROPERTIES WILL_FAIL TRUE)
add_test(NAME m-n5-nav-offmesh-bbox
         COMMAND nav-tools offmesh --link "1,0,0 200,0,0"
                 --nav ${CMAKE_CURRENT_BINARY_DIR}/nav-n5-bbox.nnav)
set_tests_properties(m-n5-nav-offmesh-bbox PROPERTIES WILL_FAIL TRUE)
add_test(NAME m-n5-nav-offmesh-usage COMMAND nav-tools offmesh --bogus)
set_tests_properties(m-n5-nav-offmesh-usage PROPERTIES WILL_FAIL TRUE)
