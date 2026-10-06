// cellJpgDec: the JPEG decoder games use for their images (D-200), pspocket's own (jpeg_decoder.cpp, written here to
// decode as Sony's does, sample for sample). Sony's libjpgdec is IJG libjpeg 6b with Sony's changes behind the same
// small API as libpngdec: a handle made with the game's malloc and free callbacks, one stream at a time (a file or a
// buffer), its header, the output wanted (scale, method, colour space, alpha, row order), the rows decoded into the
// game's buffer.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellJpgDec.cpp/.h (the structures; RPCS3 decodes with stb_image). Sony's
// behaviour is measured call by call in tests/hle/test_jpgdec.cpp against the user's libjpgdec (run 107), with SPU
// threads off and on (the images are the same; with them on, Create takes three more blocks):
// - Methods 0 (quality: libjpeg's float IDCT in double, fancy upsampling) and 5 (fast: Sony's 16-bit AltiVec IDCT,
//   box upsampling); anything else ARG.
// - downScale rounds down to 1, 2, 4 or 8 (0 is 1, 16 is 8).
// - Colour spaces: gray, RGB, YCbCr (the same samples as "upsample only": upsampled, not converted), RGBA and ARGB
//   (the alpha asked for), and for gray images "gray to alpha" (RGBA, ARGB: only the alpha byte is written, the gray
//   sample; the colour bytes are left as they were). YCbCr from a gray image, gray to alpha from a colour one, and 30
//   and 31 are ARG.
// - A refused parameter set puts the stream in an error state, as libpngdec's.
// - useMemorySpace is Sony's estimate over libjpeg's state; the mean is Sony's, from DC sums its decoder keeps (-1 for
//   progressive files).
// - Damaged files: one that ends early stops the decode (STREAM_FORMAT, the lines written so far counted); libjpeg's
//   warnings (corrupt data) give the whole image and STREAM_FORMAT.
// What is not pspocket's yet: the Ext functions and six exports with no known names; a game that imports one gets
// Sony's whole libjpgdec (D-175).

#include <bit>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "jpeg_decoder.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellJpgDec";

enum : u32 {
    CELL_JPGDEC_ERROR_HEADER = 0x80611101,
    CELL_JPGDEC_ERROR_STREAM_FORMAT = 0x80611102,
    CELL_JPGDEC_ERROR_ARG = 0x80611103,
    CELL_JPGDEC_ERROR_SEQ = 0x80611104,
    CELL_JPGDEC_ERROR_FATAL = 0x80611106,
    CELL_JPGDEC_ERROR_OPEN_FILE = 0x80611107,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 kCodecVersion = 0x00430000;
constexpr u32 kHandleMagic = 0xFE96AD31, kStreamMagic = 0xFD87BC21; // libjpgdec's
constexpr u32 kHandleSize = 0x428, kStreamSize = 0x5F0;
constexpr u32 kInitSpaceAllocated = 0x6BC0;

namespace hd {
constexpr u32 magic = 0x00, malloc_fn = 0x04, malloc_arg = 0x08, free_fn = 0x0C, free_arg = 0x10, busy = 0x1C,
              stream = 0x20, state = 0x28, spu_blocks = 0x30; // spu_blocks: three addresses (0: SPU threads off)
}
// With SPU threads on, libjpgdec's Create also allocates its SPURS taskset's memory (measured, run 107: these three
// blocks, in this order) and Open counts the first in initSpaceAllocated. pspocket decodes on the PPU either way (the
// images are the same), but takes the same memory from the game's allocator so its budget sees the same.
constexpr u32 kSpuBlocks[3] = {0x187FF, 0x17F, 0x167};
namespace st {
constexpr u32 magic = 0x00, handle = 0x04, state = 0x5A4;
}
enum State : u32 { OPENED = 1, HEADER = 3, PARAMETERS = 4, DECODED = 5, FAILED = 6 };

enum : u32 { GRAYSCALE = 1, RGB = 2, YCBCR = 3, RGBA = 10, UPSAMPLE_ONLY = 11, ARGB = 20, GRAY_TO_ALPHA_RGBA = 40,
             GRAY_TO_ALPHA_ARGB = 41 };

struct Stream {
    std::vector<u8> data;
    jpeg::Decoder decoder;
    bool header_read = false;
    u32 scale = 1, method = 0, mode = 0, space = 0, components = 0, width = 0, height = 0;
    u8 alpha = 0;
};

struct JpgDecs {
    std::mutex m;
    std::map<u32, std::shared_ptr<Stream>> streams;
};

JpgDecs& decs() { return HleProcess::current()->state<JpgDecs>(); }

std::shared_ptr<Stream> stream_of(u32 s)
{
    JpgDecs& d = decs();
    std::lock_guard lock(d.m);
    const auto it = d.streams.find(s);
    return it == d.streams.end() ? nullptr : it->second;
}

s32 finish(s32 r, const std::string& call)
{
    if (r < 0) PS_LOG_WARN(kChan, "{}: {:#010x}", call, static_cast<u32>(r));
    else PS_LOG_DEBUG(kChan, "{}: CELL_OK", call);
    return r;
}

bool readable(PPUContext& ctx, u32 addr, u32 size) { return addr && guest_readable(thread_of(ctx), addr, size); }

bool live_handle(PPUContext& ctx, u32 h)
{
    return readable(ctx, h, kHandleSize) && ps_read32(guest_base(ctx), h + hd::magic) == kHandleMagic;
}

bool live_stream(PPUContext& ctx, u32 h, u32 s)
{
    u8* base = guest_base(ctx);
    return readable(ctx, s, kStreamSize) && ps_read32(base, s + st::magic) == kStreamMagic &&
        ps_read32(base, s + st::handle) == h;
}

u32 guest_malloc(PPUContext& ctx, u32 h, u32 size)
{
    u8* base = guest_base(ctx);
    return static_cast<u32>(call_guest(ctx, ps_read32(base, h + hd::malloc_fn), {size, ps_read32(base, h + hd::malloc_arg)}));
}

void guest_free(PPUContext& ctx, u32 h, u32 p)
{
    u8* base = guest_base(ctx);
    call_guest(ctx, ps_read32(base, h + hd::free_fn), {p, ps_read32(base, h + hd::free_arg)});
}

void set_state(u8* base, u32 h, u32 s, u32 state)
{
    ps_write32(base, s + st::state, state);
    ps_write32(base, h + hd::state, state);
}

s32 create(PPUContext& ctx, u32 handle_out, u32 in, u32 out)
{
    u8* base = guest_base(ctx);
    if (!readable(ctx, handle_out, 4) || !readable(ctx, in, 0x1C) || !readable(ctx, out, 4)) return error(CELL_JPGDEC_ERROR_ARG);
    // CellJpgDecThreadInParam: spuThreadEnable, ppuThreadPriority, spuThreadPriority, malloc, its argument, free, its
    // argument.
    const u32 malloc_fn = ps_read32(base, in + 12), free_fn = ps_read32(base, in + 20);
    if (!malloc_fn || !free_fn) return error(CELL_JPGDEC_ERROR_ARG);
    const u32 malloc_arg = ps_read32(base, in + 16), free_arg = ps_read32(base, in + 24);
    const u32 h = static_cast<u32>(call_guest(ctx, malloc_fn, {kHandleSize, malloc_arg}));
    if (!h || !readable(ctx, h, kHandleSize)) return error(CELL_JPGDEC_ERROR_FATAL);
    std::memset(base + h, 0, kHandleSize);
    if (ps_read32(base, in) != 0) { // spuThreadEnable
        for (u32 i = 0; i < 3; i++) {
            const u32 p = static_cast<u32>(call_guest(ctx, malloc_fn, {kSpuBlocks[i], malloc_arg}));
            if (!p) {
                for (u32 k = i; k-- > 0;) call_guest(ctx, free_fn, {ps_read32(base, h + hd::spu_blocks + 4 * k), free_arg});
                call_guest(ctx, free_fn, {h, free_arg});
                return error(CELL_JPGDEC_ERROR_FATAL);
            }
            ps_write32(base, h + hd::spu_blocks + 4 * i, p);
        }
    }
    ps_write32(base, h + hd::malloc_fn, malloc_fn);
    ps_write32(base, h + hd::malloc_arg, malloc_arg);
    ps_write32(base, h + hd::free_fn, free_fn);
    ps_write32(base, h + hd::free_arg, free_arg);
    ps_write32(base, h + hd::state, OPENED);
    ps_write32(base, h + hd::magic, kHandleMagic);
    ps_write32(base, handle_out, h);
    ps_write32(base, out, kCodecVersion); // CellJpgDecThreadOutParam
    return lv2::CELL_OK;
}

s32 cellJpgDecCreate(PPUContext& ctx, u32 handle_out, u32 thread_in, u32 thread_out)
{
    const std::string call = std::format("cellJpgDecCreate({:#x}, {:#x}, {:#x})", handle_out, thread_in, thread_out);
    return finish(create(ctx, handle_out, thread_in, thread_out), call);
}

s32 destroy(PPUContext& ctx, u32 h)
{
    if (!live_handle(ctx, h)) return error(CELL_JPGDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (ps_read32(base, h + hd::busy)) return error(CELL_JPGDEC_ERROR_SEQ);
    ps_write32(base, h + hd::magic, 0);
    const u32 free_fn = ps_read32(base, h + hd::free_fn), free_arg = ps_read32(base, h + hd::free_arg);
    for (u32 k = 3; k-- > 0;)
        if (const u32 p = ps_read32(base, h + hd::spu_blocks + 4 * k)) call_guest(ctx, free_fn, {p, free_arg});
    call_guest(ctx, free_fn, {h, free_arg});
    return lv2::CELL_OK;
}

s32 cellJpgDecDestroy(PPUContext& ctx, u32 handle)
{
    return finish(destroy(ctx, handle), std::format("cellJpgDecDestroy({:#x})", handle));
}

// The source's bytes (CellJpgDecSrc: srcSelect, fileName, fileOffset (8 bytes at +8), fileSize, streamPtr,
// streamSize, spuThreadEnable).
s32 read_source(PPUContext& ctx, u32 src, std::vector<u8>& out)
{
    u8* base = guest_base(ctx);
    const u32 select = ps_read32(base, src);
    if (select == 1) {
        const u32 ptr = ps_read32(base, src + 20), size = ps_read32(base, src + 24);
        if (!ptr || !size) return error(CELL_JPGDEC_ERROR_ARG);
        if (!readable(ctx, ptr, size)) {
            log_call_site(ctx, base, std::format("cellJpgDecOpen: the buffer ({:#x}, {} bytes) is not guest memory", ptr, size));
            return error(CELL_JPGDEC_ERROR_ARG);
        }
        out.assign(base + ptr, base + ptr + size);
        return lv2::CELL_OK;
    }
    if (select != 0) return error(CELL_JPGDEC_ERROR_ARG);
    const GuestString name = read_cstr(thread_of(ctx), ps_read32(base, src + 4), 1024);
    if (!name.terminated) return error(CELL_JPGDEC_ERROR_ARG);
    std::vector<u8> file;
    if (!thread_of(ctx).kernel.vfs_read_file(name.text, file)) {
        PS_LOG_WARN(kChan, "cellJpgDecOpen: {} cannot be read", name.text);
        return error(CELL_JPGDEC_ERROR_OPEN_FILE);
    }
    const u64 offset = ps_read64(base, src + 8);
    const u32 size = ps_read32(base, src + 16);
    if (offset > file.size()) return error(CELL_JPGDEC_ERROR_OPEN_FILE);
    const usize end = size ? std::min<usize>(file.size(), offset + size) : file.size();
    out.assign(file.begin() + static_cast<std::ptrdiff_t>(offset), file.begin() + static_cast<std::ptrdiff_t>(end));
    return lv2::CELL_OK;
}

s32 open(PPUContext& ctx, u32 h, u32 stream_out, u32 src, u32 open_info)
{
    if (!live_handle(ctx, h)) return error(CELL_JPGDEC_ERROR_ARG);
    if (!readable(ctx, stream_out, 4)) return error(CELL_JPGDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (ps_read32(base, h + hd::busy)) return error(CELL_JPGDEC_ERROR_SEQ);
    ps_write32(base, h + hd::busy, 1);
    if (!readable(ctx, src, 0x20) || !readable(ctx, open_info, 4)) return error(CELL_JPGDEC_ERROR_ARG);
    auto s = std::make_shared<Stream>();
    if (const s32 r = read_source(ctx, src, s->data); r != lv2::CELL_OK) return r;
    const u32 sa = guest_malloc(ctx, h, kStreamSize);
    if (!sa || !readable(ctx, sa, kStreamSize)) return error(CELL_JPGDEC_ERROR_FATAL);
    std::memset(base + sa, 0, kStreamSize);
    ps_write32(base, sa + st::magic, kStreamMagic);
    ps_write32(base, sa + st::handle, h);
    set_state(base, h, sa, OPENED);
    ps_write32(base, h + hd::stream, sa);
    {
        JpgDecs& d = decs();
        std::lock_guard lock(d.m);
        d.streams[sa] = s;
    }
    ps_write32(base, open_info, kInitSpaceAllocated + (ps_read32(base, h + hd::spu_blocks) ? kSpuBlocks[0] : 0)); // CellJpgDecOpnInfo
    ps_write32(base, stream_out, sa);
    return lv2::CELL_OK;
}

s32 cellJpgDecOpen(PPUContext& ctx, u32 handle, u32 stream_out, u32 src, u32 open_info)
{
    const std::string call = std::format("cellJpgDecOpen({:#x}, {:#x}, {:#x}, {:#x})", handle, stream_out, src, open_info);
    return finish(open(ctx, handle, stream_out, src, open_info), call);
}

s32 checked(PPUContext& ctx, u32 h, u32 s, u32 state, std::shared_ptr<Stream>& out)
{
    if (!live_handle(ctx, h)) return error(CELL_JPGDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (!ps_read32(base, h + hd::stream)) return error(CELL_JPGDEC_ERROR_SEQ);
    if (!live_stream(ctx, h, s) || ps_read32(base, h + hd::stream) != s) return error(CELL_JPGDEC_ERROR_ARG);
    if (ps_read32(base, s + st::state) != state) return error(CELL_JPGDEC_ERROR_SEQ);
    out = stream_of(s);
    if (!out) PS_FATAL(kChan, "stream {:#x} has no host side", s);
    return lv2::CELL_OK;
}

s32 close(PPUContext& ctx, u32 h, u32 s)
{
    if (!live_handle(ctx, h)) return error(CELL_JPGDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (!ps_read32(base, h + hd::stream)) return error(CELL_JPGDEC_ERROR_SEQ);
    if (!live_stream(ctx, h, s) || ps_read32(base, h + hd::stream) != s) return error(CELL_JPGDEC_ERROR_ARG);
    {
        JpgDecs& d = decs();
        std::lock_guard lock(d.m);
        d.streams.erase(s);
    }
    ps_write32(base, s + st::magic, 0);
    guest_free(ctx, h, s);
    ps_write32(base, h + hd::stream, 0);
    ps_write32(base, h + hd::busy, 0);
    ps_write32(base, h + hd::state, OPENED);
    return lv2::CELL_OK;
}

s32 cellJpgDecClose(PPUContext& ctx, u32 handle, u32 stream)
{
    return finish(close(ctx, handle, stream), std::format("cellJpgDecClose({:#x}, {:#x})", handle, stream));
}

// CellJpgDecInfo's colour space for the JPEG's (libjpgdec's J_COLOR_SPACE mapping).
u32 info_space(jpeg::ColorSpace c)
{
    switch (c) {
    case jpeg::ColorSpace::Gray: return GRAYSCALE;
    case jpeg::ColorSpace::YCbCr: return YCBCR;
    case jpeg::ColorSpace::RGB: return RGB;
    case jpeg::ColorSpace::CMYK: return 4;
    case jpeg::ColorSpace::YCCK: return 5;
    default: return 0;
    }
}

s32 read_header(PPUContext& ctx, u32 h, u32 sa, u32 info)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, OPENED, s); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    if (!readable(ctx, info, 0x10)) return error(CELL_JPGDEC_ERROR_ARG);
    std::string why;
    if (s->data.size() < 2 || s->data[0] != 0xFF || s->data[1] != 0xD8) {
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_HEADER);
    }
    if (!s->decoder.read_header(s->data.data(), s->data.size(), why)) {
        PS_LOG_WARN(kChan, "cellJpgDecReadHeader: {}", why);
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_STREAM_FORMAT);
    }
    const jpeg::Header& hdr = s->decoder.header();
    // CellJpgDecInfo: width, height, components, colour space.
    ps_write32(base, info + 0, hdr.width);
    ps_write32(base, info + 4, hdr.height);
    ps_write32(base, info + 8, static_cast<u32>(hdr.components));
    ps_write32(base, info + 12, info_space(hdr.color_space));
    s->header_read = true;
    set_state(base, h, sa, HEADER);
    return lv2::CELL_OK;
}

s32 cellJpgDecReadHeader(PPUContext& ctx, u32 handle, u32 stream, u32 info)
{
    const std::string call = std::format("cellJpgDecReadHeader({:#x}, {:#x}, {:#x})", handle, stream, info);
    return finish(read_header(ctx, handle, stream, info), call);
}

// libjpeg's output_components for a cellJpgDec colour space (what libjpgdec's useMemorySpace counts).
u32 output_components(u32 space, bool gray)
{
    switch (space) {
    case GRAYSCALE: return 1;
    case UPSAMPLE_ONLY: return gray ? 1 : 3;
    case RGBA:
    case ARGB:
    case GRAY_TO_ALPHA_RGBA:
    case GRAY_TO_ALPHA_ARGB: return 4;
    default: return 3;
    }
}

s32 set_parameter(PPUContext& ctx, u32 h, u32 sa, u32 in, u32 out)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, HEADER, s); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    if (!readable(ctx, in, 0x18) || !readable(ctx, out, 0x28)) return error(CELL_JPGDEC_ERROR_ARG);
    // CellJpgDecInParam: commandPtr, downScale, method, outputMode, outputColorSpace, outputColorAlpha (a byte).
    const u32 scale = ps_read32(base, in + 4), method = ps_read32(base, in + 8), mode = ps_read32(base, in + 12);
    const u32 space = ps_read32(base, in + 16);
    const u8 alpha = ps_read8(base, in + 20);
    const jpeg::Header& hdr = s->decoder.header();
    const bool gray = hdr.color_space == jpeg::ColorSpace::Gray;
    const auto refuse = [&](std::string_view why) {
        PS_LOG_WARN(kChan, "cellJpgDecSetParameter: {} (scale {}, method {}, mode {}, colour space {})", why, scale, method,
            mode, space);
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_ARG);
    };
    if (method != 0 && method != 5) return refuse("an unknown method");
    if (mode > 1) return refuse("an unknown row order");
    u32 components = 0;
    switch (space) {
    case GRAYSCALE: components = 1; break;
    case RGB: components = 3; break;
    case YCBCR:
        if (gray) return refuse("YCbCr from a gray image");
        components = 3;
        break;
    case UPSAMPLE_ONLY: components = gray ? 1 : 3; break;
    case RGBA:
    case ARGB: components = 4; break;
    case GRAY_TO_ALPHA_RGBA:
    case GRAY_TO_ALPHA_ARGB:
        if (!gray) return refuse("gray to alpha from a colour image");
        components = 4;
        break;
    default: return refuse("an unknown colour space");
    }
    if (hdr.color_space != jpeg::ColorSpace::Gray && hdr.color_space != jpeg::ColorSpace::YCbCr &&
        hdr.color_space != jpeg::ColorSpace::RGB)
        return refuse("a colour space pspocket does not decode (CMYK, YCCK)");
    s->scale = scale >= 8 ? 8 : scale >= 4 ? 4 : scale >= 2 ? 2 : 1;
    s->method = method;
    s->mode = mode;
    s->space = space;
    s->alpha = alpha;
    s->components = components;
    s->width = (hdr.width + s->scale - 1) / s->scale;
    s->height = (hdr.height + s->scale - 1) / s->scale;
    // CellJpgDecOutParam: outputWidthByte (8 bytes), width, height, components, mode, colour space, downScale,
    // useMemorySpace.
    ps_write64(base, out + 0x00, u64{s->width} * components);
    ps_write32(base, out + 0x08, s->width);
    ps_write32(base, out + 0x0C, s->height);
    ps_write32(base, out + 0x10, components);
    ps_write32(base, out + 0x14, mode);
    ps_write32(base, out + 0x18, space);
    ps_write32(base, out + 0x1C, s->scale);
    ps_write32(base, out + 0x20, s->decoder.memory_estimate(static_cast<int>(s->scale), method == 5, s->width,
                                     output_components(space, gray)));
    set_state(base, h, sa, PARAMETERS);
    return lv2::CELL_OK;
}

s32 cellJpgDecSetParameter(PPUContext& ctx, u32 handle, u32 stream, u32 in_param, u32 out_param)
{
    const std::string call =
        std::format("cellJpgDecSetParameter({:#x}, {:#x}, {:#x}, {:#x})", handle, stream, in_param, out_param);
    return finish(set_parameter(ctx, handle, stream, in_param, out_param), call);
}

s32 decode_data(PPUContext& ctx, u32 h, u32 sa, u32 data, u32 ctrl, u32 out)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, PARAMETERS, s); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    if (!readable(ctx, ctrl, 8) || !readable(ctx, out, 0x0C)) return error(CELL_JPGDEC_ERROR_ARG);
    const u64 pitch = ps_read64(base, ctrl); // CellJpgDecDataCtrlParam: outputBytesPerLine
    const u64 row = u64{s->width} * s->components;
    if (pitch < row) {
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_ARG);
    }
    const u64 size = s->height ? pitch * (s->height - 1) + row : 0;
    if (size > 0xFFFFFFFFull || !readable(ctx, data, static_cast<u32>(size))) {
        log_call_site(ctx, base, std::format("cellJpgDecDecodeData: the image ({:#x}, {} bytes) is not guest memory", data, size));
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_ARG);
    }
    std::string why;
    const bool gray_out = s->space == GRAYSCALE || s->space == GRAY_TO_ALPHA_RGBA || s->space == GRAY_TO_ALPHA_ARGB ||
        (s->space == UPSAMPLE_ONLY && s->decoder.header().color_space == jpeg::ColorSpace::Gray);
    const bool ycc_out = s->space == YCBCR || (s->space == UPSAMPLE_ONLY && !gray_out);
    const bool gray_in = s->decoder.header().color_space == jpeg::ColorSpace::Gray;
    jpeg::Decoder::Output img;
    const jpeg::ColorSpace want = gray_out || gray_in ? jpeg::ColorSpace::Gray : ycc_out ? jpeg::ColorSpace::YCbCr
                                                                                          : jpeg::ColorSpace::RGB;
    if (!s->decoder.decode(why) || !s->decoder.output(static_cast<int>(s->scale), s->method == 5, want, img, why)) {
        PS_LOG_WARN(kChan, "cellJpgDecDecodeData: {}", why);
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_STREAM_FORMAT);
    }
    // Damaged data (measured, run 107): a file that ends early stops libjpgdec where it was, the lines written so far
    // counted and nothing else told; libjpeg's warnings (corrupt data it decodes anyway) give the whole image and its
    // mean, and the same error.
    const bool ran_out = s->decoder.ran_out();
    const u32 lines = s->decoder.lines_written(static_cast<int>(s->scale), s->method == 5, want, s->height);
    for (u32 y = 0; y < lines; y++) {
        u8* o = base + data + static_cast<u32>((s->mode == 1 ? s->height - 1 - y : y) * pitch);
        const u8* p = img.pixels.data() + usize{y} * img.width * img.components;
        for (u32 x = 0; x < s->width; x++, p += img.components) {
            const u8 r = p[0], g = img.components == 3 ? p[1] : p[0], b = img.components == 3 ? p[2] : p[0];
            switch (s->space) {
            case RGBA: *o++ = r; *o++ = g; *o++ = b; *o++ = s->alpha; break;
            case ARGB: *o++ = s->alpha; *o++ = r; *o++ = g; *o++ = b; break;
            case GRAY_TO_ALPHA_RGBA: o[3] = r; o += 4; break; // only the alpha: the gray sample
            case GRAY_TO_ALPHA_ARGB: o[0] = r; o += 4; break;
            case GRAYSCALE: *o++ = r; break;
            case UPSAMPLE_ONLY:
                if (s->components == 1) {
                    *o++ = r;
                    break;
                }
                [[fallthrough]];
            default: *o++ = r; *o++ = g; *o++ = b; break;
            }
        }
    }
    // CellJpgDecDataOutInfo: mean (a float), outputLines, status.
    ps_write32(base, out + 4, lines);
    if (ran_out) {
        PS_LOG_WARN(kChan, "cellJpgDecDecodeData: the file ends before its image does ({} of {} lines)", lines, s->height);
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_STREAM_FORMAT);
    }
    ps_write32(base, out + 0, std::bit_cast<u32>(s->decoder.mean()));
    ps_write32(base, out + 8, 0);
    set_state(base, h, sa, DECODED);
    if (s->decoder.warnings()) {
        PS_LOG_WARN(kChan, "cellJpgDecDecodeData: damaged data ({} libjpeg warnings)", s->decoder.warnings());
        return error(CELL_JPGDEC_ERROR_STREAM_FORMAT);
    }
    return lv2::CELL_OK;
}

s32 cellJpgDecDecodeData(PPUContext& ctx, u32 handle, u32 stream, u32 data, u32 ctrl, u32 out)
{
    const std::string call =
        std::format("cellJpgDecDecodeData({:#x}, {:#x}, {:#x}, {:#x}, {:#x})", handle, stream, data, ctrl, out);
    return finish(decode_data(ctx, handle, stream, data, ctrl, out), call);
}

} // namespace

void register_cellJpgDec(Registry& r)
{
    r.add("cellJpgDec", {
        PS_HLE_FUNC(0xA7978F59, cellJpgDecCreate),
        PS_HLE_FUNC(0xD8EA91F8, cellJpgDecDestroy),
        PS_HLE_FUNC(0x976CA5C2, cellJpgDecOpen),
        PS_HLE_FUNC(0x9338A07A, cellJpgDecClose),
        PS_HLE_FUNC(0x6D9EBCCF, cellJpgDecReadHeader),
        PS_HLE_FUNC(0xE08F3910, cellJpgDecSetParameter),
        PS_HLE_FUNC(0xAF8BB012, cellJpgDecDecodeData),
    });
}

} // namespace pspocket::hle
