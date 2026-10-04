#pragma once

// The host's audio output for cellAudio (runtime/hle/src/cellAudio.cpp): platform::AudioOutput on the Mac
// (pspocket-boot --audio) and the iPhone. Until something is attached, the audio is discarded.
//
// cellAudio's thread plays one period of 256 samples at 48 kHz (5.33 ms) at a time, from cellAudioInit to
// cellAudioQuit. Each period it mixes the block that every started port is playing into one stereo block and hands it
// to the sink. Then it advances the ports' read indices and notifies the game. How it times the periods depends on the
// sink (D-074):
// - A sink with a buffer query (what the host holds between cellAudio and the device): RPCS3's buffering mode. The
//   periods keep the host's buffer at its target (34 ms, or more for a device that takes more at once), and each one
//   waits briefly for the game to finish writing its blocks, so a game that runs a little late still plays whole
//   blocks.
// - No sink, or one without a query (a capture): RPCS3 without buffering, a fixed clock of exactly 256 / 48000 s.
//   Without a sink nothing is mixed.

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

// What the host's buffer holds for the device.
struct AudioBufferLevel {
    u32 queued = 0;        // frames pushed that the device hasn't played yet
    u32 device_frames = 0; // the most frames the device has taken at once (its callback length); 0 while unknown
};

// Called on cellAudio's thread, often (up to every half millisecond while it waits for the game): it must be quick.
using AudioBufferQuery = std::function<AudioBufferLevel()>;

// Attaches the host's audio output to the process's cellAudio, replacing the previous one; an empty sink detaches it
// (and its query). Thread-safe. Once it returns, the previous sink and query are no longer called. A sink or query
// must not call it.
void set_audio_sink(HleProcess& process, AudioSink sink, AudioBufferQuery query = {});

} // namespace pspocket::hle
