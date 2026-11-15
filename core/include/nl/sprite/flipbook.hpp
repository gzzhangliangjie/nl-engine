#pragma once

// nl/sprite — M-SP0 flipbook player (core POD, ZERO graphics symbols).
//
// Semantics mirror UE 5.8.4 Paper2D (read only, Plugins/2D/Paper2D):
//   FrameRun = one frame repeated N times on a timeline;
//   Clip     = ordered runs played at `fps`, with loop + reverse + playRate;
//   Player   = Play/Pause/Stop + tick(dt) + OnFinished latch.
// bevy has no flipbook; RmlUi RCSS transitions cover property animation only,
// not frame swapping -- that is exactly why this module belongs in core.

#include <cstdint>
#include <vector>

namespace nl::sprite {

struct FrameRun {
    std::uint32_t frame = 0;
    std::uint32_t repeat = 1;   // 0 is normalized to 1
};

struct Clip {
    float fps = 12.0f;          // must be > 0
    std::vector<FrameRun> runs;

    std::uint32_t frameCount() const;   // total timeline frames (repeats counted)
    // Sample the timeline at absolute frame position `f` (no loop applied).
    // Returns false on an empty/invalid clip.
    bool sample(float f, std::uint32_t* outFrame) const;
};

class FlipbookPlayer {
public:
    enum State : std::uint32_t { Stopped = 0, Playing = 1, Paused = 2 };

    void setClip(const Clip* clip);       // resets time/state, does not own clip
    void play();
    void pause();
    void stop();                          // time = 0, finished cleared
    void tick(float dtSeconds);           // advances by dt * playRate * fps

    void  setLoop(bool loop)      { loop_ = loop; }
    void  setReverse(bool rev)    { reverse_ = rev; }
    void  setPlayRate(float rate) { playRate_ = rate > 0.0f ? rate : 1.0f; }

    bool  loop() const     { return loop_; }
    bool  reverse() const  { return reverse_; }
    float playRate() const { return playRate_; }
    State state() const    { return state_; }
    bool  finished() const { return finished_; }  // latched; clear on play()/stop()
    float timeFrames() const { return time_; }

    // Current atlas/sprite frame index. 0 when no clip or stopped.
    std::uint32_t frame() const;

private:
    const Clip*  clip_     = nullptr;
    State        state_    = Stopped;
    bool         loop_     = false;
    bool         reverse_  = false;
    bool         finished_ = false;
    float        playRate_ = 1.0f;
    float        time_     = 0.0f;   // in timeline frames
};

inline std::uint32_t Clip::frameCount() const {
    std::uint32_t n = 0;
    for (const FrameRun& r : runs) n += (r.repeat == 0u ? 1u : r.repeat);
    return n;
}

inline bool Clip::sample(float f, std::uint32_t* outFrame) const {
    if (!outFrame || fps <= 0.0f || f < 0.0f) return false;
    const std::uint32_t idx = static_cast<std::uint32_t>(f);
    std::uint32_t base = 0;
    for (const FrameRun& r : runs) {
        const std::uint32_t rep = (r.repeat == 0u ? 1u : r.repeat);
        if (idx < base + rep) { *outFrame = r.frame; return true; }
        base += rep;
    }
    return false;  // past the end
}

inline void FlipbookPlayer::setClip(const Clip* clip) {
    clip_ = clip;
    state_ = Stopped;
    time_ = 0.0f;
    finished_ = false;
}

inline void FlipbookPlayer::play() {
    if (!clip_) return;
    if (state_ == Stopped) time_ = 0.0f;
    state_ = Playing;
    finished_ = false;
}

inline void FlipbookPlayer::pause() {
    if (state_ == Playing) state_ = Paused;
}

inline void FlipbookPlayer::stop() {
    state_ = Stopped;
    time_ = 0.0f;
    finished_ = false;
}

inline void FlipbookPlayer::tick(float dtSeconds) {
    if (state_ != Playing || !clip_ || clip_->fps <= 0.0f || dtSeconds <= 0.0f) return;
    const std::uint32_t total = clip_->frameCount();
    if (total == 0u) { finished_ = true; state_ = Stopped; return; }
    time_ += dtSeconds * playRate_ * clip_->fps;
    const float fTotal = static_cast<float>(total);
    if (time_ >= fTotal) {
        if (loop_) {
            // Wrap, keeping the fractional remainder (no drift on long runs).
            while (time_ >= fTotal) time_ -= fTotal;
        } else {
            time_ = fTotal - 1.0e-4f;
            finished_ = true;
            state_ = Stopped;
        }
    }
}

inline std::uint32_t FlipbookPlayer::frame() const {
    if (!clip_ || state_ == Stopped && !finished_) return 0u;
    const std::uint32_t total = clip_->frameCount();
    if (total == 0u) return 0u;
    float f = time_;
    if (f < 0.0f) f = 0.0f;
    if (f >= static_cast<float>(total)) f = static_cast<float>(total - 1u);
    if (reverse_) f = static_cast<float>(total - 1u) - f;
    std::uint32_t out = 0;
    if (!clip_->sample(f, &out)) {
        // Out-of-range after reverse: clamp to last valid timeline slot.
        clip_->sample(static_cast<float>(total - 1u), &out);
    }
    return out;
}

} // namespace nl::sprite
