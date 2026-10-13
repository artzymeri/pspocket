// The cellAdec codec cores' own exports (D-591). On a PS3 each core module exports its operation table, a
// CellAdecCoreOps variable of twelve function descriptors (getMemSize, open, close, startSeq, endSeq, decodeAu,
// getVersion, realign, releasePcm, getPcmHandleNum, getBsiInfoSize, openExt), and for the codecs whose module corrects
// a missing PTS itself a _SceAdecCorrectPtsValue_* function; Sony's libadec and other modules call cores only through
// these. pspocket's own cores (LPCM, adec_lpcm.cpp; ATRAC3plus, adec_atracx.cpp; MP3, MPEG audio layers I and II,
// MPEG-4 AAC, AC-3 and ATRAC3 on FFmpeg, adec_ffmpeg.hpp; and, D-623, Dolby Digital Plus, MPEG-2 AAC, MP3 Surround,
// ATRAC3 multichannel, TrueHD, DTS, DTS-HD, its core, DTS Express, WMA, WMA Pro and WMA Lossless, D-621 and D-622) are
// offered here the same way: each table is a variable of its library, laid out when first linked with descriptors of
// internal functions of cellAdec that run the core's code (AdecCore) with the arguments, notifications and answers of
// the ABI. A core opened through a table notifies the functions open was given, as guest calls (CoreNotifier);
// pspocket's own front end opens its cores directly and is notified directly. The cores on FFmpeg are not compared with
// Sony's yet: their libraries are firmware-first (D-563), so a module importing one gets Sony's where the system
// software has it, and pspocket's where it does not.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellAdec.cpp (g_cell_adec_core_ops_lpcm and _CellAdecCoreOp*_lpcm; each core
// module's REG_VNID table and REG_FUNC _SceAdecCorrectPtsValue_*), cellAdec.h (CellAdecCoreOps, CellAdecCoreOp*,
// AdecNotify*), cellAtracXdec.cpp (the four g_cell_adec_core_ops_atracx* tables and _CellAdecCoreOp*_atracx).
//
// Differences from RPCS3: pointers RPCS3 ensure()s (LLE would fault on them) are checked: a core handle, resource,
// parameter block, access unit, attribute or version pointer that is not guest memory, or a notification without a
// function, is logged and refused with CELL_ADEC_ERROR_ARG (D-042). RPCS3 has no core of its own for the codecs on
// FFmpeg (it runs Sony's modules for them): their tables and PTS functions follow its LPCM and ATRAC3plus ones.

#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "adec.hpp"
#include "adec_ffmpeg.hpp"
#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

using namespace adec;

constexpr std::string_view kChan = "cellAdec";

enum class Table : u8 {
    Lpcm, AtracX, AtracX2ch, AtracX6ch, AtracX8ch, Mp3, MpegL1, MpegL2, M4Aac, M4Aac2ch, M4Aac2chMod, Ac3, Atrac3,
    // D-623
    Ddp, M2Aac, Mp3s, Atrac3Multi, TrueHd, DtsCore, DtsHd, DtsHdCore, DtsLbr, Wma, WmaPro, WmaLsl
};

struct TableInfo {
    std::string_view library, variable, tag; // tag: the internal functions' names end with it
    u32 nid;                                 // the variable's (RPCS3's REG_VNID; LPCM's is nid_of of its name)
    s32 type;                                // the cellAdec type of a core on FFmpeg; 0 for LPCM and ATRAC3plus
    u32 channels;                            // ATRAC3plus: the channels its memory is for
    u32 context_size;                        // the core's context at its handle
};

// In Table's order.
constexpr TableInfo kTables[] = {
    {"cellAdec", "g_cell_adec_core_ops_lpcm", "lpcm", 0x369D9D3A, 0, 0, 0x1C8},
    {"cellAtracXdec", "g_cell_adec_core_ops_atracx", "atracx", 0x4944AF9A, 0, 8, 0x268},
    {"cellAtracXdec", "g_cell_adec_core_ops_atracx2ch", "atracx2ch", 0x076B33AB, 0, 2, 0x268},
    {"cellAtracXdec", "g_cell_adec_core_ops_atracx6ch", "atracx6ch", 0x1D210EAA, 0, 6, 0x268},
    {"cellAtracXdec", "g_cell_adec_core_ops_atracx8ch", "atracx8ch", 0xE9A86E54, 0, 8, 0x268},
    {"cellMP3dec", "g_cell_adec_core_ops_Mp3", "Mp3", 0x84276A23, CELL_ADEC_TYPE_MP3, 0, ffcore::fx::size},
    {"cellMPL1dec", "g_cell_adec_core_ops_mpmcl1", "mpmcl1", 0xBBC70551, CELL_ADEC_TYPE_MPEG_L1, 0, ffcore::fx::size},
    {"cellM2BCdec", "g_cell_adec_core_ops_mpmc", "mpmc", 0x300AFE4D, CELL_ADEC_TYPE_MPEG_L2, 0, ffcore::fx::size},
    {"cellM4AacDec", "g_cell_adec_core_ops_M4Aac", "M4Aac", 0xAB61278D, CELL_ADEC_TYPE_M4AAC, 0, ffcore::fx::size},
    {"cellM4AacDec2ch", "g_cell_adec_core_ops_M4Aac2ch", "M4Aac2ch", 0xE996C664, CELL_ADEC_TYPE_M4AAC_2CH, 0,
        ffcore::fx::size},
    {"cellM4AacDec2chmod", "g_cell_adec_core_ops_M4Aac2chmod", "M4Aac2chmod", 0xDBD26836, CELL_ADEC_TYPE_M4AAC_2CH_MOD,
        0, ffcore::fx::size},
    {"cell_libac3dec", "g_cell_adec_core_ops_ac3", "ac3", 0xC58BB170, CELL_ADEC_TYPE_AC3, 0, ffcore::fx::size},
    {"cellAtrac3dec", "g_cell_adec_core_ops_atrac3", "atrac3", 0x60487D65, CELL_ADEC_TYPE_ATRAC3, 0, ffcore::fx::size},
    {"cellDDPdec", "g_cell_adec_core_ops_Ddp", "Ddp", 0xF5E9C15C, CELL_ADEC_TYPE_EAC3, 0, ffcore::fx::size},
    {"cellM2AACdec", "g_cell_adec_core_ops_Aac", "Aac", 0xAC2F0831, CELL_ADEC_TYPE_M2AAC, 0, ffcore::fx::size},
    {"cellMP3Sdec", "g_cell_adec_core_ops_Mp3s", "Mp3s", 0x292CDF0A, CELL_ADEC_TYPE_MP3S, 0, ffcore::fx::size},
    {"cellAtrac3multidec", "g_cell_adec_core_ops_atrac3multi", "atrac3multi", 0xC20C6BD7, CELL_ADEC_TYPE_ATRAC3MULTI, 0,
        ffcore::fx::size},
    {"cellTRHDdec", "g_cell_adec_core_ops_truehd", "truehd", 0xE88E381B, CELL_ADEC_TYPE_TRUEHD, 0, ffcore::fx::size},
    {"cellDTSdec", "g_cell_adec_core_ops_DtsCore", "DtsCore", 0x15248EC5, CELL_ADEC_TYPE_DTS, 0, ffcore::fx::size},
    {"cellDTSHDdec", "g_cell_adec_core_ops_DtsHd", "DtsHd", 0x51AC2B6C, CELL_ADEC_TYPE_DTSHD, 0, ffcore::fx::size},
    {"cellDTSHDCOREdec", "g_cell_adec_core_ops_DtsHd_Core", "DtsHd_Core", 0x6C8F4F1C, CELL_ADEC_TYPE_DTSHDCORE, 0,
        ffcore::fx::size},
    {"cellDTSLBRdec", "g_cell_adec_core_ops_DtsLbr", "DtsLbr", 0x8C50AF52, CELL_ADEC_TYPE_DTSLBR, 0, ffcore::fx::size},
    {"cellWMAdec", "g_cell_adec_core_ops_wma", "wma", 0x88320B10, CELL_ADEC_TYPE_WMA, 0, ffcore::fx::size},
    {"cellWMAPROdec", "g_cell_adec_core_ops_WmaPro", "WmaPro", 0xA8BAC670, CELL_ADEC_TYPE_WMAPRO_CORE, 0,
        ffcore::fx::size},
    {"cellWMALSLdec", "g_cell_adec_core_ops_WmaLsl", "WmaLsl", 0x602AAB16, CELL_ADEC_TYPE_WMALSL_CORE, 0,
        ffcore::fx::size},
};

constexpr const TableInfo& info_of(Table t) { return kTables[static_cast<usize>(t)]; }

// CellAdecCoreOps's entries, in order (RPCS3's _CellAdecCoreOp<name>_<core>).
constexpr u32 kOps = 12;
constexpr std::string_view kOpNames[kOps] = {"GetMemSize", "Open", "Close", "StartSeq", "EndSeq", "DecodeAu",
    "GetVersion", "Realign", "ReleasePcm", "GetPcmHandleNum", "GetBsiInfoSize", "OpenExt"};

// The internal function behind entry `op` of table `t`: _CellAdecCoreOp<op>_<tag>, pspocket's own NID nid_of of it.
std::string_view op_name(Table t, u32 op)
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const TableInfo& i : kTables)
            for (const std::string_view n : kOpNames) v.push_back(std::format("_CellAdecCoreOp{}_{}", n, i.tag));
        return v;
    }();
    return names[static_cast<usize>(t) * kOps + op];
}

// The core behind a table (pspocket's own; a table whose core this build lacks is not registered).
std::unique_ptr<AdecCore> core_of_table(Table t)
{
    const TableInfo& i = info_of(t);
    std::unique_ptr<AdecCore> c = t == Table::Lpcm ? make_lpcm_core()
        : i.channels                               ? make_atracx_core(i.channels)
                                                   : make_ffmpeg_core(i.type);
    if (!c) PS_FATAL(kChan, "{}: no core behind it in this build", i.variable);
    return c;
}

bool usable(PPUContext& ctx, Table t, u32 addr, u32 size, std::string_view what)
{
    if (addr && guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{}: {} ({:#x}, {} bytes) is not guest memory", info_of(t).variable,
        what, addr, size));
    return false;
}

bool core_usable(PPUContext& ctx, Table t, u32 handle)
{
    return usable(ctx, t, handle, info_of(t).context_size, "the core");
}

// ---- the operations (CellAdecCoreOps, RPCS3 cellAdec.h)

template <Table T>
s32 op_get_mem_size(PPUContext& ctx, u32 attr) // CellAdecAttr: the core writes workMemSize
{
    if (!usable(ctx, T, attr, 4, "the attributes")) return error(CELL_ADEC_ERROR_ARG);
    const std::unique_ptr<AdecCore> core = core_of_table(T);
    ps_write32(guest_base(ctx), attr, core->mem_size(ctx));
    // libm2bcdec's and libm4aacdec's also write their version (D-944).
    if (const u32 v = core->version_in_attr(ctx); v && usable(ctx, T, attr, 12, "the attributes")) ps_write32(guest_base(ctx), attr + 8, v);
    return lv2::CELL_OK;
}

template <Table T>
s32 open_with(PPUContext& ctx, u32 handle, const CoreNotify& n, u32 res, u32 spurs_res)
{
    if (!core_usable(ctx, T, handle) || !usable(ctx, T, res, 20, "the resource")) return error(CELL_ADEC_ERROR_ARG);
    for (u32 i = 0; i < 4; i++) {
        if (n.fn[i]) continue;
        log_call_site(ctx, guest_base(ctx),
            std::format("{}: notification {} without a function", info_of(T).variable, i));
        return error(CELL_ADEC_ERROR_ARG);
    }
    return core_of_table(T)->open(ctx, handle, n, res, spurs_res);
}

template <Table T>
s32 op_open(PPUContext& ctx, u32 handle, u32 au_done, u32 au_done_arg, u32 pcm_out, u32 pcm_out_arg, u32 err,
    u32 err_arg, u32 seq_done, u32 seq_done_arg, u32 res)
{
    const CoreNotify n{{au_done, pcm_out, err, seq_done}, {au_done_arg, pcm_out_arg, err_arg, seq_done_arg}, false};
    return open_with<T>(ctx, handle, n, res, 0);
}

template <Table T>
s32 op_open_ext(PPUContext& ctx, u32 handle, u32 au_done, u32 au_done_arg, u32 pcm_out, u32 pcm_out_arg, u32 err,
    u32 err_arg, u32 seq_done, u32 seq_done_arg, u32 res, u32 spurs_res)
{
    const CoreNotify n{{au_done, pcm_out, err, seq_done}, {au_done_arg, pcm_out_arg, err_arg, seq_done_arg}, false};
    return open_with<T>(ctx, handle, n, res, spurs_res);
}

template <Table T>
s32 op_close(PPUContext& ctx, u32 handle)
{
    if (!core_usable(ctx, T, handle)) return error(CELL_ADEC_ERROR_ARG);
    return core_of_table(T)->close(ctx, handle);
}

template <Table T>
s32 op_start_seq(PPUContext& ctx, u32 handle, u32 param)
{
    // LPCM reads its four words here; the other cores check their parameters' size themselves.
    if (!core_usable(ctx, T, handle) || !usable(ctx, T, param, T == Table::Lpcm ? 16 : 4, "the parameters"))
        return error(CELL_ADEC_ERROR_ARG);
    return core_of_table(T)->start_seq(ctx, handle, param);
}

template <Table T>
s32 op_end_seq(PPUContext& ctx, u32 handle)
{
    if (!core_usable(ctx, T, handle)) return error(CELL_ADEC_ERROR_ARG);
    return core_of_table(T)->end_seq(ctx, handle);
}

template <Table T>
s32 op_decode_au(PPUContext& ctx, u32 handle, s32 pcm_handle, u32 au_info) // CellAdecAuInfo
{
    if (!core_usable(ctx, T, handle) || !usable(ctx, T, au_info, 0x18, "the access unit's information"))
        return error(CELL_ADEC_ERROR_ARG);
    return core_of_table(T)->decode_au(ctx, handle, pcm_handle, au_info);
}

template <Table T>
void op_get_version(PPUContext& ctx, u32 version) // void, as RPCS3's CellAdecCoreOpGetVersion
{
    if (usable(ctx, T, version, 4, "the version")) ps_write32(guest_base(ctx), version, core_of_table(T)->version(ctx));
}

template <Table T>
s32 op_realign(PPUContext& ctx, u32 handle, u32 out_buffer, u32 pcm_start)
{
    if (!core_usable(ctx, T, handle)) return error(CELL_ADEC_ERROR_ARG);
    return core_of_table(T)->realign(ctx, handle, out_buffer, pcm_start);
}

template <Table T>
s32 op_release_pcm(PPUContext& ctx, u32 handle, s32 pcm_handle, u32 out_buffer)
{
    if (!core_usable(ctx, T, handle)) return error(CELL_ADEC_ERROR_ARG);
    return core_of_table(T)->release_pcm(ctx, handle, pcm_handle, out_buffer);
}

template <Table T>
s32 op_get_pcm_handle_num(PPUContext& ctx)
{
    return core_of_table(T)->pcm_handle_num(ctx);
}

template <Table T>
u32 op_get_bsi_info_size(PPUContext& ctx)
{
    return core_of_table(T)->bsi_info_size(ctx);
}

// The table, laid out when the variable is first linked: each entry the descriptor of its internal function.
template <Table T>
void init_table(u8* base, u32 at)
{
    HleProcess& p = *HleProcess::current();
    for (u32 op = 0; op < kOps; op++) {
        const std::string_view name = op_name(T, op);
        ps_write32(base, at + 4 * op, p.link_function("cellAdec", recomp::nid_of(name), name));
    }
}

template <Table T>
void add_table(Registry& r)
{
    const HleFn fns[kOps] = {&detail::thunk<&op_get_mem_size<T>>, &detail::thunk<&op_open<T>>,
        &detail::thunk<&op_close<T>>, &detail::thunk<&op_start_seq<T>>, &detail::thunk<&op_end_seq<T>>,
        &detail::thunk<&op_decode_au<T>>, &detail::thunk<&op_get_version<T>>, &detail::thunk<&op_realign<T>>,
        &detail::thunk<&op_release_pcm<T>>, &detail::thunk<&op_get_pcm_handle_num<T>>,
        &detail::thunk<&op_get_bsi_info_size<T>>, &detail::thunk<&op_open_ext<T>>};
    for (u32 op = 0; op < kOps; op++) {
        const std::string_view name = op_name(T, op);
        r.add("cellAdec", {Function{{}, name, recomp::nid_of(name), fns[op], true}});
    }
    const TableInfo& i = info_of(T);
    r.add_variables(i.library, {Variable{{}, i.variable, i.nid, kOps * 4, 4, &init_table<T>}});
}

#if PSPOCKET_HAVE_FFMPEG

// ---- the PTS functions: _SceAdecCorrectPtsValue_<codec>(coreHandle, CellCodecTimeStamp* pts) (RPCS3 cellAdec.cpp,
// AdecContext::correct_pts_value). pspocket's core of the codec corrects it as for its own decoders (AdecCore::
// correct_pts, D-577): its own last PTS and the length of that frame. CELL_OK, or an error the front end makes FATAL.
template <Table T>
s32 correct_pts(PPUContext& ctx, u32 core_handle, u32 au_pts)
{
    if (!core_usable(ctx, T, core_handle) || !usable(ctx, T, au_pts, 8, "the timestamp"))
        return error(CELL_ADEC_ERROR_FATAL);
    const s32 type = static_cast<s32>(ps_read32(guest_base(ctx), core_handle + ffcore::fx::type));
    if (type != info_of(T).type) {
        PS_LOG_ERROR(kChan, "{}'s PTS correction on core {:#x}, a core of codec type {}", info_of(T).library,
            core_handle, type);
        return error(CELL_ADEC_ERROR_FATAL);
    }
    return core_of_table(T)->correct_pts(ctx, core_handle, au_pts);
}

s32 _SceAdecCorrectPtsValue_mpmcl1(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::MpegL1>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_mpmc(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::MpegL2>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_M4Aac(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::M4Aac>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_M4Aac2ch(PPUContext& ctx, u32 h, u32 pts)
{
    return correct_pts<Table::M4Aac2ch>(ctx, h, pts);
}
s32 _SceAdecCorrectPtsValue_M4Aac2chmod(PPUContext& ctx, u32 h, u32 pts)
{
    return correct_pts<Table::M4Aac2chMod>(ctx, h, pts);
}
// D-623: the PTS functions of the cores of D-621 and D-622 (ATRAC3 multichannel has none: libadec steps its PTS).
s32 _SceAdecCorrectPtsValue_Ddp(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::Ddp>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_Aac(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::M2Aac>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_Mp3s(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::Mp3s>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_truehd(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::TrueHd>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_DtsCore(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::DtsCore>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_DtsHd(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::DtsHd>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_DtsHd_Core(PPUContext& ctx, u32 h, u32 pts)
{
    return correct_pts<Table::DtsHdCore>(ctx, h, pts);
}
s32 _SceAdecCorrectPtsValue_DtsLbr(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::DtsLbr>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_wma(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::Wma>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_WmaPro(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::WmaPro>(ctx, h, pts); }
s32 _SceAdecCorrectPtsValue_WmaLsl(PPUContext& ctx, u32 h, u32 pts) { return correct_pts<Table::WmaLsl>(ctx, h, pts); }

#endif

} // namespace

void register_adec_tables(Registry& r)
{
    add_table<Table::Lpcm>(r);
#if PSPOCKET_HAVE_FFMPEG
    add_table<Table::AtracX>(r);
    add_table<Table::AtracX2ch>(r);
    add_table<Table::AtracX6ch>(r);
    add_table<Table::AtracX8ch>(r);
    add_table<Table::Mp3>(r);
    add_table<Table::MpegL1>(r);
    add_table<Table::MpegL2>(r);
    add_table<Table::M4Aac>(r);
    add_table<Table::M4Aac2ch>(r);
    add_table<Table::M4Aac2chMod>(r);
    add_table<Table::Ac3>(r);
    add_table<Table::Atrac3>(r);
    r.add("cellMPL1dec", {PS_HLE_FUNC(0xA4A9A309, _SceAdecCorrectPtsValue_mpmcl1)});
    r.add("cellM2BCdec", {PS_HLE_FUNC(0xEC7DE355, _SceAdecCorrectPtsValue_mpmc)});
    r.add("cellM4AacDec", {PS_HLE_FUNC(0x431A1DCA, _SceAdecCorrectPtsValue_M4Aac)});
    r.add("cellM4AacDec2ch", {PS_HLE_FUNC(0x869E328D, _SceAdecCorrectPtsValue_M4Aac2ch)});
    r.add("cellM4AacDec2chmod", {PS_HLE_FUNC(0x156FE2F0, _SceAdecCorrectPtsValue_M4Aac2chmod)});
    // D-623: the cores of D-621 and D-622.
    add_table<Table::Ddp>(r);
    add_table<Table::M2Aac>(r);
    add_table<Table::Mp3s>(r);
    add_table<Table::Atrac3Multi>(r);
    add_table<Table::TrueHd>(r);
    add_table<Table::DtsCore>(r);
    add_table<Table::DtsHd>(r);
    add_table<Table::DtsHdCore>(r);
    add_table<Table::DtsLbr>(r);
    add_table<Table::Wma>(r);
    add_table<Table::WmaPro>(r);
    add_table<Table::WmaLsl>(r);
    r.add("cellDDPdec", {PS_HLE_FUNC(0xEE2731D3, _SceAdecCorrectPtsValue_Ddp)});
    r.add("cellM2AACdec", {PS_HLE_FUNC(0x7CBA9865, _SceAdecCorrectPtsValue_Aac)});
    r.add("cellMP3Sdec", {PS_HLE_FUNC(0x73A2C895, _SceAdecCorrectPtsValue_Mp3s)});
    r.add("cellTRHDdec", {PS_HLE_FUNC(0x75080F7B, _SceAdecCorrectPtsValue_truehd)});
    r.add("cellDTSdec", {PS_HLE_FUNC(0x8C7AD5ED, _SceAdecCorrectPtsValue_DtsCore)});
    r.add("cellDTSHDdec", {PS_HLE_FUNC(0xF3AF1093, _SceAdecCorrectPtsValue_DtsHd)});
    r.add("cellDTSHDCOREdec", {PS_HLE_FUNC(0xD4C180D4, _SceAdecCorrectPtsValue_DtsHd_Core)});
    r.add("cellDTSLBRdec", {PS_HLE_FUNC(0x185591D6, _SceAdecCorrectPtsValue_DtsLbr)});
    r.add("cellWMAdec", {PS_HLE_FUNC(0x46E6C806, _SceAdecCorrectPtsValue_wma)});
    r.add("cellWMAPROdec", {PS_HLE_FUNC(0x8D45F21A, _SceAdecCorrectPtsValue_WmaPro)});
    r.add("cellWMALSLdec", {PS_HLE_FUNC(0xF2F9C183, _SceAdecCorrectPtsValue_WmaLsl)});
    // The cores on FFmpeg are not compared with Sony's yet (D-577 to D-579, D-621, D-622): Sony's module serves first
    // (D-563).
    for (const std::string_view library : {"cellMP3dec", "cellMPL1dec", "cellM2BCdec", "cellM4AacDec",
             "cellM4AacDec2ch", "cellM4AacDec2chmod", "cell_libac3dec", "cellAtrac3dec", "cellDDPdec", "cellM2AACdec",
             "cellMP3Sdec", "cellAtrac3multidec", "cellTRHDdec", "cellDTSdec", "cellDTSHDdec", "cellDTSHDCOREdec",
             "cellDTSLBRdec", "cellWMAdec", "cellWMAPROdec", "cellWMALSLdec"})
        r.firmware_first(library);
#endif
}

} // namespace pspocket::hle
