# nl-engine vendored ENet —— 见 thirdparty/enet/VENDOR.md（M-NET0，2026-11-13）。
# 上游 CMakeLists 的 Windows 口径：ENET_BUILDING_LIB + winmm/ws2_32；unix.c 剔除
#（本项目只跑 Windows/MSVC，与「构建链不依赖厂商 SDK」铁律同向）。
add_library(nl_enet STATIC
    callbacks.c
    compress.c
    host.c
    list.c
    packet.c
    peer.c
    protocol.c
    win32.c
)

target_include_directories(nl_enet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(nl_enet PRIVATE ENET_BUILDING_LIB)

if(WIN32)
    target_link_libraries(nl_enet PUBLIC winmm ws2_32)
endif()
