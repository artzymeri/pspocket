#pragma once

// cellAdec and its decoder cores (D-197). On a PS3, libadec (the game's API) keeps its context in the game's memory and
// calls each codec's core (LPCM inside libadec; ATRAC3plus, MP3, AC-3, ... in their own modules) through the core's
// table of operations (CellAdecCoreOps); the cores report back through four notification functions. pspocket's front
// end (cellAdec.cpp) keeps the same context, laid out as Sony's, and calls a core through AdecCore: its own (LPCM,
// adec_lpcm.cpp; ATRAC3plus, adec_atracx.cpp) directly, or Sony's core module through its operation table, as RPCS3's
// cellAdec calls the LLE cores. For MP3, MPEG audio layers I and II, MPEG-4 AAC, AC-3 and ATRAC3 (D-577), and Dolby
// Digital Plus, MPEG-2 AAC, MP3 Surround, ATRAC3 multichannel, TrueHD, DTS and WMA (D-621, D-622) the firmware's core
// serves first and pspocket's own core on FFmpeg (adec_ffmpeg.hpp) where no firmware module gives one.
// pspocket's own cores are also the firmware modules' exports, operation tables and PTS functions, for Sony's modules
// and others to call (adec_tables.cpp, D-591).
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellAdec.cpp/.h.

#include <array>
#include <memory>

#include "pspocket/core/types.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle::adec {

enum : u32 {
    CELL_ADEC_ERROR_FATAL = 0x80610001,
    CELL_ADEC_ERROR_SEQ = 0x80610002,
    CELL_ADEC_ERROR_ARG = 0x80610003,
    CELL_ADEC_ERROR_BUSY = 0x80610004,
    CELL_ADEC_ERROR_EMPTY = 0x80610005,
    CELL_ADEC_ERROR_LPCM_ARG = 0x80612003,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

enum : s32 {
    CELL_ADEC_TYPE_LPCM_PAMF = 1,
    CELL_ADEC_TYPE_AC3 = 2,
    CELL_ADEC_TYPE_ATRACX = 3,
    CELL_ADEC_TYPE_MP3 = 4,
    CELL_ADEC_TYPE_ATRAC3 = 5,
    CELL_ADEC_TYPE_MPEG_L2 = 6,
    CELL_ADEC_TYPE_M2AAC = 7,
    CELL_ADEC_TYPE_EAC3 = 8,
    CELL_ADEC_TYPE_TRUEHD = 9,
    CELL_ADEC_TYPE_DTS = 10,
    CELL_ADEC_TYPE_CELP = 11,
    CELL_ADEC_TYPE_LPCM_BLURAY = 12,
    CELL_ADEC_TYPE_ATRACX_2CH = 13,
    CELL_ADEC_TYPE_ATRACX_6CH = 14,
    CELL_ADEC_TYPE_ATRACX_8CH = 15,
    CELL_ADEC_TYPE_M4AAC = 16,
    CELL_ADEC_TYPE_LPCM_DVD = 17,
    CELL_ADEC_TYPE_WMA = 18,
    CELL_ADEC_TYPE_DTSLBR = 19,
    CELL_ADEC_TYPE_M4AAC_2CH = 20,
    CELL_ADEC_TYPE_DTSHD = 21,
    CELL_ADEC_TYPE_MPEG_L1 = 22,
    CELL_ADEC_TYPE_MP3S = 23,
    CELL_ADEC_TYPE_M4AAC_2CH_MOD = 24,
    CELL_ADEC_TYPE_CELP8 = 25,
    // RPCS3's RESERVED22 and RESERVED23, "either WMA Pro or WMA Lossless, never released": libadec has no core for
    // them, so cellAdec refuses them. pspocket's WMA Pro and WMA Lossless cores, reached only through their own
    // modules' tables (cellWMAPROdec, cellWMALSLdec), carry these numbers in their contexts, in the order of the PTS
    // types (WMA Pro's 35 before WMA Lossless's 36, D-622).
    CELL_ADEC_TYPE_RESERVED22 = 28,
    CELL_ADEC_TYPE_RESERVED23 = 29,
    CELL_ADEC_TYPE_DTSHDCORE = 30,
    CELL_ADEC_TYPE_ATRAC3MULTI = 31,
};
constexpr s32 CELL_ADEC_TYPE_WMAPRO_CORE = CELL_ADEC_TYPE_RESERVED22, CELL_ADEC_TYPE_WMALSL_CORE = CELL_ADEC_TYPE_RESERVED23;

// The output channel layouts (RPCS3 CellAdecChannel), as LPCM's parameters and information name them.
enum : u32 {
    CELL_ADEC_CH_MONO = 1, CELL_ADEC_CH_STEREO = 3, CELL_ADEC_CH_3_0 = 4, CELL_ADEC_CH_2_1 = 5, CELL_ADEC_CH_3_1 = 6,
    CELL_ADEC_CH_2_2 = 7, CELL_ADEC_CH_3_2 = 8, CELL_ADEC_CH_3_2_LFE = 9, CELL_ADEC_CH_3_4 = 10, CELL_ADEC_CH_3_4_LFE = 11,
};
constexpr bool is_lpcm(s32 type)
{
    return type == CELL_ADEC_TYPE_LPCM_PAMF || type == CELL_ADEC_TYPE_LPCM_BLURAY || type == CELL_ADEC_TYPE_LPCM_DVD;
}

// How the front end corrects the PTS of an access unit the game gave none (RPCS3 AdecCorrectPtsValueType): a fixed step
// after the previous one for these; the others call the core module's _SceAdecCorrectPtsValue_* (or, for pspocket's own
// core of such a codec, AdecCore::correct_pts).
constexpr s8 CORRECT_PTS_LPCM_HDMV = 0;
constexpr s8 CORRECT_PTS_AC3 = 5, CORRECT_PTS_ATRAC3 = 6, CORRECT_PTS_MP3_48000 = 7, CORRECT_PTS_MP3_44100 = 8,
             CORRECT_PTS_MP3_32000 = 9, CORRECT_PTS_ATRAC3MULTI = 39;
constexpr s8 CORRECT_PTS_EAC3 = 17, CORRECT_PTS_DTSHD = 21, CORRECT_PTS_M2AAC = 25, CORRECT_PTS_MPEG_L2 = 26,
             CORRECT_PTS_TRUEHD = 27, CORRECT_PTS_DTS = 28, CORRECT_PTS_M4AAC = 29, CORRECT_PTS_WMA = 30,
             CORRECT_PTS_DTSLBR = 31, CORRECT_PTS_MPEG_L1 = 32, CORRECT_PTS_MP3S = 33, CORRECT_PTS_WMAPRO = 35,
             CORRECT_PTS_WMALSL = 36, CORRECT_PTS_DTSHDCORE = 37;
constexpr u32 CODEC_TS_INVALID = 0xFFFFFFFF;

// AdecContext (0x530, the CellAdecHandle): its fields' offsets. res is CellAdecResource (totalMemSize, startAddr,
// ppuThreadPriority, spuThreadPriority, ppuThreadStackSize), cb CellAdecCb (cbFunc, cbArg), previous_pts a
// CellCodecTimeStamp (upper, lower); pcm_queue and pcm_item_queue are AdecOutputQueues (out_off).
namespace ctx_off {
constexpr u32 this_ = 0x00, this_size = 0x04, unk = 0x08, sequence_state = 0x0C, type = 0x10, res = 0x14, cb_func = 0x28,
              cb_arg = 0x2C, core_handle = 0x30, core_ops = 0x34, previous_pts = 0x38, frames_num = 0x40,
              reserved1 = 0x44, frames_head = 0x48, frames_tail = 0x4C, frames = 0x50, bsi_size = 0x54,
              mutex_attribute = 0x58, mutex = 0x80, pcm_queue = 0x84, pcm_item_queue = 0xD8, size = 0x530;
}
// AdecFrame (0x68): in use, its index, AU done and PCM out flags, the AU's CellAdecAuInfo, its CellAdecPcmItem.
// CellAdecAuInfo (0x18): startAddr, size, pts (upper, lower), userData (u64). CellAdecPcmItem (0x30): pcmHandle,
// status, startAddr, size, bsiInfo (pcmAttr), then the CellAdecAuInfo at +0x18.
namespace frame_off {
constexpr u32 in_use = 0x00, this_index = 0x04, au_done = 0x08, unk1 = 0x09, pcm_out = 0x0A, unk2 = 0x0B, au_info = 0x10,
              pcm_item = 0x28, reserved1 = 0x58, reserved2 = 0x5C, next = 0x60, prev = 0x64, size = 0x68;
}
namespace item_off {
constexpr u32 pcm_handle = 0x00, status = 0x04, start_addr = 0x08, size = 0x0C, bsi_info = 0x10, au_info = 0x18;
}

// The four notifications a core's open is given (RPCS3 AdecCb: a function and its argument each), in the order of
// CellAdecCoreOps's open: AUDONE, PCMOUT, ERROR, SEQDONE. `direct`: they are pspocket's own front end's (cellAdec.cpp,
// which opened the core itself), and the core calls them as C++ with their argument; otherwise they are guest functions
// (Sony's libadec, or another module driving the core through its exported table, D-591), called as RPCS3's cores call
// cbFunc.
struct CoreNotify {
    std::array<u32, 4> fn{}, arg{};
    bool direct = false;
};
// The front end's own: its four internal functions (linked with cellAdec), `context` (its AdecContext) the argument of
// each.
CoreNotify front_end_notify(u32 context);

// Where a core keeps its notifications in its context: the four (function, argument) pairs from `pairs`, then a word at
// `flag`, 1 when they are guest functions and 0 when they are the front end's own (RPCS3 keeps the pairs; the flag is
// pspocket's, in a word its layout leaves unused).
struct NotifySlots {
    u32 pairs, flag;
};
void store_notify(u8* base, u32 core_handle, NotifySlots slots, const CoreNotify& n);

// A core's notifications through what its open stored (on the core's thread): the front end's own called directly, a
// guest function through call_guest (PCMOUT's information then passed as RPCS3 passes it, a pointer to its address).
class CoreNotifier {
public:
    CoreNotifier(PPUContext& ctx, u32 core_handle, NotifySlots slots);
    void au_done(s32 pcm_handle) const;
    void pcm_out(s32 pcm_handle, u32 pcm_addr, u32 pcm_size, u32 bsi_info, s8 correct_pts_type, s32 error_code) const;
    void error(s32 error_code) const;
    void seq_done() const;

private:
    u32 fn(u32 i) const;
    u32 arg(u32 i) const;
    PPUContext& ctx_;
    u8* base_;
    u32 pairs_;
    bool guest_;
};

// A decoder core. Its own state lives in the game's memory at its handle (the front end's context's core_handle).
class AdecCore {
public:
    virtual ~AdecCore() = default;
    // getMemSize, getPcmHandleNum, getBsiInfoSize, getVersion.
    virtual u32 mem_size(PPUContext& ctx) = 0;
    virtual s32 pcm_handle_num(PPUContext& ctx) = 0;
    virtual u32 bsi_info_size(PPUContext& ctx) = 0;
    virtual u32 version(PPUContext& ctx) = 0;
    // `notify`: whom the core notifies (front_end_notify from pspocket's front end; open's arguments when it is called
    // through an operation table). `res`: a CellAdecResource; `spurs_res`: a CellAdecResourceSpurs
    // (cellAdecOpenEx/OpenExt, openExt) or 0.
    virtual s32 open(PPUContext& ctx, u32 core_handle, const CoreNotify& notify, u32 res, u32 spurs_res) = 0;
    virtual s32 close(PPUContext& ctx, u32 core_handle) = 0;
    virtual s32 start_seq(PPUContext& ctx, u32 core_handle, u32 param) = 0;
    virtual s32 end_seq(PPUContext& ctx, u32 core_handle) = 0;
    virtual s32 decode_au(PPUContext& ctx, u32 core_handle, s32 pcm_handle, u32 au_info) = 0;
    virtual s32 realign(PPUContext& ctx, u32 core_handle, u32 out_buffer, u32 pcm_start) = 0;
    virtual s32 release_pcm(PPUContext& ctx, u32 core_handle, s32 pcm_handle, u32 out_buffer) = 0;
    // The PTS of an access unit the game gave none, for the codecs whose core module corrects it with a function of its
    // own (_SceAdecCorrectPtsValue_*, called with the core's handle and the frame's CellCodecTimeStamp): pspocket's own
    // cores of those codecs answer here; CELL_OK or an error (the front end makes it FATAL). The others never get it.
    virtual s32 correct_pts(PPUContext& ctx, u32 core_handle, u32 au_pts);
};

// pspocket's LPCM core (adec_lpcm.cpp), for the three LPCM types.
std::unique_ptr<AdecCore> make_lpcm_core();
// pspocket's ATRAC3plus core on FFmpeg (adec_atracx.cpp), for up to `max_channels` channels (its memory size); none in a
// build without FFmpeg.
std::unique_ptr<AdecCore> make_atracx_core(u32 max_channels);
// pspocket's own CELP core (adec_celp.cpp, D-932): libcelpdec's, for MPEG-4 CELP at 16 kHz.
std::unique_ptr<AdecCore> make_celp_core();
// pspocket's own core on FFmpeg for a codec whose firmware core serves first (adec_ffmpeg.cpp, D-577): MP3, MPEG audio
// layers I and II, MPEG-4 AAC (the three types), AC-3 and ATRAC3; Dolby Digital Plus, MPEG-2 AAC, MP3 Surround and
// ATRAC3 multichannel (D-621); TrueHD, the four DTS types and WMA, and the WMA Pro and WMA Lossless cores under the
// reserved types 28 and 29 (D-622). None for another type or in a build without FFmpeg.
std::unique_ptr<AdecCore> make_ffmpeg_core(s32 type);

// What the front end sets in the LPCM core's context (RPCS3 adecOpen, adecSetLpcmBlurayParams, adecSetLpcmDvdParams,
// cellAdecGetPcm): the DVD packing before it opens, the Blu-ray and DVD forms' parameters from each access unit's user
// data before a decode (CELL_OK or FATAL), the size of the PCM taken before it is copied out.
void set_lpcm_dvd_packing(u8* base, u32 core_handle, bool dvd);
s32 set_lpcm_bluray_params(u8* base, u32 core_handle, u64 user_data);
s32 set_lpcm_dvd_params(u8* base, u32 core_handle, u64 user_data);
void set_lpcm_output_size(u8* base, u32 core_handle, u32 size);

// ---- The front end's notifications (cellAdec.cpp), called on a core's thread: RPCS3 adecNotify*. `context` is the
// front end's AdecContext; `bsi_info` the address of the core's bitstream information for this PCM.
s32 notify_au_done(PPUContext& ctx, s32 pcm_handle, u32 context);
s32 notify_pcm_out(PPUContext& ctx, s32 pcm_handle, u32 pcm_addr, u32 pcm_size, u32 context, u32 bsi_info,
    s8 correct_pts_type, s32 error_code);
s32 notify_error(PPUContext& ctx, s32 error_code, u32 context);
s32 notify_seq_done(PPUContext& ctx, u32 context);

// The four notifications as guest functions, for Sony's cores (internal functions of cellAdec, linked with it), and
// the LPCM core's thread. pspocket's own NIDs: nid_of of each name.
constexpr u32 NID_notify_au_done = 0x22351FEC, NID_notify_pcm_out = 0xAF08E5A0, NID_notify_error = 0xF1642C29,
              NID_notify_seq_done = 0xD2B167B0, NID_lpcm_dec_thread = 0x4ED4A064, NID_atracx_dec_thread = 0x346C0076,
              NID_ffmpeg_dec_thread = 0x01A3568A;
// A core's thread function, linked when the core first opens one: a core opened through its exported table (D-591)
// may be in a process that does not import cellAdec, whose internal functions are then not linked at load.
u32 core_thread_entry(u32 nid, const char* name);
s32 _pspocket_adec_notify_au_done(PPUContext& ctx, s32 pcm_handle, u32 context);
s32 _pspocket_adec_notify_pcm_out(PPUContext& ctx, s32 pcm_handle, u32 pcm_addr, u32 pcm_size, u32 context,
    u32 bsi_info_ptr, u8 correct_pts_type, s32 error_code);
s32 _pspocket_adec_notify_error(PPUContext& ctx, s32 error_code, u32 context);
s32 _pspocket_adec_notify_seq_done(PPUContext& ctx, u32 context);
void _pspocket_lpcm_dec_thread(PPUContext& ctx, u64 core_handle);
void _pspocket_atracx_dec_thread(PPUContext& ctx, u64 core_handle);
void _pspocket_adec_ffmpeg_dec_thread(PPUContext& ctx, u64 core_handle);
void _pspocket_celp_dec_thread(PPUContext& ctx, u64 core_handle);

} // namespace pspocket::hle::adec
