// cellAdec: the audio decoder games use for their movies' and streams' audio (D-197). The front end keeps Sony's
// context in the memory the game gives (AdecContext, its frames and bitstream information blocks, then the core's
// memory) and decodes through a core: pspocket's own for the three LPCM types (adec_lpcm.cpp) and ATRAC3plus
// (adec_atracx.cpp, on FFmpeg, D-198); for every other codec, the firmware's core module (MP3, AC-3, AAC, ...) through
// the operation table it exports, as Sony's libadec and RPCS3's cellAdec call it. The core modules are the host's
// imports of cellAdec (Registry::add_imports): the loader chooses and links them for a game importing cellAdec, as it
// would for libadec. Where no firmware module gives one, pspocket's own core on FFmpeg serves the codecs it has one of
// (D-577: MP3, MPEG audio layers I and II, MPEG-4 AAC, AC-3, ATRAC3; adec_ffmpeg.hpp): firmware first, as D-563 does
// for whole libraries, until each is compared with Sony's core on the Mac.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellAdec.cpp/.h (get_core_ops, AdecContext, the notifications, the API).

//
// Differences from RPCS3:
// - cellAdecQueryAttr's adecVerUpper is 0x492000, Sony's (4.92); RPCS3 has 0x491000.
// - A codec whose core no firmware module gives (DTS, DTS-HD core, WMA Pro, WMA Lossless; or no firmware installed) is
//   logged and refused with CELL_ADEC_ERROR_FATAL: Sony's libadec calls through the empty table and crashes, RPCS3
//   aborts. Unless pspocket has a core of its own for it (D-577), which then serves.
// - Pointers RPCS3 dereferences without a check (LLE would fault) are checked; one that is not guest memory is logged
//   and refused with CELL_ADEC_ERROR_ARG (D-042). A PCM handle out of range is FATAL (LLE reads past the frames).

#include <array>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "adec.hpp"
#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

// What Sony's libadec imports from the core modules (and so pspocket's cellAdec, D-197): each core's operation table
// and each core's _SceAdecCorrectPtsValue_* (RPCS3 cellAdec.cpp's module declarations; checked against libadec's own
// imports in the user's 4.92 firmware). DTS, DTS-HD core, WMA Pro and WMA Lossless are in no retail firmware.
constexpr HostImport kImports[] = {
    {{}, "cell_libac3dec", 0xC58BB170, true},     // AC-3 operations
#if !PSPOCKET_HAVE_FFMPEG // pspocket's own otherwise (D-198)
    {{}, "cellAtracXdec", 0x4944AF9A, true},      // ATRAC3plus
    {{}, "cellAtracXdec", 0x076B33AB, true},      // ATRAC3plus, 2 channels
    {{}, "cellAtracXdec", 0x1D210EAA, true},      // ATRAC3plus, 6 channels
    {{}, "cellAtracXdec", 0xE9A86E54, true},      // ATRAC3plus, 8 channels
#endif
    {{}, "cellMP3dec", 0x84276A23, true},         // MP3
    {{}, "cellAtrac3dec", 0x60487D65, true},      // ATRAC3
    {{}, "cellM2BCdec", 0x300AFE4D, true},        // MPEG-1/2 layer II
    {{}, "cellM2AACdec", 0xAC2F0831, true},       // MPEG-2 AAC
    {{}, "cellDDPdec", 0xF5E9C15C, true},         // E-AC-3
    {{}, "cellTRHDdec", 0xE88E381B, true},        // TrueHD
    {{}, "cellDTSdec", 0x15248EC5, true},         // DTS
    {{}, "cellCelpDec", 0xFFE42C22, true},        // CELP
    {{}, "cellM4AacDec", 0xAB61278D, true},       // MPEG-4 AAC
    {{}, "cellWMAdec", 0x88320B10, true},         // WMA
    {{}, "cellDTSLBRdec", 0x8C50AF52, true},      // DTS Express
    {{}, "cellM4AacDec2ch", 0xE996C664, true},    // MPEG-4 AAC, 2 channels
    {{}, "cellDTSHDdec", 0x51AC2B6C, true},       // DTS-HD
    {{}, "cellMPL1dec", 0xBBC70551, true},        // MPEG-1 layer I
    {{}, "cellMP3Sdec", 0x292CDF0A, true},        // MP3 surround
    {{}, "cellM4AacDec2chmod", 0xDBD26836, true}, // MPEG-4 AAC, 2 channels (mod)
    {{}, "cellCelp8Dec", 0xF0190C6C, true},       // CELP 8 kHz
    {{}, "cellWMAPROdec", 0xA8BAC670, true},      // WMA Pro
    {{}, "cellWMALSLdec", 0x602AAB16, true},      // WMA Lossless
    {{}, "cellDTSHDCOREdec", 0x6C8F4F1C, true},   // DTS-HD core
    {{}, "cellAtrac3multidec", 0xC20C6BD7, true}, // ATRAC3 multichannel
    {{}, "cellDDPdec", 0xEE2731D3, false},         // _SceAdecCorrectPtsValue_Ddp
    {{}, "cellDTSHDdec", 0xF3AF1093, false},       // _SceAdecCorrectPtsValue_DtsHd
    {{}, "cellCelpDec", 0xE4B8FC4D, false},        // _SceAdecCorrectPtsValue_Celp
    {{}, "cellM2AACdec", 0x7CBA9865, false},       // _SceAdecCorrectPtsValue_Aac
    {{}, "cellM2BCdec", 0xEC7DE355, false},        // _SceAdecCorrectPtsValue_mpmc
    {{}, "cellTRHDdec", 0x75080F7B, false},        // _SceAdecCorrectPtsValue_truehd
    {{}, "cellDTSdec", 0x8C7AD5ED, false},         // _SceAdecCorrectPtsValue_DtsCore
    {{}, "cellM4AacDec", 0x431A1DCA, false},       // _SceAdecCorrectPtsValue_M4Aac
    {{}, "cellM4AacDec2ch", 0x869E328D, false},    // _SceAdecCorrectPtsValue_M4Aac2ch
    {{}, "cellM4AacDec2chmod", 0x156FE2F0, false}, // _SceAdecCorrectPtsValue_M4Aac2chmod
    {{}, "cellWMAdec", 0x46E6C806, false},         // _SceAdecCorrectPtsValue_wma
    {{}, "cellDTSLBRdec", 0x185591D6, false},      // _SceAdecCorrectPtsValue_DtsLbr
    {{}, "cellMPL1dec", 0xA4A9A309, false},        // _SceAdecCorrectPtsValue_mpmcl1
    {{}, "cellMP3Sdec", 0x73A2C895, false},        // _SceAdecCorrectPtsValue_Mp3s
    {{}, "cellCelp8Dec", 0x881583C3, false},       // _SceAdecCorrectPtsValue_Celp8
    {{}, "cellWMAPROdec", 0x8D45F21A, false},      // _SceAdecCorrectPtsValue_WmaPro
    {{}, "cellWMALSLdec", 0xF2F9C183, false},      // _SceAdecCorrectPtsValue_WmaLsl
    {{}, "cellDTSHDCOREdec", 0xD4C180D4, false},   // _SceAdecCorrectPtsValue_DtsHd_Core
};

constexpr std::string_view kChan = "cellAdec";

using namespace adec;

constexpr s32 CELL_ADEC_MSG_TYPE_AUDONE = 0, CELL_ADEC_MSG_TYPE_PCMOUT = 1, CELL_ADEC_MSG_TYPE_ERROR = 2,
              CELL_ADEC_MSG_TYPE_SEQDONE = 3;
constexpr u32 SEQ_DORMANT = 0x100, SEQ_READY = 0x200, SEQ_CLOSED = 0xA00; // AdecSequenceState
constexpr u32 kVersionUpper = 0x492000;
constexpr u32 kTsInvalid = 0xFFFFFFFF; // CODEC_TS_INVALID
constexpr u32 kFrameSize = frame_off::size;

// Each codec type's core module: its library and operation table (RPCS3 get_core_ops). Types 0, 26 and 27 are
// invalid; 28 and 29 were never released (WMA Pro or WMA Lossless); the LPCM types have pspocket's core.
struct CoreOps {
    std::string_view library;
    u32 nid = 0;
};
constexpr std::array<CoreOps, 32> kCoreOps = {{
    {},                                    // 0 invalid
    {},                                    // 1 LPCM (PAMF)
    {"cell_libac3dec", 0xC58BB170},        // 2 AC-3
    {"cellAtracXdec", 0x4944AF9A},         // 3 ATRAC3plus
    {"cellMP3dec", 0x84276A23},            // 4 MP3
    {"cellAtrac3dec", 0x60487D65},         // 5 ATRAC3
    {"cellM2BCdec", 0x300AFE4D},           // 6 MPEG layer II
    {"cellM2AACdec", 0xAC2F0831},          // 7 MPEG-2 AAC
    {"cellDDPdec", 0xF5E9C15C},            // 8 E-AC-3
    {"cellTRHDdec", 0xE88E381B},           // 9 TrueHD
    {"cellDTSdec", 0x15248EC5},            // 10 DTS
    {"cellCelpDec", 0xFFE42C22},           // 11 CELP
    {},                                    // 12 LPCM (Blu-ray)
    {"cellAtracXdec", 0x076B33AB},         // 13 ATRAC3plus, 2 channels
    {"cellAtracXdec", 0x1D210EAA},         // 14 ATRAC3plus, 6 channels
    {"cellAtracXdec", 0xE9A86E54},         // 15 ATRAC3plus, 8 channels
    {"cellM4AacDec", 0xAB61278D},          // 16 MPEG-4 AAC
    {},                                    // 17 LPCM (DVD)
    {"cellWMAdec", 0x88320B10},            // 18 WMA
    {"cellDTSLBRdec", 0x8C50AF52},         // 19 DTS Express
    {"cellM4AacDec2ch", 0xE996C664},       // 20 MPEG-4 AAC, 2 channels
    {"cellDTSHDdec", 0x51AC2B6C},          // 21 DTS-HD
    {"cellMPL1dec", 0xBBC70551},           // 22 MPEG layer I
    {"cellMP3Sdec", 0x292CDF0A},           // 23 MP3 surround
    {"cellM4AacDec2chmod", 0xDBD26836},    // 24 MPEG-4 AAC, 2 channels (mod)
    {"cellCelp8Dec", 0xF0190C6C},          // 25 CELP 8 kHz
    {},                                    // 26 invalid
    {},                                    // 27 invalid
    {},                                    // 28 reserved
    {},                                    // 29 reserved
    {"cellDTSHDCOREdec", 0x6C8F4F1C},      // 30 DTS-HD core
    {"cellAtrac3multidec", 0xC20C6BD7},    // 31 ATRAC3 multichannel
}};

constexpr bool invalid_type(s32 type) { return type < 0 || type >= 32 || type == 0 || type == 26 || type == 27; }

// A core module's operations through its table (CellAdecCoreOps: getMemSize, open, close, startSeq, endSeq, decodeAu,
// getVersion, realign, releasePcm, getPcmHandleNum, getBsiInfoSize, openExt), each a function descriptor.
class GuestCore final : public AdecCore {
public:
    explicit GuestCore(u32 ops) : ops_(ops) {}

    u32 mem_size(PPUContext& ctx) override
    {
        u8* base = guest_base(ctx);
        StackVar attr(ctx, base, 0x10);
        call(ctx, 0, {attr.addr()});
        return ps_read32(base, attr.addr());
    }
    s32 pcm_handle_num(PPUContext& ctx) override { return call(ctx, 9, {}); }
    u32 bsi_info_size(PPUContext& ctx) override { return static_cast<u32>(call(ctx, 10, {})); }
    u32 version(PPUContext& ctx) override
    {
        u8* base = guest_base(ctx);
        StackVar v(ctx, base, 0x10);
        call(ctx, 6, {v.addr()});
        return ps_read32(base, v.addr());
    }
    s32 open(PPUContext& ctx, u32 core_handle, u32 context, u32 res, u32 spurs_res) override
    {
        HleProcess& p = *HleProcess::current();
        const u32 au_done = p.descriptor_of("cellAdec", NID_notify_au_done),
                  pcm_out = p.descriptor_of("cellAdec", NID_notify_pcm_out),
                  err = p.descriptor_of("cellAdec", NID_notify_error),
                  seq_done = p.descriptor_of("cellAdec", NID_notify_seq_done);
        if (!au_done || !pcm_out || !err || !seq_done) {
            PS_LOG_ERROR(kChan, "the notifications for the core are not linked (cellAdec was not imported)");
            return error(CELL_ADEC_ERROR_FATAL);
        }
        if (spurs_res)
            return call(ctx, 11, {core_handle, au_done, context, pcm_out, context, err, context, seq_done, context, res,
                spurs_res});
        return call(ctx, 1, {core_handle, au_done, context, pcm_out, context, err, context, seq_done, context, res});
    }
    s32 close(PPUContext& ctx, u32 core_handle) override { return call(ctx, 2, {core_handle}); }
    s32 start_seq(PPUContext& ctx, u32 core_handle, u32 param) override { return call(ctx, 3, {core_handle, param}); }
    s32 end_seq(PPUContext& ctx, u32 core_handle) override { return call(ctx, 4, {core_handle}); }
    s32 decode_au(PPUContext& ctx, u32 core_handle, s32 pcm_handle, u32 au_info) override
    {
        return call(ctx, 5, {core_handle, static_cast<u64>(static_cast<s64>(pcm_handle)), au_info});
    }
    s32 realign(PPUContext& ctx, u32 core_handle, u32 out_buffer, u32 pcm_start) override
    {
        return call(ctx, 7, {core_handle, out_buffer, pcm_start});
    }
    s32 release_pcm(PPUContext& ctx, u32 core_handle, s32 pcm_handle, u32 out_buffer) override
    {
        return call(ctx, 8, {core_handle, static_cast<u64>(static_cast<s64>(pcm_handle)), out_buffer});
    }

private:
    s32 call(PPUContext& ctx, u32 op, std::initializer_list<u64> args)
    {
        const u32 fn = ps_read32(guest_base(ctx), ops_ + 4 * op);
        return static_cast<s32>(static_cast<u32>(call_guest(ctx, fn, args)));
    }
    u32 ops_;
};

// What a decoder whose core is gone answers (a handle whose type has no core: logged, FATAL).
class NoCore final : public AdecCore {
public:
    explicit NoCore(s32 type) : type_(type) {}
    u32 mem_size(PPUContext&) override { return fail(), 0; }
    s32 pcm_handle_num(PPUContext&) override { return fail(); }
    u32 bsi_info_size(PPUContext&) override { return fail(), 0; }
    u32 version(PPUContext&) override { return fail(), 0; }
    s32 open(PPUContext&, u32, u32, u32, u32) override { return fail(); }
    s32 close(PPUContext&, u32) override { return fail(); }
    s32 start_seq(PPUContext&, u32, u32) override { return fail(); }
    s32 end_seq(PPUContext&, u32) override { return fail(); }
    s32 decode_au(PPUContext&, u32, s32, u32) override { return fail(); }
    s32 realign(PPUContext&, u32, u32, u32) override { return fail(); }
    s32 release_pcm(PPUContext&, u32, s32, u32) override { return fail(); }

private:
    s32 fail() const
    {
        PS_LOG_ERROR(kChan, "a decoder of codec type {} with no core (not opened on one)", type_);
        return error(CELL_ADEC_ERROR_FATAL);
    }
    s32 type_;
};

// pspocket's own core that serves a type before any firmware module's: LPCM (D-197) and ATRAC3plus on FFmpeg (D-198).
std::unique_ptr<AdecCore> own_first_core(s32 type)
{
    if (is_lpcm(type)) return make_lpcm_core();
    switch (type) {
    case CELL_ADEC_TYPE_ATRACX:
    case CELL_ADEC_TYPE_ATRACX_8CH: return make_atracx_core(8);
    case CELL_ADEC_TYPE_ATRACX_2CH: return make_atracx_core(2);
    case CELL_ADEC_TYPE_ATRACX_6CH: return make_atracx_core(6);
    default: return nullptr;
    }
}

// The core for a codec type (D-577): pspocket's own first for LPCM and ATRAC3plus; else the core module's table the
// loader linked (Sony's serves first where the installed system software has it); else pspocket's own core on FFmpeg
// for the codecs it has one of; else none (logged). *ops_out: the table chosen, 0 for pspocket's own (open keeps it in
// the context, so the decoder stays on what it was opened on).
std::unique_ptr<AdecCore> core_for(s32 type, u32* ops_out = nullptr)
{
    if (ops_out) *ops_out = 0;
    if (std::unique_ptr<AdecCore> own = own_first_core(type)) return own;
    const CoreOps& c = kCoreOps[static_cast<usize>(type)];
    const u32 ops = c.library.empty() ? 0 : HleProcess::current()->imported(c.library, c.nid);
    if (ops) {
        if (ops_out) *ops_out = ops;
        return std::make_unique<GuestCore>(ops);
    }
    if (std::unique_ptr<AdecCore> own = make_ffmpeg_core(type)) {
        if (ops_out)
            PS_LOG_INFO(kChan, "codec type {}: no firmware module gave {} ({:#010x}): pspocket's own core (D-577)", type,
                c.library, c.nid);
        return own;
    }
    PS_LOG_ERROR(kChan, "codec type {}: no core ({}): pspocket has none of its own and no firmware module gave {}", type,
        c.library.empty() ? "a reserved type" : "not in the firmware, or none installed",
        c.library.empty() ? "one" : std::format("{} ({:#010x})", c.library, c.nid));
    return nullptr;
}

// The core of an open decoder: what open chose (D-577): pspocket's own first-served core, else the table it was opened
// on, else pspocket's own core of its codec.
std::unique_ptr<AdecCore> core_of(u8* base, u32 h)
{
    const s32 type = static_cast<s32>(ps_read32(base, h + ctx_off::type));
    if (std::unique_ptr<AdecCore> own = own_first_core(type)) return own;
    if (const u32 ops = ps_read32(base, h + ctx_off::core_ops)) return std::make_unique<GuestCore>(ops);
    if (std::unique_ptr<AdecCore> own = make_ffmpeg_core(type)) return own;
    return std::make_unique<NoCore>(type);
}

// Whether an open decoder runs on pspocket's own core of a codec the firmware serves first (D-577).
bool on_own_fallback_core(u8* base, u32 h)
{
    return !ps_read32(base, h + ctx_off::core_ops) && !own_first_core(static_cast<s32>(ps_read32(base, h + ctx_off::type)));
}

u32 core_handle(u8* base, u32 h) { return ps_read32(base, h + ctx_off::core_handle); }

s32 finish(s32 r, const std::string& call)
{
    if (r < 0) PS_LOG_WARN(kChan, "{}: {:#010x}", call, static_cast<u32>(r));
    else PS_LOG_DEBUG(kChan, "{}: CELL_OK", call);
    return r;
}

bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (addr && guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("cellAdec: {} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// A handle the game passes: guest memory for the context.
bool context_usable(PPUContext& ctx, u32 h, std::string_view call) { return h && usable(ctx, h, ctx_off::size, call); }

void put_name(u8* base, u32 at, std::string_view name)
{
    for (u32 i = 0; i < 8; i++) ps_write8(base, at + i, i < name.size() ? static_cast<u8>(name[i]) : 0);
}

void write_mutex_attr(u8* base, u32 at, std::string_view name)
{
    ps_write32(base, at + 0x00, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, at + 0x04, lv2::SYS_SYNC_NOT_RECURSIVE);
    ps_write32(base, at + 0x08, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    ps_write32(base, at + 0x0C, lv2::SYS_SYNC_NOT_ADAPTIVE);
    for (u32 i = 0x10; i < 0x20; i++) ps_write8(base, at + i, 0);
    put_name(base, at + 0x20, name);
}

// ---- AdecOutputQueue (0x54): four entries (this_index, state 0xFF empty / 0x10 filled, pcm_item, pcm_handle), front,
// back, size, a mutex and an unused cond (RPCS3).
namespace q {
constexpr u32 kEntry = 0x10, e_state = 4, e_item = 8, e_handle = 12, front = 0x40, back = 0x44, size = 0x48,
              mutex = 0x4C, cond = 0x50;
}

void queue_init(PPUContext& ctx, u32 queue)
{
    u8* base = guest_base(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    ps_write32(base, queue + q::front, 0);
    ps_write32(base, queue + q::back, 0);
    ps_write32(base, queue + q::size, 0);
    StackVar attr(ctx, base, 0x28);
    write_mutex_attr(base, attr.addr(), "_adem07");
    lv2::sys_mutex_create(t, queue + q::mutex, attr.addr()); // unchecked on LLE
    StackVar cattr(ctx, base, 0x18);
    ps_write32(base, cattr.addr(), lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    put_name(base, cattr.addr() + 0x10, "_adec05");
    lv2::sys_cond_create(t, queue + q::cond, ps_read32(base, queue + q::mutex), cattr.addr());
    for (u32 i = 0; i < 4; i++) {
        const u32 e = queue + q::kEntry * i;
        ps_write32(base, e, i);
        ps_write32(base, e + q::e_state, 0xFF);
        ps_write32(base, e + q::e_item, 0);
        ps_write32(base, e + q::e_handle, 0xFFFFFFFF);
    }
}

s32 queue_finalize(PPUContext& ctx, u32 queue)
{
    u8* base = guest_base(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    if (const s32 r = lv2::sys_cond_destroy(t, ps_read32(base, queue + q::cond)); r != lv2::CELL_OK) return r;
    return lv2::sys_mutex_destroy(t, ps_read32(base, queue + q::mutex));
}

s32 queue_lock(PPUContext& ctx, u32 queue)
{
    return lv2::sys_mutex_lock(thread_of(ctx), ps_read32(guest_base(ctx), queue + q::mutex), 0);
}
s32 queue_unlock(PPUContext& ctx, u32 queue)
{
    return lv2::sys_mutex_unlock(thread_of(ctx), ps_read32(guest_base(ctx), queue + q::mutex));
}

// CELL_OK, or 1 when the back entry is still filled (LLE returns that comparison).
s32 queue_push(PPUContext& ctx, u32 queue, u32 item, s32 pcm_handle)
{
    u8* base = guest_base(ctx);
    queue_lock(ctx, queue);
    const u32 back = ps_read32(base, queue + q::back), e = queue + q::kEntry * back;
    if (ps_read32(base, e + q::e_state) != 0xFF) {
        queue_unlock(ctx, queue);
        return 1;
    }
    ps_write32(base, e + q::e_state, 0x10);
    ps_write32(base, e + q::e_item, item);
    ps_write32(base, e + q::e_handle, static_cast<u32>(pcm_handle));
    ps_write32(base, queue + q::back, (back + 1) & 3);
    ps_write32(base, queue + q::size, ps_read32(base, queue + q::size) + 1);
    queue_unlock(ctx, queue);
    return lv2::CELL_OK;
}

// The front entry's PCM item, taken; none when the queue is empty.
std::optional<u32> queue_pop(PPUContext& ctx, u32 queue)
{
    u8* base = guest_base(ctx);
    queue_lock(ctx, queue);
    const u32 front = ps_read32(base, queue + q::front), e = queue + q::kEntry * front;
    if (ps_read32(base, e + q::e_state) == 0xFF) {
        queue_unlock(ctx, queue);
        return std::nullopt;
    }
    const u32 item = ps_read32(base, e + q::e_item);
    ps_write32(base, e + q::e_state, 0xFF);
    ps_write32(base, e + q::e_handle, 0xFFFFFFFF);
    ps_write32(base, queue + q::front, (front + 1) & 3);
    ps_write32(base, queue + q::size, ps_read32(base, queue + q::size) - 1);
    queue_unlock(ctx, queue);
    return item;
}

s32 queue_peek_handle(PPUContext& ctx, u32 queue)
{
    u8* base = guest_base(ctx);
    queue_lock(ctx, queue);
    const s32 h = static_cast<s32>(ps_read32(base, queue + q::kEntry * ps_read32(base, queue + q::front) + q::e_handle));
    queue_unlock(ctx, queue);
    return h;
}

// ---- AdecContext's frames (RPCS3 AdecContext::*)

u32 frame_at(u8* base, u32 h, s32 pcm_handle)
{
    return ps_read32(base, h + ctx_off::frames) + kFrameSize * static_cast<u32>(pcm_handle);
}

s32 frames_num(u8* base, u32 h) { return static_cast<s32>(ps_read32(base, h + ctx_off::frames_num)); }

s32 get_new_pcm_handle(u8* base, u32 h, u32 au_info)
{
    for (s32 i = 0; i < frames_num(base, h); i++) {
        const u32 f = frame_at(base, h, i);
        if (ps_read8(base, f + frame_off::in_use)) continue;
        ps_write8(base, f + frame_off::in_use, 1);
        std::memmove(base + f + frame_off::au_info, base + au_info, 0x18);
        return i;
    }
    return error(CELL_ADEC_ERROR_BUSY);
}

s32 verify_pcm_handle(u8* base, u32 h, s32 pcm_handle)
{
    if (pcm_handle < 0 || pcm_handle >= frames_num(base, h)) {
        PS_LOG_ERROR(kChan, "decoder {:#x}: PCM handle {} of {}", h, pcm_handle, frames_num(base, h));
        return error(CELL_ADEC_ERROR_FATAL);
    }
    if (!ps_read8(base, frame_at(base, h, pcm_handle) + frame_off::in_use)) return error(CELL_ADEC_ERROR_FATAL);
    return lv2::CELL_OK;
}

u32 get_au_info(u8* base, u32 h, s32 pcm_handle)
{
    if (verify_pcm_handle(base, h, pcm_handle) != lv2::CELL_OK) return 0;
    return frame_at(base, h, pcm_handle) + frame_off::au_info;
}

s32 set_pcm_item(u8* base, u32 h, s32 pcm_handle, u32 pcm_addr, u32 pcm_size, u32 bsi_info)
{
    if (verify_pcm_handle(base, h, pcm_handle) != lv2::CELL_OK) return error(CELL_ADEC_ERROR_FATAL);
    const u32 f = frame_at(base, h, pcm_handle), item = f + frame_off::pcm_item;
    ps_write32(base, item + item_off::pcm_handle, static_cast<u32>(pcm_handle));
    ps_write32(base, item + item_off::status, 0);
    ps_write32(base, item + item_off::start_addr, pcm_addr);
    ps_write32(base, item + item_off::size, pcm_size);
    std::memmove(base + item + item_off::au_info, base + f + frame_off::au_info, 0x18);
    std::memmove(base + ps_read32(base, item + item_off::bsi_info), base + bsi_info, ps_read32(base, h + ctx_off::bsi_size));
    return lv2::CELL_OK;
}

s32 link_frame(PPUContext& ctx, u32 h, s32 pcm_handle)
{
    u8* base = guest_base(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    const u32 mutex = ps_read32(base, h + ctx_off::mutex);
    lv2::sys_mutex_lock(t, mutex, 0);
    s32 r = verify_pcm_handle(base, h, pcm_handle);
    const s32 head = static_cast<s32>(ps_read32(base, h + ctx_off::frames_head));
    const s32 tail = static_cast<s32>(ps_read32(base, h + ctx_off::frames_tail));
    if (r == lv2::CELL_OK) {
        const u32 f = frame_at(base, h, pcm_handle);
        if (tail == -1 && head == -1) {
            ps_write32(base, f + frame_off::next, static_cast<u32>(pcm_handle));
            ps_write32(base, f + frame_off::prev, static_cast<u32>(pcm_handle));
            ps_write32(base, h + ctx_off::frames_head, static_cast<u32>(pcm_handle));
            ps_write32(base, h + ctx_off::frames_tail, static_cast<u32>(pcm_handle));
        } else if (tail != -1 && head != -1) {
            ps_write32(base, f + frame_off::next, static_cast<u32>(pcm_handle));
            ps_write32(base, f + frame_off::prev, static_cast<u32>(tail));
            ps_write32(base, frame_at(base, h, tail) + frame_off::next, static_cast<u32>(pcm_handle));
            ps_write32(base, h + ctx_off::frames_tail, static_cast<u32>(pcm_handle));
        } else {
            r = error(CELL_ADEC_ERROR_FATAL);
        }
    }
    lv2::sys_mutex_unlock(t, mutex);
    return r;
}

s32 unlink_frame(PPUContext& ctx, u32 h, s32 pcm_handle)
{
    u8* base = guest_base(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    const u32 mutex = ps_read32(base, h + ctx_off::mutex);
    lv2::sys_mutex_lock(t, mutex, 0);
    const auto done = [&](s32 r) {
        lv2::sys_mutex_unlock(t, mutex);
        return r;
    };
    if (verify_pcm_handle(base, h, pcm_handle) != lv2::CELL_OK) return done(error(CELL_ADEC_ERROR_FATAL));
    const s32 head = static_cast<s32>(ps_read32(base, h + ctx_off::frames_head));
    const s32 tail = static_cast<s32>(ps_read32(base, h + ctx_off::frames_tail));
    if (head == -1 || tail == -1) return done(error(CELL_ADEC_ERROR_FATAL));
    const u32 f = frame_at(base, h, pcm_handle);
    const s32 next = static_cast<s32>(ps_read32(base, f + frame_off::next));
    const s32 prev = static_cast<s32>(ps_read32(base, f + frame_off::prev));
    // The neighbours' links, as LLE writes them (indices it never checks: kept in range here).
    const auto set_link = [&](s32 index, u32 field, s32 value) {
        if (index >= 0 && index < frames_num(base, h)) ps_write32(base, frame_at(base, h, index) + field, static_cast<u32>(value));
    };
    if (head == tail) {
        if (pcm_handle != tail) return done(error(CELL_ADEC_ERROR_FATAL));
        ps_write32(base, h + ctx_off::frames_head, 0xFFFFFFFF);
        ps_write32(base, h + ctx_off::frames_tail, 0xFFFFFFFF);
        ps_write32(base, f + frame_off::next, 0xFFFFFFFF);
        ps_write32(base, f + frame_off::prev, 0xFFFFFFFF);
    } else if (pcm_handle == head) {
        ps_write32(base, h + ctx_off::frames_head, static_cast<u32>(next));
        set_link(prev, frame_off::next, next);
    } else if (pcm_handle == tail) {
        ps_write32(base, h + ctx_off::frames_tail, static_cast<u32>(prev));
        set_link(next, frame_off::prev, prev);
    } else {
        set_link(next, frame_off::prev, prev);
        set_link(prev, frame_off::next, next);
    }
    return done(lv2::CELL_OK);
}

void reset_frame(u8* base, u32 h, s32 pcm_handle)
{
    const u32 f = frame_at(base, h, pcm_handle);
    ps_write8(base, f + frame_off::in_use, 0);
    ps_write8(base, f + frame_off::au_done, 0);
    ps_write8(base, f + frame_off::pcm_out, 0);
    ps_write32(base, f + frame_off::next, 0xFFFFFFFF);
    ps_write32(base, f + frame_off::prev, 0xFFFFFFFF);
}

// The core module function that corrects a frame's PTS, for these types (RPCS3 AdecContext::correct_pts_value): its
// library and NID.
struct PtsFunction {
    s8 type;
    std::string_view library;
    u32 nid;
};
constexpr PtsFunction kPtsFunctions[] = {
    {17, "cellDDPdec", 0xEE2731D3}, {21, "cellDTSHDdec", 0xF3AF1093}, {24, "cellCelpDec", 0xE4B8FC4D},
    {25, "cellM2AACdec", 0x7CBA9865}, {26, "cellM2BCdec", 0xEC7DE355}, {27, "cellTRHDdec", 0x75080F7B},
    {28, "cellDTSdec", 0x8C7AD5ED}, {30, "cellWMAdec", 0x46E6C806}, {31, "cellDTSLBRdec", 0x185591D6},
    {32, "cellMPL1dec", 0xA4A9A309}, {33, "cellMP3Sdec", 0x73A2C895}, {34, "cellCelp8Dec", 0x881583C3},
    {35, "cellWMAPROdec", 0x8D45F21A}, {36, "cellWMALSLdec", 0xF2F9C183}, {37, "cellDTSHDCOREdec", 0xD4C180D4},
    {38, "cellDTSHDCOREdec", 0xD4C180D4},
};

s32 call_pts_function(PPUContext& ctx, u32 h, std::string_view library, u32 nid, u32 au_pts)
{
    const u32 fn = HleProcess::current()->imported(library, nid);
    if (!fn) {
        PS_LOG_ERROR(kChan, "decoder {:#x}: {}'s PTS correction ({:#010x}) is not linked", h, library, nid);
        return error(CELL_ADEC_ERROR_FATAL);
    }
    const s32 r = static_cast<s32>(static_cast<u32>(call_guest(ctx, fn, {core_handle(guest_base(ctx), h), au_pts})));
    return r != lv2::CELL_OK ? error(CELL_ADEC_ERROR_FATAL) : lv2::CELL_OK;
}

// pspocket's own core of a codec whose core module corrects PTS with a function of its own (D-577): its own correction.
s32 own_pts_function(PPUContext& ctx, u32 h, u32 au_pts)
{
    u8* base = guest_base(ctx);
    return core_of(base, h)->correct_pts(ctx, core_handle(base, h), au_pts) != lv2::CELL_OK ? error(CELL_ADEC_ERROR_FATAL)
                                                                                         : lv2::CELL_OK;
}

// RPCS3 AdecContext::correct_pts_value: a PTS the game did not give follows the previous one by the core's step.
s32 correct_pts_value(PPUContext& ctx, u32 h, s32 pcm_handle, s8 type)
{
    u8* base = guest_base(ctx);
    if (verify_pcm_handle(base, h, pcm_handle) != lv2::CELL_OK) return error(CELL_ADEC_ERROR_FATAL);
    const u32 pts = frame_at(base, h, pcm_handle) + frame_off::au_info + 8;
    const bool own = on_own_fallback_core(base, h);
    for (const PtsFunction& f : kPtsFunctions)
        if (f.type == type) return own ? own_pts_function(ctx, h, pts) : call_pts_function(ctx, h, f.library, f.nid, pts);
    if (type == CORRECT_PTS_M4AAC) {
        if (own) return own_pts_function(ctx, h, pts);
        switch (static_cast<s32>(ps_read32(base, h + ctx_off::type))) {
        case CELL_ADEC_TYPE_M4AAC: return call_pts_function(ctx, h, "cellM4AacDec", 0x431A1DCA, pts);
        case CELL_ADEC_TYPE_M4AAC_2CH: return call_pts_function(ctx, h, "cellM4AacDec2ch", 0x869E328D, pts);
        case CELL_ADEC_TYPE_M4AAC_2CH_MOD: return call_pts_function(ctx, h, "cellM4AacDec2chmod", 0x156FE2F0, pts);
        default: return lv2::CELL_OK;
        }
    }
    const u32 prev_upper = ps_read32(base, h + ctx_off::previous_pts), prev_lower = ps_read32(base, h + ctx_off::previous_pts + 4);
    if (ps_read32(base, pts) == kTsInvalid) {
        if (ps_read32(base, pts + 4) != kTsInvalid) return error(CELL_ADEC_ERROR_FATAL);
        if (prev_upper == kTsInvalid && prev_lower == kTsInvalid) return lv2::CELL_OK;
        u32 adjust = 0;
        switch (type) {
        case 0: adjust = 450; break;   // LPCM (HDMV)
        case 1: adjust = 150; break;   // LPCM (DVD)
        case 2: adjust = 3840; break;  // ATRAC3plus 48 kHz
        case 3: adjust = 4180; break;  // ATRAC3plus 44.1 kHz
        case 4: adjust = 5760; break;  // ATRAC3plus 32 kHz
        case 5: adjust = 2880; break;  // AC-3
        case 6: adjust = 4180; break;  // ATRAC3
        case 7: adjust = 2160; break;  // MP3 48 kHz
        case 8: adjust = 2351; break;  // MP3 44.1 kHz
        case 9: adjust = 3240; break;  // MP3 32 kHz
        case 39: adjust = 3840; break; // ATRAC3 multichannel
        default: break;
        }
        u32 upper = prev_upper;
        const u32 lower = prev_lower + adjust;
        if (lower < prev_lower) upper = (upper + 1) & 1; // overflow
        ps_write32(base, pts, upper);
        ps_write32(base, pts + 4, lower);
        if (upper == kTsInvalid) return error(CELL_ADEC_ERROR_FATAL);
    }
    if (ps_read32(base, pts + 4) == kTsInvalid) return error(CELL_ADEC_ERROR_FATAL);
    ps_write32(base, h + ctx_off::previous_pts, ps_read32(base, pts));
    ps_write32(base, h + ctx_off::previous_pts + 4, ps_read32(base, pts + 4));
    return lv2::CELL_OK;
}

// The game's callback: (handle, msgType, msgData, cbArg).
void callback(PPUContext& ctx, u32 h, s32 type, s32 data)
{
    u8* base = guest_base(ctx);
    call_guest(ctx, ps_read32(base, h + ctx_off::cb_func),
        {h, static_cast<u64>(type), static_cast<u64>(static_cast<s64>(data)), ps_read32(base, h + ctx_off::cb_arg)});
}

// ---- the API

s32 query_attr(PPUContext& ctx, u32 type_ptr, u32 attr, const std::string& call)
{
    if (!type_ptr || !attr) return error(CELL_ADEC_ERROR_ARG);
    if (!usable(ctx, type_ptr, 4, call) || !usable(ctx, attr, 12, call)) return error(CELL_ADEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    const s32 type = static_cast<s32>(ps_read32(base, type_ptr));
    if (invalid_type(type)) return error(CELL_ADEC_ERROR_ARG);
    const std::unique_ptr<AdecCore> core = core_for(type);
    if (!core) return error(CELL_ADEC_ERROR_FATAL);
    const u32 mem = core->mem_size(ctx);
    const s32 handles = core->pcm_handle_num(ctx);
    const u32 bsi = core->bsi_info_size(ctx);
    ps_write32(base, attr, mem + (bsi + kFrameSize) * static_cast<u32>(handles) + ctx_off::size + 0x7F);
    ps_write32(base, attr + 4, kVersionUpper);
    ps_write32(base, attr + 8, core->version(ctx));
    return lv2::CELL_OK;
}

// RPCS3 adecOpen. `res`: a CellAdecResource; `spurs_res`: a CellAdecResourceSpurs or 0.
s32 open(PPUContext& ctx, u32 type_ptr, u32 res, u32 cb, u32 handle_out, u32 spurs_res, const std::string& call)
{
    u8* base = guest_base(ctx);
    if (!type_ptr || !res || !cb || !handle_out) return error(CELL_ADEC_ERROR_ARG);
    if (!usable(ctx, type_ptr, 4, call) || !usable(ctx, res, 20, call) || !usable(ctx, cb, 8, call) ||
        !usable(ctx, handle_out, 4, call))
        return error(CELL_ADEC_ERROR_ARG);
    const s32 type = static_cast<s32>(ps_read32(base, type_ptr));
    const u32 total = ps_read32(base, res), start = ps_read32(base, res + 4), ppu_prio = ps_read32(base, res + 8),
              spu_prio = ps_read32(base, res + 12), stack = ps_read32(base, res + 16);
    if (type < 0 || type >= 32 || !start || ppu_prio >= 0xA00 || spu_prio >= 0x100 || stack < 0x1000 || !ps_read32(base, cb))
        return error(CELL_ADEC_ERROR_ARG);
    {
        StackVar attr(ctx, base, 0x10);
        if (const s32 r = query_attr(ctx, type_ptr, attr.addr(), call); r != lv2::CELL_OK) return r;
        if (total < ps_read32(base, attr.addr())) return error(CELL_ADEC_ERROR_FATAL);
    }
    if (!usable(ctx, start, total, "the decoder's memory")) return error(CELL_ADEC_ERROR_ARG);
    u32 ops = 0;
    const std::unique_ptr<AdecCore> core = core_for(type, &ops);
    if (!core) return error(CELL_ADEC_ERROR_FATAL);
    const s32 handles = core->pcm_handle_num(ctx);
    const u32 bsi = core->bsi_info_size(ctx);
    const u32 h = (start + 0x7F) & ~0x7Fu;
    const u32 frames = h + ctx_off::size;
    const u32 bsi_infos = frames + static_cast<u32>(handles) * kFrameSize;
    const u32 core_mem = (bsi_infos + bsi * static_cast<u32>(handles) + 0x7F) & ~0x7Fu;
    if (type == CELL_ADEC_TYPE_LPCM_DVD) set_lpcm_dvd_packing(base, core_mem, true);
    else if (is_lpcm(type)) set_lpcm_dvd_packing(base, core_mem, false);
    ps_write32(base, h + ctx_off::this_, h);
    ps_write32(base, h + ctx_off::this_size, ctx_off::size + (bsi + kFrameSize) * static_cast<u32>(handles));
    ps_write32(base, h + ctx_off::unk, 0);
    ps_write32(base, h + ctx_off::sequence_state, SEQ_DORMANT);
    ps_write32(base, h + ctx_off::type, static_cast<u32>(type));
    std::memmove(base + h + ctx_off::res, base + res, 20);
    ps_write32(base, h + ctx_off::cb_func, ps_read32(base, cb));
    ps_write32(base, h + ctx_off::cb_arg, ps_read32(base, cb + 4));
    ps_write32(base, h + ctx_off::core_handle, core_mem);
    ps_write32(base, h + ctx_off::core_ops, ops);
    ps_write32(base, h + ctx_off::previous_pts, kTsInvalid);
    ps_write32(base, h + ctx_off::previous_pts + 4, kTsInvalid);
    ps_write32(base, h + ctx_off::frames_num, static_cast<u32>(handles));
    ps_write32(base, h + ctx_off::reserved1, 0);
    ps_write32(base, h + ctx_off::frames_head, 0xFFFFFFFF);
    ps_write32(base, h + ctx_off::frames_tail, 0xFFFFFFFF);
    ps_write32(base, h + ctx_off::frames, frames);
    ps_write32(base, h + ctx_off::bsi_size, bsi);
    write_mutex_attr(base, h + ctx_off::mutex_attribute, "_adem03");
    queue_init(ctx, h + ctx_off::pcm_queue);
    queue_init(ctx, h + ctx_off::pcm_item_queue);
    for (s32 i = 0; i < handles; i++) {
        const u32 f = frames + kFrameSize * static_cast<u32>(i);
        for (const u32 off : {frame_off::in_use, frame_off::au_done, frame_off::unk1, frame_off::pcm_out, frame_off::unk2})
            ps_write8(base, f + off, 0);
        ps_write32(base, f + frame_off::this_index, static_cast<u32>(i));
        ps_write32(base, f + frame_off::pcm_item + item_off::bsi_info, bsi_infos + bsi * static_cast<u32>(i));
        ps_write32(base, f + frame_off::reserved1, 0);
        ps_write32(base, f + frame_off::reserved2, 0);
        ps_write32(base, f + frame_off::next, 0);
        ps_write32(base, f + frame_off::prev, 0);
    }
    lv2::sys_mutex_create(thread_of(ctx), h + ctx_off::mutex, h + ctx_off::mutex_attribute); // unchecked on LLE
    ps_write32(base, handle_out, h);
    return core->open(ctx, core_mem, h, res, spurs_res);
}

s32 cellAdecQueryAttr(PPUContext& ctx, u32 type, u32 attr)
{
    const std::string call = std::format("cellAdecQueryAttr({:#x}, {:#x})", type, attr);
    return finish(query_attr(ctx, type, attr, call), call);
}

s32 cellAdecOpen(PPUContext& ctx, u32 type, u32 res, u32 cb, u32 handle)
{
    const std::string call = std::format("cellAdecOpen({:#x}, {:#x}, {:#x}, {:#x})", type, res, cb, handle);
    return finish(open(ctx, type, res, cb, handle, 0, call), call);
}

// CellAdecResourceEx (totalMemSize, startAddr, ppuThreadPriority, ppuThreadStackSize, spurs_addr, priority (8 bytes,
// unaligned), maxContention) as a CellAdecResource and a CellAdecResourceSpurs (RPCS3 cellAdecOpenExt).
s32 open_ext(PPUContext& ctx, u32 type, u32 res, u32 cb, u32 handle, const std::string& call)
{
    if (!res) return error(CELL_ADEC_ERROR_ARG);
    if (!usable(ctx, res, 0x20, call)) return error(CELL_ADEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    StackVar r(ctx, base, 0x20);
    StackVar s(ctx, base, 0x10);
    const u32 v[5] = {ps_read32(base, res), ps_read32(base, res + 4), ps_read32(base, res + 8), 0, ps_read32(base, res + 12)};
    for (u32 i = 0; i < 5; i++) ps_write32(base, r.addr() + 4 * i, v[i]);
    std::memmove(base + s.addr(), base + res + 0x10, 0x10);
    return open(ctx, type, r.addr(), cb, handle, s.addr(), call);
}

s32 cellAdecOpenExt(PPUContext& ctx, u32 type, u32 res, u32 cb, u32 handle)
{
    const std::string call = std::format("cellAdecOpenExt({:#x}, {:#x}, {:#x}, {:#x})", type, res, cb, handle);
    return finish(open_ext(ctx, type, res, cb, handle, call), call);
}

s32 cellAdecOpenEx(PPUContext& ctx, u32 type, u32 res, u32 cb, u32 handle)
{
    const std::string call = std::format("cellAdecOpenEx({:#x}, {:#x}, {:#x}, {:#x})", type, res, cb, handle);
    return finish(open_ext(ctx, type, res, cb, handle, call), call);
}

s32 close(PPUContext& ctx, u32 h, const std::string& call)
{
    if (!context_usable(ctx, h, call)) return error(CELL_ADEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (ps_read32(base, h + ctx_off::this_) != h) return error(CELL_ADEC_ERROR_ARG);
    if (const s32 r = core_of(base, h)->close(ctx, core_handle(base, h)); r != lv2::CELL_OK) return r;
    if (const s32 r = lv2::sys_mutex_destroy(thread_of(ctx), ps_read32(base, h + ctx_off::mutex)); r != lv2::CELL_OK)
        return r;
    if (const s32 r = queue_finalize(ctx, h + ctx_off::pcm_queue); r != lv2::CELL_OK) return r;
    if (const s32 r = queue_finalize(ctx, h + ctx_off::pcm_item_queue); r != lv2::CELL_OK) return r;
    ps_write32(base, h + ctx_off::this_, 0);
    ps_write32(base, h + ctx_off::sequence_state, SEQ_CLOSED);
    return lv2::CELL_OK;
}

s32 cellAdecClose(PPUContext& ctx, u32 handle)
{
    const std::string call = std::format("cellAdecClose({:#x})", handle);
    return finish(close(ctx, handle, call), call);
}

s32 start_seq(PPUContext& ctx, u32 h, u32 param, const std::string& call)
{
    if (!h || !param) return error(CELL_ADEC_ERROR_ARG);
    if (!context_usable(ctx, h, call) || !usable(ctx, param, 4, call)) return error(CELL_ADEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    const u32 state = ps_read32(base, h + ctx_off::sequence_state);
    if (state != SEQ_DORMANT && state != SEQ_READY) return error(CELL_ADEC_ERROR_SEQ);
    if (is_lpcm(static_cast<s32>(ps_read32(base, h + ctx_off::type))) && !usable(ctx, param, 16, call))
        return error(CELL_ADEC_ERROR_ARG);
    if (const s32 r = core_of(base, h)->start_seq(ctx, core_handle(base, h), param); r != lv2::CELL_OK) return r;
    ps_write32(base, h + ctx_off::sequence_state, SEQ_READY);
    return lv2::CELL_OK;
}

s32 cellAdecStartSeq(PPUContext& ctx, u32 handle, u32 param)
{
    const std::string call = std::format("cellAdecStartSeq({:#x}, {:#x})", handle, param);
    return finish(start_seq(ctx, handle, param, call), call);
}

s32 end_seq(PPUContext& ctx, u32 h, const std::string& call)
{
    if (!context_usable(ctx, h, call)) return error(CELL_ADEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (ps_read32(base, h + ctx_off::this_) != h) return error(CELL_ADEC_ERROR_ARG);
    if (ps_read32(base, h + ctx_off::sequence_state) != SEQ_READY) return error(CELL_ADEC_ERROR_SEQ);
    return core_of(base, h)->end_seq(ctx, core_handle(base, h)); // LLE leaves the state ready
}

s32 cellAdecEndSeq(PPUContext& ctx, u32 handle)
{
    const std::string call = std::format("cellAdecEndSeq({:#x})", handle);
    return finish(end_seq(ctx, handle, call), call);
}

s32 decode_au(PPUContext& ctx, u32 h, u32 au_info, const std::string& call)
{
    if (!h || !au_info) return error(CELL_ADEC_ERROR_ARG);
    if (!context_usable(ctx, h, call) || !usable(ctx, au_info, 0x18, call)) return error(CELL_ADEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (!ps_read32(base, au_info + 4)) return error(CELL_ADEC_ERROR_ARG);
    if (ps_read32(base, h + ctx_off::sequence_state) != SEQ_READY) return error(CELL_ADEC_ERROR_SEQ);
    if (!ps_read32(base, au_info)) return error(CELL_ADEC_ERROR_ARG);
    const s32 pcm_handle = get_new_pcm_handle(base, h, au_info);
    if (pcm_handle < 0) return pcm_handle; // BUSY
    const s32 type = static_cast<s32>(ps_read32(base, h + ctx_off::type));
    const u64 user_data = ps_read64(base, au_info + 16);
    if (type == CELL_ADEC_TYPE_LPCM_BLURAY && set_lpcm_bluray_params(base, core_handle(base, h), user_data) != lv2::CELL_OK)
        return error(CELL_ADEC_ERROR_FATAL);
    if (type == CELL_ADEC_TYPE_LPCM_DVD && set_lpcm_dvd_params(base, core_handle(base, h), user_data) != lv2::CELL_OK)
        return error(CELL_ADEC_ERROR_FATAL);
    return core_of(base, h)->decode_au(ctx, core_handle(base, h), pcm_handle, au_info);
}

s32 cellAdecDecodeAu(PPUContext& ctx, u32 handle, u32 au_info)
{
    const std::string call = std::format("cellAdecDecodeAu({:#x}, {:#x})", handle, au_info);
    return finish(decode_au(ctx, handle, au_info, call), call);
}

s32 get_pcm(PPUContext& ctx, u32 h, u32 out_buffer, const std::string& call)
{
    if (!context_usable(ctx, h, call)) return error(CELL_ADEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    // Without a cellAdecGetPcmItem first, the item queue's entry goes too.
    if (queue_peek_handle(ctx, h + ctx_off::pcm_item_queue) == queue_peek_handle(ctx, h + ctx_off::pcm_queue))
        queue_pop(ctx, h + ctx_off::pcm_item_queue);
    const std::optional<u32> item = queue_pop(ctx, h + ctx_off::pcm_queue);
    if (!item) return error(CELL_ADEC_ERROR_EMPTY);
    const s32 pcm_handle = static_cast<s32>(ps_read32(base, *item + item_off::pcm_handle));
    if (verify_pcm_handle(base, h, pcm_handle) != lv2::CELL_OK) return error(CELL_ADEC_ERROR_FATAL);
    const std::unique_ptr<AdecCore> core = core_of(base, h);
    const u32 ch = core_handle(base, h);
    if (is_lpcm(static_cast<s32>(ps_read32(base, h + ctx_off::type))))
        set_lpcm_output_size(base, ch, ps_read32(base, *item + item_off::size));
    if (const s32 r = core->realign(ctx, ch, out_buffer, ps_read32(base, *item + item_off::start_addr)); r != lv2::CELL_OK)
        return r;
    if (const s32 r = core->release_pcm(ctx, ch, pcm_handle, out_buffer); r != lv2::CELL_OK) return r;
    if (unlink_frame(ctx, h, pcm_handle) != lv2::CELL_OK) return error(CELL_ADEC_ERROR_FATAL);
    reset_frame(base, h, pcm_handle);
    return lv2::CELL_OK;
}

s32 cellAdecGetPcm(PPUContext& ctx, u32 handle, u32 out_buffer)
{
    const std::string call = std::format("cellAdecGetPcm({:#x}, {:#x})", handle, out_buffer);
    return finish(get_pcm(ctx, handle, out_buffer, call), call);
}

s32 get_pcm_item(PPUContext& ctx, u32 h, u32 item_out, const std::string& call)
{
    if (!context_usable(ctx, h, call)) return error(CELL_ADEC_ERROR_ARG);
    if (!item_out) return error(CELL_ADEC_ERROR_FATAL);
    if (!usable(ctx, item_out, 4, call)) return error(CELL_ADEC_ERROR_ARG);
    const std::optional<u32> item = queue_pop(ctx, h + ctx_off::pcm_item_queue);
    if (!item) return error(CELL_ADEC_ERROR_EMPTY);
    ps_write32(guest_base(ctx), item_out, *item);
    return lv2::CELL_OK;
}

s32 cellAdecGetPcmItem(PPUContext& ctx, u32 handle, u32 item_out)
{
    const std::string call = std::format("cellAdecGetPcmItem({:#x}, {:#x})", handle, item_out);
    return finish(get_pcm_item(ctx, handle, item_out, call), call);
}

} // namespace

// ---- the notifications (RPCS3 adecNotify*)

namespace adec {

s32 notify_au_done(PPUContext& ctx, s32 pcm_handle, u32 h)
{
    u8* base = guest_base(ctx);
    const u32 au_info = get_au_info(base, h, pcm_handle);
    if (!au_info) return error(CELL_ADEC_ERROR_FATAL);
    ps_write8(base, frame_at(base, h, pcm_handle) + frame_off::au_done, 1);
    callback(ctx, h, CELL_ADEC_MSG_TYPE_AUDONE, static_cast<s32>(au_info));
    return lv2::CELL_OK;
}

s32 notify_pcm_out(PPUContext& ctx, s32 pcm_handle, u32 pcm_addr, u32 pcm_size, u32 h, u32 bsi_info,
    s8 correct_pts_type, s32 error_code)
{
    u8* base = guest_base(ctx);
    if (!get_au_info(base, h, pcm_handle)) return error(CELL_ADEC_ERROR_FATAL);
    if (const s32 r = correct_pts_value(ctx, h, pcm_handle, correct_pts_type); r != lv2::CELL_OK) return r;
    if (link_frame(ctx, h, pcm_handle) != lv2::CELL_OK) return error(CELL_ADEC_ERROR_FATAL);
    ps_write8(base, frame_at(base, h, pcm_handle) + frame_off::pcm_out, 1);
    if (set_pcm_item(base, h, pcm_handle, pcm_addr, pcm_size, bsi_info) != lv2::CELL_OK) return error(CELL_ADEC_ERROR_FATAL);
    const u32 item = frame_at(base, h, pcm_handle) + frame_off::pcm_item;
    if (queue_push(ctx, h + ctx_off::pcm_queue, item, pcm_handle) != lv2::CELL_OK) return error(CELL_ADEC_ERROR_FATAL);
    if (queue_push(ctx, h + ctx_off::pcm_item_queue, item, pcm_handle) != lv2::CELL_OK) return error(CELL_ADEC_ERROR_FATAL);
    callback(ctx, h, CELL_ADEC_MSG_TYPE_PCMOUT, error_code);
    return lv2::CELL_OK;
}

s32 notify_error(PPUContext& ctx, s32 error_code, u32 h)
{
    PS_LOG_ERROR(kChan, "decoder {:#x}: the core reports {:#010x}", h, static_cast<u32>(error_code));
    callback(ctx, h, CELL_ADEC_MSG_TYPE_ERROR, error_code);
    return lv2::CELL_OK;
}

s32 notify_seq_done(PPUContext& ctx, u32 h)
{
    callback(ctx, h, CELL_ADEC_MSG_TYPE_SEQDONE, lv2::CELL_OK);
    return lv2::CELL_OK;
}

s32 _pspocket_adec_notify_au_done(PPUContext& ctx, s32 pcm_handle, u32 context)
{
    return notify_au_done(ctx, pcm_handle, context);
}

// `bsi_info_ptr` points at the core's bitstream information's address.
s32 _pspocket_adec_notify_pcm_out(PPUContext& ctx, s32 pcm_handle, u32 pcm_addr, u32 pcm_size, u32 context,
    u32 bsi_info_ptr, u8 correct_pts_type, s32 error_code)
{
    if (!usable(ctx, bsi_info_ptr, 4, "the core's bitstream information")) return error(CELL_ADEC_ERROR_FATAL);
    return notify_pcm_out(ctx, pcm_handle, pcm_addr, pcm_size, context, ps_read32(guest_base(ctx), bsi_info_ptr),
        static_cast<s8>(correct_pts_type), error_code);
}

s32 _pspocket_adec_notify_error(PPUContext& ctx, s32 error_code, u32 context)
{
    return notify_error(ctx, error_code, context);
}

s32 _pspocket_adec_notify_seq_done(PPUContext& ctx, u32 context) { return notify_seq_done(ctx, context); }

} // namespace adec

void register_cellAdec(Registry& r)
{
    using adec::_pspocket_adec_notify_au_done;
    using adec::_pspocket_adec_notify_error;
    using adec::_pspocket_adec_notify_pcm_out;
    using adec::_pspocket_adec_notify_seq_done;
    using adec::_pspocket_adec_ffmpeg_dec_thread;
    using adec::_pspocket_atracx_dec_thread;
    using adec::_pspocket_lpcm_dec_thread;
    r.add("cellAdec", {
        PS_HLE_FUNC(0x7E4A4A49, cellAdecQueryAttr),
        PS_HLE_FUNC(0xD00A6988, cellAdecOpen),
        PS_HLE_FUNC(0x8B5551A4, cellAdecOpenEx),
        PS_HLE_FUNC(0xDF982D2C, cellAdecOpenExt),
        PS_HLE_FUNC(0x847D2380, cellAdecClose),
        PS_HLE_FUNC(0x487B613E, cellAdecStartSeq),
        PS_HLE_FUNC(0xE2EA549B, cellAdecEndSeq),
        PS_HLE_FUNC(0x1529E506, cellAdecDecodeAu),
        PS_HLE_FUNC(0x97FF2AF1, cellAdecGetPcm),
        PS_HLE_FUNC(0xBD75F78B, cellAdecGetPcmItem),
        PS_HLE_INTERNAL(adec::NID_notify_au_done, _pspocket_adec_notify_au_done),
        PS_HLE_INTERNAL(adec::NID_notify_pcm_out, _pspocket_adec_notify_pcm_out),
        PS_HLE_INTERNAL(adec::NID_notify_error, _pspocket_adec_notify_error),
        PS_HLE_INTERNAL(adec::NID_notify_seq_done, _pspocket_adec_notify_seq_done),
        PS_HLE_INTERNAL(adec::NID_lpcm_dec_thread, _pspocket_lpcm_dec_thread),
        PS_HLE_INTERNAL(adec::NID_atracx_dec_thread, _pspocket_atracx_dec_thread),
        PS_HLE_INTERNAL(adec::NID_ffmpeg_dec_thread, _pspocket_adec_ffmpeg_dec_thread),
    });
    r.add_imports("cellAdec", kImports);
}

} // namespace pspocket::hle
