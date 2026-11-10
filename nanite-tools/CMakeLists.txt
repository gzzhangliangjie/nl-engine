# ---- M1.0：nanite-tools —— S1 离线网格工具（cluster / simplify / dag / stat） ----
# 只链 nl_core（日志 + mesh_io），零图形 API；不依赖 renderer / viewer。
# M2.1：另链 meshoptimizer —— 仅 simplify 子命令的对照 oracle（打印对照行），
# core 自身不依赖 meshopt；viewer 不链它。
# 2026-11-10 自根 CMakeLists.txt 拆出（target + ctest 用例一块搬）。
add_executable(nanite-tools main.cpp)
target_link_libraries(nanite-tools PRIVATE nl_core meshoptimizer)

if(MSVC)
    target_compile_options(nanite-tools PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(nanite-tools PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# M1.0：nanite-tools 对官方 Suzanne 资产的机械验收（顶点 / 三角 / 子网格数）
add_test(NAME s1-tools-stat
         COMMAND nanite-tools stat ${NL_ROOT}/assets/models/Suzanne.glb)
set_tests_properties(s1-tools-stat PROPERTIES
    PASS_REGULAR_EXPRESSION "triangles : 3936")
# M1.1：切簇算法机械单测（覆盖率 / 128 tri 与 64 vert 上限 / bounds 合理性 / 负向输入）
add_test(NAME s1-cluster-selftest COMMAND nanite-tools selftest cluster)
set_tests_properties(s1-cluster-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "cluster selftest: PASS")
# M1.1：真实资产切簇验收（覆盖率 100% + limits PASS）
add_test(NAME s1-cluster-suzanne
         COMMAND nanite-tools cluster ${NL_ROOT}/assets/models/Suzanne.glb)
set_tests_properties(s1-cluster-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "limits     : PASS")
# M1.2：.nmesh roundtrip 单测（字段一致 + 逐字节重写一致 + 坏 magic / 截断拒绝）
add_test(NAME s1-nmesh-roundtrip COMMAND nanite-tools selftest nmesh)
set_tests_properties(s1-nmesh-roundtrip PROPERTIES
    PASS_REGULAR_EXPRESSION "nmesh selftest: PASS")
# M1.2：真实资产写盘 → stat 读回（-cluster -o 与 .nmesh stat 的端到端验收）
add_test(NAME s1-nmesh-suzanne
         COMMAND nanite-tools cluster ${NL_ROOT}/assets/models/Suzanne.glb
                 -o ${CMAKE_CURRENT_BINARY_DIR}/Suzanne.nmesh)
set_tests_properties(s1-nmesh-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "saved      :")
# M2.0：聚组算法机械单测（覆盖连续 / 组号连续 / 组内连通交叉验证 / 断连块不跨组 / 负向）
add_test(NAME s2-group-selftest COMMAND nanite-tools selftest group)
set_tests_properties(s2-group-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "group selftest: PASS")
# M2.0：真实资产聚组验收（Suzanne.nmesh 由 s1-nmesh-suzanne 先写出；依赖同目录测试顺序）
add_test(NAME s2-group-suzanne
         COMMAND nanite-tools simplify ${CMAKE_CURRENT_BINARY_DIR}/Suzanne.nmesh)
set_tests_properties(s2-group-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "connectivity: PASS"
    DEPENDS s1-nmesh-suzanne)
# M2.1：QEM 简化 + 边界锁定机械单测（边界点集合交叉验证 / 边界点逐点位级不变 /
# 接缝边双侧存活 / 简化率 / 确定性重跑 / 负向拒绝）
add_test(NAME s2-qem-selftest COMMAND nanite-tools selftest qem)
set_tests_properties(s2-qem-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "qem selftest: PASS")
# M2.1：真实资产 QEM 简化验收（Suzanne 3936 tri → ~50%，边界锁定 + meshopt oracle 对照）
add_test(NAME s2-qem-suzanne
         COMMAND nanite-tools simplify ${CMAKE_CURRENT_BINARY_DIR}/Suzanne.nmesh -v)
set_tests_properties(s2-qem-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "oracle      : meshopt LockBorder"
    DEPENDS s1-nmesh-suzanne)
# M2.2：逐层 LOD 循环机械单测（每层边界独立重算 / 约减半 / parent error 单调 / 簇上限 / v1-v2 资产兼容 / 确定性）
add_test(NAME s2-loop-selftest COMMAND nanite-tools selftest loop)
set_tests_properties(s2-loop-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "loop selftest: PASS")
# M2.2：真实资产逐层落盘（L0..LN，每层 .nmesh v2 带 lockedPositions）
add_test(NAME s2-loop-suzanne
         COMMAND nanite-tools lod ${CMAKE_CURRENT_BINARY_DIR}/Suzanne.nmesh
                 -o ${CMAKE_CURRENT_BINARY_DIR}/lod-suzanne --max-levels 6)
set_tests_properties(s2-loop-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "lod files   : "
    DEPENDS s1-nmesh-suzanne)
# M3.0：DAG 血缘机械单测（全链不变量 / 顶层 terminal / 按组重切产出 / v3 roundtrip / 篡改检测）
add_test(NAME s3-dag-selftest COMMAND nanite-tools selftest dag)
set_tests_properties(s3-dag-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "dag selftest: PASS")
# M3.0：真实资产 DAG 装配 + 校验（每父簇恰一生成组 / error 链逐位精确 / 无环）
add_test(NAME s3-dag-suzanne
         COMMAND nanite-tools dag ${CMAKE_CURRENT_BINARY_DIR}/lod-suzanne)
set_tests_properties(s3-dag-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "dag         : OK"
    DEPENDS s2-loop-suzanne)

# M3.1：fallback 拼接自检（weld 收敛、三角守恒、边流形 ≤2、v3 roundtrip）
add_test(NAME s3-fallback-selftest COMMAND nanite-tools selftest fallback)
set_tests_properties(s3-fallback-selftest PROPERTIES
    PASS_REGULAR_EXPRESSION "fallback selftest: PASS")

# M3.1：真实资产 fallback 落盘（写进 lod0 尾段，输入文件不动）
add_test(NAME s3-fallback-suzanne
         COMMAND nanite-tools lod ${CMAKE_CURRENT_BINARY_DIR}/Suzanne.nmesh
                 -o ${CMAKE_CURRENT_BINARY_DIR}/fb-suzanne --max-levels 6)
set_tests_properties(s3-fallback-suzanne PROPERTIES
    PASS_REGULAR_EXPRESSION "fallback    : "
    DEPENDS s1-nmesh-suzanne)
