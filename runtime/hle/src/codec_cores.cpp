// The video codec cores' version queries (D-575): libavcdec (H.264), libmvcdec (MVC), libsjvtd, libsmvd2, libsmvd4
// and libsvc1d. These modules are the decoders Sony's libvdec loads; pspocket's libvdec decodes by itself and never
// calls them, and games do not import them. What RPCS3 knows of each is its version and, for the AVC and MVC cores, two
// numbers of their characteristics; their decoding calls need Sony's modules, so each library is firmware-first
// (D-563): a module that imports one gets Sony's where the system software is installed.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/libavcdec.cpp, libmvcdec.cpp, libsjvtd.cpp, libsmvd2.cpp, libsmvd4.cpp,
// libsvc1d.cpp and their headers (the versions, the characteristics, the ARG error codes). A pointer that is not guest
// memory gets the ARG error here, where RPCS3 checks only for null.

#include <string_view>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/hle/hle.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

enum : u32 {
    AVC_DEC_ERROR_ARG = 0x80615201,
    MVC_DEC_ERROR_ARG = 0x80615901,
    SJVTD_ERROR_ARG = 0x80615841,
    SMVD2_ERROR_ARG = 0x80615041,
    SMVD4_ERROR_ARG = 0x80615141,
    SVC1D_ERROR_ARG = 0x80615341,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

bool writable(PPUContext& ctx, u32 addr, u32 size) { return addr && guest_readable(thread_of(ctx), addr, size); }

// void avcDecGetVersion(uint32_t *version) and mvcDecGetVersion: nothing written without a version pointer; no answer.
void avcDecGetVersion(PPUContext& ctx, u32 version)
{
    if (writable(ctx, version, 4)) ps_write32(guest_base(ctx), version, 0x11300);
}

void mvcDecGetVersion(PPUContext& ctx, u32 version)
{
    if (writable(ctx, version, 4)) ps_write32(guest_base(ctx), version, 0x10804);
}

// QueryCharacteristics(uint32_t out[2]): the command queue's depth (5) and a second number RPCS3 has without its name.
s32 characteristics(PPUContext& ctx, u32 out, u32 second, u32 arg_error)
{
    if (!writable(ctx, out, 8)) return error(arg_error);
    ps_write32(guest_base(ctx), out, 5);
    ps_write32(guest_base(ctx), out + 4, second);
    return lv2::CELL_OK;
}

s32 avcDecQueryCharacteristics(PPUContext& ctx, u32 out) { return characteristics(ctx, out, 20, AVC_DEC_ERROR_ARG); }
s32 mvcDecQueryCharacteristics(PPUContext& ctx, u32 out) { return characteristics(ctx, out, 36, MVC_DEC_ERROR_ARG); }

// GetVersionNumber(uint32_t *version) of the MPEG-2, MPEG-4 and VC-1 cores.
s32 version_number(PPUContext& ctx, u32 version, u32 value, u32 arg_error)
{
    if (!writable(ctx, version, 4)) return error(arg_error);
    ps_write32(guest_base(ctx), version, value);
    return lv2::CELL_OK;
}

s32 sjvtdGetVersionNumber(PPUContext& ctx, u32 v) { return version_number(ctx, v, 0x1050000, SJVTD_ERROR_ARG); }
s32 smvd2GetVersionNumber(PPUContext& ctx, u32 v) { return version_number(ctx, v, 0x1030000, SMVD2_ERROR_ARG); }
s32 smvd4GetVersionNumber(PPUContext& ctx, u32 v) { return version_number(ctx, v, 0x1080000, SMVD4_ERROR_ARG); }
s32 svc1dGetVersionNumber(PPUContext& ctx, u32 v) { return version_number(ctx, v, 0x1090000, SVC1D_ERROR_ARG); }

} // namespace

void register_codec_cores(Registry& r)
{
    r.add("libavcdec", {
        PS_HLE_FUNC(0x5267175E, avcDecGetVersion),
        PS_HLE_FUNC(0x00E2D84B, avcDecQueryCharacteristics),
    });
    r.add("libmvcdec", {
        PS_HLE_FUNC(0x9A0E3C6E, mvcDecGetVersion),
        PS_HLE_FUNC(0xEC29CAB1, mvcDecQueryCharacteristics),
    });
    r.add("libsjvtd", {PS_HLE_FUNC(0x50F8A377, sjvtdGetVersionNumber)});
    r.add("libsmvd2", {PS_HLE_FUNC(0x52BB9416, smvd2GetVersionNumber)});
    r.add("libsmvd4", {PS_HLE_FUNC(0xC1D6771B, smvd4GetVersionNumber)});
    r.add("libsvc1d", {PS_HLE_FUNC(0x6C366C43, svc1dGetVersionNumber)});
    for (const std::string_view library : {"libavcdec", "libmvcdec", "libsjvtd", "libsmvd2", "libsmvd4", "libsvc1d"})
        r.firmware_first(library);
}

} // namespace pspocket::hle
