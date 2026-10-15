#pragma once

// Sound out of cellAudio on Apple platforms (macOS and iOS): the stereo mix cellAudio hands its sink every period
// (pspocket/hle/audio.hpp: 256 frames at 48 kHz) goes through an AudioRing to an AVAudioEngine source node, then a
// time-pitch unit (bypassed unless cellAudio stretches, D-075), to the default output. Objective-C++ inside
// (src/audio_output.mm); this header is plain C++.

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

    // Starts the engine again where the system's sound now goes (D-997). The engine starts itself again after its
    // configuration changed (AVAudioEngineConfigurationChangeNotification: another output, such as AirPlay screen
    // mirroring starting or ending); the platform calls this after what the engine is not told of (on iOS: an
    // interruption ended, the route changed, the app came back, `rebuild` after the media services were reset: the
    // engine and its nodes made anew). Does nothing while it runs, unless `rebuild`. Any thread.
    void restart(const char* why, bool rebuild = false);
    bool running() const;

    // One period from cellAudio's sink: `frames` interleaved stereo float frames. Never blocks.
    void push(const f32* samples, u32 frames);

    // For cellAudio's buffer query (pspocket/hle/audio.hpp): the frames waiting for the device, and the most frames the
    // device has taken at once (0 before it played).
    u32 queued() const;
    u32 device_frames() const;

    // For cellAudio's stretcher (hle::AudioStretch): on, the ring plays at `tempo` times real time with its pitch kept
    // (AVAudioUnitTimePitch); off, unprocessed. Any thread.
    void set_stretch(bool on, f32 tempo);

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
