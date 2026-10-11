#pragma once

// Synthetic SPU-ADPCM ("VAG") sounds for the libsynth2 tests (D-610): a small closed-loop encoder, each block of 28
// samples taking the filter and shift whose decoding comes nearest (psx-spx "SPU ADPCM Samples": a shift/filter byte,
// a flag byte, 28 nibbles low first; the prediction (old * f0 + older * f1 + 32) >> 6), and the sines it encodes.
// Shared by test_synth2.cpp and test_synth2_compare.cpp. No game data.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::test::vag {

// The five prediction filters (psx-spx "CDROM XA Audio ADPCM Compression", SPU-ADPCM having the fifth).
inline constexpr s32 kF0[5] = {0, 60, 115, 98, 122}, kF1[5] = {0, 0, -52, -55, -60};

inline s32 clamp16(s64 v) { return static_cast<s32>(std::clamp<s64>(v, -0x8000, 0x7FFF)); }

struct Encoder {
    s32 p1 = 0, p2 = 0; // the decoder's history after the blocks written so far

    void block(const s16* in, u8 flags, u8* out)
    {
        s64 best_err = -1;
        u8 best[16] = {};
        s32 best_p1 = 0, best_p2 = 0;
        // A loop's first block predicts nothing, so that every pass through the loop decodes it alike.
        for (s32 filter = 0; filter < ((flags & 4) ? 1 : 5); filter++)
            for (s32 shift = 0; shift <= 12; shift++) {
                u8 b[16] = {static_cast<u8>(filter << 4 | shift), flags};
                s32 q1 = p1, q2 = p2;
                s64 err = 0;
                for (s32 i = 0; i < 28; i++) {
                    const s32 predicted = (q1 * kF0[filter] + q2 * kF1[filter] + 32) >> 6;
                    const s32 step = 1 << (12 - shift);
                    const s32 nibble =
                        std::clamp(static_cast<s32>(std::lround(double(in[i] - predicted) / step)), -8, 7);
                    const s32 got = clamp16(nibble * step + predicted);
                    err += s64{in[i] - got} * (in[i] - got);
                    q2 = q1;
                    q1 = got;
                    b[2 + i / 2] |= static_cast<u8>((nibble & 15) << ((i & 1) * 4));
                }
                if (best_err < 0 || err < best_err) {
                    best_err = err;
                    std::memcpy(best, b, 16);
                    best_p1 = q1;
                    best_p2 = q2;
                }
            }
        std::memcpy(out, best, 16);
        p1 = best_p1;
        p2 = best_p2;
    }
};

// A whole sound: the samples (a multiple of 28) as blocks, the first block's flags, the middle ones', the last's
// (bit 0 loop end, bit 1 repeat, bit 2 loop start).
inline std::vector<u8> encode(const std::vector<s16>& pcm, u8 first, u8 middle, u8 last)
{
    REQUIRE(pcm.size() % 28 == 0);
    const usize blocks = pcm.size() / 28;
    std::vector<u8> out(blocks * 16);
    Encoder e;
    for (usize i = 0; i < blocks; i++) {
        const u8 flags = i + 1 == blocks ? last : i == 0 ? first : middle;
        e.block(pcm.data() + 28 * i, flags, out.data() + 16 * i);
    }
    return out;
}

// `cycles` periods of a sine of `amplitude` over `samples` samples.
inline std::vector<s16> sine(usize samples, double cycles, double amplitude)
{
    std::vector<s16> pcm(samples);
    for (usize n = 0; n < samples; n++)
        pcm[n] = static_cast<s16>(std::lround(amplitude * std::sin(2 * M_PI * cycles * double(n) / double(samples))));
    return pcm;
}

// A looped sine: 24 blocks (672 samples, 7 periods), the loop over the whole sound.
inline std::vector<s16> looped_sine_pcm() { return sine(672, 7, 20000); }
inline std::vector<u8> looped_sine() { return encode(looped_sine_pcm(), 6, 2, 3); }

} // namespace pspocket::hle::test::vag
