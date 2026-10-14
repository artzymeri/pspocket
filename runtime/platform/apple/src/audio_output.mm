// AudioOutput (pspocket/platform/audio_output.hpp): an AVAudioEngine whose source node pulls from an AudioRing that
// cellAudio's thread fills, through a time-pitch unit that is bypassed unless cellAudio stretches (D-075). 48 kHz
// stereo float, non-interleaved on the engine side; the main mixer converts to the device's format. Objective-C++ with
// ARC.

#import <AVFoundation/AVFoundation.h>

#include "pspocket/platform/audio_output.hpp"

#include <algorithm>
#include <cstdlib>

#include "pspocket/core/audio_ring.hpp"
#include "pspocket/core/log.hpp"

namespace pspocket::platform {
namespace {

constexpr std::string_view kChan = "audio";
constexpr double kSampleRate = 48000.0; // cellAudio's rate (hle::kAudioSampleRate)
constexpr u32 kRingFrames = 16384;      // about 340 ms
constexpr u32 kPrebufferFrames = 2048;  // about 43 ms before playing, and again after running dry

} // namespace

struct AudioOutput::Impl {
    AudioRing ring{kRingFrames, kPrebufferFrames};
    AVAudioEngine* engine = nil;
    AVAudioSourceNode* source = nil;
    AVAudioUnitTimePitch* pitch = nil; // cellAudio's stretcher
    bool valid = false;
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>())
{
    Impl* impl = impl_.get();
    AVAudioFormat* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:kSampleRate channels:2];
    impl->engine = [AVAudioEngine new];
    impl->source = [[AVAudioSourceNode alloc] initWithFormat:format
        renderBlock:^OSStatus(BOOL* silence, const AudioTimeStamp*, AVAudioFrameCount frames, AudioBufferList* out) {
            if (out->mNumberBuffers < 2) return noErr;
            auto* left = static_cast<f32*>(out->mBuffers[0].mData);
            auto* right = static_cast<f32*>(out->mBuffers[1].mData);
            *silence = impl->ring.pull(left, right, frames) == 0;
            return noErr;
        }];
    impl->pitch = [AVAudioUnitTimePitch new];
    impl->pitch.bypass = YES;
    [impl->engine attachNode:impl->source];
    [impl->engine attachNode:impl->pitch];
    [impl->engine connect:impl->source to:impl->pitch format:format];
    [impl->engine connect:impl->pitch to:impl->engine.mainMixerNode format:format];
    // PSPOCKET_AUDIO_VOLUME=0..1 (D-976): the device still pulls the sound at its pace (cellAudio's "sound:" lines
    // measure the game's lateness against it) at that volume: 0 measures a walk without playing it aloud.
    if (const char* v = std::getenv("PSPOCKET_AUDIO_VOLUME"); v && *v) {
        impl->engine.mainMixerNode.outputVolume = std::clamp(static_cast<f32>(std::atof(v)), 0.0f, 1.0f);
        PS_LOG_INFO(kChan, "sound out at volume {}", impl->engine.mainMixerNode.outputVolume);
    }
    NSError* error = nil;
    if (![impl->engine startAndReturnError:&error]) {
        PS_LOG_ERROR(kChan, "the audio engine didn't start: {}",
            error ? error.localizedDescription.UTF8String : "no reason given");
        return;
    }
    impl->valid = true;
    PS_LOG_INFO(kChan, "sound out: {} Hz stereo through AVAudioEngine (device {} Hz)", kSampleRate,
        [impl->engine.outputNode outputFormatForBus:0].sampleRate);
}

AudioOutput::~AudioOutput()
{
    if (impl_->engine) [impl_->engine stop];
}

bool AudioOutput::valid() const { return impl_->valid; }

void AudioOutput::push(const f32* samples, u32 frames)
{
    if (impl_->valid) impl_->ring.push(samples, frames);
}

u32 AudioOutput::queued() const { return impl_->ring.queued(); }

u32 AudioOutput::device_frames() const { return impl_->ring.largest_pull(); }

void AudioOutput::set_stretch(bool on, f32 tempo)
{
    if (!impl_->valid) return;
    impl_->pitch.rate = on ? tempo : 1.0f;
    impl_->pitch.bypass = on ? NO : YES;
}

AudioOutput::Stats AudioOutput::stats() const
{
    return {impl_->ring.played(), impl_->ring.underruns(), impl_->ring.overruns()};
}

} // namespace pspocket::platform
