// AudioOutput (pspocket/platform/audio_output.hpp): an AVAudioEngine whose source node pulls from an AudioRing that
// cellAudio's thread fills. 48 kHz stereo float, non-interleaved on the engine side; the main mixer converts to the
// device's format. Objective-C++ with ARC.

#import <AVFoundation/AVFoundation.h>

#include "pspocket/platform/audio_output.hpp"

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
    [impl->engine attachNode:impl->source];
    [impl->engine connect:impl->source to:impl->engine.mainMixerNode format:format];
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

AudioOutput::Stats AudioOutput::stats() const
{
    return {impl_->ring.played(), impl_->ring.underruns(), impl_->ring.overruns()};
}

} // namespace pspocket::platform
