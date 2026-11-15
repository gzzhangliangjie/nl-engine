#pragma once

// nl/sprite — M-SP0 sprite atlas grid (core POD, ZERO graphics symbols).
//
// Evidence (2026-11-15): bevy_sprite TextureAtlas layout is a uniform grid with
// a padding/minimum-index convention; UE Paper2D's sprite is a single source
// rect. We keep the simplest useful thing: uniform grid + uvRect(index), with
// three addressing modes for out-of-range indices.
//
// Graphics handles (TextureHandle, GPU buffer) live in renderer/ only.

#include <cstdint>

namespace nl::sprite {

struct UvRect {
    float u0 = 0.0f, v0 = 0.0f;
    float u1 = 1.0f, v1 = 1.0f;
};

enum class Addressing : std::uint32_t {
    Throw  = 0,  // invalid index -> ok=false, rect left untouched by caller
    Repeat = 1,  // modulo cellCount
    Clamp  = 2,  // clamp to [0, cellCount-1]
};

struct AtlasGrid {
    std::uint32_t dimX = 1;
    std::uint32_t dimY = 1;
    Addressing     addressing = Addressing::Throw;

    std::uint32_t cellCount() const { return dimX * dimY; }

    // Returns false only for Throw + invalid index (or dimX/dimY == 0).
    // rect is left untouched on failure.
    bool uvRect(std::uint32_t index, UvRect* rect) const;
};

inline bool AtlasGrid::uvRect(std::uint32_t index, UvRect* rect) const {
    if (!rect || dimX == 0u || dimY == 0u) return false;
    const std::uint32_t count = dimX * dimY;
    std::uint32_t i = index;
    if (i >= count) {
        switch (addressing) {
        case Addressing::Repeat: i %= count; break;
        case Addressing::Clamp:  i = count - 1u; break;
        case Addressing::Throw:
        default: return false;
        }
    }
    const std::uint32_t cx = i % dimX;
    const std::uint32_t cy = i / dimX;
    const float dx = 1.0f / static_cast<float>(dimX);
    const float dy = 1.0f / static_cast<float>(dimY);
    rect->u0 = static_cast<float>(cx) * dx;
    rect->v0 = static_cast<float>(cy) * dy;
    rect->u1 = rect->u0 + dx;
    rect->v1 = rect->v0 + dy;
    return true;
}

} // namespace nl::sprite
