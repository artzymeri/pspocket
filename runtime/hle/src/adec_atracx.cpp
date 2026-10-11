// cellAdec's ATRAC3plus core (D-198): pspocket's own, on FFmpeg's atrac3p decoder, in place of the firmware's
// libatxdec (cellAtracXdec). Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellAtracXdec.cpp/.h, which reproduces libatxdec's
// PPU side (its context in the game's memory, its command queue, mutexes and condition variables, the ATS header, the
// first frame of a sequence or after an error replaced by silence, the output formats and channel order) and decodes
// with FFmpeg. libatxdec decodes on an SPU (a SPURS task); pspocket, as RPCS3, on a PPU thread of the decoder's
// ("HLE ATRAC3plus decoder"). FFmpeg's objects live on the host (Decoders, keyed by the core's handle); RPCS3 keeps
// their pointers in the guest context.
//
// AtracXdecContext (0x268): the thread, the queue mutex and its "not empty" cond, the command queue (4 commands of 0x34,
// front, back, size), the output mutex, its "consumed" cond and output_locked, the run_thread mutex, cond and flag, the
// four notifications, the work memory, (pspocket's: the TLS block at +0x128, whether the notifications are guest
// functions at +0x12C, D-591) skip_next_frame +0x165, the sequence's CellAdecParamAtracX +0x1A0, first_decode +0x1BD,
// then the decoder's state (AtracXdecDecoder) +0x1C0.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "adec.hpp"
#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/syscalls.hpp"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#endif

namespace pspocket::hle::adec {

#if PSPOCKET_HAVE_FFMPEG

namespace {

constexpr std::string_view kChan = "cellAdec";

enum : u32 {
    CELL_ADEC_ERROR_ATX_BUSY = 0x80612264,
    CELL_ADEC_ERROR_ATX_ATSHDR = 0x80612266,
    CELL_ADEC_ERROR_ATX_NON_FATAL = 0x80612281,
    CELL_ADEC_ERROR_ATX_POSCFG_PRESENT = 0x806122B2,
    CELL_ADEC_ERROR_ATX_FATAL = 0x8061228C,
    ATX_CONFIG_FAILED = 0x80004005, // RPCS3's code for a configuration libatxdec's SPU program refuses
};
enum : u32 { WORD_SZ_16BIT = 2, WORD_SZ_24BIT = 3, WORD_SZ_32BIT = 4, WORD_SZ_FLOAT = 0x84 };

namespace ax {
constexpr u32 thread_id = 0x00, queue_mutex = 0x08, queue_not_empty = 0x0C, kCmdSize = 0x34, cmd_front = 0xE0,
              cmd_back = 0xE4, cmd_size = 0xE8, output_mutex = 0xEC, output_consumed = 0xF0, output_locked = 0xF4,
              run_thread_mutex = 0xF8, run_thread_cond = 0xFC, run_thread = 0x100, notify = 0x104, work_mem = 0x124,
              tls = 0x128, notify_guest = 0x12C, skip_next_frame = 0x165, param = 0x1A0, first_decode = 0x1BD,
              decoder = 0x1C0, size = 0x268;
// AtracXdecCmd: type, pcm_handle, au_start_addr, au_size, two unused words, CellAdecParamAtracX.
constexpr u32 c_type = 0x00, c_pcm_handle = 0x04, c_au_start = 0x08, c_au_size = 0x0C, c_param = 0x18;
// CellAdecParamAtracX (0x1C): sampling_freq, ch_config_idx, nch_out, nbytes, extra_config_data[4], bw_pcm,
// downmix_flag, au_includes_ats_hdr_flg.
constexpr u32 kParamSize = 0x1C, p_freq = 0x00, p_ch_config = 0x04, p_nch_out = 0x08, p_nbytes = 0x0C, p_bw_pcm = 0x14,
              p_ats = 0x19;
// AtracXdecDecoder: sampling_freq, ch_config_idx, nch_in, nch_blocks, nbytes, nch_out, bw_pcm, nbytes_128_aligned,
// status, pcm_output_size, work_mem.
constexpr u32 d_freq = 0x00, d_ch_config = 0x04, d_nch_in = 0x08, d_nch_blocks = 0x0C, d_nbytes = 0x10, d_nch_out = 0x14,
              d_bw_pcm = 0x18, d_nbytes_aligned = 0x1C, d_pcm_output_size = 0x24, d_work_mem = 0x28;
} // namespace ax

enum CmdType : u32 { INVALID = 0, START_SEQ = 1, DECODE_AU = 2, END_SEQ = 3, CLOSE = 4 };

constexpr u32 kSpursStructsSize = 0x1CF00; // CellSpurs, CellSpursTaskset, their context: libatxdec's
constexpr u32 kSamplesPerFrame = 0x800;
constexpr u32 kMaxFrameLength = 0x2000;
constexpr u8 kChannelBlocks[8] = {0, 1, 1, 2, 3, 4, 5, 5};

// libatxdec's SPU memory for its input channels: the interleaved output follows it in the work memory.
constexpr u32 spurs_mem_size(u32 nch_in)
{
    switch (nch_in) {
    case 1:
    case 2: return 0x6000;
    case 3: return 0x12880;
    case 4: return 0x19C80;
    case 6: return 0x23080;
    case 7: return 0x2A480;
    case 8: return 0x2C480;
    default: return 0xFFFFFFFF;
    }
}

// FFmpeg (5.1.3 and later) gives front left, front right, center, LFE, rear left, rear right, rear center, side left,
// side right; libatxdec's order for 1 to 7 channels is front left, center, front right, rear left, rear right, rear
// center, LFE, and for 8 front left, front right, center, LFE, rear left, rear right, side left, side right. Each
// FFmpeg channel's place in the output (RPCS3 ATXDEC_AVCODEC_CH_MAP).
constexpr u8 kChannelMap[7][8] = {
    {0}, {0, 1}, {0, 2, 1}, {0, 2, 1, 3}, {0, 2, 1, 4, 5, 3}, {0, 2, 1, 4, 5, 6, 3}, {0, 1, 2, 3, 4, 5, 6, 7},
};

// FFmpeg's decoder for one core handle.
struct Decoder {
    const AVCodec* codec = nullptr;
    AVCodecContext* av = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    bool config_is_set = false;
    ~Decoder()
    {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&av);
    }
};

struct Decoders {
    std::mutex m;
    std::map<u32, std::shared_ptr<Decoder>> by_handle;
};

Decoders& decoders() { return HleProcess::current()->state<Decoders>(); }

std::shared_ptr<Decoder> decoder_of(u32 h)
{
    Decoders& all = decoders();
    std::lock_guard lock(all.m);
    std::shared_ptr<Decoder>& d = all.by_handle[h];
    if (!d) d = std::make_shared<Decoder>();
    return d;
}

void forget_decoder(u32 h)
{
    Decoders& all = decoders();
    std::lock_guard lock(all.m);
    all.by_handle.erase(h);
}

lv2::PpuThread& thread(PPUContext& ctx) { return thread_of(ctx); }

// The four notifications (AU done, PCM out, error, sequence done) are (function, argument) pairs from ax::notify.
constexpr NotifySlots kSlots{ax::notify, ax::notify_guest};

void put_name(u8* base, u32 at, std::string_view name)
{
    for (u32 i = 0; i < 8; i++) ps_write8(base, at + i, i < name.size() ? static_cast<u8>(name[i]) : 0);
}

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
s32 signal(PPUContext& ctx, u32 h, u32 field) { return lv2::sys_cond_signal(thread(ctx), ps_read32(guest_base(ctx), h + field)); }
s32 wait(PPUContext& ctx, u32 h, u32 field) { return lv2::sys_cond_wait(thread(ctx), ps_read32(guest_base(ctx), h + field), 0); }

u32 cmd_at(u32 h, u32 index) { return h + 0x10 + ax::kCmdSize * (index & 3); }

struct Cmd {
    u32 type = INVALID;
    s32 pcm_handle = 0;
    u32 au_start = 0, au_size = 0;
};

// RPCS3 AtracXdecContext::send_command: queued (a close only when nothing is), the thread signalled.
s32 send_command(PPUContext& ctx, u32 h, const Cmd& c, u32 param)
{
    u8* base = guest_base(ctx);
    lock(ctx, h, ax::queue_mutex);
    const u32 size = ps_read32(base, h + ax::cmd_size);
    if (c.type == CLOSE && size != 0) {
        unlock(ctx, h, ax::queue_mutex);
        return lv2::CELL_OK;
    }
    if (size >= 4) {
        unlock(ctx, h, ax::queue_mutex);
        return error(CELL_ADEC_ERROR_ATX_BUSY);
    }
    const u32 back = ps_read32(base, h + ax::cmd_back), at = cmd_at(h, back);
    ps_write32(base, at + ax::c_type, c.type);
    if (c.type == DECODE_AU) {
        ps_write32(base, at + ax::c_pcm_handle, static_cast<u32>(c.pcm_handle));
        ps_write32(base, at + ax::c_au_start, c.au_start);
        ps_write32(base, at + ax::c_au_size, c.au_size);
    }
    if (param) std::memmove(base + at + ax::c_param, base + param, ax::kParamSize);
    ps_write32(base, h + ax::cmd_back, (back + 1) & 3);
    ps_write32(base, h + ax::cmd_size, size + 1);
    unlock(ctx, h, ax::queue_mutex);
    signal(ctx, h, ax::queue_not_empty);
    return lv2::CELL_OK;
}

// AdecCmdQueue::pop: the front command (its handle set to 0xFF).
Cmd pop_command(u8* base, u32 h)
{
    const u32 front = ps_read32(base, h + ax::cmd_front), at = cmd_at(h, front);
    Cmd c;
    c.type = ps_read32(base, at + ax::c_type);
    c.pcm_handle = static_cast<s32>(ps_read32(base, at + ax::c_pcm_handle));
    c.au_start = ps_read32(base, at + ax::c_au_start);
    c.au_size = ps_read32(base, at + ax::c_au_size);
    ps_write32(base, at + ax::c_pcm_handle, 0xFF);
    ps_write32(base, h + ax::cmd_front, (front + 1) & 3);
    ps_write32(base, h + ax::cmd_size, ps_read32(base, h + ax::cmd_size) - 1);
    return c;
}

// RPCS3 AtracXdecDecoder::set_config_info: what libatxdec's SPU program accepts.
s32 set_config_info(u8* base, u32 h, Decoder& d, u32 freq, u32 ch_config, u32 nbytes)
{
    const u32 dec = h + ax::decoder;
    ps_write32(base, dec + ax::d_freq, freq);
    ps_write32(base, dec + ax::d_ch_config, ch_config);
    ps_write32(base, dec + ax::d_nbytes, nbytes);
    ps_write32(base, dec + ax::d_nbytes_aligned, (nbytes + 0x7F) & ~0x7Fu);
    ps_write32(base, dec + ax::d_nch_in, ch_config <= 4 ? ch_config : ch_config + 1);
    d.config_is_set = false;
    if (ch_config > 7) {
        PS_LOG_ERROR(kChan, "ATRAC3plus: channel configuration {}", ch_config);
        return error(ATX_CONFIG_FAILED);
    }
    ps_write32(base, dec + ax::d_nch_blocks, kChannelBlocks[ch_config]);
    if (ch_config == 0 || (freq != 48000 && freq != 44100) || nbytes == 0 || nbytes > kMaxFrameLength) {
        // 32 kHz is refused too, though Sony's documentation lists it (RPCS3)
        PS_LOG_ERROR(kChan, "ATRAC3plus: configuration refused: channels {}, {} Hz, {:#x}-byte frames", ch_config, freq, nbytes);
        return error(ATX_CONFIG_FAILED);
    }
    d.config_is_set = true;
    return lv2::CELL_OK;
}

// RPCS3 AtracXdecDecoder::init_decode and init_avcodec: the output format, then FFmpeg opened for the configuration.
s32 init_decode(u8* base, u32 h, Decoder& d, u32 bw_pcm, u32 nch_out)
{
    if (bw_pcm < WORD_SZ_16BIT || (bw_pcm > WORD_SZ_32BIT && bw_pcm != WORD_SZ_FLOAT)) {
        PS_LOG_ERROR(kChan, "ATRAC3plus: PCM format {:#x}", bw_pcm);
        return error(ATX_CONFIG_FAILED);
    }
    const u32 dec = h + ax::decoder;
    const u32 nch_in = ps_read32(base, dec + ax::d_nch_in);
    ps_write32(base, dec + ax::d_bw_pcm, bw_pcm);
    ps_write32(base, dec + ax::d_nch_out, nch_out); // unchecked on LLE
    ps_write32(base, dec + ax::d_pcm_output_size, (bw_pcm == WORD_SZ_16BIT ? 2 : 4) * nch_in * kSamplesPerFrame);
    if (!d.codec) {
        d.codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
        d.packet = av_packet_alloc();
        d.frame = av_frame_alloc();
        if (!d.codec || !d.packet || !d.frame) PS_FATAL(kChan, "ATRAC3plus: FFmpeg's decoder is not in this build");
    }
    avcodec_free_context(&d.av);
    d.av = avcodec_alloc_context3(d.codec);
    d.av->block_align = static_cast<int>(ps_read32(base, dec + ax::d_nbytes));
    av_channel_layout_default(&d.av->ch_layout, static_cast<int>(nch_in));
    d.av->sample_rate = static_cast<int>(ps_read32(base, dec + ax::d_freq));
    if (const int err = avcodec_open2(d.av, d.codec, nullptr); err < 0)
        PS_FATAL(kChan, "ATRAC3plus: FFmpeg's decoder did not open ({} channels, {} Hz): {}", nch_in,
            ps_read32(base, dec + ax::d_freq), err);
    return lv2::CELL_OK;
}

// RPCS3 AtracXdecDecoder::parse_ats_header: the 8-byte header some streams put before each frame (sync 0x0FD0, the
// rate, channels and frame length).
s32 parse_ats_header(u8* base, u32 h, Decoder& d, u32 au)
{
    const u16 sync = ps_read16(base, au), params = ps_read16(base, au + 2);
    if (sync != 0x0FD0) {
        PS_LOG_ERROR(kChan, "ATRAC3plus: ATS header sync {:#06x}", sync);
        return error(CELL_ADEC_ERROR_ATX_ATSHDR);
    }
    const u32 rate_idx = params >> 13, ch_config = params >> 10 & 7, nbytes = ((params & 0x3FFu) + 1) * 8;
    if (ch_config == 0 || (rate_idx != 1 && rate_idx != 2)) {
        PS_LOG_ERROR(kChan, "ATRAC3plus: ATS header channels {}, rate index {}", ch_config, rate_idx);
        return error(CELL_ADEC_ERROR_ATX_ATSHDR);
    }
    return set_config_info(base, h, d, rate_idx == 1 ? 44100 : 48000, ch_config, nbytes);
}

void write_float(u8* base, u32 at, f32 v) { ps_write32(base, at, std::bit_cast<u32>(v)); }

// Whether [addr, addr + size) is guest memory, page by page: a frame is copied whole from where its unit starts.
bool guest_range(lv2::PpuThread& t, u32 addr, u64 size)
{
    if (!size) return true;
    if (u64{addr} + size > GuestMemory::kGuestSize) return false;
    const u64 page = GuestMemory::host_page_size();
    for (u64 a = addr & ~(page - 1); a < u64{addr} + size; a += page)
        if (!t.kernel.memory().is_committed(static_cast<u32>(a))) return false;
    return true;
}

// The decoded frame into the output in the PCM format asked for, interleaved in libatxdec's channel order (RPCS3).
void convert(u8* base, u32 output, const AVFrame* frame, u32 samples, u32 nch_in, u32 bw_pcm, u32 ch_config)
{
    const u8* map = kChannelMap[ch_config - 1];
    for (u32 c = 0; c < nch_in; c++) {
        const f32* in = reinterpret_cast<const f32*>(frame->extended_data[c]);
        for (u32 i = 0, o = map[c]; i < samples; i++, o += nch_in) {
            const f32 s = in[i];
            switch (bw_pcm) {
            case WORD_SZ_FLOAT:
                if (s >= std::bit_cast<f32>(std::bit_cast<u32>(1.f) - 1)) ps_write32(base, output + 4 * o, 0x3F7FFFFF);
                else write_float(base, output + 4 * o, s <= -1.f ? -1.f : s);
                break;
            case WORD_SZ_16BIT:
                ps_write16(base, output + 2 * o, static_cast<u16>(s >= 1.f ? 0x7FFF : s <= -1.f ? -0x8000 :
                    static_cast<s16>(std::floor(s * 32768.f))));
                break;
            case WORD_SZ_24BIT:
                ps_write32(base, output + 4 * o, s >= 1.f ? 0x007FFFFFu : s <= -1.f ? 0x00800000u :
                    static_cast<u32>(static_cast<s32>(std::floor(s * 8388608.f))) & 0x00FFFFFFu);
                break;
            case WORD_SZ_32BIT:
                ps_write32(base, output + 4 * o, s >= 1.f ? 0x7FFFFFFFu : s <= -1.f ? 0x80000000u :
                    static_cast<u32>(static_cast<s32>(std::floor(static_cast<double>(s) * 2147483648.0))));
                break;
            default: break;
            }
        }
    }
}

// RPCS3 AtracXdecContext::exec, decode_au, once the output was taken: the frame decoded, then AUDONE and PCMOUT.
void decode_au(PPUContext& ctx, u32 h, Decoder& d, const Cmd& cmd)
{
    u8* base = guest_base(ctx);
    const u32 dec = h + ax::decoder, param = h + ax::param;
    const bool ats = ps_read8(base, param + ax::p_ats) != 0;
    const CoreNotifier notify(ctx, h, kSlots); // what open was given (D-591)
    s32 err = lv2::CELL_OK;
    // Only the first ATS header of a sequence is read.
    if (ps_read8(base, h + ax::first_decode) && ats) {
        if ((err = parse_ats_header(base, h, d, cmd.au_start)) != lv2::CELL_OK)
            notify.error(err);
        else if (init_decode(base, h, d, ps_read32(base, param + ax::p_bw_pcm), ps_read32(base, param + ax::p_nch_out)) !=
            lv2::CELL_OK)
            notify.error(error(CELL_ADEC_ERROR_ATX_FATAL));
    }
    u32 output = 0, decoded = 0; // LLE leaves the output address unset when the ATS header is bad
    if (err != error(CELL_ADEC_ERROR_ATX_ATSHDR)) {
        const u32 frame_at = cmd.au_start + (ats ? 8 : 0), frame_bytes = ps_read32(base, dec + ax::d_nbytes);
        if (!d.config_is_set || !d.av) {
            // libatxdec's SPU program would crash (RPCS3 aborts): logged, reported as a fatal error, nothing out.
            PS_LOG_ERROR(kChan, "ATRAC3plus: a decode without a valid configuration (decoder {:#x})", h);
            err = error(CELL_ADEC_ERROR_ATX_FATAL);
            notify.error(err);
        } else if (!guest_range(thread_of(ctx), frame_at, frame_bytes)) {
            // The frame is copied whole from the unit's start, past a unit shorter than it; one that runs past guest
            // memory would fault libatxdec's DMA. Logged, a fatal error, nothing out, as above (D-605).
            PS_LOG_ERROR(kChan, "ATRAC3plus: a frame of {:#x} bytes at {:#010x} runs past guest memory (decoder {:#x})",
                frame_bytes, frame_at, h);
            err = error(CELL_ADEC_ERROR_ATX_FATAL);
            notify.error(err);
        } else {
            const u32 nch_in = ps_read32(base, dec + ax::d_nch_in), nbytes = ps_read32(base, dec + ax::d_nbytes);
            const u32 bw_pcm = ps_read32(base, dec + ax::d_bw_pcm), nch_out = ps_read32(base, dec + ax::d_nch_out);
            const u32 work = ps_read32(base, h + ax::work_mem);
            output = work + spurs_mem_size(nch_in);
            std::memmove(base + work, base + frame_at, nbytes); // as libatxdec copies it
            d.packet->data = base + work;
            d.packet->size = static_cast<int>(nbytes);
            if (ps_read8(base, work) & 0x80) {
                // A frame whose first bit is set: libatxdec's own error for it (D-198; FFmpeg's would be generic).
                err = error(CELL_ADEC_ERROR_ATX_POSCFG_PRESENT);
                av_frame_unref(d.frame);
            } else if (const int e = avcodec_send_packet(d.av, d.packet); e < 0) {
                PS_LOG_ERROR(kChan, "ATRAC3plus: FFmpeg refused a frame ({}): the game sent invalid data", e);
                err = error(CELL_ADEC_ERROR_ATX_NON_FATAL); // FFmpeg has no finer errors than this
                av_frame_unref(d.frame);
            } else if (const int e2 = avcodec_receive_frame(d.av, d.frame); e2 < 0) {
                PS_FATAL(kChan, "ATRAC3plus: FFmpeg gave no frame for a packet ({})", e2);
            }
            d.packet->data = nullptr;
            d.packet->size = 0;
            decoded = static_cast<u32>(std::max(d.frame->nb_samples, 0));
            if (decoded != 0 && decoded != kSamplesPerFrame)
                PS_FATAL(kChan, "ATRAC3plus: FFmpeg gave {} samples for a frame", decoded);
            // The first frame of a sequence or after an error comes out as silence.
            if (ps_read8(base, h + ax::skip_next_frame) && err == lv2::CELL_OK) {
                ps_write8(base, h + ax::skip_next_frame, 0);
                decoded = 0;
                std::memset(base + output, 0, kSamplesPerFrame * (bw_pcm & 0x7F) * nch_out);
            }
            convert(base, output, d.frame, decoded, nch_in, bw_pcm, ps_read32(base, dec + ax::d_ch_config));
            ps_write8(base, h + ax::first_decode, 0);
            if (err != lv2::CELL_OK) {
                ps_write8(base, h + ax::skip_next_frame, 1);
                notify.error(err);
            }
        }
    }
    // AUDONE and PCMOUT follow every decode, even a failed one; the output is to be taken in any case.
    notify.au_done(cmd.pcm_handle);
    ps_write32(base, h + ax::output_locked, 1);
    unlock(ctx, h, ax::output_mutex);
    const u32 freq = ps_read32(base, dec + ax::d_freq);
    const u32 output_size = decoded * (ps_read32(base, dec + ax::d_bw_pcm) & 0x7F) * ps_read32(base, dec + ax::d_nch_out);
    StackVar bsi(ctx, base, 12); // CellAdecAtracXInfo: samplingFreq, channelConfigIndex, nbytes
    ps_write32(base, bsi.addr(), freq);
    ps_write32(base, bsi.addr() + 4, ps_read32(base, dec + ax::d_ch_config));
    ps_write32(base, bsi.addr() + 8, ps_read32(base, dec + ax::d_nbytes));
    const s8 pts_type = freq == 32000 ? 4 : freq == 44100 ? 3 : freq == 48000 ? 2 : -1;
    notify.pcm_out(cmd.pcm_handle, output, output_size, bsi.addr(), pts_type, err);
}

// Whether the context at `h` is still the calling thread's (D-645): `r`, the answer of the call just made on its
// mutexes or conditions, is CELL_OK, and its first word names the thread (open writes the ID there before it starts the
// thread). The context is the game's memory, and a game can overwrite it while the decoder is open: a damaged file's
// PCM run past its buffer onto it (the cellAtrac fuzzing's), memory used again after a close cellAdec refused.
// libatxdec's thread would go on with what is there then (RPCS3 ensure()s each call and stops the emulator): a lock and
// a wait that fail at once, round and round, past the process's end, or the queue of another decoder opened in the same
// place, taking its commands. This one ends instead, logged; a thread waiting on its own condition when the context
// goes stays so, as on a PS3, until the process ends.
bool own_context(PPUContext& ctx, u32 h, s32 r, std::string_view what)
{
    const u32 self = thread(ctx).id;
    const u64 named = ps_read64(guest_base(ctx), h + ax::thread_id);
    if (r == lv2::CELL_OK && named == self) return true;
    const std::string answer =
        r == lv2::CELL_OK ? "" : std::format("; {} answered {:#010x}", what, static_cast<u32>(r));
    PS_LOG_ERROR(kChan, "ATRAC3plus decoder {:#x}: its context is no longer its own (it names thread {:#x}{}): the "
        "game overwrote it while the decoder was open; the decoder's thread {:#x} ends", h, named, answer, self);
    return false;
}

// The thread's run_thread check (RPCS3: the decoder stops at the next command once Close cleared it); false too when
// the context is no longer the thread's (D-645).
bool still_running(PPUContext& ctx, u32 h)
{
    const s32 r = lock(ctx, h, ax::run_thread_mutex);
    const bool run = own_context(ctx, h, r, "run_thread's mutex") && ps_read32(guest_base(ctx), h + ax::run_thread);
    if (r == lv2::CELL_OK) unlock(ctx, h, ax::run_thread_mutex);
    return run;
}

// RPCS3 AtracXdecContext::exec. Its context lost (own_context), the thread ends where RPCS3's ensure()s would stop.
void run(PPUContext& ctx, u32 h)
{
    u8* base = guest_base(ctx);
    const std::shared_ptr<Decoder> d = decoder_of(h);
    for (;;) {
        for (;;) {
            const s32 r = lock(ctx, h, ax::queue_mutex);
            if (!own_context(ctx, h, r, "the queue's mutex")) {
                if (r == lv2::CELL_OK) unlock(ctx, h, ax::queue_mutex);
                return;
            }
            if (ps_read32(base, h + ax::cmd_size) != 0) break;
            const s32 w = wait(ctx, h, ax::queue_not_empty);
            unlock(ctx, h, ax::queue_mutex);
            if (!own_context(ctx, h, w, "the queue's condition")) return;
        }
        const u32 front = ps_read32(base, h + ax::cmd_front);
        const u32 param_at = cmd_at(h, front) + ax::c_param;
        u8 param[ax::kParamSize];
        std::memcpy(param, base + param_at, sizeof(param));
        const Cmd cmd = pop_command(base, h);
        unlock(ctx, h, ax::queue_mutex);
        if (!still_running(ctx, h)) return;
        switch (cmd.type) {
        case START_SEQ: {
            ps_write8(base, h + ax::first_decode, 1);
            ps_write8(base, h + ax::skip_next_frame, 1);
            const auto p = [&](u32 off) { return ps_read32(param, off); };
            // With ATS headers, the parameters come in the first access unit's.
            if (param[ax::p_ats] == 0) {
                if (set_config_info(base, h, *d, p(ax::p_freq), p(ax::p_ch_config), p(ax::p_nbytes)) != lv2::CELL_OK) break;
                if (init_decode(base, h, *d, p(ax::p_bw_pcm), p(ax::p_nch_out)) != lv2::CELL_OK) break;
            }
            std::memcpy(base + h + ax::param, param, sizeof(param));
            break;
        }
        case END_SEQ: CoreNotifier(ctx, h, kSlots).seq_done(); break;
        case DECODE_AU: {
            const s32 locked = lock(ctx, h, ax::output_mutex);
            s32 r = locked;
            while (r == lv2::CELL_OK && ps_read32(base, h + ax::output_locked)) r = wait(ctx, h, ax::output_consumed);
            if (!own_context(ctx, h, r, "the output's mutex or condition") || !still_running(ctx, h)) {
                if (locked == lv2::CELL_OK) unlock(ctx, h, ax::output_mutex);
                return;
            }
            decode_au(ctx, h, *d, cmd);
            break;
        }
        case CLOSE: break; // run_thread is clear: the next turn ends
        default: PS_FATAL(kChan, "ATRAC3plus: command type {} in the queue of {:#x}", cmd.type, h);
        }
    }
}

class AtracXCore final : public AdecCore {
public:
    explicit AtracXCore(u32 nch) : nch_(nch) {}

    // RPCS3 _CellAdecCoreOpGetMemSize_atracx: the context, libatxdec's SPURS structures and SPU memory, a frame of floats
    // per channel.
    u32 mem_size(PPUContext&) override
    {
        const u32 size = ax::size + 0x7F + kSpursStructsSize + 0x1D8 + spurs_mem_size(nch_) + kSamplesPerFrame * 4 * nch_;
        return (size + 0x7F) & ~0x7Fu;
    }
    s32 pcm_handle_num(PPUContext&) override { return 3; }
    u32 bsi_info_size(PPUContext&) override { return 12; } // CellAdecAtracXInfo
    u32 version(PPUContext&) override { return 0x01020000; }

    s32 open(PPUContext& ctx, u32 h, const CoreNotify& notify, u32 res, u32) override
    {
        u8* base = guest_base(ctx);
        std::memset(base + h, 0, ax::size);
        HleProcess& process = *HleProcess::current();
        store_notify(base, h, kSlots, notify);
        const u32 work = h + ((ax::size + 0x7F) & ~0x7Fu) + kSpursStructsSize;
        ps_write32(base, h + ax::work_mem, work);
        ps_write32(base, h + ax::decoder + ax::d_work_mem, work);
        ps_write32(base, h + ax::run_thread, 1);
        s32 r = create_mutex(ctx, h + ax::queue_mutex, "_atd001");
        if (r == lv2::CELL_OK) r = create_cond(ctx, h + ax::queue_not_empty, h + ax::queue_mutex, "_atd002");
        if (r == lv2::CELL_OK) r = create_mutex(ctx, h + ax::run_thread_mutex, "_atd003");
        if (r == lv2::CELL_OK) r = create_cond(ctx, h + ax::run_thread_cond, h + ax::run_thread_mutex, "_atd004");
        if (r == lv2::CELL_OK) r = create_mutex(ctx, h + ax::output_mutex, "_atd005");
        if (r == lv2::CELL_OK) r = create_cond(ctx, h + ax::output_consumed, h + ax::output_mutex, "_atd006");
        if (r != lv2::CELL_OK) return r;
        lock(ctx, h, ax::output_mutex);
        ps_write32(base, h + ax::output_locked, 0);
        signal(ctx, h, ax::output_consumed);
        unlock(ctx, h, ax::output_mutex);
        forget_decoder(h); // a decoder opened again in the same memory starts afresh
        lv2::PpuThread& t = thread(ctx);
        const u32 entry = core_thread_entry(NID_atracx_dec_thread, "_pspocket_atracx_dec_thread");
        if (!entry) {
            PS_LOG_ERROR(kChan, "ATRAC3plus: no memory to link the decoder's thread function");
            return error(CELL_ADEC_ERROR_FATAL);
        }
        const u32 tls = process.alloc_tls();
        ps_write32(base, h + ax::tls, tls);
        StackVar sv(ctx, base, 0x30);
        ps_write32(base, sv.addr() + 8, entry);
        ps_write32(base, sv.addr() + 12, tls ? tls + kTlsR13Offset : 0);
        std::memcpy(base + sv.addr() + 0x10, "HLE ATRAC3plus decoder", 23);
        r = lv2::_sys_ppu_thread_create(t, sv.addr(), sv.addr() + 8, h, 0, static_cast<s32>(ps_read32(base, res + 8)),
            ps_read32(base, res + 16), 1, sv.addr() + 0x10);
        if (r == lv2::CELL_OK) {
            ps_write64(base, h + ax::thread_id, ps_read64(base, sv.addr()));
            r = lv2::sys_ppu_thread_start(t, static_cast<u32>(ps_read64(base, sv.addr())));
        }
        if (r != lv2::CELL_OK && tls) {
            process.free_tls(tls);
            ps_write32(base, h + ax::tls, 0);
        }
        return r;
    }

    // RPCS3 _CellAdecCoreOpClose_atracx.
    s32 close(PPUContext& ctx, u32 h) override
    {
        u8* base = guest_base(ctx);
        lv2::PpuThread& t = thread(ctx);
        lock(ctx, h, ax::run_thread_mutex);
        ps_write32(base, h + ax::run_thread, 0);
        unlock(ctx, h, ax::run_thread_mutex);
        send_command(ctx, h, Cmd{CLOSE, 0, 0, 0}, 0);
        lock(ctx, h, ax::output_mutex);
        ps_write32(base, h + ax::output_locked, 0);
        unlock(ctx, h, ax::output_mutex);
        signal(ctx, h, ax::output_consumed);
        StackVar status(ctx, base, 8);
        lv2::sys_ppu_thread_join(t, static_cast<u32>(ps_read64(base, h + ax::thread_id)), status.addr());
        if (const u32 tls = ps_read32(base, h + ax::tls)) HleProcess::current()->free_tls(tls);
        ps_write32(base, h + ax::tls, 0);
        forget_decoder(h);
        s32 r = lv2::sys_cond_destroy(t, ps_read32(base, h + ax::queue_not_empty));
        if (r == lv2::CELL_OK) r = lv2::sys_cond_destroy(t, ps_read32(base, h + ax::run_thread_cond));
        if (r == lv2::CELL_OK) r = lv2::sys_cond_destroy(t, ps_read32(base, h + ax::output_consumed));
        if (r == lv2::CELL_OK) r = lv2::sys_mutex_destroy(t, ps_read32(base, h + ax::queue_mutex));
        if (r == lv2::CELL_OK) r = lv2::sys_mutex_destroy(t, ps_read32(base, h + ax::run_thread_mutex));
        if (r == lv2::CELL_OK) r = lv2::sys_mutex_destroy(t, ps_read32(base, h + ax::output_mutex));
        return r != lv2::CELL_OK ? error(CELL_ADEC_ERROR_FATAL) : lv2::CELL_OK;
    }

    s32 start_seq(PPUContext& ctx, u32 h, u32 param) override
    {
        if (!guest_readable(thread(ctx), param, ax::kParamSize)) {
            log_call_site(ctx, guest_base(ctx), std::format("cellAdecStartSeq: the ATRAC3plus parameters ({:#x}) are not "
                "guest memory", param));
            return error(CELL_ADEC_ERROR_ARG);
        }
        return send_command(ctx, h, Cmd{START_SEQ, 0, 0, 0}, param);
    }

    s32 end_seq(PPUContext& ctx, u32 h) override { return send_command(ctx, h, Cmd{END_SEQ, 0, 0, 0}, 0); }

    s32 decode_au(PPUContext& ctx, u32 h, s32 pcm_handle, u32 au_info) override
    {
        u8* base = guest_base(ctx);
        return send_command(ctx, h, Cmd{DECODE_AU, pcm_handle, ps_read32(base, au_info), ps_read32(base, au_info + 4)}, 0);
    }

    // RPCS3 _CellAdecCoreOpRealign_atracx: a whole frame's PCM copied out, when the game gave a buffer.
    s32 realign(PPUContext& ctx, u32 h, u32 out_buffer, u32 pcm_start) override
    {
        if (!out_buffer) return lv2::CELL_OK;
        u8* base = guest_base(ctx);
        const u32 size = ps_read32(base, h + ax::decoder + ax::d_pcm_output_size);
        if (!pcm_start || !guest_readable(thread(ctx), out_buffer, size)) {
            log_call_site(ctx, base, std::format("cellAdecGetPcm: the buffer ({:#x}, {} bytes) or the PCM ({:#x}) is not "
                "guest memory", out_buffer, size, pcm_start));
            return error(CELL_ADEC_ERROR_ARG);
        }
        std::memmove(base + out_buffer, base + pcm_start, size);
        return lv2::CELL_OK;
    }

    s32 release_pcm(PPUContext& ctx, u32 h, s32, u32) override
    {
        lock(ctx, h, ax::output_mutex);
        ps_write32(guest_base(ctx), h + ax::output_locked, 0);
        signal(ctx, h, ax::output_consumed);
        unlock(ctx, h, ax::output_mutex);
        return lv2::CELL_OK;
    }

private:
    u32 nch_;
};

} // namespace

std::unique_ptr<AdecCore> make_atracx_core(u32 max_channels) { return std::make_unique<AtracXCore>(max_channels); }

void _pspocket_atracx_dec_thread(PPUContext& ctx, u64 core_handle)
{
    run(ctx, static_cast<u32>(core_handle));
    lv2::_sys_ppu_thread_exit(thread_of(ctx), 0);
}

#else

std::unique_ptr<AdecCore> make_atracx_core(u32) { return nullptr; }

void _pspocket_atracx_dec_thread(PPUContext& ctx, u64)
{
    PS_LOG_ERROR("cellAdec", "ATRAC3plus: pspocket was built without FFmpeg");
    lv2::_sys_ppu_thread_exit(thread_of(ctx), 0);
}

#endif

} // namespace pspocket::hle::adec
