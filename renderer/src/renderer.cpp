#include "nl/renderer/renderer.hpp"

namespace nl::renderer {

Status status() {
    return Status{
        "none (no graphics API loaded)",
        "M0.1 skeleton",
        false,
    };
}

} // namespace nl::renderer
