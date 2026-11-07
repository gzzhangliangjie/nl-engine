# nl-engine vendored meshoptimizer 1.3 — 自写构建（不用上游 CMakeLists：
# 其顶层挂着 gltfpack/demo 的文件列表与选项，本工程只要静态库本体）。
# 仅 nanite-tools 链接（simplify 的对照 oracle）；core/viewer 不依赖。
file(GLOB MESHOPT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(meshoptimizer STATIC ${MESHOPT_SOURCES})
target_include_directories(meshoptimizer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(meshoptimizer PRIVATE NDEBUG)
