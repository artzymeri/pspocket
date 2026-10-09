// The common part of pspocket's own cellAdec cores on FFmpeg (D-577): see adec_ffmpeg.hpp. The queue, the decoder's
// thread and the hand-over of the output follow adec_atracx.cpp, RPCS3's port of libatxdec's PPU side
// (rpcs3/Emu/Cell/Modules/cellAtracXdec.cpp): a command queued (a close only when nothing is), the thread signalled;
// the thread pops one, decodes it once the previous output was taken, notifies AUDONE, locks the output and notifies
// PCMOUT; cellAdecGetPcm copies the PCM out (realign) and releases the output (releasePcm).

#include "adec_ffmpeg.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <set>
#include <string>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/syscalls.hpp"

#if PSPOCKET_HAVE_FFMPEG
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
}
#endif

namespace pspocket::hle::adec {

// The cores that correct no PTS of their own (the front end's fixed steps serve them): never asked.
s32 AdecCore::correct_pts(PPUContext&, u32 core_handle, u32)
{
    PS_LOG_ERROR("cellAdec", "core {:#x}: a PTS correction of its own was asked of a core that has none", core_handle);
    return error(CELL_ADEC_ERROR_FATAL);
}

#if PSPOCKET_HAVE_FFMPEG

namespace ffcore {

namespace {

constexpr std::string_view kChan = "cellAdec";

enum CmdType : u32 { INVALID = 0, START_SEQ = 1, DECODE_AU = 2, END_SEQ = 3, CLOSE = 4 };

lv2::PpuThread& thread(PPUContext& ctx) { return thread_of(ctx); }

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

u32 cmd_at(u32 h, u32 index) { return h + fx::kCmdSize * (index & 3); }

struct Cmd {
    u32 type = INVALID;
    s32 pcm_handle = 0;
    u32 au_start = 0, au_size = 0;
};

// As AtracXdecContext::send_command (RPCS3): queued (a close only when nothing is), the thread signalled. `param`
// (host bytes, `param_size` of them) goes with a start.
s32 send_command(PPUContext& ctx, u32 h, const Cmd& c, const u8* param, u32 param_size, u32 busy)
{
    u8* base = guest_base(ctx);
    lock(ctx, h, fx::queue_mutex);
    const u32 size = ps_read32(base, h + fx::cmd_size);
    if (c.type == CLOSE && size != 0) {
        unlock(ctx, h, fx::queue_mutex);
        return lv2::CELL_OK;
    }
    if (size >= 4) {
        unlock(ctx, h, fx::queue_mutex);
        return error(busy);
    }
    const u32 back = ps_read32(base, h + fx::cmd_back), at = cmd_at(h, back);
    ps_write32(base, at + fx::c_type, c.type);
    ps_write32(base, at + fx::c_pcm_handle, static_cast<u32>(c.pcm_handle));
    ps_write32(base, at + fx::c_au_start, c.au_start);
    ps_write32(base, at + fx::c_au_size, c.au_size);
    if (param) std::memcpy(base + at + fx::c_param, param, param_size);
    ps_write32(base, h + fx::cmd_back, (back + 1) & 3);
    ps_write32(base, h + fx::cmd_size, size + 1);
    unlock(ctx, h, fx::queue_mutex);
    signal(ctx, h, fx::queue_not_empty);
    return lv2::CELL_OK;
}

// AdecCmdQueue::pop: the front command (its handle set to 0xFF), with its parameters.
Cmd pop_command(u8* base, u32 h, u8 (&param)[fx::kParamMax])
{
    const u32 front = ps_read32(base, h + fx::cmd_front), at = cmd_at(h, front);
    Cmd c;
    c.type = ps_read32(base, at + fx::c_type);
    c.pcm_handle = static_cast<s32>(ps_read32(base, at + fx::c_pcm_handle));
    c.au_start = ps_read32(base, at + fx::c_au_start);
    c.au_size = ps_read32(base, at + fx::c_au_size);
    std::memcpy(param, base + at + fx::c_param, sizeof(param));
    ps_write32(base, at + fx::c_pcm_handle, 0xFF);
    ps_write32(base, h + fx::cmd_front, (front + 1) & 3);
    ps_write32(base, h + fx::cmd_size, ps_read32(base, h + fx::cmd_size) - 1);
    return c;
}

bool still_running(PPUContext& ctx, u32 h)
{
    lock(ctx, h, fx::run_thread_mutex);
    const bool run = ps_read32(guest_base(ctx), h + fx::run_thread) != 0;
    unlock(ctx, h, fx::run_thread_mutex);
    return run;
}

// One access unit: decoded (the codec), then AUDONE, the output locked, PCMOUT with the information block.
void decode_au(PPUContext& ctx, u32 h, const Spec& spec, Codec& codec, const Cmd& cmd)
{
    u8* base = guest_base(ctx);
    const u32 output = ps_read32(base, h + fx::output);
    StackVar bsi(ctx, base, std::max<u32>(spec.bsi_size, 4));
    Decoded d;
    if (!cmd.au_size || !guest_readable(thread(ctx), cmd.au_start, cmd.au_size)) {
        PS_LOG_ERROR(kChan, "{}: the access unit ({:#x}, {} bytes) is not guest memory", spec.name, cmd.au_start, cmd.au_size);
        d = codec.decode(base, 0, 0, output, bsi.addr()); // the codec's answer to an empty unit
    } else {
        d = codec.decode(base, cmd.au_start, cmd.au_size, output, bsi.addr());
    }
    if (d.size > spec.output_bytes) PS_FATAL(kChan, "{}: {} bytes of PCM for a {}-byte output", spec.name, d.size, spec.output_bytes);
    if (d.error != lv2::CELL_OK) notify_error(ctx, d.error, ps_read32(base, h + fx::notify_error + 4));
    // AUDONE and PCMOUT follow every decode, even a failed one, as libatxdec's (adec_atracx.cpp).
    notify_au_done(ctx, cmd.pcm_handle, ps_read32(base, h + fx::notify_au_done + 4));
    ps_write32(base, h + fx::output_locked, 1);
    unlock(ctx, h, fx::output_mutex);
    ps_write32(base, h + fx::pcm_size, d.size);
    ps_write32(base, h + fx::duration, d.duration);
    notify_pcm_out(ctx, cmd.pcm_handle, output, d.size, ps_read32(base, h + fx::notify_pcm_out + 4), bsi.addr(),
        d.pts_type, d.error);
}

// The decoder's thread (as AtracXdecContext::exec): one command at a time until Close.
void run(PPUContext& ctx, u32 h)
{
    u8* base = guest_base(ctx);
    const s32 type = static_cast<s32>(ps_read32(base, h + fx::type));
    const Spec* spec = spec_of(type);
    if (!spec) PS_FATAL(kChan, "core {:#x}: codec type {} has no core on FFmpeg", h, type);
    const std::unique_ptr<Codec> codec = spec->make();
    for (;;) {
        for (;;) {
            lock(ctx, h, fx::queue_mutex);
            if (ps_read32(base, h + fx::cmd_size) != 0) break;
            wait(ctx, h, fx::queue_not_empty);
            unlock(ctx, h, fx::queue_mutex);
        }
        u8 param[fx::kParamMax];
        const Cmd cmd = pop_command(base, h, param);
        unlock(ctx, h, fx::queue_mutex);
        if (!still_running(ctx, h)) return;
        switch (cmd.type) {
        case START_SEQ:
            std::memcpy(base + h + fx::param, param, spec->param_size);
            if (const s32 r = codec->start(param); r != lv2::CELL_OK)
                PS_LOG_ERROR(kChan, "{} decoder {:#x}: the sequence's parameters were refused ({:#010x})", spec->name, h,
                    static_cast<u32>(r));
            break;
        case END_SEQ: notify_seq_done(ctx, ps_read32(base, h + fx::notify_seq_done + 4)); break;
        case DECODE_AU:
            lock(ctx, h, fx::output_mutex);
            while (ps_read32(base, h + fx::output_locked)) wait(ctx, h, fx::output_consumed);
            if (!still_running(ctx, h)) {
                unlock(ctx, h, fx::output_mutex);
                return;
            }
            decode_au(ctx, h, *spec, *codec, cmd);
            break;
        case CLOSE: break; // run_thread is clear: the next turn ends
        default: PS_FATAL(kChan, "{}: command type {} in the queue of {:#x}", spec->name, cmd.type, h);
        }
    }
}

class FfCore final : public AdecCore {
public:
    explicit FfCore(const Spec& spec) : spec_(spec) {}

    // The context, the output buffer, and room for the front end's alignment of the core's memory (up to 0x7F bytes
    // past what it asks for, as libatxdec's and LPCM's sizes allow).
    u32 mem_size(PPUContext&) override { return ((fx::size + spec_.output_bytes + 0x7F) & ~0x7Fu) + 0x80; }
    s32 pcm_handle_num(PPUContext&) override { return 3; } // as libatxdec's (adec_atracx.cpp)
    u32 bsi_info_size(PPUContext&) override { return spec_.bsi_size; }
    u32 version(PPUContext&) override { return spec_.version; }

    s32 open(PPUContext& ctx, u32 h, u32 context, u32 res, u32) override
    {
        u8* base = guest_base(ctx);
        std::memset(base + h, 0, fx::size);
        HleProcess& process = *HleProcess::current();
        const u32 fns[4] = {NID_notify_au_done, NID_notify_pcm_out, NID_notify_error, NID_notify_seq_done};
        for (u32 i = 0; i < 4; i++) {
            ps_write32(base, h + fx::notify_au_done + 8 * i, process.descriptor_of("cellAdec", fns[i]));
            ps_write32(base, h + fx::notify_au_done + 8 * i + 4, context);
        }
        ps_write32(base, h + fx::type, static_cast<u32>(spec_.type));
        ps_write32(base, h + fx::output, h + fx::size);
        ps_write32(base, h + fx::run_thread, 1);
        ps_write32(base, h + fx::prev_pts, CODEC_TS_INVALID);
        ps_write32(base, h + fx::prev_pts + 4, CODEC_TS_INVALID);
        s32 r = create_mutex(ctx, h + fx::queue_mutex, "_adf001");
        if (r == lv2::CELL_OK) r = create_cond(ctx, h + fx::queue_not_empty, h + fx::queue_mutex, "_adf002");
        if (r == lv2::CELL_OK) r = create_mutex(ctx, h + fx::run_thread_mutex, "_adf003");
        if (r == lv2::CELL_OK) r = create_mutex(ctx, h + fx::output_mutex, "_adf004");
        if (r == lv2::CELL_OK) r = create_cond(ctx, h + fx::output_consumed, h + fx::output_mutex, "_adf005");
        if (r != lv2::CELL_OK) return r;
        const u32 entry = process.descriptor_of("cellAdec", NID_ffmpeg_dec_thread);
        if (!entry) {
            PS_LOG_ERROR(kChan, "{}: the decoder's thread function is not linked (cellAdec was not imported)", spec_.name);
            return error(CELL_ADEC_ERROR_FATAL);
        }
        // The decoder's thread (CellAdecResource: ppuThreadPriority +8, ppuThreadStackSize +16), joinable.
        lv2::PpuThread& t = thread(ctx);
        const u32 tls = process.alloc_tls();
        ps_write32(base, h + fx::tls, tls);
        StackVar sv(ctx, base, 0x40);
        ps_write32(base, sv.addr() + 8, entry);
        ps_write32(base, sv.addr() + 12, tls ? tls + kTlsR13Offset : 0);
        const std::string name = std::format("HLE {} decoder", spec_.name);
        std::memcpy(base + sv.addr() + 0x10, name.c_str(), std::min<usize>(name.size() + 1, 0x30));
        r = lv2::_sys_ppu_thread_create(t, sv.addr(), sv.addr() + 8, h, 0, static_cast<s32>(ps_read32(base, res + 8)),
            ps_read32(base, res + 16), 1, sv.addr() + 0x10);
        if (r == lv2::CELL_OK) {
            ps_write64(base, h + fx::thread_id, ps_read64(base, sv.addr()));
            r = lv2::sys_ppu_thread_start(t, static_cast<u32>(ps_read64(base, sv.addr())));
        }
        if (r != lv2::CELL_OK && tls) {
            process.free_tls(tls);
            ps_write32(base, h + fx::tls, 0);
        }
        return r;
    }

    // As _CellAdecCoreOpClose_atracx (RPCS3): the thread stopped and joined, everything destroyed.
    s32 close(PPUContext& ctx, u32 h) override
    {
        u8* base = guest_base(ctx);
        lv2::PpuThread& t = thread(ctx);
        lock(ctx, h, fx::run_thread_mutex);
        ps_write32(base, h + fx::run_thread, 0);
        unlock(ctx, h, fx::run_thread_mutex);
        send_command(ctx, h, Cmd{CLOSE, 0, 0, 0}, nullptr, 0, spec_.busy);
        lock(ctx, h, fx::output_mutex);
        ps_write32(base, h + fx::output_locked, 0);
        unlock(ctx, h, fx::output_mutex);
        signal(ctx, h, fx::output_consumed);
        StackVar status(ctx, base, 8);
        lv2::sys_ppu_thread_join(t, static_cast<u32>(ps_read64(base, h + fx::thread_id)), status.addr());
        if (const u32 tls = ps_read32(base, h + fx::tls)) HleProcess::current()->free_tls(tls);
        ps_write32(base, h + fx::tls, 0);
        s32 r = lv2::sys_cond_destroy(t, ps_read32(base, h + fx::queue_not_empty));
        if (r == lv2::CELL_OK) r = lv2::sys_cond_destroy(t, ps_read32(base, h + fx::output_consumed));
        if (r == lv2::CELL_OK) r = lv2::sys_mutex_destroy(t, ps_read32(base, h + fx::queue_mutex));
        if (r == lv2::CELL_OK) r = lv2::sys_mutex_destroy(t, ps_read32(base, h + fx::run_thread_mutex));
        if (r == lv2::CELL_OK) r = lv2::sys_mutex_destroy(t, ps_read32(base, h + fx::output_mutex));
        return r != lv2::CELL_OK ? error(CELL_ADEC_ERROR_FATAL) : lv2::CELL_OK;
    }

    s32 start_seq(PPUContext& ctx, u32 h, u32 param) override
    {
        u8* base = guest_base(ctx);
        if (!guest_readable(thread(ctx), param, spec_.param_size)) {
            log_call_site(ctx, base, std::format("cellAdecStartSeq: the {} parameters ({:#x}, {} bytes) are not guest "
                "memory", spec_.name, param, spec_.param_size));
            return error(CELL_ADEC_ERROR_ARG);
        }
        u8 p[fx::kParamMax] = {};
        std::memcpy(p, base + param, spec_.param_size);
        if (spec_.check)
            if (const s32 r = spec_.check(p); r != lv2::CELL_OK) return r;
        return send_command(ctx, h, Cmd{START_SEQ, 0, 0, 0}, p, spec_.param_size, spec_.busy);
    }

    s32 end_seq(PPUContext& ctx, u32 h) override
    {
        return send_command(ctx, h, Cmd{END_SEQ, 0, 0, 0}, nullptr, 0, spec_.busy);
    }

    s32 decode_au(PPUContext& ctx, u32 h, s32 pcm_handle, u32 au_info) override
    {
        u8* base = guest_base(ctx);
        return send_command(ctx, h, Cmd{DECODE_AU, pcm_handle, ps_read32(base, au_info), ps_read32(base, au_info + 4)},
            nullptr, 0, spec_.busy);
    }

    // The PCM out copied to the game's buffer, when it gave one (as _CellAdecCoreOpRealign_atracx, with the size of
    // the PCM given out rather than of a whole frame).
    s32 realign(PPUContext& ctx, u32 h, u32 out_buffer, u32 pcm_start) override
    {
        if (!out_buffer) return lv2::CELL_OK;
        u8* base = guest_base(ctx);
        const u32 size = ps_read32(base, h + fx::pcm_size);
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
        lock(ctx, h, fx::output_mutex);
        ps_write32(guest_base(ctx), h + fx::output_locked, 0);
        signal(ctx, h, fx::output_consumed);
        unlock(ctx, h, fx::output_mutex);
        return lv2::CELL_OK;
    }

    // The codecs whose core module corrects PTS with a function of its own (MPEG audio, AAC): the front end's rule
    // (RPCS3 AdecContext::correct_pts_value) with this core's own last PTS and the length of the frame that had it.
    s32 correct_pts(PPUContext& ctx, u32 h, u32 au_pts) override
    {
        u8* base = guest_base(ctx);
        const u32 prev_upper = ps_read32(base, h + fx::prev_pts), prev_lower = ps_read32(base, h + fx::prev_pts + 4);
        if (ps_read32(base, au_pts) == CODEC_TS_INVALID) {
            if (ps_read32(base, au_pts + 4) != CODEC_TS_INVALID) return error(CELL_ADEC_ERROR_FATAL);
            if (prev_upper == CODEC_TS_INVALID && prev_lower == CODEC_TS_INVALID) return lv2::CELL_OK;
            u32 upper = prev_upper;
            const u32 lower = prev_lower + ps_read32(base, h + fx::prev_duration);
            if (lower < prev_lower) upper = (upper + 1) & 1; // a 33-bit count
            ps_write32(base, au_pts, upper);
            ps_write32(base, au_pts + 4, lower);
            if (upper == CODEC_TS_INVALID) return error(CELL_ADEC_ERROR_FATAL);
        }
        if (ps_read32(base, au_pts + 4) == CODEC_TS_INVALID) return error(CELL_ADEC_ERROR_FATAL);
        ps_write32(base, h + fx::prev_pts, ps_read32(base, au_pts));
        ps_write32(base, h + fx::prev_pts + 4, ps_read32(base, au_pts + 4));
        ps_write32(base, h + fx::prev_duration, ps_read32(base, h + fx::duration));
        return lv2::CELL_OK;
    }

private:
    const Spec& spec_;
};

std::mutex g_log_mutex;
std::set<std::string, std::less<>> g_logged;

} // namespace

const Spec* spec_of(s32 type)
{
    switch (type) {
    case CELL_ADEC_TYPE_MP3: return &kMp3;
    case CELL_ADEC_TYPE_MPEG_L1: return &kMpegL1;
    case CELL_ADEC_TYPE_MPEG_L2: return &kMpegL2;
    case CELL_ADEC_TYPE_M4AAC: return &kM4Aac;
    case CELL_ADEC_TYPE_M4AAC_2CH: return &kM4Aac2ch;
    case CELL_ADEC_TYPE_M4AAC_2CH_MOD: return &kM4Aac2chMod;
    default: return nullptr;
    }
}

void log_once(std::string_view key, std::string_view line)
{
    {
        std::lock_guard lock(g_log_mutex);
        if (!g_logged.emplace(key).second) return;
    }
    PS_LOG_WARN(kChan, "{}", line);
}

// ---- FFmpeg

bool FfDecoder::open(const char* decoder, std::string_view name, const std::function<void(AVCodecContext*)>& setup)
{
    close();
    name_ = name;
    const AVCodec* codec = avcodec_find_decoder_by_name(decoder);
    if (!codec) PS_FATAL(kChan, "{}: FFmpeg's {} decoder is not in this build", name, decoder);
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    av_ = avcodec_alloc_context3(codec);
    if (!packet_ || !frame_ || !av_) PS_FATAL(kChan, "{}: out of host memory for FFmpeg's decoder", name);
    av_->request_sample_fmt = AV_SAMPLE_FMT_FLTP;
    if (setup) setup(av_);
    if (const int err = avcodec_open2(av_, codec, nullptr); err < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(err, text, sizeof(text));
        PS_LOG_ERROR(kChan, "{}: FFmpeg's decoder did not open: {}", name, text);
        close();
        return false;
    }
    return true;
}

void FfDecoder::close()
{
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&av_);
}

int FfDecoder::decode(const u8* data, u32 size, Planar& out, u32 max_samples)
{
    if (!av_) return AVERROR(EINVAL);
    buffer_.assign(data, data + size);
    buffer_.resize(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    packet_->data = buffer_.data();
    packet_->size = static_cast<int>(size);
    int err = avcodec_send_packet(av_, packet_);
    packet_->data = nullptr;
    packet_->size = 0;
    if (err < 0) return err;
    for (;;) {
        err = avcodec_receive_frame(av_, frame_);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return 0;
        if (err < 0) return err;
        const u32 channels = static_cast<u32>(frame_->ch_layout.nb_channels);
        if (frame_->format != AV_SAMPLE_FMT_FLTP)
            PS_FATAL(kChan, "{}: FFmpeg gave sample format {}, not planar floats", name_, frame_->format);
        if (out.ch.empty()) {
            out.ch.resize(channels);
            out.rate = static_cast<u32>(frame_->sample_rate);
            for (u32 c = 0; c < channels; c++)
                out.ids.push_back(av_channel_layout_channel_from_index(&frame_->ch_layout, c));
        } else if (out.channels() != channels || out.rate != static_cast<u32>(frame_->sample_rate)) {
            log_once(std::format("{} mixed frames", name_), std::format("{}: a packet gave frames of different channels "
                "or rates: only the first's are kept", name_));
            av_frame_unref(frame_);
            continue;
        }
        const u32 n = std::min<u32>(static_cast<u32>(frame_->nb_samples), max_samples - std::min(max_samples, out.samples));
        if (n < static_cast<u32>(frame_->nb_samples))
            log_once(std::format("{} too long", name_), std::format("{}: an access unit decoded to more than its output "
                "holds ({} samples): the rest is left out", name_, max_samples));
        for (u32 c = 0; c < channels; c++) {
            const float* in = reinterpret_cast<const float*>(frame_->extended_data[c]);
            out.ch[c].insert(out.ch[c].end(), in, in + n);
        }
        out.samples += n;
        av_frame_unref(frame_);
    }
}

// ---- MPEG audio headers

MpegHeader parse_mpeg_header(u32 word)
{
    // kbit/s by version (MPEG-1, the lower rates) and layer (ISO/IEC 11172-3 2.4.2.3, 13818-3 2.4.2.3).
    static constexpr u16 kBitrates[2][3][15] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
            {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
            {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
            {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
            {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}},
    };
    static constexpr u32 kRates[3] = {44100, 48000, 32000};
    MpegHeader h;
    h.word = word;
    h.sync = (word >> 21) == 0x7FF;
    h.version = (word >> 19) & 3;
    const u32 layer_bits = (word >> 17) & 3;
    h.layer = layer_bits ? 4 - layer_bits : 0;
    h.crc = ((word >> 16) & 1) == 0;
    h.bitrate_index = (word >> 12) & 15;
    h.rate_index = (word >> 10) & 3;
    h.padding = (word >> 9) & 1;
    h.private_bit = (word >> 8) & 1;
    h.mode = (word >> 6) & 3;
    h.mode_extension = (word >> 4) & 3;
    h.copyright = (word >> 3) & 1;
    h.original = (word >> 2) & 1;
    h.emphasis = word & 3;
    if (h.version == 1 || !h.layer) return h;
    const bool lsf = h.version != 3;
    if (h.rate_index < 3) h.rate = kRates[h.rate_index] >> (h.version == 3 ? 0 : h.version == 2 ? 1 : 2);
    if (h.bitrate_index < 15) h.bitrate = kBitrates[lsf][h.layer - 1][h.bitrate_index];
    h.samples = h.layer == 1 ? 384 : h.layer == 2 || !lsf ? 1152 : 576;
    if (h.rate && h.bitrate) {
        if (h.layer == 1) h.frame_bytes = (12000 * h.bitrate / h.rate + h.padding) * 4;
        else h.frame_bytes = (h.samples / 8) * 1000 * h.bitrate / h.rate + h.padding;
    }
    return h;
}

u16 mpeg_crc(const u8* data, u32 bits, u16 crc)
{
    for (u32 i = 0; i < bits; i++) {
        const bool in = (data[i / 8] >> (7 - i % 8)) & 1;
        const bool top = crc & 0x8000;
        crc = static_cast<u16>(crc << 1);
        if (in != top) crc ^= 0x8005;
    }
    return crc;
}

// ---- the PCM out

std::vector<int> straight(u32 n)
{
    std::vector<int> m(n);
    for (u32 i = 0; i < n; i++) m[i] = static_cast<int>(i);
    return m;
}

u32 write_pcm(u8* base, u32 out, const Planar& in, std::span<const int> map, Format fmt, float gain)
{
    const u32 nch = static_cast<u32>(map.size()), bytes = bytes_of(fmt);
    for (u32 c = 0; c < nch; c++) {
        const int from = map[c];
        const float* src = from >= 0 && static_cast<u32>(from) < in.channels() ? in.ch[static_cast<usize>(from)].data() : nullptr;
        for (u32 i = 0; i < in.samples; i++) {
            const f32 s = src ? src[i] * gain : 0.0f;
            const u32 at = out + bytes * (i * nch + c);
            switch (fmt) {
            case Format::F32:
                if (s >= std::bit_cast<f32>(std::bit_cast<u32>(1.f) - 1)) ps_write32(base, at, 0x3F7FFFFF);
                else ps_write32(base, at, std::bit_cast<u32>(s <= -1.f ? -1.f : s));
                break;
            case Format::S16:
                ps_write16(base, at, static_cast<u16>(s >= 1.f ? 0x7FFF : s <= -1.f ? -0x8000 :
                    static_cast<s16>(std::floor(s * 32768.f))));
                break;
            case Format::S24:
                ps_write32(base, at, s >= 1.f ? 0x007FFFFFu : s <= -1.f ? 0x00800000u :
                    static_cast<u32>(static_cast<s32>(std::floor(s * 8388608.f))) & 0x00FFFFFFu);
                break;
            case Format::S32:
                ps_write32(base, at, s >= 1.f ? 0x7FFFFFFFu : s <= -1.f ? 0x80000000u :
                    static_cast<u32>(static_cast<s32>(std::floor(static_cast<double>(s) * 2147483648.0))));
                break;
            }
        }
    }
    return in.samples * nch * bytes;
}

} // namespace ffcore

std::unique_ptr<AdecCore> make_ffmpeg_core(s32 type)
{
    const ffcore::Spec* spec = ffcore::spec_of(type);
    return spec ? std::make_unique<ffcore::FfCore>(*spec) : nullptr;
}

void _pspocket_adec_ffmpeg_dec_thread(PPUContext& ctx, u64 core_handle)
{
    ffcore::run(ctx, static_cast<u32>(core_handle));
    lv2::_sys_ppu_thread_exit(thread_of(ctx), 0);
}

#else

std::unique_ptr<AdecCore> make_ffmpeg_core(s32) { return nullptr; }

void _pspocket_adec_ffmpeg_dec_thread(PPUContext& ctx, u64)
{
    PS_LOG_ERROR("cellAdec", "pspocket was built without FFmpeg: no core of its own for this codec");
    lv2::_sys_ppu_thread_exit(thread_of(ctx), 0);
}

#endif

} // namespace pspocket::hle::adec
