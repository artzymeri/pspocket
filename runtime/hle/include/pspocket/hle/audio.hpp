#pragma once

// The host's audio output for cellAudio (runtime/hle/src/cellAudio.cpp). The iOS app will attach CoreAudio here.
// Until something is attached (pspocket-boot on the Mac), the audio is discarded.
//
// cellAudio's thread runs one period every 256 samples at 48 kHz (5.33 ms), from cellAudioInit to cellAudioQuit. Each
// period it mixes the block that every started port is playing into one stereo block and hands it to the sink. Then
// it advances the ports' read indices and notifies the game. Without a sink nothing is mixed. The game sees the same
// timing either way.

#include <functional>

#include "pspocket/core/types.hpp"

namespace pspocket::hle {

class HleProcess;

inline constexpr u32 kAudioSampleRate = 48000; // Hz
inline constexpr u32 kAudioBlockFrames = 256;  // frames per period (CELL_AUDIO_BLOCK_SAMPLES)
inline constexpr u32 kAudioOutChannels = 2;    // the stereo LPCM output cellSysutil reports (D-048)

// One period of output: `frames` (kAudioBlockFrames) frames of kAudioOutChannels interleaved float samples (left,
// right), full scale 1.0, not clipped. Silence while no port is started. Called on cellAudio's thread once per period;
// the samples are only valid during the call. It must return quickly (copy into the host's buffer).
using AudioSink = std::function<void(const f32* samples, u32 frames)>;

// Attaches the host's audio output to the process's cellAudio, replacing the previous sink; an empty function detaches
// it. Thread-safe. Once it returns, the previous sink is no longer called. A sink must not call it.
void set_audio_sink(HleProcess& process, AudioSink sink);

} // namespace pspocket::hle
