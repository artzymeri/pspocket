// cellAdec's decoders when the game overwrites a decoder's context while it is open (D-645). A core keeps its
// context, with its queue and the IDs of its mutexes and conditions, in the game's memory, as Sony's cores do. A
// damaged AT3+ file whose PCM ran past its buffer onto that memory (the cellAtrac fuzzing, test_fuzz_media.cpp) left
// the ATRAC3plus decoder's thread going round its queue's calls, which failed at once, past the process's end, or
// taking the commands of another decoder opened in the same place. The thread of each of pspocket's cores with one
// (LPCM, ATRAC3plus, the cores on FFmpeg) now ends when its context is no longer its own.
//
// Shown here without load: the game's callback runs on the decoder's own thread, so the context it overwrites at
// PCMOUT is gone before the thread is back at its queue (where the fuzzing needed a busy machine for the game's thread
// to get there first), and a callback held at PCMOUT keeps the thread away from its queue while another decoder opens
// in the same memory.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <vector>

#include "adec_test_util.hpp"
#include "atrac_writer.hpp"
#include "hle_test_util.hpp"
#include "internal.hpp" // runtime/lv2/src (white-box): whether a thread has ended
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::adec_test;

namespace {

constexpr u32 CELL_ADEC_ERROR_ARG = 0x80610003;

// The callback's control block: +0 where to write, +4 how many words, +8 the word written, +0xC the PCMOUTs seen, +0x10
// whether to hold the thread at PCMOUT, +0x14 released, +0x18 holding.
constexpr u32 kCtl = kData + 0x2000;
constexpr u32 kFillAt = kCtl, kFillWords = kCtl + 4, kFillWord = kCtl + 8, kPcmOuts = kCtl + 0xC, kHold = kCtl + 0x10,
              kRelease = kCtl + 0x14, kHolding = kCtl + 0x18;

// The callback (handle r3, msgType r4, msgData r5, cbArg r6 = kCtl). At PCMOUT (1): held while +0x10 asks, sleeping
// 200 us at a time (sys_timer_usleep), until +0x14; then +4 words from +0 written with +8; then +0xC counted. 0 back.
void write_overwriting_callback(u8* b)
{
    as::Assembler a(kCode);
    const auto done = a.new_label(), fill = a.new_label(), count = a.new_label();
    a.emit(as::cmpwi(0, 4, 1));
    a.bne(0, done);
    a.emit(as::lwz(9, 0x10, 6));
    a.emit(as::cmpwi(0, 9, 0));
    a.beq(0, fill);
    a.emit(as::li(9, 1));
    a.emit(as::lwsync());
    a.emit(as::stw(9, 0x18, 6));
    const auto hold = a.here();
    a.emit(as::li(3, 200));
    a.emit(as::li(11, 141)); // sys_timer_usleep
    a.emit(as::sc());
    a.emit(as::lis(6, static_cast<s32>(kCtl >> 16)));
    a.emit(as::ori(6, 6, kCtl & 0xFFFF));
    a.emit(as::lwz(9, 0x14, 6));
    a.emit(as::cmpwi(0, 9, 0));
    a.beq(0, hold);
    a.emit(as::isync());
    a.bind(fill);
    a.emit(as::lwz(7, 0, 6));
    a.emit(as::lwz(8, 4, 6));
    a.emit(as::lwz(0, 8, 6));
    a.emit(as::cmpwi(0, 8, 0));
    a.beq(0, count);
    a.emit(as::mtctr(8));
    const auto loop = a.here();
    a.emit(as::stw(0, 0, 7));
    a.emit(as::addi(7, 7, 4));
    a.bdnz(loop);
    a.bind(count);
    a.emit(as::lwsync());
    a.emit(as::lwz(9, 0xC, 6));
    a.emit(as::addi(9, 9, 1));
    a.emit(as::stw(9, 0xC, 6));
    a.bind(done);
    a.emit(as::li(3, 0));
    a.emit(as::blr());
    const std::vector<u8> code = a.finish();
    std::memcpy(b + kCode, code.data(), code.size());
    ps_write32(b, kOpd, kCode);
    ps_write32(b, kOpd + 4, 0);
    std::memset(b + kCtl, 0, 0x20);
}

// A core, and where its context (each core's own layout) keeps the decoder thread's ID (64 bits).
struct CoreCase {
    const char* name;
    u32 type;
    u32 thread_id_at;
    std::vector<u32> param; // the sequence's parameters
    std::vector<u8> au;     // an access unit: PCMOUT follows its decode, an error or not
};

// LPCM always; ATRAC3plus and the cores on FFmpeg in a build with FFmpeg.
std::vector<CoreCase> core_cases()
{
    std::vector<CoreCase> cases;
    // adec_lpcm.cpp: the thread's ID at +0xC0. PAMF's LPCM, stereo, 48 kHz, 16 bits: 1024 samples of silence.
    cases.push_back({"LPCM", 1, 0xC0, {3, 1, 1, 1024 * 4}, std::vector<u8>(1024 * 4, 0)});
#if PSPOCKET_HAVE_FFMPEG
    // adec_atracx.cpp: the thread's ID first. 48 kHz stereo, 688-byte frames, floats.
    cases.push_back({"ATRAC3plus", 3, 0x00, {48000, 2, 2, 0x2B0, 0, 0x84, 0}, hle::test::atrac::silent_frame(8)});
    // The cores on FFmpeg (adec_ffmpeg.hpp: the thread's ID at +0x148), MP3's for them: floats; bytes that are no MP3
    // frame, an error (AUDONE and PCMOUT follow it all the same).
    cases.push_back({"MP3 (the cores on FFmpeg)", 4, 0x148, {4}, std::vector<u8>(0x100, 0)});
#endif
    return cases;
}

// The decoder's memory, from its context (the handle) to past the end of its core's context (0x268 bytes, the largest
// of the three): what a PCM run past its buffer onto it covers.
constexpr u32 kOverwritten = 0x268;

bool ended(Fixture& f, u32 id)
{
    const std::shared_ptr<lv2::PpuThread> t = f.kernel->thread(id);
    if (!t) return true; // joined
    std::lock_guard lock(f.kernel->impl().join_mutex);
    return t->finished;
}

// cellAdecOpen and cellAdecStartSeq of a decoder of `c.type` on kDecoderMem, its callback the overwriting one: the
// handle.
u32 open_decoder(u8* b, const Call& call, const CoreCase& c)
{
    std::memset(b + kArgs, 0, 0x100);
    ps_write32(b, kType, c.type);
    REQUIRE(call("cellAdecQueryAttr", 0x7E4A4A49, {kType, kAttr}) == 0);
    // CellAdecResource: totalMemSize, startAddr, ppuThreadPriority, spuThreadPriority, ppuThreadStackSize.
    const u32 res[5] = {ps_read32(b, kAttr), kDecoderMem, 1000, 200, 0x4000};
    for (u32 i = 0; i < 5; i++) ps_write32(b, kRes + 4 * i, res[i]);
    ps_write32(b, kCb, kOpd);
    ps_write32(b, kCb + 4, kCtl);
    REQUIRE(call("cellAdecOpen", 0xD00A6988, {kType, kRes, kCb, kHandle}) == 0);
    const u32 handle = ps_read32(b, kHandle);
    for (u32 i = 0; i < c.param.size(); i++) ps_write32(b, kParam + 4 * i, c.param[i]);
    REQUIRE(call("cellAdecStartSeq", 0x487B613E, {handle, kParam}) == 0);
    return handle;
}

void decode(u8* b, const Call& call, u32 handle, const std::vector<u8>& au)
{
    std::memcpy(b + kStream, au.data(), au.size());
    ps_write32(b, kAu + 0, kStream);
    ps_write32(b, kAu + 4, static_cast<u32>(au.size()));
    ps_write32(b, kAu + 8, 0);
    ps_write32(b, kAu + 12, 0x1000);
    ps_write64(b, kAu + 16, 0);
    REQUIRE(call("cellAdecDecodeAu", 0x1529E506, {handle, kAu}) == 0);
}

u32 thread_of_core(u8* b, u32 handle, const CoreCase& c)
{
    const u32 core = ps_read32(b, handle + 0x30); // AdecContext's core_handle
    return static_cast<u32>(ps_read64(b, core + c.thread_id_at));
}

} // namespace

TEST_CASE("cellAdec: a decoder whose context the game overwrites ends its thread (D-645)", "[hle][adec]")
{
    const std::vector<CoreCase> cases = core_cases();
    for (const CoreCase& c : cases) {
        // Zeros (silent PCM, as in the fuzzing) and a pattern (PCM that is not silent: words that are no IDs, a queue
        // that seems full, a thread that seems to be running).
        for (const u32 word : {0u, 0xCCCCCCCCu}) {
            DYNAMIC_SECTION(c.name << ", the context overwritten with " << std::format("{:#x}", word))
            {
                InterpretMisses interpret;
                BigFixture f;
                GuestThread t(f, 0, "ppu");
                u8* b = f.base();
                const Call call = hle_calls(f, t);
                write_overwriting_callback(b);
                const u32 handle = open_decoder(b, call, c);
                const u32 core = ps_read32(b, handle + 0x30);
                const u32 thread = thread_of_core(b, handle, c);
                REQUIRE(thread != 0);
                ps_write32(b, kFillAt, handle);
                ps_write32(b, kFillWords, (core + kOverwritten - handle) / 4);
                ps_write32(b, kFillWord, word);
                decode(b, call, handle, c.au);
                REQUIRE(wait_until([&] { return load32_acquire(b, kPcmOuts) >= 1; }));
                // The decoder's thread, back at its queue, finds its context gone: it ends (it went round failing calls
                // for ever, or took the pattern for a command and stopped the process).
                CHECK(wait_until([&] { return ended(f, thread); }, std::chrono::seconds(10)));
                // Its context's first word no longer names it: refused, as Sony's libadec refuses it.
                CHECK(call("cellAdecClose", 0x847D2380, {handle}) == CELL_ADEC_ERROR_ARG);
            }
        }
    }
}

TEST_CASE("cellAdec: a decoder's thread that finds another decoder opened in its place ends (D-645)", "[hle][adec]")
{
    const std::vector<CoreCase> cases = core_cases();
    for (const CoreCase& c : cases) {
        DYNAMIC_SECTION(c.name)
        {
            InterpretMisses interpret;
            BigFixture f;
            GuestThread t(f, 0, "ppu");
            u8* b = f.base();
            const Call call = hle_calls(f, t);
            write_overwriting_callback(b);
            // The first decoder's thread held at its PCMOUT, away from its queue.
            const u32 first = open_decoder(b, call, c);
            const u32 first_thread = thread_of_core(b, first, c);
            store32_release(b, kHold, 1);
            decode(b, call, first, c.au);
            REQUIRE(wait_until([&] { return load32_acquire(b, kHolding) != 0; }));
            // Another decoder in the same memory (as a game opens one after a close that was refused): its context in
            // place of the first's, with a queue, mutexes and conditions of its own, and a thread of its own.
            const u32 second = open_decoder(b, call, c);
            REQUIRE(second == first);
            const u32 second_thread = thread_of_core(b, second, c);
            REQUIRE(second_thread != first_thread);
            store32_release(b, kHold, 0);
            store32_release(b, kRelease, 1);
            // The first thread, back at the queue now in its memory, ends: it would have taken the second decoder's
            // commands, its sequence or its close (whose join then never returned).
            REQUIRE(wait_until([&] { return ended(f, first_thread); }, std::chrono::seconds(10)));
            CHECK(load32_acquire(b, kPcmOuts) == 1);
            // The second decoder whole: its access unit decoded, its PCM taken, closed, its thread joined.
            decode(b, call, second, c.au);
            REQUIRE(wait_until([&] { return load32_acquire(b, kPcmOuts) >= 2; }));
            CHECK(call("cellAdecGetPcm", 0x97FF2AF1, {second, kPcm}) == 0);
            CHECK(call("cellAdecClose", 0x847D2380, {second}) == 0);
            CHECK(ended(f, second_thread));
        }
    }
}
