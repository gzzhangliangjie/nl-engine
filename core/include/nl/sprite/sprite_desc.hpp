#pragma once

// nl/sprite — M-SP0 sprite instance POD + CPU-side batch binning.
//
// core holds PODs and pure CPU logic ONLY. NVRHI / Slang / texture handles live
// in renderer/. This split is the frozen "A strict split" decision (2026-11-15).
//
// bevy_sprite_render (read only) contributed the three binning ideas: shared
// quad, instance buffer, texture-keyed buckets. We do NOT copy its ECS phase.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "nl/sprite/atlas.hpp"

namespace nl::sprite {

// textureId is an engine-side opaque integer. The renderer resolves it to a
// texture handle / binding set; core never sees a GPU type.
using TextureId = std::uint32_t;
constexpr TextureId kInvalidTextureId = 0xFFFFFFFFu;

struct SpriteDesc {
    float     x = 0.0f, y = 0.0f;      // world-space 2D position
    float     width = 1.0f, height = 1.0f;
    float     rotation = 0.0f;         // radians around the sprite center
    float     originX = 0.5f, originY = 0.5f;
    UvRect    uv{};
    float     color[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // straight RGBA
    TextureId textureId = kInvalidTextureId;
    std::uint32_t layer = 0;           // submission order bucket (lower = first)
    std::uint32_t sequence = 0;        // stable tie-break, preserves submit order
};

struct SpriteBucket {
    TextureId     textureId = kInvalidTextureId;
    std::uint32_t begin = 0;           // index into SpriteBatch::sprites
    std::uint32_t count = 0;
};

class SpriteBatch {
public:
    void clear() { sprites_.clear(); buckets_.clear(); }
    void add(const SpriteDesc& s) {
        SpriteDesc copy = s;
        copy.sequence = static_cast<std::uint32_t>(sprites_.size());
        sprites_.push_back(copy);
    }
    std::size_t size() const { return sprites_.size(); }
    const std::vector<SpriteDesc>& sprites() const { return sprites_; }
    const std::vector<SpriteBucket>& buckets() const { return buckets_; }

    // Stable sort by (layer, textureId, sequence): sprites sharing a texture
    // become contiguous (one draw call), while submission order inside a
    // bucket is preserved -- Brotato needs deterministic occlusion.
    // A texture change inside the same layer starts a new bucket.
    void sortByTexture() {
        std::stable_sort(sprites_.begin(), sprites_.end(),
            [](const SpriteDesc& a, const SpriteDesc& b) {
                if (a.layer != b.layer) return a.layer < b.layer;
                if (a.textureId != b.textureId) return a.textureId < b.textureId;
                return a.sequence < b.sequence;
            });
        buckets_.clear();
        std::uint32_t begin = 0;
        TextureId current = kInvalidTextureId;
        std::uint32_t currentLayer = 0;
        for (std::size_t i = 0; i < sprites_.size(); ++i) {
            const SpriteDesc& s = sprites_[static_cast<std::size_t>(i)];
            const bool split = buckets_.empty() ||
                               s.textureId != current || s.layer != currentLayer ||
                               s.textureId == kInvalidTextureId;
            if (split) {
                if (!buckets_.empty()) {
                    buckets_.back().count = static_cast<std::uint32_t>(i) - begin;
                }
                begin = static_cast<std::uint32_t>(i);
                current = s.textureId;
                currentLayer = s.layer;
                SpriteBucket b;
                b.textureId = s.textureId;
                b.begin = begin;
                b.count = 0;
                buckets_.push_back(b);
            }
        }
        if (!buckets_.empty()) {
            buckets_.back().count =
                static_cast<std::uint32_t>(sprites_.size()) - begin;
        }
    }

private:
    std::vector<SpriteDesc>   sprites_;
    std::vector<SpriteBucket> buckets_;
};

} // namespace nl::sprite
