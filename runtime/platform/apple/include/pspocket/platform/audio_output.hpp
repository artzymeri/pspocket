#pragma once

// Sound out of cellAudio on Apple platforms (macOS and iOS): the stereo mix cellAudio hands its sink every period
// (pspocket/hle/audio.hpp: 256 frames at 48 kHz) goes through an AudioRing to an AVAudioEngine source node on the
// default output. Objective-C++ inside (src/audio_output.mm); this header is plain C++.

#include <memory>

#include "pspocket/core/types.hpp"

namespace pspocket::platform {

class AudioOutput {
public:
    // Starts the engine on the default output device. On iOS the caller sets up the audio session first.
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // False when the engine couldn't start (no output device): push() then drops everything.
    bool valid() const;

    // One period from cellAudio's sink: `frames` interleaved stereo float frames. Never blocks.
    void push(const f32* samples, u32 frames);

    struct Stats {
        u64 played = 0;    // frames the device took from the ring
        u64 underruns = 0; // callbacks the ring couldn't fill (silence played)
        u64 overruns = 0;  // periods dropped because the ring was full
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pspocket::platform
