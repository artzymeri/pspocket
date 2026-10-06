// cellResc: the resolution scaler, on the host (D-180).
//
// On a PS3, libresc takes the frame a game rendered (a "source" colour buffer, cellRescSetSrc) and, at
// cellRescSetConvertAndFlip, has the RSX draw it into colour buffers of its own at the TV's resolution and mode (PAL
// 50/60 conversion, interlacing, letterbox) and flips those. Here the display is the iPhone's screen and Metal shows
// any display buffer scaled to it, so a source is flipped as it is: SetConvertAndFlip makes the source a display buffer
// (cellGcmSetDisplayBuffer, its index as the buffer id) and puts cellGcmSetFlip's command into the game's command
// buffer. No conversion pass runs and the colour buffers the game gives libresc stay unused; GetBufferSize still gives
// the sizes libresc gives, so the game's memory is laid out as on a PS3.
//
// References: RPCS3 rpcs3/Emu/Cell/Modules/cellResc.cpp/.h, its argument checks (RPCS3 runs Sony's libresc by default:
// its own functions check arguments and do nothing). Where RPCS3 says nothing, the firmware's libresc.sprx, read for
// interoperability (D-176): cellRescGetNumColorBuffers, cellRescVideoOutResolutionId2RescBufferMode,
// cellRescGcmSurface2RescSrc, and the vertex and shader sizes of cellRescGetBufferSize. Sniper: Ghost Warrior shows its
// frames only through it (phone run of 2026-10-06).

#include <algorithm>
#include <array>
#include <format>
#include <mutex>
#include <string_view>

#include "gcm_api.hpp"
#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellResc";

enum : u32 {
    CELL_RESC_ERROR_NOT_INITIALIZED = 0x80210301,
    CELL_RESC_ERROR_REINITIALIZED = 0x80210302,
    CELL_RESC_ERROR_BAD_ALIGNMENT = 0x80210303,
    CELL_RESC_ERROR_BAD_ARGUMENT = 0x80210304,
    CELL_RESC_ERROR_GCM_FLIP_QUE_FULL = 0x80210306,
    CELL_RESC_ERROR_BAD_COMBINATION = 0x80210307,
};
enum : u32 { CELL_RESC_720x480 = 0x1, CELL_RESC_720x576 = 0x2, CELL_RESC_1280x720 = 0x4, CELL_RESC_1920x1080 = 0x8 };
enum : u32 {
    CELL_RESC_PAL_60_DROP = 1,
    CELL_RESC_PAL_60_INTERPOLATE = 2,
    CELL_RESC_PAL_60_FOR_HSYNC = 5,
    CELL_RESC_DISPLAY_VSYNC = 0,
    CELL_RESC_DISPLAY_HSYNC = 1,
    CELL_RESC_MINIMUM_VRAM = 0x1,
    CELL_RESC_ELEMENT_FLOAT = 1,
};
constexpr u32 kSrcBufferNum = 8;
constexpr u32 kVertexArraySize = 384;     // libresc's vertices: one quad and its texture coordinates
constexpr u32 kFragmentShaderSize = 0x300; // libresc's largest conversion shader

constexpr s32 error(u32 code) { return static_cast<s32>(code); }

// The destination modes, by bit index: their size.
constexpr std::array<std::array<u32, 2>, 4> kModeSize = {{{720, 480}, {720, 576}, {1280, 720}, {1920, 1080}}};

struct Dsts {
    u32 format = 0, pitch = 0, height_align = 0;
};
struct Src {
    u32 format = 0, pitch = 0, width = 0, height = 0, offset = 0;
    bool operator==(const Src&) const = default;
};

struct RescState {
    std::mutex mutex;
    bool initialized = false;
    struct {
        u32 size = 0, resource_policy = 0, support_modes = 0, ratio_mode = 0, pal_temporal_mode = 0, interlace_mode = 0,
            flip_mode = 0;
    } config;
    u32 buffer_mode = 0;
    std::array<Dsts, 4> dsts{};
    std::array<Src, kSrcBufferNum> srcs{};
    std::array<bool, kSrcBufferNum> src_set{};
    std::array<Src, kSrcBufferNum> shown{}; // what display buffer id i was made from
    std::array<bool, kSrcBufferNum> shown_set{};
    f32 aspect_h = 1.0f, aspect_v = 1.0f, flex_ratio = 0.5f;
    s32 register_count = 2;
    u32 color_buffers = 0, vertex_array = 0, fragment_shader = 0;

    explicit RescState(HleProcess&) {}
};

RescState& state()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellResc called without an HLE process");
    return p->state<RescState>();
}

// The index of a single mode bit (CELL_RESC_720x480 0 .. CELL_RESC_1920x1080 3), or -1.
int mode_index(u32 mode)
{
    switch (mode) {
    case CELL_RESC_720x480: return 0;
    case CELL_RESC_720x576: return 1;
    case CELL_RESC_1280x720: return 2;
    case CELL_RESC_1920x1080: return 3;
    default: return -1;
    }
}

// libresc's cellRescGetNumColorBuffers: PAL 60 from 50 interpolates through six buffers, drops through three.
s32 num_color_buffers(u32 dst_mode, u32 pal_temporal_mode)
{
    if (dst_mode != CELL_RESC_720x576) return 2;
    if (pal_temporal_mode - CELL_RESC_PAL_60_INTERPOLATE <= 2) return 6;
    if (pal_temporal_mode == CELL_RESC_PAL_60_DROP) return 3;
    return 2;
}

// One destination mode's colour buffers: its pitch (cellRescSetDsts) times its height aligned as asked, per buffer.
u32 mode_buffers_size(const RescState& s, int index)
{
    const Dsts& d = s.dsts[static_cast<usize>(index)];
    const u32 align = std::max(d.height_align, 1u);
    const u32 height = (kModeSize[static_cast<usize>(index)][1] + align - 1) / align * align;
    return d.pitch * height * static_cast<u32>(num_color_buffers(1u << index, s.config.pal_temporal_mode));
}

bool readable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{}: {:#x} is not guest memory", what, addr));
    return false;
}

// cellRescInit(initConfig): RPCS3's checks (a configuration of at most 28 bytes); its fields kept.
s32 cellRescInit(PPUContext& ctx, u32 config)
{
    RescState& s = state();
    std::lock_guard lock(s.mutex);
    if (s.initialized) return error(CELL_RESC_ERROR_REINITIALIZED);
    if (!config || !readable(ctx, config, 28, "cellRescInit")) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    u8* base = guest_base(ctx);
    if (ps_read32(base, config) > 28) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    s.config = {ps_read32(base, config), ps_read32(base, config + 4), ps_read32(base, config + 8),
        ps_read32(base, config + 12), ps_read32(base, config + 16), ps_read32(base, config + 20),
        ps_read32(base, config + 24)};
    s.initialized = true;
    PS_LOG_INFO(kChan, "cellRescInit(size {}, resource policy {:#x}, modes {:#x}, ratio {}, PAL {}, interlace {}, flip {}): "
                       "frames go to the screen as the game draws them (Metal scales them)",
        s.config.size, s.config.resource_policy, s.config.support_modes, s.config.ratio_mode, s.config.pal_temporal_mode,
        s.config.interlace_mode, s.config.flip_mode);
    return lv2::CELL_OK;
}

void cellRescExit(PPUContext&)
{
    RescState& s = state();
    std::lock_guard lock(s.mutex);
    PS_LOG_INFO(kChan, "cellRescExit()");
    s.initialized = false;
    s.src_set = {};
    s.shown_set = {};
}

// libresc: 1080 -> 1920x1080, 720 -> 1280x720, 480 -> 720x480, 576 -> 720x576; nothing else.
s32 cellRescVideoOutResolutionId2RescBufferMode(PPUContext& ctx, u32 resolution_id, u32 buffer_mode)
{
    if (!buffer_mode) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    u32 mode = 0;
    switch (resolution_id) {
    case 1: mode = CELL_RESC_1920x1080; break;
    case 2: mode = CELL_RESC_1280x720; break;
    case 4: mode = CELL_RESC_720x480; break;
    case 5: mode = CELL_RESC_720x576; break;
    default: return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    }
    if (!readable(ctx, buffer_mode, 4, "cellRescVideoOutResolutionId2RescBufferMode")) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    ps_write32(guest_base(ctx), buffer_mode, mode);
    return lv2::CELL_OK;
}

s32 cellRescSetDsts(PPUContext& ctx, u32 buffer_mode, u32 dsts)
{
    RescState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_RESC_ERROR_NOT_INITIALIZED);
    if (!dsts || !buffer_mode || buffer_mode > CELL_RESC_1920x1080) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    if (!readable(ctx, dsts, 12, "cellRescSetDsts")) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    u8* base = guest_base(ctx);
    const Dsts d{ps_read32(base, dsts), ps_read32(base, dsts + 4), ps_read32(base, dsts + 8)};
    for (int i = 0; i < 4; i++)
        if (buffer_mode & (1u << i)) s.dsts[static_cast<usize>(i)] = d;
    PS_LOG_INFO(kChan, "cellRescSetDsts(mode {:#x}: format {}, pitch {:#x}, height align {})", buffer_mode, d.format,
        d.pitch, d.height_align);
    return lv2::CELL_OK;
}

// cellRescSetDisplayMode(bufferMode): RPCS3's checks, including the PAL modes that need a particular flip mode.
s32 cellRescSetDisplayMode(PPUContext&, u32 buffer_mode)
{
    RescState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_RESC_ERROR_NOT_INITIALIZED);
    if (!buffer_mode || buffer_mode > CELL_RESC_1920x1080 || !(s.config.support_modes & buffer_mode))
        return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    if (buffer_mode == CELL_RESC_720x576) {
        const u32 pal = s.config.pal_temporal_mode, flip = s.config.flip_mode;
        if ((pal - CELL_RESC_PAL_60_INTERPOLATE <= CELL_RESC_PAL_60_INTERPOLATE || pal == CELL_RESC_PAL_60_DROP) &&
            flip == CELL_RESC_DISPLAY_HSYNC)
            return error(CELL_RESC_ERROR_BAD_COMBINATION);
        if (pal == CELL_RESC_PAL_60_FOR_HSYNC && flip == CELL_RESC_DISPLAY_VSYNC)
            return error(CELL_RESC_ERROR_BAD_COMBINATION);
    }
    s.buffer_mode = buffer_mode;
    PS_LOG_INFO(kChan, "cellRescSetDisplayMode({:#x})", buffer_mode);
    return lv2::CELL_OK;
}

// cellRescAdjustAspectRatio(horizontal, vertical): floats in f1 and f2 (RPCS3's binder). Kept; the screen shows the
// source whole.
s32 cellRescAdjustAspectRatio(PPUContext& ctx)
{
    const f32 h = static_cast<f32>(ctx.fpr[1]), v = static_cast<f32>(ctx.fpr[2]);
    RescState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_RESC_ERROR_NOT_INITIALIZED);
    if (h < 0.5f || h > 2.0f || v < 0.5f || v > 2.0f) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    s.aspect_h = h;
    s.aspect_v = v;
    return lv2::CELL_OK;
}

s32 cellRescSetPalInterpolateDropFlexRatio(PPUContext& ctx)
{
    const f32 ratio = static_cast<f32>(ctx.fpr[1]);
    RescState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_RESC_ERROR_NOT_INITIALIZED);
    if (ratio < 0.0f || ratio > 1.0f) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    s.flex_ratio = ratio;
    return lv2::CELL_OK;
}

// cellRescGetBufferSize(colorBuffers, vertexArray, fragmentShader): what the game must allocate for libresc. Colour
// buffers: the display mode's (MINIMUM_VRAM) or the largest of the supported modes' (CONSTANT_VRAM), each mode's
// pitch (cellRescSetDsts) times its aligned height times its buffer count; libresc's 384 bytes of vertices and its
// largest shader. Only the given pointers are written (libresc).
s32 cellRescGetBufferSize(PPUContext& ctx, u32 color_buffers, u32 vertex_array, u32 fragment_shader)
{
    RescState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_RESC_ERROR_NOT_INITIALIZED);
    u32 colors = 0;
    if (s.config.resource_policy & CELL_RESC_MINIMUM_VRAM) {
        if (const int i = mode_index(s.buffer_mode); i >= 0) colors = mode_buffers_size(s, i);
    } else {
        for (int i = 0; i < 4; i++)
            if (s.config.support_modes & (1u << i)) colors = std::max(colors, mode_buffers_size(s, i));
    }
    u8* base = guest_base(ctx);
    for (const auto& [ptr, value] : {std::pair{color_buffers, colors}, std::pair{vertex_array, kVertexArraySize},
             std::pair{fragment_shader, kFragmentShaderSize}}) {
        if (!ptr) continue;
        if (!readable(ctx, ptr, 4, "cellRescGetBufferSize")) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
        ps_write32(base, ptr, value);
    }
    PS_LOG_INFO(kChan, "cellRescGetBufferSize: colour buffers {:#x}, vertices {:#x}, shader {:#x}", colors,
        kVertexArraySize, kFragmentShaderSize);
    return lv2::CELL_OK;
}

s32 cellRescGetNumColorBuffers(PPUContext&, u32 dst_mode, u32 pal_temporal_mode, s32 reserved)
{
    if (reserved != 0) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    return num_color_buffers(dst_mode, pal_temporal_mode);
}

// cellRescGcmSurface2RescSrc(gcmSurface, rescSrc), as libresc: the surface's first colour target as a source texture
// (A8R8G8B8 0x85, W16Z16Y16X16 float 0x9A, linear for a pitch surface, 0xFF for another format); with a 24- or
// 28-byte configuration the multisampled width and height (2x horizontally for DIAGONAL_CENTERED_2, 2x both ways for the
// SQUARE 4x modes).
s32 cellRescGcmSurface2RescSrc(PPUContext& ctx, u32 surface, u32 src)
{
    if (!surface || !src) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    if (!readable(ctx, surface, 60, "cellRescGcmSurface2RescSrc") || !readable(ctx, src, 16, "cellRescGcmSurface2RescSrc"))
        return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    u8* base = guest_base(ctx);
    const u8 type = ps_read8(base, surface), antialias = ps_read8(base, surface + 1);
    const u8 color_format = ps_read8(base, surface + 2);
    u8 format = 0xFF;
    if (color_format == 8 || color_format == 11) { // CELL_GCM_SURFACE_A8R8G8B8, CELL_GCM_SURFACE_F_W16Z16Y16X16
        const u8 texture = color_format == 8 ? 0x85 : 0x9A;
        format = type == 1 ? static_cast<u8>(texture | 0x20) : type == 2 ? texture : 0xFF; // PITCH: linear; SWIZZLE
    }
    u32 wmul = 1, hmul = 1;
    RescState& s = state();
    {
        std::lock_guard lock(s.mutex);
        if (s.config.size == 24 || s.config.size == 28) {
            if (antialias == 3) wmul = 2;
            else if (antialias == 4 || antialias == 5) wmul = hmul = 2;
        }
    }
    for (u32 i = 0; i < 16; i += 4) ps_write32(base, src + i, 0);
    ps_write32(base, src, format);
    ps_write32(base, src + 4, ps_read32(base, surface + 24));                          // colorPitch[0]
    ps_write16(base, src + 8, static_cast<u16>(ps_read16(base, surface + 52) * wmul));  // width
    ps_write16(base, src + 10, static_cast<u16>(ps_read16(base, surface + 54) * hmul)); // height
    ps_write32(base, src + 12, ps_read32(base, surface + 8));                          // colorOffset[0]
    return lv2::CELL_OK;
}

s32 cellRescSetSrc(PPUContext& ctx, s32 idx, u32 src)
{
    RescState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_RESC_ERROR_NOT_INITIALIZED);
    // RPCS3 checks idx against the count only; a negative one is refused here too.
    if (idx < 0 || static_cast<u32>(idx) >= kSrcBufferNum || !src) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    if (!readable(ctx, src, 16, "cellRescSetSrc")) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    u8* base = guest_base(ctx);
    const Src v{ps_read32(base, src), ps_read32(base, src + 4), ps_read16(base, src + 8), ps_read16(base, src + 10),
        ps_read32(base, src + 12)};
    if (!v.width || !v.height || v.height > 4096) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    if (!s.src_set[static_cast<usize>(idx)] || !(s.srcs[static_cast<usize>(idx)] == v))
        PS_LOG_INFO(kChan, "cellRescSetSrc({}: format {:#x}, pitch {:#x}, {}x{}, offset {:#x})", idx, v.format, v.pitch,
            v.width, v.height, v.offset);
    s.srcs[static_cast<usize>(idx)] = v;
    s.src_set[static_cast<usize>(idx)] = true;
    return lv2::CELL_OK;
}

// cellRescSetConvertAndFlip(context, idx): the source becomes display buffer idx (again only when it changed) and the
// game's command buffer gets cellGcmSetFlip's command for it: the RSX flips it in command order, and Metal shows it
// scaled to the screen.
s32 cellRescSetConvertAndFlip(PPUContext& ctx, u32 context, s32 idx)
{
    RescState& s = state();
    Src src;
    bool register_it = false;
    {
        std::lock_guard lock(s.mutex);
        if (!s.initialized) return error(CELL_RESC_ERROR_NOT_INITIALIZED);
        if (idx < 0 || static_cast<u32>(idx) >= kSrcBufferNum || !s.src_set[static_cast<usize>(idx)])
            return error(CELL_RESC_ERROR_BAD_ARGUMENT);
        src = s.srcs[static_cast<usize>(idx)];
        register_it = !s.shown_set[static_cast<usize>(idx)] || !(s.shown[static_cast<usize>(idx)] == src);
        s.shown[static_cast<usize>(idx)] = src;
        s.shown_set[static_cast<usize>(idx)] = true;
    }
    if (register_it) {
        const s32 r = gcm::set_display_buffer(ctx, static_cast<u8>(idx), src.offset, src.pitch, src.width, src.height);
        if (r != lv2::CELL_OK) {
            PS_LOG_ERROR(kChan, "cellRescSetConvertAndFlip({}): its source is not a display buffer ({:#x})", idx,
                static_cast<u32>(r));
            return error(CELL_RESC_ERROR_BAD_ARGUMENT);
        }
    }
    if (gcm::set_flip(ctx, context, static_cast<u32>(idx)) != lv2::CELL_OK) return error(CELL_RESC_ERROR_GCM_FLIP_QUE_FULL);
    return lv2::CELL_OK;
}

s32 cellRescSetWaitFlip(PPUContext& ctx, u32 context)
{
    gcm::set_wait_flip(ctx, context);
    return lv2::CELL_OK;
}

s32 cellRescSetBufferAddress(PPUContext&, u32 color_buffers, u32 vertex_array, u32 fragment_shader)
{
    RescState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.initialized) return error(CELL_RESC_ERROR_NOT_INITIALIZED);
    if (!color_buffers || !vertex_array || !fragment_shader) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    if ((color_buffers & 127) || (vertex_array & 3) || (fragment_shader & 63)) return error(CELL_RESC_ERROR_BAD_ALIGNMENT);
    s.color_buffers = color_buffers;
    s.vertex_array = vertex_array;
    s.fragment_shader = fragment_shader;
    PS_LOG_INFO(kChan, "cellRescSetBufferAddress(colour buffers {:#x}, vertices {:#x}, shader {:#x}): kept, unused",
        color_buffers, vertex_array, fragment_shader);
    return lv2::CELL_OK;
}

// The flips are GCM's: its handlers, status and time.
void cellRescSetFlipHandler(PPUContext& ctx, u32 handler) { gcm::set_flip_handler(ctx, handler); }
void cellRescSetVBlankHandler(PPUContext& ctx, u32 handler) { gcm::set_vblank_handler(ctx, handler); }
void cellRescResetFlipStatus(PPUContext& ctx) { gcm::reset_flip_status(ctx); }
s32 cellRescGetFlipStatus(PPUContext& ctx) { return static_cast<s32>(gcm::get_flip_status(ctx)); }
u64 cellRescGetLastFlipTime(PPUContext& ctx) { return gcm::get_last_flip_time(ctx); }

// The register count of libresc's conversion shader: kept for the game to read back (there is no shader here).
s32 cellRescGetRegisterCount(PPUContext&)
{
    RescState& s = state();
    std::lock_guard lock(s.mutex);
    return s.register_count;
}

void cellRescSetRegisterCount(PPUContext&, s32 count)
{
    RescState& s = state();
    std::lock_guard lock(s.mutex);
    s.register_count = count;
}

// cellRescCreateInterlaceTable(ea, srcH (f1), depth, length): RPCS3's checks. The screen is progressive: no table is
// needed, none is written.
s32 cellRescCreateInterlaceTable(PPUContext& ctx, u32 ea, u32 depth, s32 length)
{
    const f32 src_h = static_cast<f32>(ctx.fpr[1]);
    if (!ea || src_h <= 0.0f || depth > CELL_RESC_ELEMENT_FLOAT || length <= 0) return error(CELL_RESC_ERROR_BAD_ARGUMENT);
    RescState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.buffer_mode) return error(CELL_RESC_ERROR_BAD_COMBINATION);
    return lv2::CELL_OK;
}

} // namespace

void register_cellResc(Registry& r)
{
    r.add("cellResc", {
        PS_HLE_FUNC(0x516EE89E, cellRescInit),
        PS_HLE_FUNC(0x2EA3061E, cellRescExit),
        PS_HLE_FUNC(0xD1CA0503, cellRescVideoOutResolutionId2RescBufferMode),
        PS_HLE_FUNC(0x10DB5B1A, cellRescSetDsts),
        PS_HLE_FUNC(0x23134710, cellRescSetDisplayMode),
        PS_HLE_FUNC(0x22AE06D8, cellRescAdjustAspectRatio),
        PS_HLE_FUNC(0x19A2A967, cellRescSetPalInterpolateDropFlexRatio),
        PS_HLE_FUNC(0x5A338CDB, cellRescGetBufferSize),
        PS_HLE_FUNC(0x0A2069C7, cellRescGetNumColorBuffers),
        PS_HLE_FUNC(0x01220224, cellRescGcmSurface2RescSrc),
        PS_HLE_FUNC(0x6CD0F95F, cellRescSetSrc),
        PS_HLE_FUNC(0x25C107E6, cellRescSetConvertAndFlip),
        PS_HLE_FUNC(0x0D3C22CE, cellRescSetWaitFlip),
        PS_HLE_FUNC(0x8107277C, cellRescSetBufferAddress),
        PS_HLE_FUNC(0x2EA94661, cellRescSetFlipHandler),
        PS_HLE_FUNC(0x129922A0, cellRescResetFlipStatus),
        PS_HLE_FUNC(0xC47C5C22, cellRescGetFlipStatus),
        PS_HLE_FUNC(0x1DD3C4CD, cellRescGetRegisterCount),
        PS_HLE_FUNC(0x66F5E388, cellRescGetLastFlipTime),
        PS_HLE_FUNC(0x7AF8A37F, cellRescSetRegisterCount),
        PS_HLE_FUNC(0xD3758645, cellRescSetVBlankHandler),
        PS_HLE_FUNC(0xE0CEF79E, cellRescCreateInterlaceTable),
    });
}

} // namespace pspocket::hle
