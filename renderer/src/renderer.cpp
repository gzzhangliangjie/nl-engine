#include "nl/renderer/renderer.hpp"

namespace nl::renderer {
namespace {
#if defined(NL_BACKEND_VULKAN)
constexpr const char* kBackend = "vulkan";
#else
constexpr const char* kBackend = "dx12";
#endif
} // namespace

Status status() {
    return Status{
        kBackend,
        "M0.1 skeleton (no device yet)",
        false,
    };
}

} // namespace nl::renderer
