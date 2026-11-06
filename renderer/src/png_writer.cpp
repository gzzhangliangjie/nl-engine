// 最小 PNG 写出：本地 vendored 的 stb_image_write.h，零网络、零额外依赖。
// 后续 M0.3~M0.5 的验收截图全部走这一个出口，不再依赖人工截屏。
#include "backend.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace nl::renderer::detail {

bool writePngRGBA8(const char* path, const void* pixels,
                   std::uint32_t width, std::uint32_t height,
                   std::uint32_t rowPitch) {
    if (!path || !pixels || width == 0 || height == 0) return false;
    const int stride = rowPitch ? static_cast<int>(rowPitch)
                                : static_cast<int>(width * 4u);
    return stbi_write_png(path, static_cast<int>(width), static_cast<int>(height),
                          4, pixels, stride) != 0;
}

} // namespace nl::renderer::detail
