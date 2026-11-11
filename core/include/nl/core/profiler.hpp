#pragma once
// M-TR0：profiling 薄封装。业务代码只认 NL_PROFILE_* 宏，不直接暴露 tracy:: 类型。
//
// NL_TRACY=OFF（默认）：三个宏全部空转，连第三方头都不 include —— 零编译成本、
// 零运行时成本、零链接依赖（构建树里 tracy_client target 也不存在）。
// NL_TRACY=ON：透传 Tracy v0.15.0 client 宏；TRACY_ENABLE 由 tracy_client target
// PUBLIC 传播，保证所有 TU 看到同一套宏面（VENDOR.md「0.15 实测要点」）。
#if defined(NL_TRACY)
#  if defined(_MSC_VER)
#    pragma warning(push, 0)   // nl_core / nl_renderer 是 /W4，第三方头别污染警告面
#  endif
#  include <tracy/Tracy.hpp>
#  if defined(_MSC_VER)
#    pragma warning(pop)
#  endif

// 帧边界：调用点固定在 viewer 主循环 present 完成处，与 RenderDoc 的 Present 帧
// 边界对齐（P8-TRACY-PLAN §2）。
#  define NL_PROFILE_FRAME()            FrameMark
// Zone 名一律 ASCII（沿用 HUD 字符串约定）。同一作用域内不要放两个 zone
// （Tracy 宏内部变量名固定），用 {} 包开。
#  define NL_PROFILE_ZONE(name)         ZoneScopedN(name)
#  define NL_PROFILE_THREAD(name)       tracy::SetThreadName(name)
#else
#  define NL_PROFILE_FRAME()            ((void)0)
#  define NL_PROFILE_ZONE(name)         ((void)0)
#  define NL_PROFILE_THREAD(name)       ((void)0)
#endif
