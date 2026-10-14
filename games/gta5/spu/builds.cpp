// GTA V's own SPU jobs with native replacements (D-979): the known builds of RAGE's audio jobs as GTA V (BLES01807)
// links them, registered with the runtime's replacement machinery (pspocket/spu/replace.hpp, D-965) when the program
// loads. Data only: program keys, code ranges, the listed functions (entry, native implementation, the registers of
// $2-$79 their callers still read, from spu-live). The native code is in rage_audio.c.
//
// The sound mixer (SPURS 0x10016000 "Secondary", one SPU) runs these jobs a period of sound at a time; at full pace on
// the phone it needs a whole core (docs/improve.md, "A steady 30 fps on the phone").

#include "pspocket/spu/replace.hpp"

#include "rage_audio.h"

namespace {

namespace rp = pspocket::spu::replace;

constexpr rp::Registers regs(std::initializer_list<uint32_t> list)
{
    rp::Registers r{};
    for (const uint32_t n : list) r.bits[n >> 6] |= 1ull << (n & 63);
    return r;
}

// add_build() may run before main: the runtime's table of added builds is constant-initialised (an empty vector and a
// mutex), so it is ready before any dynamic initialisation.
const bool registered = [] {
    // The synthesizer's job (RAGE's modular synth: "1PoleFilter", "AllPassFilter", "BiquadFilter", "Clipper", ...).
    rp::add_build("RAGE audio synthesizer (spu_012bf13511b9f1b0)", 0x012BF13511B9F1B0ull, 0x04C00, 0x11030,
        {
            {0x05B20, ps_gta5_012b_05b20, regs({6})},
            {0x062C8, ps_gta5_012b_062c8, regs({6})},
        });
    // The mixer's submix job ("Submixes", "BiquadFilter", "BufferConversion", "R360OutputBuffer").
    rp::add_build("RAGE audio submixes (spu_28281614459ce75e)", 0x28281614459CE75Eull, 0x04C00, 0x14730,
        {
            {0x06048, ps_gta5_2828_06048, {}},
            {0x08748, ps_gta5_2828_08748, {}},
            {0x0A920, ps_gta5_2828_0a920, {}},
        });
    // The mixer's voice job ("PCMSource", "SrcDownsampleBuf", "Mp3InputBuffer"): the sample-rate converter's loop, from
    // its head to its exit (listed too: the lifted code leaves there). Live at the exit (spu-live): $79 of $2-$79; the
    // others are the ABI's.
    rp::add_build("RAGE audio voices (spu_bb12970d56fb7b8a)", 0xBB12970D56FB7B8Aull, 0x04C00, 0xBFA0,
        {
            {0x08000, ps_gta5_bb12_08000, regs({79})},
            {0x080D8, nullptr, {}},
        });
    return true;
}();

} // namespace

// Referenced by nothing: the registration above runs because this object is linked whole (games/CMakeLists.txt).
extern "C" int ps_gta5_spu_builds_registered(void);
extern "C" int ps_gta5_spu_builds_registered(void) { return registered ? 1 : 0; }
