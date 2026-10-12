// pspocket's own CELP cores for cellAdec (D-932): libcelpdec's (cellCelpDec: MPEG-4 CELP at 16 kHz, regular pulse
// excitation) and libcelp8dec's (cellCelp8Dec: 8 kHz, multi-pulse excitation), made to answer, notify and write what
// Sony's cores do, compared with them under cellAdec (tests/hle/test_celp_dec_compare.cpp). They are also the modules'
// exports: the operation tables (g_cell_adec_core_ops_Celp, g_cell_adec_core_ops_Celp8) and the PTS functions
// (_SceAdecCorrectPtsValue_Celp, _SceAdecCorrectPtsValue_Celp8), for Sony's libadec and others to call (as D-591's
// tables).
//
// What each library is: a PPU side, an input thread ("_CelpDecInputThrFunc", "_Celp8DecInputThrFunc") and an SPU
// program (a SPURS task) that decodes. Its memory (0x3CC00 or 0x42F00 bytes, from the core's handle rounded up to 128
// bytes) holds a queue of four commands, the thread's mutexes and conditions, the sequence's parameters, a PTS slot per
// decoder, the SPURS objects and four PCM slots (each its samples, then 16 bytes of information: the parameters in
// force). StartSeq, EndSeq and DecodeAu queue a command (BUSY when four wait); the thread takes them one at a time and
// sends each to the SPU program, which keeps 64 decoders (16 at 8 kHz): an access unit's user data chooses one (its low
// seven bits, at most 63 or 15), bit 7 resets that decoder (and its PTS slot) first, bits 8 to 15 mute the frame's
// largest excitation samples. StartSeq makes all the decoders for the configuration; EndSeq drops them. After each access
// unit the thread notifies AUDONE and PCMOUT (PTS correction type 24 or 34: the library's PTS function, which steps that
// decoder's PTS slot by a frame), after ERROR when the unit failed (SEQ before StartSeq; the SPU program's errors).
// Here the SPU program's decoding is done on the host, bit for bit (celp_rpe.cpp, celp_mpe.cpp); the PPU side is the
// library's. The two libraries' PPU sides are one program with other offsets, sizes, codes and parameter checks (Spec).

#include "celp_spu_fp.hpp" // first: the floating-point pragmas

#include <array>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "adec.hpp"
#include "celp_codec.hpp"
#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/object.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::adec {
namespace celpcore {
namespace {

constexpr std::string_view kChan = "cellAdec";

// The libraries' context, from the core's handle rounded up to 128 bytes: what both have at the same place.
namespace cx {
constexpr u32 queue = 0x00;                                // four commands of 0x30 bytes
constexpr u32 q_read = 0xC0, q_write = 0xC4, q_count = 0xC8;
constexpr u32 thread_id = 0xD0;                            // u64
constexpr u32 q_mutex = 0xD8, q_cond = 0xDC;
constexpr u32 state = 0xE0;                                // 0 closed, 1 closing, 2 open, 3 started, 4 decoding
constexpr u32 decodes = 0xE4;                              // access units taken: the next PCM slot
constexpr u32 notify = 0xE8;                               // AUDONE, PCMOUT, ERROR, SEQDONE (function, argument)
constexpr u32 param = 0x108;                               // the sequence's parameters in force (CellAdecParamCelp(8))
constexpr u32 start_param = 0x118;                         // the last StartSeq's
constexpr u32 channel = 0x128;                             // the last access unit's decoder
constexpr u32 pts = 0x12C;                                 // each decoder's next PTS (upper, lower)
} // namespace cx
// A command: the decoder (+0), the type (+4: start 0, end 1, decode 2, decode after a reset 3), the PCM handle (+8:
// 0xFF a free entry, 4 for a start or end), the access unit (+0xC, +0x10), the mute byte (+0x1C), a start's parameters
// (+0x20).
namespace cmd {
constexpr u32 decoder = 0x00, type = 0x04, pcm_handle = 0x08, au_addr = 0x0C, au_size = 0x10, mute = 0x1C,
              param = 0x20, size = 0x30;
}
enum : u32 { START = 0, END = 1, DECODE = 2, DECODE_RESET = 3 };
constexpr u32 kFree = 0xFF, kNoPcm = 4;

// What differs between the 16 kHz core (libcelpdec) and the 8 kHz one (libcelp8dec).
struct Spec {
    std::string_view name, module;
    bool wideband;                                   // the SPU program's coder: RPE at 16 kHz, else MPE at 8 kHz
    u32 version;
    u32 mem_size;                                    // the context's
    u32 arg, seq, busy, core_fatal, core_seq;        // the core's own error codes
    u32 decoders;                                    // the SPU program's
    u32 job_mutex, job_cond, job_busy, spurs;        // where the context has these
    u32 own_spurs;                                   // the SPURS instance it makes when given none
    u32 notify_guest, tls;                           // pspocket's own words, in that instance's place
    u32 pcm, pcm_slot, info;                         // the four PCM slots, each's information
    s8 correct_pts;                                  // PCMOUT's PTS correction type
    const char *mutex_name, *cond_name, *thread_name, *thread_fn;
    const char* const* op_names;
    std::string_view ops_variable;

    NotifySlots slots() const { return NotifySlots{cx::notify, notify_guest}; }
};

constexpr const char* kOpNames16[12] = {"_CellAdecCoreOpGetMemSize_Celp", "_CellAdecCoreOpOpen_Celp",
    "_CellAdecCoreOpClose_Celp", "_CellAdecCoreOpStartSeq_Celp", "_CellAdecCoreOpEndSeq_Celp",
    "_CellAdecCoreOpDecodeAu_Celp", "_CellAdecCoreOpGetVersion_Celp", "_CellAdecCoreOpRealign_Celp",
    "_CellAdecCoreOpReleasePcm_Celp", "_CellAdecCoreOpGetPcmHandleNum_Celp", "_CellAdecCoreOpGetBsiInfoSize_Celp",
    "_CellAdecCoreOpOpenExt_Celp"};
constexpr const char* kOpNames8[12] = {"_CellAdecCoreOpGetMemSize_Celp8", "_CellAdecCoreOpOpen_Celp8",
    "_CellAdecCoreOpClose_Celp8", "_CellAdecCoreOpStartSeq_Celp8", "_CellAdecCoreOpEndSeq_Celp8",
    "_CellAdecCoreOpDecodeAu_Celp8", "_CellAdecCoreOpGetVersion_Celp8", "_CellAdecCoreOpRealign_Celp8",
    "_CellAdecCoreOpReleasePcm_Celp8", "_CellAdecCoreOpGetPcmHandleNum_Celp8", "_CellAdecCoreOpGetBsiInfoSize_Celp8",
    "_CellAdecCoreOpOpenExt_Celp8"};

constexpr Spec kCelp16{"CELP", "cellCelpDec", true, 0x02500000, 0x3CC00,
    0x80612E03, 0x80612E04, 0x80612E01, 0x80612E81, 0x80612E83, 64,
    0x32C, 0x330, 0x334, 0x338, 0x380, 0x380, 0x384, 0x3BA80, 0x400, 0x3C0, 24,
    "_cedm01", "_cedc01", "_CelpDecInputThrFunc", "_pspocket_celp_dec_thread", kOpNames16, "g_cell_adec_core_ops_Celp"};
constexpr Spec kCelp8{"CELP8", "cellCelp8Dec", false, 0x04000000, 0x42F00,
    0x80612EA3, 0x80612EA4, 0x80612EA1, 0x80612EB1, 0x80612EB3, 16,
    0x1AC, 0x1B0, 0x1B4, 0x1B8, 0x200, 0x200, 0x204, 0x39900, 0x580, 0x500, 34,
    "_c8dm01", "_c8dc01", "_Celp8DecInputThrFunc", "_pspocket_celp8_dec_thread", kOpNames8,
    "g_cell_adec_core_ops_Celp8"};

// ---- each library's sequence parameters (CellAdecParamCelp, CellAdecParamCelp8) and what follows from them

// libcelpdec: excitation mode 1 (RPE), sampling rate 2 (16 kHz), configuration 0 to 3, word size 0 (16-bit) or 1
// (float). libcelp8dec: mode 0 (MPE), rate 5 (8 kHz), one of the ten configurations its SPU program decodes, word size 0
// (float: its only output).
bool valid_param(const Spec& spec, u32 mode, u32 rate, u32 config, u32 word_size)
{
    if (spec.wideband) return mode == 1 && rate == 2 && config <= 3 && word_size <= 1;
    return mode == 0 && rate == 5 && config < 27 && celp::mpe_supported(config) && word_size == 0;
}

// An access unit's bytes in a configuration (0 for none).
u32 au_bytes(const Spec& spec, u32 config)
{
    constexpr u32 wide[4] = {27, 20, 35, 43};
    if (spec.wideband) return config <= 3 ? wide[config] : 0;
    if (config >= 28) return 0;
    const celp::MpeConfig& c = celp::kMpeConfigs[config];
    u32 bit[12];
    u8 pos[12][80];
    if (!celp::mpe_pulses(config, bit, pos)) return 0;
    u32 sub = 8 + c.n_pulses + 6;
    for (u32 i = 0; i < c.n_pulses; i++) sub += bit[i];
    return (22 + 2 + 6 + c.n_subframes * sub + 7) / 8;
}

// A frame's PCM bytes, as the library's tables have them (configurations past them: none).
u32 pcm_bytes(const Spec& spec, u32 config, u32 word_size)
{
    constexpr u32 wide[4] = {480, 320, 480, 480};
    if (spec.wideband) return wide[config & 3] * (word_size + 1);
    return config < 28 ? celp::kMpeConfigs[config].frame_size * 4 : 0;
}

// A frame's duration in 90 kHz ticks.
u32 pts_step(const Spec& spec, u32 config)
{
    if (spec.wideband) return config == 1 ? 900 : 1350;
    return config < 28 ? celp::kMpeConfigs[config].frame_size * 45 / 4 : 0;
}

u32 align(u32 h) { return (h + 0x7F) & ~0x7Fu; }
lv2::PpuThread& thread(PPUContext& ctx) { return thread_of(ctx); }

// ---- the host's decoders: the SPU program's 64 (or 16), for each open core

struct Decoders {
    std::array<std::unique_ptr<celp::RpeDecoder>, 64> rpe;
    std::array<std::unique_ptr<celp::MpeDecoder>, 16> mpe;
};
struct State {
    std::mutex mutex;
    std::map<u32, std::shared_ptr<Decoders>> cores; // by context
};
State& state() { return HleProcess::current()->state<State>(); }

std::shared_ptr<Decoders> decoders_of(u32 c, bool make = false)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    auto it = s.cores.find(c);
    if (it != s.cores.end()) return it->second;
    if (!make) return nullptr;
    return s.cores[c] = std::make_shared<Decoders>();
}

void drop_decoders(u32 c)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    s.cores.erase(c);
}

// ---- lv2 objects, as the libraries make them ("_cedm01", "_cedc01"; "_c8dm01", "_c8dc01")

s32 make_mutex(PPUContext& ctx, const Spec& spec, u32 id_out)
{
    u8* base = guest_base(ctx);
    StackVar attr(ctx, base, 0x28);
    ps_write32(base, attr.addr() + 0x00, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, attr.addr() + 0x04, lv2::SYS_SYNC_NOT_RECURSIVE);
    ps_write32(base, attr.addr() + 0x08, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    ps_write32(base, attr.addr() + 0x0C, lv2::SYS_SYNC_NOT_ADAPTIVE);
    std::memcpy(base + attr.addr() + 0x20, spec.mutex_name, 8);
    return lv2::sys_mutex_create(thread(ctx), id_out, attr.addr());
}

s32 make_cond(PPUContext& ctx, const Spec& spec, u32 id_out, u32 mutex_at)
{
    u8* base = guest_base(ctx);
    StackVar attr(ctx, base, 0x18);
    ps_write32(base, attr.addr() + 0x00, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    std::memcpy(base + attr.addr() + 0x10, spec.cond_name, 8);
    return lv2::sys_cond_create(thread(ctx), id_out, ps_read32(base, mutex_at), attr.addr());
}

u32 word(PPUContext& ctx, u32 at) { return ps_read32(guest_base(ctx), at); }
s32 lock(PPUContext& ctx, u32 at) { return lv2::sys_mutex_lock(thread(ctx), word(ctx, at), 0); }
s32 unlock(PPUContext& ctx, u32 at) { return lv2::sys_mutex_unlock(thread(ctx), word(ctx, at)); }
s32 wait(PPUContext& ctx, u32 at) { return lv2::sys_cond_wait(thread(ctx), word(ctx, at), 0); }
s32 signal(PPUContext& ctx, u32 at) { return lv2::sys_cond_signal(thread(ctx), word(ctx, at)); }
s32 signal_all(PPUContext& ctx, u32 at) { return lv2::sys_cond_signal_all(thread(ctx), word(ctx, at)); }

// The command queue (the caller holds q_mutex): a free entry at the write index is filled; BUSY with four waiting.
template <class Fill>
s32 queue_command(PPUContext& ctx, u32 c, const Spec& spec, Fill fill)
{
    u8* base = guest_base(ctx);
    lock(ctx, c + cx::q_mutex);
    if (static_cast<s32>(ps_read32(base, c + cx::q_count)) > 3) {
        unlock(ctx, c + cx::q_mutex);
        return error(spec.busy);
    }
    const u32 w = ps_read32(base, c + cx::q_write), e = c + cx::queue + cmd::size * w;
    if (ps_read32(base, e + cmd::pcm_handle) == kFree) {
        fill(base, e);
        ps_write32(base, c + cx::q_count, ps_read32(base, c + cx::q_count) + 1);
        ps_write32(base, c + cx::q_write, (w + 1) & 3);
    }
    signal(ctx, c + cx::q_cond);
    return lv2::CELL_OK; // the caller unlocks
}

// The one job at a time the thread runs on the SPU program, which Close waits for.
void begin_job(PPUContext& ctx, const Spec& spec, u32 c)
{
    u8* base = guest_base(ctx);
    lock(ctx, c + spec.job_mutex);
    while (ps_read32(base, c + spec.job_busy) == 1) wait(ctx, c + spec.job_cond);
    ps_write32(base, c + spec.job_busy, 1);
    unlock(ctx, c + spec.job_mutex);
}

void end_job(PPUContext& ctx, const Spec& spec, u32 c)
{
    lock(ctx, c + spec.job_mutex);
    ps_write32(guest_base(ctx), c + spec.job_busy, 0);
    signal_all(ctx, c + spec.job_cond);
    unlock(ctx, c + spec.job_mutex);
}

// A notification's function, when open was given one (the libraries call each only then).
bool has_notify(u8* base, u32 c, u32 i) { return ps_read32(base, c + cx::notify + 8 * i) != 0; }

// ---- the SPU program's work, on the host

// Start: the decoders made for the parameters.
s32 spu_start(const Spec& spec, u32 c, u32 config)
{
    const std::shared_ptr<Decoders> d = decoders_of(c, true);
    celp::spu::Scope scope;
    if (spec.wideband) {
        for (auto& r : d->rpe) {
            if (!r) r = std::make_unique<celp::RpeDecoder>();
            r->reset(config & 3);
        }
        return 0;
    }
    if (config >= 28 || !celp::mpe_supported(config)) return -1;
    for (auto& r : d->mpe) {
        if (!r) r = std::make_unique<celp::MpeDecoder>();
        r->reset(config);
    }
    return 0;
}

// End: the decoders dropped.
s32 spu_end(u32 c)
{
    if (const std::shared_ptr<Decoders> d = decoders_of(c)) {
        for (auto& r : d->rpe) r.reset();
        for (auto& r : d->mpe) r.reset();
    }
    return 0;
}

// A sample (16-bit units) as the SPU programs write it: held to 16 bits, then a float (full scale 1, big-endian) or a
// 16-bit little-endian integer.
void put_sample(u8* base, u32 out, u32 n, float y, bool as_float)
{
    using namespace celp::spu;
    float v = y;
    if (fcgt(v, 32767.0f)) v = 32767.0f;
    if (fcgt(-32768.0f, v)) v = -32768.0f;
    if (as_float) {
        const float f = fm(v, from_bits(0x38000107u)); // about 1/32767.75
        ps_write32(base, out + 4 * n, bits(fcgt(f, 1.0f) ? 1.0f : fcgt(-1.0f, f) ? -1.0f : f));
    } else {
        s32 s = cflts(y);
        s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        ps_write8(base, out + 2 * n, static_cast<u8>(s & 0xFF));
        ps_write8(base, out + 2 * n + 1, static_cast<u8>((s >> 8) & 0xFF));
    }
}

// A frame: decoded by decoder `index` (made again first for a reset) into the PCM slot. 0, -2 (no such decoder) or -1
// (the frame did not decode).
s32 spu_decode(const Spec& spec, u8* base, u32 c, u32 index, bool reset, u32 config, u32 word_size, u32 mute,
    const u8* au, u32 au_size, u32 out)
{
    const std::shared_ptr<Decoders> d = decoders_of(c);
    if (!d) return -2;
    celp::spu::Scope scope;
    float y[320];
    u32 consumed = 0;
    if (spec.wideband) {
        std::unique_ptr<celp::RpeDecoder>& r = d->rpe[index & 63];
        if (reset) {
            if (!r) r = std::make_unique<celp::RpeDecoder>();
            r->reset(config & 3);
        }
        if (!r) return -2;
        if (!r->decode(au, au_size, &consumed, y, mute)) return -1;
        for (u32 n = 0; n < r->cfg.frame_size; n++) put_sample(base, out, n, y[n], word_size != 0);
        return 0;
    }
    std::unique_ptr<celp::MpeDecoder>& r = d->mpe[index & 15];
    if (reset) {
        if (config >= 28 || !celp::mpe_supported(config)) return -1;
        if (!r) r = std::make_unique<celp::MpeDecoder>();
        r->reset(config);
    }
    if (!r) return -2;
    r->decode(au, au_size, &consumed, y, mute);
    for (u32 n = 0; n < r->cfg.frame_size; n++) put_sample(base, out, n, y[n], true);
    return 0;
}

// The SPU program's answer as the library reads it: 0 done, -2 SEQ, anything else FATAL.
s32 answer(const Spec& spec, s32 r) { return r == 0 ? lv2::CELL_OK : r == -2 ? error(spec.core_seq) : error(spec.core_fatal); }

// ---- the input thread (the library's)

// Whether the context is still the calling thread's (D-645): its thread ID names the thread.
bool own_context(PPUContext& ctx, u32 c)
{
    const u32 self = thread(ctx).id;
    const u64 named = ps_read64(guest_base(ctx), c + cx::thread_id);
    if (named == self) return true;
    PS_LOG_ERROR(kChan, "CELP core {:#x}: its context is no longer its own (it names thread {:#x}): the game "
        "overwrote it while the decoder was open; the decoder's thread {:#x} ends", c, named, self);
    return false;
}

void reset_pts(u8* base, const Spec& spec, u32 c)
{
    for (u32 i = 0; i < spec.decoders; i++) {
        ps_write32(base, c + cx::pts + 8 * i, 0xFFFFFFFF);
        ps_write32(base, c + cx::pts + 8 * i + 4, 0xFFFFFFFF);
    }
}

void run(PPUContext& ctx, const Spec& spec, u32 c)
{
    u8* base = guest_base(ctx);
    for (;;) {
        if (ps_read32(base, c + cx::state) <= 1 || !own_context(ctx, c)) return;
        lock(ctx, c + cx::q_mutex);
        while (static_cast<s32>(ps_read32(base, c + cx::q_count)) <= 0) {
            if (ps_read32(base, c + cx::state) <= 1) {
                unlock(ctx, c + cx::q_mutex);
                return;
            }
            wait(ctx, c + cx::q_cond);
            if (!own_context(ctx, c)) return;
        }
        const u32 rd = ps_read32(base, c + cx::q_read), e = c + cx::queue + cmd::size * rd;
        u8 entry[cmd::size];
        std::memcpy(entry, base + e, cmd::size);
        ps_write32(base, e + cmd::pcm_handle, kFree);
        ps_write32(base, c + cx::q_read, (rd + 1) & 3);
        ps_write32(base, c + cx::q_count, ps_read32(base, c + cx::q_count) - 1);
        unlock(ctx, c + cx::q_mutex);
        const u32 index = ps_ld32(entry + cmd::decoder), type = ps_ld32(entry + cmd::type);
        const s32 pcm_handle = static_cast<s32>(ps_ld32(entry + cmd::pcm_handle));
        const CoreNotifier notify(ctx, c, spec.slots());
        if (type & 2) {
            // An access unit into the next PCM slot, its information the parameters in force.
            const u32 n = ps_read32(base, c + cx::decodes);
            ps_write32(base, c + cx::decodes, n + 1);
            const u32 out = c + spec.pcm + spec.pcm_slot * (n & 3), info = out + spec.info;
            const u32 config = ps_read32(base, c + cx::param + 8), word_size = ps_read32(base, c + cx::param + 12);
            const u32 bytes = pcm_bytes(spec, config, word_size);
            begin_job(ctx, spec, c);
            s32 err = error(spec.seq);
            u32 size = 0;
            if (ps_read32(base, c + cx::state) > 2) {
                ps_write32(base, c + cx::state, 4);
                const bool reset = type == DECODE_RESET;
                if (reset) {
                    ps_write32(base, c + cx::pts + 8 * index, 0xFFFFFFFF);
                    ps_write32(base, c + cx::pts + 8 * index + 4, 0xFFFFFFFF);
                }
                const u32 au = ps_ld32(entry + cmd::au_addr), au_size = ps_ld32(entry + cmd::au_size);
                u8 frame[64] = {};
                const bool readable = au_size <= sizeof frame && guest_readable(thread(ctx), au, au_size);
                if (readable) std::memcpy(frame, base + au, au_size);
                else PS_LOG_ERROR(kChan, "CELP core {:#x}: the access unit ({:#x}, {} bytes) is not guest memory", c, au, au_size);
                err = answer(spec, readable ? spu_decode(spec, base, c, index, reset, config, word_size,
                                                  ps_ld32(entry + cmd::mute), frame, au_size, out)
                                            : -1);
                size = err == lv2::CELL_OK ? bytes : 0;
            }
            std::memcpy(base + info, base + c + cx::param, 16);
            ps_write32(base, c + cx::channel, index);
            end_job(ctx, spec, c);
            if (err != lv2::CELL_OK && has_notify(base, c, 2)) notify.error(err);
            if (has_notify(base, c, 0)) notify.au_done(pcm_handle);
            if (has_notify(base, c, 1)) notify.pcm_out(pcm_handle, out, size, info, spec.correct_pts, err);
            continue;
        }
        begin_job(ctx, spec, c);
        if (type == START) {
            reset_pts(base, spec, c);
            std::memcpy(base + c + cx::param, entry + cmd::param, 16);
            const s32 err = answer(spec, spu_start(spec, c, ps_ld32(entry + cmd::param + 8)));
            if (err == lv2::CELL_OK) {
                if (ps_read32(base, c + cx::state) > 1) ps_write32(base, c + cx::state, 3);
            } else if (has_notify(base, c, 2)) {
                notify.error(err);
            }
        } else if (type == END) {
            const s32 err = answer(spec, spu_end(c));
            if (ps_read32(base, c + cx::state) > 1) ps_write32(base, c + cx::state, 2);
            if (has_notify(base, c, 3)) notify.seq_done();
            if (err != lv2::CELL_OK && has_notify(base, c, 2)) notify.error(err);
        }
        end_job(ctx, spec, c);
    }
}

// ---- the operations (CellAdecCoreOps), the libraries'

class CelpCore final : public AdecCore {
public:
    explicit CelpCore(const Spec& spec) : spec_(spec) {}

    u32 mem_size(PPUContext&) override { return spec_.mem_size; }
    s32 pcm_handle_num(PPUContext&) override { return 4; }
    u32 bsi_info_size(PPUContext&) override { return 16; }
    u32 version(PPUContext&) override { return spec_.version; }

    // openExt (open: no SPURS resource): the notifications AUDONE, PCMOUT and ERROR and the resource needed, the
    // priorities checked; the context set up, the thread made.
    s32 open(PPUContext& ctx, u32 h, const CoreNotify& n, u32 res, u32 spurs_res) override
    {
        if (!n.fn[0] || !n.fn[1] || !n.fn[2] || !res) return error(spec_.arg);
        u8* base = guest_base(ctx);
        if (!guest_readable(thread(ctx), res, 20)) {
            log_call_site(ctx, base, std::format("{} core: the resource ({:#x}) is not guest memory", spec_.name, res));
            return error(spec_.arg);
        }
        if (ps_read32(base, res + 8) > 0xBFF || ps_read32(base, res + 12) > 0xFF) return error(spec_.arg);
        const u32 c = align(h);
        if (!guest_readable(thread(ctx), c, spec_.mem_size)) {
            log_call_site(ctx, base, std::format("{} core: its memory ({:#x}) is not guest memory", spec_.name, h));
            return error(spec_.arg);
        }
        ps_write32(base, c + cx::decodes, 0);
        ps_write32(base, c + spec_.spurs, spurs_res && guest_readable(thread(ctx), spurs_res, 4) ? ps_read32(base, spurs_res)
                                                                                                : c + spec_.own_spurs);
        ps_write32(base, c + cx::state, 2);
        reset_pts(base, spec_, c);
        store_notify(base, c, spec_.slots(), n);
        ps_write32(base, c + cx::q_count, 0);
        ps_write32(base, c + cx::q_read, 0);
        ps_write32(base, c + cx::q_write, 0);
        for (u32 i = 0; i < 4; i++) ps_write32(base, c + cx::queue + cmd::size * i + cmd::pcm_handle, kFree);
        s32 r = make_mutex(ctx, spec_, c + cx::q_mutex);
        if (r == lv2::CELL_OK) r = make_cond(ctx, spec_, c + cx::q_cond, c + cx::q_mutex);
        if (r == lv2::CELL_OK) r = make_mutex(ctx, spec_, c + spec_.job_mutex);
        if (r == lv2::CELL_OK) r = make_cond(ctx, spec_, c + spec_.job_cond, c + spec_.job_mutex);
        if (r != lv2::CELL_OK) return r;
        lock(ctx, c + spec_.job_mutex);
        ps_write32(base, c + spec_.job_busy, 0);
        signal(ctx, c + spec_.job_cond);
        unlock(ctx, c + spec_.job_mutex);
        const u32 entry = HleProcess::current()->link_function(spec_.module, recomp::nid_of(spec_.thread_fn),
            spec_.thread_fn);
        if (!entry) {
            PS_LOG_ERROR(kChan, "{} core: no memory to link its thread's function", spec_.name);
            return error(spec_.core_fatal);
        }
        // The input thread (CellAdecResource: ppuThreadPriority +8, ppuThreadStackSize +16), joinable.
        HleProcess& process = *HleProcess::current();
        lv2::PpuThread& t = thread(ctx);
        const u32 tls = process.alloc_tls();
        ps_write32(base, c + spec_.tls, tls);
        StackVar sv(ctx, base, 0x40);
        ps_write32(base, sv.addr() + 8, entry);
        ps_write32(base, sv.addr() + 12, tls ? tls + kTlsR13Offset : 0);
        std::memcpy(base + sv.addr() + 0x10, spec_.thread_name, std::strlen(spec_.thread_name) + 1);
        r = lv2::_sys_ppu_thread_create(t, sv.addr(), sv.addr() + 8, c, 0, static_cast<s32>(ps_read32(base, res + 8)),
            ps_read32(base, res + 16), 1, sv.addr() + 0x10);
        if (r == lv2::CELL_OK) {
            ps_write64(base, c + cx::thread_id, ps_read64(base, sv.addr()));
            r = lv2::sys_ppu_thread_start(t, static_cast<u32>(ps_read64(base, sv.addr())));
        }
        if (r != lv2::CELL_OK && tls) {
            process.free_tls(tls);
            ps_write32(base, c + spec_.tls, 0);
        }
        return r;
    }

    s32 close(PPUContext& ctx, u32 h) override
    {
        u8* base = guest_base(ctx);
        const u32 c = align(h);
        lock(ctx, c + spec_.job_mutex);
        while (ps_read32(base, c + spec_.job_busy) == 1) wait(ctx, c + spec_.job_cond);
        if (ps_read32(base, c + cx::state) <= 1) {
            unlock(ctx, c + spec_.job_mutex);
            return error(spec_.seq);
        }
        ps_write32(base, c + cx::state, 1);
        unlock(ctx, c + spec_.job_mutex);
        lock(ctx, c + cx::q_mutex);
        signal(ctx, c + cx::q_cond);
        unlock(ctx, c + cx::q_mutex);
        lv2::PpuThread& t = thread(ctx);
        StackVar status(ctx, base, 8);
        s32 r = lv2::sys_ppu_thread_join(t, static_cast<u32>(ps_read64(base, c + cx::thread_id)), status.addr());
        if (r != lv2::CELL_OK) return r;
        if (const u32 tls = ps_read32(base, c + spec_.tls)) HleProcess::current()->free_tls(tls);
        ps_write32(base, c + spec_.tls, 0);
        ps_write32(base, c + cx::state, 0);
        r = lv2::sys_cond_destroy(t, ps_read32(base, c + cx::q_cond));
        if (r == lv2::CELL_OK) r = lv2::sys_cond_destroy(t, ps_read32(base, c + spec_.job_cond));
        if (r == lv2::CELL_OK) r = lv2::sys_mutex_destroy(t, ps_read32(base, c + cx::q_mutex));
        if (r == lv2::CELL_OK) r = lv2::sys_mutex_destroy(t, ps_read32(base, c + spec_.job_mutex));
        if (r != lv2::CELL_OK) return r;
        // The SPU program closes: its decoders dropped.
        spu_end(c);
        drop_decoders(c);
        return lv2::CELL_OK;
    }

    s32 start_seq(PPUContext& ctx, u32 h, u32 param) override
    {
        u8* base = guest_base(ctx);
        if (!guest_readable(thread(ctx), param, 16)) {
            log_call_site(ctx, base, std::format("{} core: the parameters ({:#x}) are not guest memory", spec_.name, param));
            return error(spec_.arg);
        }
        if (!valid_param(spec_, ps_read32(base, param), ps_read32(base, param + 4), ps_read32(base, param + 8),
                ps_read32(base, param + 12)))
            return error(spec_.arg);
        const u32 c = align(h);
        const s32 r = queue_command(ctx, c, spec_, [&](u8* b, u32 e) {
            ps_write32(b, e + cmd::type, START);
            ps_write32(b, e + cmd::pcm_handle, kNoPcm);
            std::memcpy(b + e + cmd::param, b + param, 16);
        });
        if (r != lv2::CELL_OK) return r;
        std::memcpy(base + c + cx::start_param, base + param, 16);
        unlock(ctx, c + cx::q_mutex);
        return lv2::CELL_OK;
    }

    s32 end_seq(PPUContext& ctx, u32 h) override
    {
        const u32 c = align(h);
        const s32 r = queue_command(ctx, c, spec_, [&](u8* b, u32 e) {
            ps_write32(b, e + cmd::type, END);
            ps_write32(b, e + cmd::pcm_handle, kNoPcm);
        });
        if (r != lv2::CELL_OK) return r;
        unlock(ctx, c + cx::q_mutex);
        return lv2::CELL_OK;
    }

    // DecodeAu: the access unit's size the configuration's frame (the last StartSeq's), its user data's decoder one of
    // the SPU program's; then queued.
    s32 decode_au(PPUContext& ctx, u32 h, s32 pcm_handle, u32 au_info) override
    {
        u8* base = guest_base(ctx);
        if (!guest_readable(thread(ctx), au_info, 0x18)) {
            log_call_site(ctx, base, std::format("{} core: the access unit's information ({:#x}) is not guest memory",
                spec_.name, au_info));
            return error(spec_.arg);
        }
        const u32 c = align(h);
        const u32 config = ps_read32(base, c + cx::start_param + 8);
        const u32 au = ps_read32(base, au_info), au_size = ps_read32(base, au_info + 4);
        const u32 expected = au_bytes(spec_, config);
        if (!expected || au_size != expected) return error(spec_.arg);
        const u64 user = ps_read64(base, au_info + 16);
        if ((user & 0x7F) >= spec_.decoders) return error(spec_.arg);
        const s32 r = queue_command(ctx, c, spec_, [&](u8* b, u32 e) {
            ps_write32(b, e + cmd::decoder, static_cast<u32>(user & 0x7F));
            ps_write32(b, e + cmd::type, user & 0x80 ? DECODE_RESET : DECODE);
            ps_write32(b, e + cmd::pcm_handle, static_cast<u32>(pcm_handle));
            ps_write32(b, e + cmd::au_addr, au);
            ps_write32(b, e + cmd::au_size, au_size);
            ps_write32(b, e + cmd::mute, static_cast<u32>((user >> 8) & 0xFF));
        });
        if (r != lv2::CELL_OK) return r;
        unlock(ctx, c + cx::q_mutex);
        return lv2::CELL_OK;
    }

    // The PCM out of its slot: as many bytes as the slot's information says (its configuration and word size).
    s32 realign(PPUContext& ctx, u32 h, u32 out, u32 pcm) override
    {
        u8* base = guest_base(ctx);
        if (!pcm) return error(spec_.arg);
        if (!ps_read32(base, align(h) + cx::state)) return error(spec_.seq);
        if (!out) return lv2::CELL_OK;
        if (!guest_readable(thread(ctx), pcm + spec_.info, 16)) {
            log_call_site(ctx, base, std::format("{} core: the PCM ({:#x}) is not guest memory", spec_.name, pcm));
            return error(spec_.arg);
        }
        const u32 bytes = pcm_bytes(spec_, ps_read32(base, pcm + spec_.info + 8), ps_read32(base, pcm + spec_.info + 12));
        if (!guest_readable(thread(ctx), out, bytes) || !guest_readable(thread(ctx), pcm, bytes)) {
            log_call_site(ctx, base, std::format("{} core: the PCM's copy ({:#x} to {:#x}, {} bytes) is not guest "
                "memory", spec_.name, pcm, out, bytes));
            return error(spec_.arg);
        }
        std::memmove(base + out, base + pcm, bytes);
        return lv2::CELL_OK;
    }

    s32 release_pcm(PPUContext&, u32, s32, u32) override { return lv2::CELL_OK; }

    // The PTS function: the last access unit's decoder's slot. A PTS given is kept there a frame on; one not given
    // (both words all ones) is taken from the slot (and the slot stepped on when it held one).
    s32 correct_pts(PPUContext& ctx, u32 h, u32 au_pts) override
    {
        u8* base = guest_base(ctx);
        if (!guest_readable(thread(ctx), au_pts, 8)) {
            log_call_site(ctx, base, std::format("{} core: the PTS ({:#x}) is not guest memory", spec_.name, au_pts));
            return error(CELL_ADEC_ERROR_FATAL);
        }
        const u32 c = align(h);
        const u32 slot = c + cx::pts + 8 * (ps_read32(base, c + cx::channel) & (spec_.decoders - 1));
        u32 upper = ps_read32(base, au_pts), lower = ps_read32(base, au_pts + 4);
        if (upper == 0xFFFFFFFF) {
            if (lower != 0xFFFFFFFF) {
                ps_write32(base, slot, upper);
                ps_write32(base, slot + 4, lower);
                return lv2::CELL_OK;
            }
            upper = ps_read32(base, slot);
            lower = ps_read32(base, slot + 4);
            ps_write32(base, au_pts, upper);
            ps_write32(base, au_pts + 4, lower);
            if (upper == 0xFFFFFFFF) {
                ps_write32(base, slot, upper);
                ps_write32(base, slot + 4, lower);
                return lv2::CELL_OK;
            }
        }
        const u32 next = lower + pts_step(spec_, ps_read32(base, c + cx::param + 8));
        ps_write32(base, slot, next < lower ? (upper + 1) & 1 : upper);
        ps_write32(base, slot + 4, next);
        return lv2::CELL_OK;
    }

private:
    const Spec& spec_;
};

// ---- the modules' exports: the operation tables and the PTS functions, for Sony's libadec and other modules

template <const Spec& S>
CelpCore& core()
{
    static CelpCore c(S);
    return c;
}

template <const Spec& S>
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (addr && guest_readable(thread(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{}: {} ({:#x}) is not guest memory", S.ops_variable, what, addr));
    return false;
}

// getMemSize(attr): CellAdecAttr's workMemSize and adecVerLower (its adecVerUpper as it was).
template <const Spec& S>
s32 op_get_mem_size(PPUContext& ctx, u32 attr)
{
    if (!usable<S>(ctx, attr, 12, "the attributes")) return error(S.arg);
    ps_write32(guest_base(ctx), attr, S.mem_size);
    ps_write32(guest_base(ctx), attr + 8, S.version);
    return lv2::CELL_OK;
}

template <const Spec& S>
s32 op_open_ext(PPUContext& ctx, u32 h, u32 au_done, u32 au_done_arg, u32 pcm_out, u32 pcm_out_arg, u32 err,
    u32 err_arg, u32 seq_done, u32 seq_done_arg, u32 res, u32 spurs_res)
{
    const CoreNotify n{{au_done, pcm_out, err, seq_done}, {au_done_arg, pcm_out_arg, err_arg, seq_done_arg}, false};
    return core<S>().open(ctx, h, n, res, spurs_res);
}

template <const Spec& S>
s32 op_open(PPUContext& ctx, u32 h, u32 au_done, u32 au_done_arg, u32 pcm_out, u32 pcm_out_arg, u32 err, u32 err_arg,
    u32 seq_done, u32 seq_done_arg, u32 res)
{
    return op_open_ext<S>(ctx, h, au_done, au_done_arg, pcm_out, pcm_out_arg, err, err_arg, seq_done, seq_done_arg, res,
        0);
}

template <const Spec& S>
s32 op_close(PPUContext& ctx, u32 h)
{
    return usable<S>(ctx, h, 4, "the core") ? core<S>().close(ctx, h) : error(S.arg);
}
template <const Spec& S>
s32 op_start_seq(PPUContext& ctx, u32 h, u32 param)
{
    return usable<S>(ctx, h, 4, "the core") ? core<S>().start_seq(ctx, h, param) : error(S.arg);
}
template <const Spec& S>
s32 op_end_seq(PPUContext& ctx, u32 h)
{
    return usable<S>(ctx, h, 4, "the core") ? core<S>().end_seq(ctx, h) : error(S.arg);
}
template <const Spec& S>
s32 op_decode_au(PPUContext& ctx, u32 h, s32 pcm_handle, u32 au_info)
{
    return usable<S>(ctx, h, 4, "the core") ? core<S>().decode_au(ctx, h, pcm_handle, au_info) : error(S.arg);
}
template <const Spec& S>
void op_get_version(PPUContext& ctx, u32 version)
{
    if (usable<S>(ctx, version, 4, "the version")) ps_write32(guest_base(ctx), version, S.version);
}
template <const Spec& S>
s32 op_realign(PPUContext& ctx, u32 h, u32 out, u32 pcm)
{
    return usable<S>(ctx, h, 4, "the core") ? core<S>().realign(ctx, h, out, pcm) : error(S.arg);
}
s32 op_release_pcm(PPUContext&, u32, s32, u32) { return lv2::CELL_OK; }
s32 op_get_pcm_handle_num(PPUContext&) { return 4; }
u32 op_get_bsi_info_size(PPUContext&) { return 16; }

template <const Spec& S>
s32 correct_pts_value(PPUContext& ctx, u32 h, u32 pts)
{
    return usable<S>(ctx, h, 4, "the core") ? core<S>().correct_pts(ctx, h, pts) : error(CELL_ADEC_ERROR_FATAL);
}

s32 _SceAdecCorrectPtsValue_Celp(PPUContext& ctx, u32 h, u32 pts) { return correct_pts_value<kCelp16>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_Celp8(PPUContext& ctx, u32 h, u32 pts) { return correct_pts_value<kCelp8>(ctx, h, pts); }

// CellAdecCoreOps's entries, in order: each library's internal functions.
template <const Spec& S>
void init_ops(u8* base, u32 at)
{
    HleProcess& p = *HleProcess::current();
    for (u32 op = 0; op < 12; op++)
        ps_write32(base, at + 4 * op, p.link_function(S.module, recomp::nid_of(S.op_names[op]), S.op_names[op]));
}

template <const Spec& S>
void register_core(Registry& r, u32 table_nid, HleFn pts_fn, u32 pts_nid, std::string_view pts_name, HleFn thread_fn)
{
    const HleFn fns[12] = {&detail::thunk<&op_get_mem_size<S>>, &detail::thunk<&op_open<S>>,
        &detail::thunk<&op_close<S>>, &detail::thunk<&op_start_seq<S>>, &detail::thunk<&op_end_seq<S>>,
        &detail::thunk<&op_decode_au<S>>, &detail::thunk<&op_get_version<S>>, &detail::thunk<&op_realign<S>>,
        &detail::thunk<&op_release_pcm>, &detail::thunk<&op_get_pcm_handle_num>,
        &detail::thunk<&op_get_bsi_info_size>, &detail::thunk<&op_open_ext<S>>};
    for (u32 op = 0; op < 12; op++)
        r.add(S.module, {Function{{}, S.op_names[op], recomp::nid_of(S.op_names[op]), fns[op], true}});
    r.add(S.module, {
        Function{{}, pts_name, pts_nid, pts_fn, false},
        Function{{}, S.thread_fn, recomp::nid_of(S.thread_fn), thread_fn, true},
    });
    r.add_variables(S.module, {Variable{{}, S.ops_variable, table_nid, 12 * 4, 4, &init_ops<S>}});
}

} // namespace
} // namespace celpcore

std::unique_ptr<AdecCore> make_celp_core() { return std::make_unique<celpcore::CelpCore>(celpcore::kCelp16); }
std::unique_ptr<AdecCore> make_celp8_core() { return std::make_unique<celpcore::CelpCore>(celpcore::kCelp8); }

void _pspocket_celp_dec_thread(PPUContext& ctx, u64 core_handle)
{
    celpcore::run(ctx, celpcore::kCelp16, static_cast<u32>(core_handle));
    lv2::_sys_ppu_thread_exit(thread_of(ctx), 0);
}

void _pspocket_celp8_dec_thread(PPUContext& ctx, u64 core_handle)
{
    celpcore::run(ctx, celpcore::kCelp8, static_cast<u32>(core_handle));
    lv2::_sys_ppu_thread_exit(thread_of(ctx), 0);
}

} // namespace pspocket::hle::adec

namespace pspocket::hle {

// libcelpdec's and libcelp8dec's exports (D-932): each one's operation table (a variable: twelve descriptors of
// internal functions) and PTS function; and each core's thread function.
void register_cellCelpDec(Registry& r)
{
    using namespace adec::celpcore;
    register_core<kCelp16>(r, 0xFFE42C22, &detail::thunk<&_SceAdecCorrectPtsValue_Celp>, 0xE4B8FC4D,
        "_SceAdecCorrectPtsValue_Celp", &detail::thunk<&adec::_pspocket_celp_dec_thread>);
}

void register_cellCelp8Dec(Registry& r)
{
    using namespace adec::celpcore;
    register_core<kCelp8>(r, 0xF0190C6C, &detail::thunk<&_SceAdecCorrectPtsValue_Celp8>, 0x881583C3,
        "_SceAdecCorrectPtsValue_Celp8", &detail::thunk<&adec::_pspocket_celp8_dec_thread>);
}

} // namespace pspocket::hle
