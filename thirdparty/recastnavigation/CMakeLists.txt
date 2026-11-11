# nl-engine vendored Recast/Detour/DebugUtils —— 自写薄 CMakeLists。
# 不直接 add_subdirectory 上游模块脚本：上游 CMakeLists 带 SOVERSION/LIB_VERSION（依赖仓库根变量）、
# install() 导出，且 DebugUtils 链接 DetourTileCache（M-N3 已入库，但仍维持薄脚本避免引入上游 install/导出链）。
# 源文件列表显式枚举（防 file(GLOB) 静默漏/多），与 jolt「新 checkout 必验 configure」教训配套。
# 入库子集 / 版本锁 / 许可证核查见 thirdparty/recastnavigation/VENDOR.md。

add_library(nl_recast STATIC
    Recast/Source/Recast.cpp
    Recast/Source/RecastAlloc.cpp
    Recast/Source/RecastArea.cpp
    Recast/Source/RecastAssert.cpp
    Recast/Source/RecastContour.cpp
    Recast/Source/RecastFilter.cpp
    Recast/Source/RecastLayers.cpp
    Recast/Source/RecastMesh.cpp
    Recast/Source/RecastMeshDetail.cpp
    Recast/Source/RecastRasterization.cpp
    Recast/Source/RecastRegion.cpp
    Detour/Source/DetourAlloc.cpp
    Detour/Source/DetourAssert.cpp
    Detour/Source/DetourCommon.cpp
    Detour/Source/DetourNavMesh.cpp
    Detour/Source/DetourNavMeshBuilder.cpp
    Detour/Source/DetourNavMeshQuery.cpp
    Detour/Source/DetourNode.cpp
    # M-N3：DetourTileCache（同一 v1.6.0 锁定 commit，零编辑，ZLib）。
    DetourTileCache/Source/DetourTileCache.cpp
    DetourTileCache/Source/DetourTileCacheBuilder.cpp
    DebugUtils/Source/DebugDraw.cpp
    # DetourDebugDraw.cpp 上游 1.6.0 头依赖 DetourTileCacheBuilder.h
    #（M-N0 剔除留痕；M-N3 TileCache 已入库，重评结论仍不启用——M-N2 自绘 PNG 基线锁定）。
    # M-N2 NavMesh 叠加自绘（nl/nav polyEdges 已具备数据面）。
    DebugUtils/Source/RecastDebugDraw.cpp
    DebugUtils/Source/RecastDump.cpp
)

target_include_directories(nl_recast PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Recast/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/Detour/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/DetourTileCache/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/DebugUtils/Include
)
