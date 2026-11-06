#pragma once

// 断言框架第一天就位（Carrot 46799759 里验证层 debugCallback 是 Init 的一部分）。
// handler 可被 --selftest 接管，用来验证「失败会被抓到」而不真的终止进程。

namespace nl::core {

using AssertHandler = void (*)(const char* file, int line, const char* expr, const char* message);

// 传 nullptr 恢复默认行为（打印后 std::abort）
void setAssertHandler(AssertHandler handler);
void onAssertFailed(const char* file, int line, const char* expr, const char* message);

} // namespace nl::core

#define NL_CHECK(expr) \
    do { if (!(expr)) ::nl::core::onAssertFailed(__FILE__, __LINE__, #expr, nullptr); } while (0)

#define NL_CHECK_MSG(expr, msg) \
    do { if (!(expr)) ::nl::core::onAssertFailed(__FILE__, __LINE__, #expr, (msg)); } while (0)

#define NL_FAIL(msg) ::nl::core::onAssertFailed(__FILE__, __LINE__, "NL_FAIL", (msg))
