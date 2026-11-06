#include "nl/core/assert.hpp"
#include "nl/core/log.hpp"

#include <cstdlib>
#include <mutex>

namespace nl::core {
namespace {

std::mutex      g_mutex;
AssertHandler   g_handler = nullptr;

} // namespace

void setAssertHandler(AssertHandler handler) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handler = handler;
}

void onAssertFailed(const char* file, int line, const char* expr, const char* message) {
    AssertHandler handler = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        handler = g_handler;
    }

    if (handler) {
        handler(file, line, expr, message);
        return;
    }

    log::fatal("assert failed at ", file, ":", line, " [", expr, "]",
               message ? std::string(" ") + message : std::string());
    std::abort();
}

} // namespace nl::core
