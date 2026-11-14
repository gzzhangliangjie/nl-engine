# M-SC4 生成物新鲜度闸（纯 CMake，零 Python 依赖）。
# 用法：cmake -DSRC=<exports.api> -DGEN1=<生成物1> ... -P check_freshness.cmake
# 规则：SRC 不存在或任一生成物缺失/比 SRC 旧 → FATAL_ERROR（构建红，强制重跑
# python tools/scriptapi/gen.py）。生成物入 git、构建不依赖 Python；本闸只保证
# 不悄悄使用过期生成物。

if(NOT DEFINED SRC)
    message(FATAL_ERROR "check_freshness: SRC (exports.api) not defined")
endif()
if(NOT EXISTS "${SRC}")
    message(FATAL_ERROR "check_freshness: missing ${SRC}")
endif()

set(_gens "")
set(_i 1)
while(DEFINED GEN${_i})
    list(APPEND _gens "${GEN${_i}}")
    math(EXPR _i "${_i} + 1")
endwhile()
if(NOT _gens)
    message(FATAL_ERROR "check_freshness: no GEN<N> given")
endif()

foreach(g IN LISTS _gens)
    if(NOT EXISTS "${g}")
        message(FATAL_ERROR "scriptapi freshness: missing generated file ${g}\n"
            "  -> run: python tools/scriptapi/gen.py")
    endif()
    if("${SRC}" IS_NEWER_THAN "${g}")
        message(FATAL_ERROR "scriptapi freshness: ${SRC} newer than ${g}\n"
            "  -> run: python tools/scriptapi/gen.py (then rebuild)")
    endif()
endforeach()
