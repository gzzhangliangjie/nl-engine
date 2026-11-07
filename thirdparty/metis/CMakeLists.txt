# METIS 5 vendored (nl-engine M2.0)
#
# 用途：nanite-tools 离线管线的簇聚组（S2：节点=簇、边=共享顶点数权重的图分割）。
# 来源（2026-11-07 浅克隆）：
#   METIS: https://github.com/KarypisLab/METIS        (main @ HEAD)
#   GKlib: https://github.com/KarypisLab/GKlib        (main @ HEAD)
# 布局（相对上游重排）：
#   include/       = GKlib include/*.h + METIS include/metis.h（宽度宏已固定 32 位，
#                    取代上游「手动取消注释」流程）
#   src/metis/     = METIS libmetis/（.c + .h 全量，含 gklib.c/gklib_defs.h 等 shim）
#   src/gklib/     = GKlib src/*.c
# 不用上游 CMakeLists（其顶层还挂着悬空的 build/xinclude 子目录与 GKLIB_PATH 外部依赖），
# 这里直接把两个源集编进一个静态库。
#
# Windows 编译定义（照 GKlib 上游 CMakeLists 的检测逻辑固化）：
#   USE_GKREGEX —— Windows 无 <regex.h>，GKlib 内置 gkregex 兜底；Linux/CI 下有
#   NDEBUG      —— 关闭 GKlib 断言（与项目 Release 一致）
# 其他宏（__MSC__、LINUX、HAVE_EXECINFO_H 等）上游按平台自动/检测处理，无需手动。

set(METIS_SOURCES "")
file(GLOB _metis_c ${CMAKE_CURRENT_SOURCE_DIR}/src/metis/*.c)
file(GLOB _gklib_c ${CMAKE_CURRENT_SOURCE_DIR}/src/gklib/*.c)
list(APPEND METIS_SOURCES ${_metis_c} ${_gklib_c})

add_library(nl_metis STATIC ${METIS_SOURCES})
target_include_directories(nl_metis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(nl_metis PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/metis)
if(WIN32)
    target_compile_definitions(nl_metis PRIVATE USE_GKREGEX)
endif()
target_compile_definitions(nl_metis PRIVATE NDEBUG)
# MSVC 无 `__thread` 关键字：上游 GKlib CMake 用 HAVE_TLS 检测注入
# `__thread=__declspec(thread)`，vendored 版直接固化该映射（MSVC 响应文件里
# 带括号宏必须走 compile_options 才能正确转义）。
if(MSVC)
    target_compile_options(nl_metis PRIVATE "/D__thread=__declspec(thread)")
endif()

# 第三方 C 代码不跟项目的 /W4：保持默认告警级别，只关安全函数告警
if(MSVC)
    target_compile_options(nl_metis PRIVATE /W1 /wd4996)
endif()
