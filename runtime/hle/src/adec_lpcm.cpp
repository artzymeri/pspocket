// cellAdec's LPCM core (D-197): linear PCM from PAMF movies, Blu-ray and DVD streams, decoded to big-endian floats.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellAdec.cpp/.h (LpcmDecContext, _CellAdecCoreOp*_lpcm), which reproduces
// the core inside Sony's libadec: its context in the game's memory after the front end's, its command queue, lv2
// mutexes and condition variables, the samples converted and the channels reordered, quirks included. libadec runs the
// conversion as a SPURS task; pspocket, as RPCS3, on a PPU thread of the decoder's ("HLE LPCM decoder"), so no SPURS
// instance is made. The vector loops are written out sample by sample with the same results, including which words
// past the samples they touch.
//
// LpcmDecContext (0x1C8): the command queue (4 commands of 0x2C, front, back, size), the thread, the queue size mutex
// and an unused cond/mutex/cond, run_thread, the four notifications (function, argument), output_locked, the output
// buffer, the parameters, then (LLE: 36 bytes of SPURS command data; pspocket: the thread's TLS block at +0x110 and at
// +0x114 whether the notifications are guest functions, D-591) the output mutex and its "consumed" cond, two semaphores
// (value, mutex, cond), the queue mutex, error_occurred, SPURS queue mutexes, dvd_packing, output_size.

#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "adec.hpp"
#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle::adec {
namespace {

constexpr std::string_view kChan = "cellAdec";

namespace lp {
constexpr u32 kCmdSize = 0x2C, cmd_front = 0xB0, cmd_back = 0xB4, cmd_size = 0xB8, thread_id = 0xC0,
              queue_size_mutex = 0xC8, queue_size_cond = 0xCC, unk_mutex = 0xD0, unk_cond = 0xD4, run_thread = 0xD8,
              notify = 0xDC, output_locked = 0xFC, output = 0x100, lpcm_param = 0x104, tls = 0x110,
              notify_guest = 0x114, output_mutex = 0x130, output_consumed = 0x134, cmd_available = 0x138,
              reserved2 = 0x144, queue_mutex = 0x150, error_occurred = 0x154, spurs_queue_pop_mutex = 0x178,
              spurs_queue_push_mutex = 0x17C, dvd_packing = 0x184, output_size = 0x188, size = 0x1C8;
// LpcmDecCmd: pcm_handle, au_start_addr, au_size, 8 reserved bytes, CellAdecParamLpcm, type, 4 reserved bytes.
constexpr u32 c_pcm_handle = 0x00, c_au_start = 0x04, c_au_size = 0x08, c_param = 0x14, c_type = 0x24;
// LpcmDecSemaphore: value, mutex, cond.
constexpr u32 sem_value = 0, sem_mutex = 4, sem_cond = 8;
} // namespace lp

// CellAdecParamLpcm: channelNumber, sampleRate, sizeOfWord, audioPayloadSize.
struct Param {
    u32 channels = 0, rate = 0, word = 0, payload = 0;
};

enum CmdType : u32 { START_SEQ = 0, END_SEQ = 1, DECODE_AU = 2, CLOSE = 3 };

struct Cmd {
    s32 pcm_handle = 0;
    u32 au_start = 0, au_size = 0, type = 0;
};

constexpr u32 kOutputBufferSize = 0x40000;
constexpr u32 kMemSize = 0x7EBD5; // RPCS3 _CellAdecCoreOpGetMemSize_lpcm, Sony's (tests/hle/test_adec.cpp)

enum : u32 { BIT_LENGTH_16 = 1, BIT_LENGTH_20 = 2, BIT_LENGTH_24 = 3 };
enum : u32 {
    CH_MONO = 1, CH_STEREO = 3, CH_3_0 = 4, CH_2_1 = 5, CH_3_1 = 6, CH_2_2 = 7, CH_3_2 = 8, CH_3_2_LFE = 9, CH_3_4 = 10,
    CH_3_4_LFE = 11,
};
enum : u32 { DVD_CH_MONO = 1, DVD_CH_STEREO = 3, DVD_CH_3_2 = 6, DVD_CH_3_2_LFE = 7, DVD_CH_3_4 = 8, DVD_CH_3_4_LFE = 9 };
enum : u32 { FS_48kHz = 1, FS_96kHz = 3, FS_192kHz = 4 };

lv2::PpuThread& thread(PPUContext& ctx) { return thread_of(ctx); }

// The four notifications (AU done, PCM out, error, sequence done) are (function, argument) pairs from lp::notify.
constexpr NotifySlots kSlots{lp::notify, lp::notify_guest};

Param read_param(u8* base, u32 at)
{
    return {ps_read32(base, at), ps_read32(base, at + 4), ps_read32(base, at + 8), ps_read32(base, at + 12)};
}

void put_name(u8* base, u32 at, std::string_view name)
{
    for (u32 i = 0; i < 8; i++) ps_write8(base, at + i, i < name.size() ? static_cast<u8>(name[i]) : 0);
}

// sys_mutex_create into the guest word `id_out` (priority, not recursive, named).
s32 create_mutex(PPUContext& ctx, u32 id_out, std::string_view name)
{
    u8* base = guest_base(ctx);
    StackVar attr(ctx, base, 0x28);
    ps_write32(base, attr.addr() + 0x00, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, attr.addr() + 0x04, lv2::SYS_SYNC_NOT_RECURSIVE);
    ps_write32(base, attr.addr() + 0x08, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    ps_write32(base, attr.addr() + 0x0C, lv2::SYS_SYNC_NOT_ADAPTIVE);
    put_name(base, attr.addr() + 0x20, name);
    return lv2::sys_mutex_create(thread(ctx), id_out, attr.addr());
}

// sys_cond_create into the guest word `id_out`, on the mutex in the guest word `mutex_at`.
s32 create_cond(PPUContext& ctx, u32 id_out, u32 mutex_at, std::string_view name)
{
    u8* base = guest_base(ctx);
    StackVar attr(ctx, base, 0x18);
    ps_write32(base, attr.addr() + 0x00, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    put_name(base, attr.addr() + 0x10, name);
    return lv2::sys_cond_create(thread(ctx), id_out, ps_read32(base, mutex_at), attr.addr());
}

s32 lock(PPUContext& ctx, u32 h, u32 field) { return lv2::sys_mutex_lock(thread(ctx), ps_read32(guest_base(ctx), h + field), 0); }
s32 unlock(PPUContext& ctx, u32 h, u32 field) { return lv2::sys_mutex_unlock(thread(ctx), ps_read32(guest_base(ctx), h + field)); }

// LpcmDecSemaphore (RPCS3): a count under a mutex, a cond to wait for it.
s32 sem_init(PPUContext& ctx, u32 sem)
{
    u8* base = guest_base(ctx);
    ps_write32(base, sem + lp::sem_value, 0);
    if (const s32 r = create_mutex(ctx, sem + lp::sem_mutex, "_adem01"); r != lv2::CELL_OK) return r;
    return create_cond(ctx, sem + lp::sem_cond, sem + lp::sem_mutex, "_adec01");
}

s32 sem_finalize(PPUContext& ctx, u32 sem)
{
    u8* base = guest_base(ctx);
    if (const s32 r = lv2::sys_cond_destroy(thread(ctx), ps_read32(base, sem + lp::sem_cond)); r != lv2::CELL_OK) return r;
    return lv2::sys_mutex_destroy(thread(ctx), ps_read32(base, sem + lp::sem_mutex));
}

s32 sem_release(PPUContext& ctx, u32 sem)
{
    u8* base = guest_base(ctx);
    if (const s32 r = lock(ctx, sem, lp::sem_mutex); r != lv2::CELL_OK) return r;
    ps_write32(base, sem + lp::sem_value, ps_read32(base, sem + lp::sem_value) + 1);
    if (const s32 r = lv2::sys_cond_signal(thread(ctx), ps_read32(base, sem + lp::sem_cond)); r != lv2::CELL_OK)
        return r; // LLE doesn't unlock the mutex
    return unlock(ctx, sem, lp::sem_mutex);
}

s32 sem_acquire(PPUContext& ctx, u32 sem)
{
    u8* base = guest_base(ctx);
    if (const s32 r = lock(ctx, sem, lp::sem_mutex); r != lv2::CELL_OK) return r;
    if (ps_read32(base, sem + lp::sem_value) == 0) // once, as LLE
        if (const s32 r = lv2::sys_cond_wait(thread(ctx), ps_read32(base, sem + lp::sem_cond), 0); r != lv2::CELL_OK)
            return r; // LLE doesn't unlock the mutex
    ps_write32(base, sem + lp::sem_value, ps_read32(base, sem + lp::sem_value) - 1);
    return unlock(ctx, sem, lp::sem_mutex);
}

s32 release_output(PPUContext& ctx, u32 h)
{
    u8* base = guest_base(ctx);
    if (const s32 r = lock(ctx, h, lp::output_mutex); r != lv2::CELL_OK) return r;
    ps_write32(base, h + lp::output_locked, 0);
    if (const s32 r = lv2::sys_cond_signal(thread(ctx), ps_read32(base, h + lp::output_consumed)); r != lv2::CELL_OK)
        return r; // LLE doesn't unlock the mutex
    return unlock(ctx, h, lp::output_mutex);
}

u32 cmd_at(u32 h, u32 index) { return h + lp::kCmdSize * (index & 3); }

// The queue's size: written under queue_mutex, and read without it by send_command's check that the queue is full,
// which holds queue_size_mutex only (RPCS3 cellAdec.cpp LpcmDecContext::send_command, as LLE: the decoder's thread only
// takes commands out, so a stale size makes one BUSY too many). Relaxed atomics, the PPU's word loads and stores they
// stand for (D-587).
u32 queue_size(u8* base, u32 h)
{
    return ps_bswap32(__atomic_load_n(host_u32(base, h + lp::cmd_size), __ATOMIC_RELAXED));
}
void set_queue_size(u8* base, u32 h, u32 size)
{
    __atomic_store_n(host_u32(base, h + lp::cmd_size), ps_bswap32(size), __ATOMIC_RELAXED);
}

void write_cmd(u8* base, u32 at, const Cmd& c, bool with_handle)
{
    if (with_handle) {
        ps_write32(base, at + lp::c_pcm_handle, static_cast<u32>(c.pcm_handle));
        ps_write32(base, at + lp::c_au_start, c.au_start);
        ps_write32(base, at + lp::c_au_size, c.au_size);
    }
    ps_write32(base, at + lp::c_type, c.type);
}

// RPCS3 LpcmDecContext::send_command: the start's parameters copied into the context, the command queued, the
// thread's semaphore released.
s32 send_command(PPUContext& ctx, u32 h, const Cmd& c, const Param* param)
{
    u8* base = guest_base(ctx);
    if (const s32 r = lock(ctx, h, lp::queue_size_mutex); r != lv2::CELL_OK) return r;
    if (static_cast<s32>(queue_size(base, h)) >= 4) {
        unlock(ctx, h, lp::queue_size_mutex);
        return error(CELL_ADEC_ERROR_BUSY);
    }
    const u32 back = ps_read32(base, h + lp::cmd_back);
    if (param) {
        const u32 p = ps_read32(base, h + lp::lpcm_param);
        const u32 v[4] = {param->channels, param->rate, param->word, param->payload};
        for (u32 i = 0; i < 4; i++) {
            ps_write32(base, p + 4 * i, v[i]);
            ps_write32(base, cmd_at(h, back) + lp::c_param + 4 * i, v[i]);
        }
    }
    if (const s32 r = lock(ctx, h, lp::queue_mutex); r != lv2::CELL_OK) {
        unlock(ctx, h, lp::queue_size_mutex);
        return r;
    }
    write_cmd(base, cmd_at(h, back), c, c.type == DECODE_AU);
    ps_write32(base, h + lp::cmd_back, (back + 1) & 3);
    set_queue_size(base, h, queue_size(base, h) + 1);
    s32 r = unlock(ctx, h, lp::queue_mutex);
    if (r == lv2::CELL_OK) r = sem_release(ctx, h + lp::cmd_available);
    if (r != lv2::CELL_OK) {
        unlock(ctx, h, lp::queue_size_mutex);
        return r;
    }
    return unlock(ctx, h, lp::queue_size_mutex);
}

// AdecCmdQueue::pop: the front command; its handle set to 0xFF.
Cmd pop_command(u8* base, u32 h)
{
    const u32 front = ps_read32(base, h + lp::cmd_front), at = cmd_at(h, front);
    Cmd c;
    c.pcm_handle = static_cast<s32>(ps_read32(base, at + lp::c_pcm_handle));
    c.au_start = ps_read32(base, at + lp::c_au_start);
    c.au_size = ps_read32(base, at + lp::c_au_size);
    c.type = ps_read32(base, at + lp::c_type);
    ps_write32(base, at + lp::c_pcm_handle, 0xFF);
    ps_write32(base, h + lp::cmd_front, (front + 1) & 3);
    set_queue_size(base, h, queue_size(base, h) - 1);
    return c;
}

// The output buffer as host words (the floats' big-endian bits), read and written back over [0, n).
struct Output {
    u8* base;
    u32 at;
    std::vector<u32> w;
    Output(u8* b, u32 addr, u32 n) : base(b), at(addr), w(n)
    {
        for (u32 i = 0; i < n; i++) w[i] = ps_read32(base, at + 4 * i);
    }
    void store() const
    {
        for (u32 i = 0; i < w.size(); i++) ps_write32(base, at + 4 * i, w[i]);
    }
    u32& operator[](s64 i)
    {
        if (i < 0 || static_cast<usize>(i) >= w.size()) PS_FATAL(kChan, "LPCM: output word {} of {}", i, w.size());
        return w[static_cast<usize>(i)];
    }
};

u32 float_bits(s32 v) // v / 2^31 as LLE converts (a 32-bit sample, its low bits zero)
{
    const f32 f = static_cast<f32>(v) * (1.f / static_cast<f32>(0x80000000u));
    u32 bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

s32 s16_sample(u8* base, u32 at) { return static_cast<s32>(static_cast<u32>(ps_read16(base, at)) << 16); }

// 32-bit samples from 16 bytes `v` by a little-endian byte shuffle (-1: zero) and mask, as gv_shuffle8 then & does.
s32 shuffled(const u8 (&v)[16], const s8* ctrl, const u8* mask)
{
    u32 r = 0;
    for (u32 j = 0; j < 4; j++) {
        const u8 byte = ctrl[j] < 0 ? 0 : v[ctrl[j]];
        r |= static_cast<u32>(byte & (mask ? mask[j] : 0xFF)) << (8 * j);
    }
    return static_cast<s32>(r);
}

// RPCS3 LpcmDecContext::exec, decode_au: the access unit to floats in the output buffer. Returns the output's size.
u32 decode(PPUContext& ctx, u32 h, const Cmd& cmd)
{
    u8* base = guest_base(ctx);
    const Param p = read_param(base, ps_read32(base, h + lp::lpcm_param));
    const u32 in = cmd.au_start, out_addr = ps_read32(base, h + lp::output);
    const s64 au_u8 = cmd.au_size, au_s16 = au_u8 / 2;
    s64 output_size = cmd.au_size;
    s32 sample_num = static_cast<s32>((p.payload + 0xF) & ~0xFu);
    s32 channel_num = 0;
    // The output words this decode may touch: the samples converted (at most half the access unit's bytes) and the
    // reordering's groups, up to a few words past the samples.
    Output o(base, out_addr,
        static_cast<u32>(std::min<s64>(kOutputBufferSize / 4 + 16, std::max<s64>(au_u8 / 2 + 8, s64{sample_num} + 16))));
    const auto too_big = [&](s64 size) {
        if (size <= kOutputBufferSize) return false;
        PS_LOG_ERROR(kChan, "LPCM: an access unit of {:#x} bytes decodes to {:#x}, more than the {:#x}-byte buffer", au_u8,
            size, kOutputBufferSize);
        return true;
    };
    if (!ps_read32(base, h + lp::dvd_packing)) {
        switch (p.word) {
        case BIT_LENGTH_16: output_size = output_size * 32 / 16; sample_num /= 2; break;
        case BIT_LENGTH_20:
        case BIT_LENGTH_24: output_size = output_size * 32 / 24; sample_num /= 3; break;
        default: break; // LLE skips decoding: the buffer keeps what was there
        }
        // Streams with an odd number of channels carry an empty dummy channel.
        switch (p.channels) {
        case CH_MONO: channel_num = 1; output_size = output_size * 1 / 2; break;
        case CH_STEREO: channel_num = 2; break;
        case CH_3_0:
        case CH_2_1: channel_num = 3; output_size = output_size * 3 / 4; break;
        case CH_3_1:
        case CH_2_2: channel_num = 4; break;
        case CH_3_2: channel_num = 5; output_size = output_size * 5 / 6; break;
        case CH_3_2_LFE: channel_num = 6; break;
        case CH_3_4: channel_num = 7; output_size = output_size * 7 / 8; break;
        case CH_3_4_LFE: channel_num = 8; break;
        default: break; // LLE leaves the channels as they are
        }
        // LLE doesn't check the output size (RPCS3 aborts).
        if (too_big(output_size) || too_big(static_cast<s64>(sample_num) * 4)) return static_cast<u32>(output_size);
        if (p.word == BIT_LENGTH_16) {
            for (s64 i = 0; i < au_s16; i++) o[i] = float_bits(s16_sample(base, in + static_cast<u32>(2 * i)));
        } else if (p.word == BIT_LENGTH_20 || p.word == BIT_LENGTH_24) {
            for (s64 i = 0; i * 3 <= au_u8 - 3; i++) {
                const u32 a = in + static_cast<u32>(3 * i);
                o[i] = float_bits(static_cast<s32>(static_cast<u32>(ps_read8(base, a)) << 24 |
                    static_cast<u32>(ps_read8(base, a + 1)) << 16 | static_cast<u32>(ps_read8(base, a + 2)) << 8));
            }
        }
        // Channel order in: front left, front right, center, side left, rear left, rear right, side right, LFE. Out:
        // up to 3_4 front left, center, front right, side left, side right, rear left, rear right, LFE; 3_4_LFE front
        // left, front right, center, LFE, side left, side right, rear left, rear right. The dummy channel goes. Each
        // loop works in RPCS3's (and LLE's) groups of four, so it may touch a few words past the samples.
        switch (p.channels) {
        case CH_MONO:
            for (s32 i = 0; i < sample_num / 2; i += 4) {
                const u32 t[4] = {o[i * 2], o[i * 2 + 2], o[i * 2 + 4], o[i * 2 + 6]};
                for (s32 k = 0; k < 4; k++) o[i + k] = t[k];
            }
            break;
        case CH_3_0:
            for (s32 i_in = 0, i_out = 0; i_in < sample_num; i_in += 4, i_out += 3) {
                const u32 t[4] = {o[i_in], o[i_in + 2], o[i_in + 1], o[i_in + 3]};
                for (s32 k = 0; k < 4; k++) o[i_out + k] = t[k];
            }
            break;
        case CH_2_1:
            for (s32 i_in = 0, i_out = 0; i_in < sample_num; i_in += 4, i_out += 3) {
                const u32 t[4] = {o[i_in], o[i_in + 1], o[i_in + 2], o[i_in + 3]};
                for (s32 k = 0; k < 4; k++) o[i_out + k] = t[k];
            }
            break;
        case CH_3_1:
        case CH_3_2_LFE:
            for (s32 i = 0; i < sample_num; i += channel_num) std::swap(o[i + 1], o[i + 2]);
            break;
        case CH_3_2:
            for (s32 i_in = 0, i_out = 0; i_in < sample_num; i_in += 6, i_out += 5) {
                const u32 t[4] = {o[i_in], o[i_in + 2], o[i_in + 1], o[i_in + 3]};
                for (s32 k = 0; k < 4; k++) o[i_out + k] = t[k];
                o[i_out + 4] = o[i_in + 4];
            }
            break;
        case CH_3_4:
            for (s32 i_in = 0, i_out = 0; i_in < sample_num; i_in += 8, i_out += 7) {
                const u32 t1[4] = {o[i_in], o[i_in + 2], o[i_in + 1], o[i_in + 3]};
                const u32 t2[4] = {o[i_in + 6], o[i_in + 4], o[i_in + 5], o[i_in + 7]};
                for (s32 k = 0; k < 4; k++) o[i_out + k] = t1[k];
                for (s32 k = 0; k < 4; k++) o[i_out + 4 + k] = t2[k];
            }
            break;
        case CH_3_4_LFE:
            for (s32 i = 0; i < sample_num; i += 8) {
                const u32 t[4] = {o[i + 7], o[i + 6], o[i + 4], o[i + 5]};
                for (s32 k = 0; k < 4; k++) o[i + 4 + k] = t[k];
                std::swap(o[i + 3], o[i + 4]);
            }
            break;
        default: break; // stereo and 2_2 are in order already
        }
    } else {
        // DVD: only the front left and right channels are decoded. The parameters were checked from the user data.
        switch (p.word) {
        case BIT_LENGTH_16: output_size = output_size * 32 / 16; break;
        case BIT_LENGTH_20: output_size = output_size * 32 / 20; break;
        case BIT_LENGTH_24: output_size = output_size * 32 / 24; break;
        default: PS_FATAL(kChan, "LPCM (DVD): word size {} passed the parameter check", p.word);
        }
        switch (p.channels) {
        case DVD_CH_MONO: // two for mono as well
        case DVD_CH_STEREO: channel_num = 2; break;
        case 4: channel_num = 3; output_size = output_size * 2 / 3; break;
        case 5: channel_num = 4; output_size = output_size * 2 / 4; break;
        case DVD_CH_3_2: channel_num = 5; output_size = output_size * 2 / 5; break;
        case DVD_CH_3_2_LFE: channel_num = 6; output_size = output_size * 2 / 6; break;
        case DVD_CH_3_4: channel_num = 7; output_size = output_size * 2 / 7; break;
        case DVD_CH_3_4_LFE: channel_num = 8; output_size = output_size * 2 / 8; break;
        default: PS_FATAL(kChan, "LPCM (DVD): channel layout {} passed the parameter check", p.channels);
        }
        if (too_big(output_size)) return static_cast<u32>(output_size);
        const s64 ch = channel_num;
        const bool mono = p.channels == DVD_CH_MONO;
        const auto byte = [&](s64 i) { return ps_read8(base, in + static_cast<u32>(i)); };
        // Four samples from 16 bytes: two from `hi12`, two from `hi34`, the low bits at `lo12` and `lo34`.
        const auto gather = [&](s64 at, s64 hi34, s64 lo12, s64 lo34, u8 (&v)[16]) {
            for (s64 k = 0; k < 4; k++) {
                v[k] = byte(at + k);
                v[4 + k] = byte(at + hi34 + k);
            }
            v[8] = byte(at + lo12);
            v[9] = byte(at + lo12 + 1);
            v[10] = byte(at + lo34);
            v[11] = byte(at + lo34 + 1);
            for (u32 k = 12; k < 16; k++) v[k] = 0;
        };
        switch (p.word) {
        case BIT_LENGTH_16: {
            s64 i_in = 0, i_out = 0;
            for (; i_in <= au_s16 - ch - 2; i_in += ch * 2, i_out += 4) {
                const s64 from[4] = {i_in, i_in + 1, i_in + ch, i_in + ch + 1};
                for (s64 k = 0; k < 4; k++) o[i_out + k] = float_bits(s16_sample(base, in + static_cast<u32>(2 * from[k])));
            }
            for (; i_in <= au_s16 - 2; i_in += ch, i_out += 2)
                for (s64 k = 0; k < 2; k++) o[i_out + k] = float_bits(s16_sample(base, in + static_cast<u32>(2 * (i_in + k))));
            break;
        }
        case BIT_LENGTH_20: {
            const s64 hi34 = mono ? 5 : ch * 2, lo12 = mono ? 4 : ch * 4;
            const s64 lo34 = ch * 4 + ch / 2 - !(ch & 1), next = ch * 5;
            // If the channel count is odd, the low bits of samples three and four are in different bytes. LLE does
            // not shift the nibbles ANDed with 0x0F left by 4 (RPCS3 keeps that).
            static constexpr s8 kSame[16] = {-1, 8, 1, 0, -1, 8, 3, 2, -1, 11, 5, 4, -1, 11, 7, 6};
            static constexpr s8 kDifferent[16] = {-1, 8, 1, 0, -1, 8, 3, 2, -1, 10, 5, 4, -1, 11, 7, 6};
            static constexpr u8 kMaskSame[16] = {0x00, 0xf0, 0xff, 0xff, 0x00, 0x0f, 0xff, 0xff, 0x00, 0xf0, 0xff, 0xff,
                0x00, 0x0f, 0xff, 0xff};
            static constexpr u8 kMaskDifferent[16] = {0x00, 0xf0, 0xff, 0xff, 0x00, 0x0f, 0xff, 0xff, 0x00, 0x0f, 0xff,
                0xff, 0x00, 0xf0, 0xff, 0xff};
            const s8* ctrl = (ch & 1) ? kDifferent : kSame;
            const u8* mask = (ch & 1) ? kMaskDifferent : kMaskSame;
            for (s64 i_in = 0, i_out = 0; i_in <= au_u8 - lo34 - (ch & 1); i_in += next, i_out += 4) {
                u8 v[16];
                gather(i_in, hi34, lo12, lo34, v);
                for (u32 k = 0; k < 4; k++) o[i_out + k] = float_bits(shuffled(v, ctrl + 4 * k, mask + 4 * k));
            }
            break;
        }
        case BIT_LENGTH_24: {
            const s64 hi34 = mono ? 6 : ch * 2, lo12 = mono ? 4 : ch * 4, lo34 = ch * 5, next = ch * 6;
            static constexpr s8 kCtrl[16] = {-1, 8, 1, 0, -1, 9, 3, 2, -1, 10, 5, 4, -1, 11, 7, 6};
            for (s64 i_in = 0, i_out = 0; i_in <= au_u8 - lo34 - 2; i_in += next, i_out += 4) {
                u8 v[16];
                gather(i_in, hi34, lo12, lo34, v);
                for (u32 k = 0; k < 4; k++) o[i_out + k] = float_bits(shuffled(v, kCtrl + 4 * k, nullptr));
            }
            break;
        }
        default: break;
        }
    }
    o.store();
    return static_cast<u32>(output_size);
}

// Whether the context at `h` is still the calling thread's, as adec_atracx.cpp's own_context (D-645): `r`, the answer
// of the call just made on its semaphore, mutexes or conditions, is CELL_OK, and its thread ID names the thread. LLE
// notes such a failure in error_occurred and goes on: with the context overwritten by the game while the decoder was
// open, that went round failing calls for ever (or took the commands of another decoder opened in its memory). The
// thread ends instead, logged.
bool own_context(PPUContext& ctx, u32 h, s32 r, std::string_view what)
{
    const u32 self = thread(ctx).id;
    const u64 named = ps_read64(guest_base(ctx), h + lp::thread_id);
    if (r == lv2::CELL_OK && named == self) return true;
    const std::string answer =
        r == lv2::CELL_OK ? "" : std::format("; {} answered {:#010x}", what, static_cast<u32>(r));
    PS_LOG_ERROR(kChan, "LPCM decoder {:#x}: its context is no longer its own (it names thread {:#x}{}): the game "
        "overwrote it while the decoder was open; the decoder's thread {:#x} ends", h, named, answer, self);
    return false;
}

// RPCS3 LpcmDecContext::exec: one command at a time, each after the previous output was taken; until its context is
// no longer its own (own_context, D-645).
void run(PPUContext& ctx, u32 h)
{
    u8* base = guest_base(ctx);
    const auto failed = [&](s32 r) {
        if (r != lv2::CELL_OK) ps_write32(base, h + lp::error_occurred, 1);
    };
    while (ps_read32(base, h + lp::run_thread)) {
        if (!own_context(ctx, h, lv2::CELL_OK, {}) ||
            !own_context(ctx, h, sem_acquire(ctx, h + lp::cmd_available), "the command semaphore"))
            return;
        // The output is taken first, even before a command that decodes nothing.
        const s32 locked = lock(ctx, h, lp::output_mutex);
        s32 r = locked;
        while (r == lv2::CELL_OK && ps_read32(base, h + lp::output_locked))
            r = lv2::sys_cond_wait(thread(ctx), ps_read32(base, h + lp::output_consumed), 0); // unchecked on LLE
        if (!own_context(ctx, h, r, "the output's mutex or condition")) {
            if (locked == lv2::CELL_OK) unlock(ctx, h, lp::output_mutex);
            return;
        }
        lock(ctx, h, lp::queue_mutex);
        const Cmd cmd = pop_command(base, h);
        unlock(ctx, h, lp::queue_mutex);
        // Whom the notifications go to: what open was given (the front end's own, or a table caller's, D-591).
        const CoreNotifier notify(ctx, h, kSlots);
        switch (cmd.type) {
        case START_SEQ: // LLE sends it to its SPU task, which does nothing with it
            failed(unlock(ctx, h, lp::output_mutex));
            break;
        case END_SEQ:
            notify.seq_done();
            failed(unlock(ctx, h, lp::output_mutex));
            break;
        case CLOSE:
            unlock(ctx, h, lp::output_mutex);
            return;
        case DECODE_AU: {
            const u32 output_size = decode(ctx, h, cmd);
            const bool error_occurred = ps_read32(base, h + lp::error_occurred) != 0;
            if (error_occurred) notify.error(error(CELL_ADEC_ERROR_FATAL));
            notify.au_done(cmd.pcm_handle);
            ps_write32(base, h + lp::output_locked, 1);
            failed(unlock(ctx, h, lp::output_mutex));
            // CellAdecLpcmInfo: channelNumber, sampleRate, outputDataSize.
            const Param p = read_param(base, ps_read32(base, h + lp::lpcm_param));
            StackVar bsi(ctx, base, 12);
            ps_write32(base, bsi.addr(), p.channels);
            ps_write32(base, bsi.addr() + 4, p.rate);
            ps_write32(base, bsi.addr() + 8, output_size);
            const s32 status = ps_read32(base, h + lp::error_occurred) ? error(CELL_ADEC_ERROR_FATAL) : lv2::CELL_OK;
            notify.pcm_out(cmd.pcm_handle, ps_read32(base, h + lp::output), output_size, bsi.addr(),
                CORRECT_PTS_LPCM_HDMV, status);
            break;
        }
        default: PS_FATAL(kChan, "LPCM: command type {} in the queue of {:#x}", cmd.type, h);
        }
    }
}

class LpcmCore final : public AdecCore {
public:
    u32 mem_size(PPUContext&) override { return kMemSize; }
    s32 pcm_handle_num(PPUContext&) override { return 8; }
    u32 bsi_info_size(PPUContext&) override { return 12; } // CellAdecLpcmInfo
    u32 version(PPUContext&) override { return 0x20070323; }

    // RPCS3 _CellAdecCoreOpOpenExt_lpcm (Open is the same without SPURS: LLE uses the game's SPURS instance or makes
    // one; pspocket uses neither).
    s32 open(PPUContext& ctx, u32 h, const CoreNotify& notify, u32 res, u32) override
    {
        u8* base = guest_base(ctx);
        const u32 end_of_context = h + ((lp::size + 0x7F) & ~0x7Fu);
        // No thread ID until the new thread's: an earlier decoder's thread still going in this memory does not take the
        // new context for its own (D-645).
        ps_write64(base, h + lp::thread_id, 0);
        ps_write32(base, h + lp::cmd_front, 0);
        ps_write32(base, h + lp::cmd_back, 0);
        set_queue_size(base, h, 0);
        ps_write32(base, h + lp::run_thread, 1);
        HleProcess& process = *HleProcess::current();
        store_notify(base, h, kSlots, notify);
        ps_write32(base, h + lp::output, end_of_context + 0x180);
        ps_write32(base, h + lp::lpcm_param, end_of_context);
        ps_write32(base, h + lp::error_occurred, 0);
        s32 r = create_mutex(ctx, h + lp::queue_size_mutex, "_adem04");
        if (r == lv2::CELL_OK) r = create_cond(ctx, h + lp::queue_size_cond, h + lp::queue_size_mutex, "_adec03");
        if (r == lv2::CELL_OK) r = create_mutex(ctx, h + lp::unk_mutex, "_adem04");
        if (r == lv2::CELL_OK) r = create_cond(ctx, h + lp::unk_cond, h + lp::unk_mutex, "_adec03");
        if (r == lv2::CELL_OK) r = create_mutex(ctx, h + lp::output_mutex, "_adem05");
        if (r == lv2::CELL_OK) r = create_cond(ctx, h + lp::output_consumed, h + lp::output_mutex, "_adec03");
        if (r == lv2::CELL_OK) r = create_mutex(ctx, h + lp::queue_mutex, "_adem06");
        if (r == lv2::CELL_OK) r = release_output(ctx, h);
        if (r == lv2::CELL_OK) r = sem_init(ctx, h + lp::cmd_available);
        if (r == lv2::CELL_OK) r = sem_init(ctx, h + lp::reserved2);
        if (r != lv2::CELL_OK) return r;
        // The decoder's thread (CellAdecResource: ppuThreadPriority +8, ppuThreadStackSize +16), joinable.
        lv2::PpuThread& t = thread(ctx);
        const u32 entry = core_thread_entry(NID_lpcm_dec_thread, "_pspocket_lpcm_dec_thread");
        if (!entry) {
            PS_LOG_ERROR(kChan, "LPCM: no memory to link the decoder's thread function");
            return error(CELL_ADEC_ERROR_FATAL);
        }
        const u32 tls = process.alloc_tls();
        ps_write32(base, h + lp::tls, tls);
        StackVar sv(ctx, base, 0x30);
        ps_write32(base, sv.addr() + 8, entry);
        ps_write32(base, sv.addr() + 12, tls ? tls + kTlsR13Offset : 0);
        std::memcpy(base + sv.addr() + 0x10, "HLE LPCM decoder", 17);
        r = lv2::_sys_ppu_thread_create(t, sv.addr(), sv.addr() + 8, h, 0, static_cast<s32>(ps_read32(base, res + 8)),
            ps_read32(base, res + 16), 1, sv.addr() + 0x10);
        if (r == lv2::CELL_OK) {
            ps_write64(base, h + lp::thread_id, ps_read64(base, sv.addr()));
            r = lv2::sys_ppu_thread_start(t, static_cast<u32>(ps_read64(base, sv.addr())));
        }
        if (r != lv2::CELL_OK) {
            if (tls) process.free_tls(tls);
            ps_write32(base, h + lp::tls, 0);
            return r;
        }
        r = create_mutex(ctx, h + lp::spurs_queue_pop_mutex, "_adem04");
        if (r == lv2::CELL_OK) r = create_mutex(ctx, h + lp::spurs_queue_push_mutex, "_adem04");
        return r;
    }

    // RPCS3 _CellAdecCoreOpClose_lpcm: a close command (or every queued command turned into one), the output released,
    // the thread joined, everything destroyed.
    s32 close(PPUContext& ctx, u32 h) override
    {
        u8* base = guest_base(ctx);
        lv2::PpuThread& t = thread(ctx);
        if (const s32 r = lock(ctx, h, lp::queue_size_mutex); r != lv2::CELL_OK) return r;
        if (const s32 r = lock(ctx, h, lp::queue_mutex); r != lv2::CELL_OK) return r;
        if (queue_size(base, h) == 0) {
            const u32 back = ps_read32(base, h + lp::cmd_back);
            write_cmd(base, cmd_at(h, back), Cmd{0, 0, 0, CLOSE}, false);
            ps_write32(base, h + lp::cmd_back, (back + 1) & 3);
            set_queue_size(base, h, queue_size(base, h) + 1);
            if (const s32 r = unlock(ctx, h, lp::queue_mutex); r != lv2::CELL_OK) return r;
            if (const s32 r = sem_release(ctx, h + lp::cmd_available); r != lv2::CELL_OK) {
                unlock(ctx, h, lp::queue_size_mutex);
                return r;
            }
        } else {
            for (u32 i = 0; i < 4; i++) ps_write32(base, cmd_at(h, i) + lp::c_type, CLOSE);
            if (const s32 r = unlock(ctx, h, lp::queue_mutex); r != lv2::CELL_OK) return r;
        }
        s32 r = unlock(ctx, h, lp::queue_size_mutex);
        if (r == lv2::CELL_OK) r = release_output(ctx, h);
        if (r == lv2::CELL_OK) {
            StackVar status(ctx, base, 8);
            r = lv2::sys_ppu_thread_join(t, static_cast<u32>(ps_read64(base, h + lp::thread_id)), status.addr());
        }
        if (r == lv2::CELL_OK) {
            if (const u32 tls = ps_read32(base, h + lp::tls)) HleProcess::current()->free_tls(tls);
            ps_write32(base, h + lp::tls, 0);
        }
        const auto destroy_cond = [&](u32 field) { return lv2::sys_cond_destroy(t, ps_read32(base, h + field)); };
        const auto destroy_mutex = [&](u32 field) { return lv2::sys_mutex_destroy(t, ps_read32(base, h + field)); };
        if (r == lv2::CELL_OK) r = destroy_cond(lp::queue_size_cond);
        if (r == lv2::CELL_OK) r = destroy_cond(lp::unk_cond);
        if (r == lv2::CELL_OK) r = destroy_cond(lp::output_consumed);
        if (r == lv2::CELL_OK) r = destroy_mutex(lp::queue_mutex);
        if (r == lv2::CELL_OK) r = destroy_mutex(lp::queue_size_mutex);
        if (r == lv2::CELL_OK) r = destroy_mutex(lp::unk_mutex);
        if (r == lv2::CELL_OK) r = destroy_mutex(lp::output_mutex);
        if (r == lv2::CELL_OK) r = sem_finalize(ctx, h + lp::cmd_available);
        if (r == lv2::CELL_OK) r = sem_finalize(ctx, h + lp::reserved2);
        if (r == lv2::CELL_OK) r = destroy_mutex(lp::spurs_queue_pop_mutex);
        if (r == lv2::CELL_OK) r = destroy_mutex(lp::spurs_queue_push_mutex);
        return r;
    }

    s32 start_seq(PPUContext& ctx, u32 h, u32 param) override
    {
        const Param p = read_param(guest_base(ctx), param);
        PS_LOG_DEBUG(kChan, "LPCM start: channels {}, rate {}, word {}, payload {:#x}", p.channels, p.rate, p.word, p.payload);
        if (p.channels >= 0x20 || p.rate >= 0x20 || p.word >= 0x20 || p.payload == 0) return error(CELL_ADEC_ERROR_LPCM_ARG);
        return send_command(ctx, h, Cmd{0, 0, 0, START_SEQ}, &p);
    }

    s32 end_seq(PPUContext& ctx, u32 h) override { return send_command(ctx, h, Cmd{0, 0, 0, END_SEQ}, nullptr); }

    s32 decode_au(PPUContext& ctx, u32 h, s32 pcm_handle, u32 au_info) override
    {
        u8* base = guest_base(ctx);
        return send_command(ctx, h, Cmd{pcm_handle, ps_read32(base, au_info), ps_read32(base, au_info + 4), DECODE_AU},
            nullptr);
    }

    // RPCS3 _CellAdecCoreOpRealign_lpcm: the PCM copied out, when the game gave a buffer.
    s32 realign(PPUContext& ctx, u32 h, u32 out_buffer, u32 pcm_start) override
    {
        if (!pcm_start) return error(CELL_ADEC_ERROR_LPCM_ARG);
        if (!out_buffer) return lv2::CELL_OK;
        u8* base = guest_base(ctx);
        const u32 size = ps_read32(base, h + lp::output_size);
        if (!guest_readable(thread(ctx), out_buffer, size)) {
            log_call_site(ctx, base, std::format("cellAdecGetPcm: the buffer ({:#x}, {} bytes) is not guest memory",
                out_buffer, size));
            return error(CELL_ADEC_ERROR_ARG);
        }
        std::memmove(base + out_buffer, base + pcm_start, size);
        return lv2::CELL_OK;
    }

    s32 release_pcm(PPUContext& ctx, u32 h, s32, u32) override { return release_output(ctx, h); }
};

} // namespace

std::unique_ptr<AdecCore> make_lpcm_core() { return std::make_unique<LpcmCore>(); }

void set_lpcm_dvd_packing(u8* base, u32 core_handle, bool dvd) { ps_write32(base, core_handle + lp::dvd_packing, dvd); }

void set_lpcm_output_size(u8* base, u32 core_handle, u32 size) { ps_write32(base, core_handle + lp::output_size, size); }

// RPCS3 adecSetLpcmBlurayParams: the user data's channels, rate, word size and frame count give the payload's size.
s32 set_lpcm_bluray_params(u8* base, u32 h, u64 user_data)
{
    const u8 channels = static_cast<u8>(user_data >> 32), rate = static_cast<u8>(user_data >> 40),
             word = static_cast<u8>(user_data >> 48), frames = static_cast<u8>(user_data >> 56);
    const u32 p = ps_read32(base, h + lp::lpcm_param);
    ps_write32(base, p, channels);
    ps_write32(base, p + 4, rate);
    ps_write32(base, p + 8, word);
    u32 allocated_channels = 0, samples_per_frame = 0, bytes_per_sample = 0;
    switch (channels) {
    case CH_MONO:
    case CH_STEREO: allocated_channels = 2; break;
    case CH_3_0:
    case CH_2_1:
    case CH_3_1:
    case CH_2_2: allocated_channels = 4; break;
    case CH_3_2:
    case CH_3_2_LFE: allocated_channels = 6; break;
    case CH_3_4:
    case CH_3_4_LFE: allocated_channels = 8; break;
    default: return error(CELL_ADEC_ERROR_FATAL);
    }
    switch (rate) {
    case FS_48kHz: samples_per_frame = 48000 / 200; break;
    case FS_96kHz: samples_per_frame = 96000 / 200; break;
    case FS_192kHz: samples_per_frame = 192000 / 200; break;
    default: return error(CELL_ADEC_ERROR_FATAL);
    }
    switch (word) {
    case BIT_LENGTH_16: bytes_per_sample = 2; break;
    case BIT_LENGTH_20:
    case BIT_LENGTH_24: bytes_per_sample = 3; break;
    default: return error(CELL_ADEC_ERROR_FATAL);
    }
    ps_write32(base, p + 12, bytes_per_sample * allocated_channels * samples_per_frame * frames);
    return lv2::CELL_OK;
}

// RPCS3 adecSetLpcmDvdParams: the same for DVD's packing, whose layouts allow fewer combinations.
s32 set_lpcm_dvd_params(u8* base, u32 h, u64 user_data)
{
    const u8 layout = static_cast<u8>(user_data >> 32), rate = static_cast<u8>(user_data >> 40),
             word = static_cast<u8>(user_data >> 48), frames = static_cast<u8>(user_data >> 56);
    const u32 p = ps_read32(base, h + lp::lpcm_param);
    ps_write32(base, p, layout);
    ps_write32(base, p + 4, rate);
    ps_write32(base, p + 8, word);
    u32 samples_per_frame = 0, bits = 0, channels = 0;
    switch (rate) {
    case FS_48kHz: samples_per_frame = 48000 / 600; break;
    case FS_96kHz: samples_per_frame = 96000 / 600; break;
    default: return error(CELL_ADEC_ERROR_FATAL);
    }
    switch (word) {
    case BIT_LENGTH_16: bits = 16; break;
    case BIT_LENGTH_20: bits = 20; break;
    case BIT_LENGTH_24: bits = 24; break;
    default: return error(CELL_ADEC_ERROR_FATAL);
    }
    const bool fs96 = rate == FS_96kHz;
    switch (layout) {
    case DVD_CH_MONO: channels = 1; break;
    case DVD_CH_STEREO: channels = 2; break;
    case 4:
        if (fs96 && word == BIT_LENGTH_24) return error(CELL_ADEC_ERROR_FATAL);
        channels = 3;
        break;
    case 5:
        if (fs96 && word != BIT_LENGTH_16) return error(CELL_ADEC_ERROR_FATAL);
        channels = 4;
        break;
    case DVD_CH_3_2:
        if (fs96) return error(CELL_ADEC_ERROR_FATAL);
        channels = 5;
        break;
    case DVD_CH_3_2_LFE:
        if (fs96 || word == BIT_LENGTH_24) return error(CELL_ADEC_ERROR_FATAL);
        channels = 6;
        break;
    case DVD_CH_3_4:
    case DVD_CH_3_4_LFE:
        if (fs96 || word != BIT_LENGTH_16) return error(CELL_ADEC_ERROR_FATAL);
        channels = layout == DVD_CH_3_4 ? 7 : 8;
        break;
    default: return error(CELL_ADEC_ERROR_FATAL);
    }
    ps_write32(base, p + 12, bits * channels * samples_per_frame / 8 * frames);
    return lv2::CELL_OK;
}

void _pspocket_lpcm_dec_thread(PPUContext& ctx, u64 core_handle)
{
    run(ctx, static_cast<u32>(core_handle));
    lv2::_sys_ppu_thread_exit(thread_of(ctx), 0);
}

} // namespace pspocket::hle::adec
