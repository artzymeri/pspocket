// pspocket's own CELP cores for cellAdec (D-932): libcelpdec's (cellCelpDec: MPEG-4 CELP at 16 kHz, regular pulse
// excitation), made to answer, notify and write what Sony's core does, compared with it under cellAdec
// (tests/hle/test_celp_dec_compare.cpp). They are also the modules' exports: the operation table
// (g_cell_adec_core_ops_Celp) and the PTS function (_SceAdecCorrectPtsValue_Celp), for Sony's libadec and others to
// call (as D-591's tables).
//
// What libcelpdec is: a PPU side, an input thread ("_CelpDecInputThrFunc") and an SPU program (a SPURS task) that
// decodes. Its memory (0x3CC00 bytes, from the core's handle rounded up to 128 bytes) holds a queue of four commands,
// the thread's mutexes and conditions, the sequence's parameters, 64 PTS slots, the SPURS objects and four PCM slots
// of 0x400 bytes (960 bytes of samples, then 16 bytes of information: the parameters in force). StartSeq, EndSeq and
// DecodeAu queue a command (BUSY when four wait); the thread takes them one at a time and sends each to the SPU
// program, which keeps 64 decoders: an access unit's user data chooses one (its low seven bits, at most 63), bit 7
// resets that decoder (and its PTS slot) first, bits 8 to 15 mute the frame's largest excitation samples. StartSeq
// makes all 64 decoders for the configuration; EndSeq drops them. After each access unit the thread notifies AUDONE and
// PCMOUT (PTS correction type 24: _SceAdecCorrectPtsValue_Celp, which steps that decoder's PTS slot by a frame, 1350
// or 900 ticks of 90 kHz), after ERROR when the unit failed (SEQ before StartSeq; the SPU program's errors). Here the
// SPU program's decoding is done on the host, bit for bit (celp_rpe.cpp); the PPU side is libcelpdec's.

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

// libcelpdec's context, from the core's handle rounded up to 128 bytes.
namespace cx {
constexpr u32 queue = 0x00;                                // four commands of 0x30 bytes
constexpr u32 q_read = 0xC0, q_write = 0xC4, q_count = 0xC8;
constexpr u32 thread_id = 0xD0;                            // u64
constexpr u32 q_mutex = 0xD8, q_cond = 0xDC;
constexpr u32 state = 0xE0;                                // 0 closed, 1 closing, 2 open, 3 started, 4 decoding
constexpr u32 decodes = 0xE4;                              // access units taken: the next PCM slot
constexpr u32 notify = 0xE8;                               // AUDONE, PCMOUT, ERROR, SEQDONE (function, argument)
constexpr u32 param = 0x108;                               // the sequence's parameters in force (CellAdecParamCelp)
constexpr u32 start_param = 0x118;                         // the last StartSeq's
constexpr u32 channel = 0x128;                             // the last access unit's decoder
constexpr u32 pts = 0x12C;                                 // each decoder's next PTS (upper, lower)
constexpr u32 job_mutex = 0x32C, job_cond = 0x330, job_busy = 0x334;
constexpr u32 spurs = 0x338;
// pspocket's own words, where libcelpdec has its SPURS instance (which pspocket does not make).
constexpr u32 notify_guest = 0x380, tls = 0x384;
constexpr u32 pcm = 0x3BA80, pcm_slot = 0x400, info = 0x3C0;
constexpr u32 size = 0x3CC00;
} // namespace cx
// A command: the decoder (+0), the type (+4: start 0, end 1, decode 2, decode after a reset 3), the PCM handle (+8:
// 0xFF a free entry, 4 for a start or end), the access unit (+0xC, +0x10), the mute byte (+0x1C), a start's parameters
// (+0x20).
namespace cmd {
constexpr u32 decoder = 0x00, type = 0x04, pcm_handle = 0x08, au_addr = 0x0C, au_size = 0x10, mute = 0x1C,
              param = 0x20, size = 0x30;
}
enum : u32 { START = 0, END = 1, DECODE = 2, DECODE_RESET = 3 };
constexpr u32 kFree = 0xFF, kNoPcm = 4, kDecoders = 64;
constexpr NotifySlots kSlots{cx::notify, cx::notify_guest};
constexpr s8 kCorrectPtsCelp = 24;

// What differs between the 16 kHz core and (later) the 8 kHz one.
struct Spec {
    std::string_view name;
    u32 version;
    u32 arg, seq, busy, core_fatal, core_seq; // the core's own error codes
    std::array<u32, 4> au_bytes, pcm16_bytes; // by configuration
    std::array<u32, 4> pts_step;
};
constexpr Spec kCelp16{"CELP", 0x02500000, 0x80612E03, 0x80612E04, 0x80612E01, 0x80612E81, 0x80612E83,
    {27, 20, 35, 43}, {480, 320, 480, 480}, {1350, 900, 1350, 1350}};

u32 align(u32 h) { return (h + 0x7F) & ~0x7Fu; }
lv2::PpuThread& thread(PPUContext& ctx) { return thread_of(ctx); }

// ---- the host's decoders: the SPU program's 64, for each open core

struct Decoders {
    std::array<std::unique_ptr<celp::RpeDecoder>, kDecoders> rpe;
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

// ---- lv2 objects, as libcelpdec makes them ("_cedm01", "_cedc01")

s32 make_mutex(PPUContext& ctx, u32 id_out)
{
    u8* base = guest_base(ctx);
    StackVar attr(ctx, base, 0x28);
    ps_write32(base, attr.addr() + 0x00, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, attr.addr() + 0x04, lv2::SYS_SYNC_NOT_RECURSIVE);
    ps_write32(base, attr.addr() + 0x08, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    ps_write32(base, attr.addr() + 0x0C, lv2::SYS_SYNC_NOT_ADAPTIVE);
    std::memcpy(base + attr.addr() + 0x20, "_cedm01", 8);
    return lv2::sys_mutex_create(thread(ctx), id_out, attr.addr());
}

s32 make_cond(PPUContext& ctx, u32 id_out, u32 mutex_at)
{
    u8* base = guest_base(ctx);
    StackVar attr(ctx, base, 0x18);
    ps_write32(base, attr.addr() + 0x00, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    std::memcpy(base + attr.addr() + 0x10, "_cedc01", 8);
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
void begin_job(PPUContext& ctx, u32 c)
{
    u8* base = guest_base(ctx);
    lock(ctx, c + cx::job_mutex);
    while (ps_read32(base, c + cx::job_busy) == 1) wait(ctx, c + cx::job_cond);
    ps_write32(base, c + cx::job_busy, 1);
    unlock(ctx, c + cx::job_mutex);
}

void end_job(PPUContext& ctx, u32 c)
{
    lock(ctx, c + cx::job_mutex);
    ps_write32(guest_base(ctx), c + cx::job_busy, 0);
    signal_all(ctx, c + cx::job_cond);
    unlock(ctx, c + cx::job_mutex);
}

// A notification's function, when open was given one (libcelpdec calls each only then).
bool has_notify(u8* base, u32 c, u32 i) { return ps_read32(base, c + cx::notify + 8 * i) != 0; }

// ---- the SPU program's work, on the host

// Start: the 64 decoders made for the parameters.
s32 spu_start(u32 c, u32 config)
{
    const std::shared_ptr<Decoders> d = decoders_of(c, true);
    celp::spu::Scope scope;
    for (auto& r : d->rpe) {
        if (!r) r = std::make_unique<celp::RpeDecoder>();
        r->reset(config);
    }
    return 0;
}

// End: the decoders dropped.
s32 spu_end(u32 c)
{
    if (const std::shared_ptr<Decoders> d = decoders_of(c))
        for (auto& r : d->rpe) r.reset();
    return 0;
}

// A frame: decoded by decoder `index` (made again first for a reset) into the PCM slot, as 16-bit little-endian
// samples or big-endian floats (full scale 1). 0, -2 (no such decoder) or -1 (the frame did not decode).
s32 spu_decode(u8* base, u32 c, u32 index, bool reset, u32 config, u32 word_size, u32 mute, const u8* au, u32 au_size,
    u32 out)
{
    const std::shared_ptr<Decoders> d = decoders_of(c);
    if (!d) return -2;
    celp::spu::Scope scope;
    std::unique_ptr<celp::RpeDecoder>& r = d->rpe[index & (kDecoders - 1)];
    if (reset) {
        if (!r) r = std::make_unique<celp::RpeDecoder>();
        r->reset(config);
    }
    if (!r) return -2;
    float y[240];
    u32 consumed = 0;
    if (!r->decode(au, au_size, &consumed, y, mute)) return -1;
    using namespace celp::spu;
    const float scale = from_bits(0x38000107u); // about 1/32767.75
    for (u32 n = 0; n < r->cfg.frame_size; n++) {
        float v = y[n];
        if (fcgt(v, 32767.0f)) v = 32767.0f;
        if (fcgt(-32768.0f, v)) v = -32768.0f;
        if (word_size) {
            const float f = fm(v, scale);
            ps_write32(base, out + 4 * n, bits(fcgt(f, 1.0f) ? 1.0f : fcgt(-1.0f, f) ? -1.0f : f));
        } else {
            s32 s = cflts(y[n]);
            s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
            ps_write8(base, out + 2 * n, static_cast<u8>(s & 0xFF));
            ps_write8(base, out + 2 * n + 1, static_cast<u8>((s >> 8) & 0xFF));
        }
    }
    return 0;
}

// The SPU program's answer as libcelpdec reads it: 0 done, -2 SEQ, anything else FATAL.
s32 answer(const Spec& spec, s32 r) { return r == 0 ? lv2::CELL_OK : r == -2 ? error(spec.core_seq) : error(spec.core_fatal); }

// ---- the input thread (libcelpdec's)

const Spec& spec_of(u8*, u32) { return kCelp16; }

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

void run(PPUContext& ctx, u32 c)
{
    u8* base = guest_base(ctx);
    const Spec& spec = spec_of(base, c);
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
        const CoreNotifier notify(ctx, c, kSlots);
        if (type & 2) {
            // An access unit into the next PCM slot, its information the parameters in force.
            const u32 n = ps_read32(base, c + cx::decodes);
            ps_write32(base, c + cx::decodes, n + 1);
            const u32 out = c + cx::pcm + cx::pcm_slot * (n & 3), info = out + cx::info;
            const u32 config = ps_read32(base, c + cx::param + 8), word_size = ps_read32(base, c + cx::param + 12);
            const u32 bytes = spec.pcm16_bytes[config & 3] * (word_size + 1);
            begin_job(ctx, c);
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
                err = answer(spec, readable ? spu_decode(base, c, index, reset, config, word_size,
                                                  ps_ld32(entry + cmd::mute), frame, au_size, out)
                                            : -1);
                size = err == lv2::CELL_OK ? bytes : 0;
            }
            std::memcpy(base + info, base + c + cx::param, 16);
            ps_write32(base, c + cx::channel, index);
            end_job(ctx, c);
            if (err != lv2::CELL_OK && has_notify(base, c, 2)) notify.error(err);
            if (has_notify(base, c, 0)) notify.au_done(pcm_handle);
            if (has_notify(base, c, 1)) notify.pcm_out(pcm_handle, out, size, info, kCorrectPtsCelp, err);
            continue;
        }
        begin_job(ctx, c);
        if (type == START) {
            for (u32 i = 0; i < kDecoders; i++) {
                ps_write32(base, c + cx::pts + 8 * i, 0xFFFFFFFF);
                ps_write32(base, c + cx::pts + 8 * i + 4, 0xFFFFFFFF);
            }
            std::memcpy(base + c + cx::param, entry + cmd::param, 16);
            const s32 err = answer(spec, spu_start(c, ps_ld32(entry + cmd::param + 8) & 3));
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
        end_job(ctx, c);
    }
}

// ---- the operations (CellAdecCoreOps), libcelpdec's

class CelpCore final : public AdecCore {
public:
    explicit CelpCore(const Spec& spec) : spec_(spec) {}

    u32 mem_size(PPUContext&) override { return cx::size; }
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
            log_call_site(ctx, base, std::format("CELP core: the resource ({:#x}) is not guest memory", res));
            return error(spec_.arg);
        }
        if (ps_read32(base, res + 8) > 0xBFF || ps_read32(base, res + 12) > 0xFF) return error(spec_.arg);
        const u32 c = align(h);
        if (!guest_readable(thread(ctx), c, cx::size)) {
            log_call_site(ctx, base, std::format("CELP core: its memory ({:#x}) is not guest memory", h));
            return error(spec_.arg);
        }
        ps_write32(base, c + cx::decodes, 0);
        ps_write32(base, c + cx::spurs, spurs_res && guest_readable(thread(ctx), spurs_res, 4) ? ps_read32(base, spurs_res)
                                                                                              : c + 0x380);
        ps_write32(base, c + cx::state, 2);
        for (u32 i = 0; i < kDecoders; i++) {
            ps_write32(base, c + cx::pts + 8 * i, 0xFFFFFFFF);
            ps_write32(base, c + cx::pts + 8 * i + 4, 0xFFFFFFFF);
        }
        store_notify(base, c, kSlots, n);
        ps_write32(base, c + cx::q_count, 0);
        ps_write32(base, c + cx::q_read, 0);
        ps_write32(base, c + cx::q_write, 0);
        for (u32 i = 0; i < 4; i++) ps_write32(base, c + cx::queue + cmd::size * i + cmd::pcm_handle, kFree);
        s32 r = make_mutex(ctx, c + cx::q_mutex);
        if (r == lv2::CELL_OK) r = make_cond(ctx, c + cx::q_cond, c + cx::q_mutex);
        if (r == lv2::CELL_OK) r = make_mutex(ctx, c + cx::job_mutex);
        if (r == lv2::CELL_OK) r = make_cond(ctx, c + cx::job_cond, c + cx::job_mutex);
        if (r != lv2::CELL_OK) return r;
        lock(ctx, c + cx::job_mutex);
        ps_write32(base, c + cx::job_busy, 0);
        signal(ctx, c + cx::job_cond);
        unlock(ctx, c + cx::job_mutex);
        const u32 entry = HleProcess::current()->link_function("cellCelpDec", recomp::nid_of("_pspocket_celp_dec_thread"),
            "_pspocket_celp_dec_thread");
        if (!entry) {
            PS_LOG_ERROR(kChan, "CELP core: no memory to link its thread's function");
            return error(spec_.core_fatal);
        }
        // "_CelpDecInputThrFunc" (CellAdecResource: ppuThreadPriority +8, ppuThreadStackSize +16), joinable.
        HleProcess& process = *HleProcess::current();
        lv2::PpuThread& t = thread(ctx);
        const u32 tls = process.alloc_tls();
        ps_write32(base, c + cx::tls, tls);
        StackVar sv(ctx, base, 0x40);
        ps_write32(base, sv.addr() + 8, entry);
        ps_write32(base, sv.addr() + 12, tls ? tls + kTlsR13Offset : 0);
        std::memcpy(base + sv.addr() + 0x10, "_CelpDecInputThrFunc", 21);
        r = lv2::_sys_ppu_thread_create(t, sv.addr(), sv.addr() + 8, c, 0, static_cast<s32>(ps_read32(base, res + 8)),
            ps_read32(base, res + 16), 1, sv.addr() + 0x10);
        if (r == lv2::CELL_OK) {
            ps_write64(base, c + cx::thread_id, ps_read64(base, sv.addr()));
            r = lv2::sys_ppu_thread_start(t, static_cast<u32>(ps_read64(base, sv.addr())));
        }
        if (r != lv2::CELL_OK && tls) {
            process.free_tls(tls);
            ps_write32(base, c + cx::tls, 0);
        }
        return r;
    }

    s32 close(PPUContext& ctx, u32 h) override
    {
        u8* base = guest_base(ctx);
        const u32 c = align(h);
        lock(ctx, c + cx::job_mutex);
        while (ps_read32(base, c + cx::job_busy) == 1) wait(ctx, c + cx::job_cond);
        if (ps_read32(base, c + cx::state) <= 1) {
            unlock(ctx, c + cx::job_mutex);
            return error(spec_.seq);
        }
        ps_write32(base, c + cx::state, 1);
        unlock(ctx, c + cx::job_mutex);
        lock(ctx, c + cx::q_mutex);
        signal(ctx, c + cx::q_cond);
        unlock(ctx, c + cx::q_mutex);
        lv2::PpuThread& t = thread(ctx);
        StackVar status(ctx, base, 8);
        s32 r = lv2::sys_ppu_thread_join(t, static_cast<u32>(ps_read64(base, c + cx::thread_id)), status.addr());
        if (r != lv2::CELL_OK) return r;
        if (const u32 tls = ps_read32(base, c + cx::tls)) HleProcess::current()->free_tls(tls);
        ps_write32(base, c + cx::tls, 0);
        ps_write32(base, c + cx::state, 0);
        r = lv2::sys_cond_destroy(t, ps_read32(base, c + cx::q_cond));
        if (r == lv2::CELL_OK) r = lv2::sys_cond_destroy(t, ps_read32(base, c + cx::job_cond));
        if (r == lv2::CELL_OK) r = lv2::sys_mutex_destroy(t, ps_read32(base, c + cx::q_mutex));
        if (r == lv2::CELL_OK) r = lv2::sys_mutex_destroy(t, ps_read32(base, c + cx::job_mutex));
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
            log_call_site(ctx, base, std::format("CELP core: the parameters ({:#x}) are not guest memory", param));
            return error(spec_.arg);
        }
        const u32 mode = ps_read32(base, param), rate = ps_read32(base, param + 4);
        const u32 config = ps_read32(base, param + 8), word_size = ps_read32(base, param + 12);
        if (mode != 1 || rate != 2 || config > 3 || word_size > 1) return error(spec_.arg);
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

    // DecodeAu: the access unit's size the configuration's frame (the last StartSeq's), its user data's decoder at
    // most 63; then queued.
    s32 decode_au(PPUContext& ctx, u32 h, s32 pcm_handle, u32 au_info) override
    {
        u8* base = guest_base(ctx);
        if (!guest_readable(thread(ctx), au_info, 0x18)) {
            log_call_site(ctx, base, std::format("CELP core: the access unit's information ({:#x}) is not guest memory",
                au_info));
            return error(spec_.arg);
        }
        const u32 c = align(h);
        const u32 config = ps_read32(base, c + cx::start_param + 8);
        const u32 au = ps_read32(base, au_info), au_size = ps_read32(base, au_info + 4);
        if (config > 3 || au_size != spec_.au_bytes[config]) return error(spec_.arg);
        const u64 user = ps_read64(base, au_info + 16);
        if ((user & 0x7F) > 63) return error(spec_.arg);
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
        if (!guest_readable(thread(ctx), pcm + cx::info, 16)) {
            log_call_site(ctx, base, std::format("CELP core: the PCM ({:#x}) is not guest memory", pcm));
            return error(spec_.arg);
        }
        const u32 bytes = spec_.pcm16_bytes[ps_read32(base, pcm + cx::info + 8) & 3] * (ps_read32(base, pcm + cx::info + 12) + 1);
        if (!guest_readable(thread(ctx), out, bytes) || !guest_readable(thread(ctx), pcm, bytes)) {
            log_call_site(ctx, base, std::format("CELP core: the PCM's copy ({:#x} to {:#x}, {} bytes) is not guest "
                "memory", pcm, out, bytes));
            return error(spec_.arg);
        }
        std::memmove(base + out, base + pcm, bytes);
        return lv2::CELL_OK;
    }

    s32 release_pcm(PPUContext&, u32, s32, u32) override { return lv2::CELL_OK; }

    // _SceAdecCorrectPtsValue_Celp: the last access unit's decoder's slot. A PTS given is kept there a frame on; one not
    // given (both words all ones) is taken from the slot (and the slot stepped on when it held one).
    s32 correct_pts(PPUContext& ctx, u32 h, u32 au_pts) override
    {
        u8* base = guest_base(ctx);
        if (!guest_readable(thread(ctx), au_pts, 8)) {
            log_call_site(ctx, base, std::format("CELP core: the PTS ({:#x}) is not guest memory", au_pts));
            return error(CELL_ADEC_ERROR_FATAL);
        }
        const u32 c = align(h);
        const u32 slot = c + cx::pts + 8 * (ps_read32(base, c + cx::channel) & (kDecoders - 1));
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
        const u32 next = lower + spec_.pts_step[ps_read32(base, c + cx::param + 8) & 3];
        ps_write32(base, slot, next < lower ? (upper + 1) & 1 : upper);
        ps_write32(base, slot + 4, next);
        return lv2::CELL_OK;
    }

private:
    const Spec& spec_;
};

// ---- the module's exports: the operation table and the PTS function, for Sony's libadec and other modules

CelpCore& core16()
{
    static CelpCore core(kCelp16);
    return core;
}

bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (addr && guest_readable(thread(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("g_cell_adec_core_ops_Celp: {} ({:#x}) is not guest memory", what, addr));
    return false;
}

// getMemSize(attr): CellAdecAttr's workMemSize and adecVerLower (its adecVerUpper as it was).
s32 op_get_mem_size(PPUContext& ctx, u32 attr)
{
    if (!usable(ctx, attr, 12, "the attributes")) return error(kCelp16.arg);
    ps_write32(guest_base(ctx), attr, cx::size);
    ps_write32(guest_base(ctx), attr + 8, kCelp16.version);
    return lv2::CELL_OK;
}

s32 op_open_ext(PPUContext& ctx, u32 h, u32 au_done, u32 au_done_arg, u32 pcm_out, u32 pcm_out_arg, u32 err,
    u32 err_arg, u32 seq_done, u32 seq_done_arg, u32 res, u32 spurs_res)
{
    const CoreNotify n{{au_done, pcm_out, err, seq_done}, {au_done_arg, pcm_out_arg, err_arg, seq_done_arg}, false};
    return core16().open(ctx, h, n, res, spurs_res);
}

s32 op_open(PPUContext& ctx, u32 h, u32 au_done, u32 au_done_arg, u32 pcm_out, u32 pcm_out_arg, u32 err, u32 err_arg,
    u32 seq_done, u32 seq_done_arg, u32 res)
{
    return op_open_ext(ctx, h, au_done, au_done_arg, pcm_out, pcm_out_arg, err, err_arg, seq_done, seq_done_arg, res, 0);
}

s32 op_close(PPUContext& ctx, u32 h) { return usable(ctx, h, 4, "the core") ? core16().close(ctx, h) : error(kCelp16.arg); }
s32 op_start_seq(PPUContext& ctx, u32 h, u32 param)
{
    return usable(ctx, h, 4, "the core") ? core16().start_seq(ctx, h, param) : error(kCelp16.arg);
}
s32 op_end_seq(PPUContext& ctx, u32 h) { return usable(ctx, h, 4, "the core") ? core16().end_seq(ctx, h) : error(kCelp16.arg); }
s32 op_decode_au(PPUContext& ctx, u32 h, s32 pcm_handle, u32 au_info)
{
    return usable(ctx, h, 4, "the core") ? core16().decode_au(ctx, h, pcm_handle, au_info) : error(kCelp16.arg);
}
void op_get_version(PPUContext& ctx, u32 version)
{
    if (usable(ctx, version, 4, "the version")) ps_write32(guest_base(ctx), version, kCelp16.version);
}
s32 op_realign(PPUContext& ctx, u32 h, u32 out, u32 pcm)
{
    return usable(ctx, h, 4, "the core") ? core16().realign(ctx, h, out, pcm) : error(kCelp16.arg);
}
s32 op_release_pcm(PPUContext&, u32, s32, u32) { return lv2::CELL_OK; }
s32 op_get_pcm_handle_num(PPUContext&) { return 4; }
u32 op_get_bsi_info_size(PPUContext&) { return 16; }

s32 _SceAdecCorrectPtsValue_Celp(PPUContext& ctx, u32 h, u32 pts)
{
    return usable(ctx, h, 4, "the core") ? core16().correct_pts(ctx, h, pts) : error(CELL_ADEC_ERROR_FATAL);
}

// CellAdecCoreOps's entries, in order, and their internal functions.
constexpr std::string_view kOpNames[12] = {"_CellAdecCoreOpGetMemSize_Celp", "_CellAdecCoreOpOpen_Celp",
    "_CellAdecCoreOpClose_Celp", "_CellAdecCoreOpStartSeq_Celp", "_CellAdecCoreOpEndSeq_Celp",
    "_CellAdecCoreOpDecodeAu_Celp", "_CellAdecCoreOpGetVersion_Celp", "_CellAdecCoreOpRealign_Celp",
    "_CellAdecCoreOpReleasePcm_Celp", "_CellAdecCoreOpGetPcmHandleNum_Celp", "_CellAdecCoreOpGetBsiInfoSize_Celp",
    "_CellAdecCoreOpOpenExt_Celp"};

void init_ops(u8* base, u32 at)
{
    HleProcess& p = *HleProcess::current();
    for (u32 op = 0; op < 12; op++)
        ps_write32(base, at + 4 * op, p.link_function("cellCelpDec", recomp::nid_of(kOpNames[op]), kOpNames[op]));
}

} // namespace
} // namespace celpcore

std::unique_ptr<AdecCore> make_celp_core() { return std::make_unique<celpcore::CelpCore>(celpcore::kCelp16); }

void _pspocket_celp_dec_thread(PPUContext& ctx, u64 core_handle)
{
    celpcore::run(ctx, static_cast<u32>(core_handle));
    lv2::_sys_ppu_thread_exit(thread_of(ctx), 0);
}

} // namespace pspocket::hle::adec

namespace pspocket::hle {

// libcelpdec's exports (D-932): its operation table (a variable: twelve descriptors of internal functions) and its PTS
// function; and the core's thread function.
void register_cellCelpDec(Registry& r)
{
    using namespace adec::celpcore;
    const HleFn fns[12] = {&detail::thunk<&op_get_mem_size>, &detail::thunk<&op_open>, &detail::thunk<&op_close>,
        &detail::thunk<&op_start_seq>, &detail::thunk<&op_end_seq>, &detail::thunk<&op_decode_au>,
        &detail::thunk<&op_get_version>, &detail::thunk<&op_realign>, &detail::thunk<&op_release_pcm>,
        &detail::thunk<&op_get_pcm_handle_num>, &detail::thunk<&op_get_bsi_info_size>, &detail::thunk<&op_open_ext>};
    for (u32 op = 0; op < 12; op++)
        r.add("cellCelpDec", {Function{{}, kOpNames[op], recomp::nid_of(kOpNames[op]), fns[op], true}});
    r.add("cellCelpDec", {
        PS_HLE_FUNC(0xE4B8FC4D, _SceAdecCorrectPtsValue_Celp),
        Function{{}, "_pspocket_celp_dec_thread", recomp::nid_of("_pspocket_celp_dec_thread"),
            &detail::thunk<&adec::_pspocket_celp_dec_thread>, true},
    });
    r.add_variables("cellCelpDec", {Variable{{}, "g_cell_adec_core_ops_Celp", 0xFFE42C22, 12 * 4, 4, &init_ops}});
}

} // namespace pspocket::hle
