// cellPngDec: the PNG decoder games use for their images (D-199), pspocket's own on libpng. Sony's libpngdec is libpng
// 1.2.8 on zlib 1.2.3 behind a small API: a decoder handle made with the game's malloc and free callbacks, one stream
// at a time on it (a file or a buffer), its header, the output wanted (colour space, bit depth, alpha, row order), the
// rows decoded into the game's buffer, and queries for the PNG's chunks.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellPngDec.cpp/.h and cellPng.h (the structures, libpng's transformations as
// libpngdec sets them, png_set_strip_16 for 16 to 8 bits). Everything else here is Sony's behaviour as measured in
// tests/hle/test_pngdec.cpp against the user's libpngdec (run 106), where RPCS3 does not check or stubs:
// - Sequence: Open, ReadHeader, SetParameter, DecodeData, each once and in that order (else SEQ); the chunk queries
//   only after DecodeData. One stream per handle: a second Open, or Destroy with a stream open, is SEQ.
// - A refused header, parameter set, line pitch or chunk query puts the stream in an error state: everything but Close
//   is SEQ afterwards. An Open refused for its source (or a missing source or open information) leaves the handle
//   busy for good, as libpngdec does: later Opens and Destroy are SEQ.
// - The output libpngdec refuses (ARG): 16 bits from an image of less; the palette from an image without one; alpha
//   from the stream when the image has none (no alpha channel, no tRNS).
// - The numbers it reports: initSpaceAllocated 0x4604; useMemorySpace 0x8441 + 2 x width x output components.
// - The chunk queries answer from libpng's information; the doubles (gAMA, cHRM) at libpng 1.2.8's float precision.
// pspocket's guest memory use differs from libpngdec's: the handle (0x450 and 0xA80 bytes, Sony's sizes), the stream
// (0x98) and the chunk queries' answers are the game's memory, libpng's own allocations are the host's (as RPCS3).
// The Ext functions (D-595) are made from RPCS3 (pngDecCreate, pngDecOpen, pngDecSetParameter and pngDecodeData with
// their ext arguments) on this decoder, without Sony's module to compare them with: they are unverified (D-594), so a
// game importing one still gets Sony's whole libpngdec where the system software is installed. They add stream input
// (the PNG's next bytes from the game's callback as libpng reads them) and partial output (outputCounts lines at a time
// to the game's display callback); see ext_open and decode_parts.

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"

#if PSPOCKET_HAVE_LIBPNG
#include <csetjmp>

#include <png.h>
#endif

namespace pspocket::hle {

#if PSPOCKET_HAVE_LIBPNG

namespace {

constexpr std::string_view kChan = "cellPngDec";

enum : u32 {
    CELL_PNGDEC_ERROR_HEADER = 0x80611201,
    CELL_PNGDEC_ERROR_STREAM_FORMAT = 0x80611202,
    CELL_PNGDEC_ERROR_ARG = 0x80611203,
    CELL_PNGDEC_ERROR_SEQ = 0x80611204,
    CELL_PNGDEC_ERROR_FATAL = 0x80611206,
    CELL_PNGDEC_ERROR_OPEN_FILE = 0x80611207,
    CELL_PNGDEC_ERROR_CB_PARAM = 0x8061120A,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 kCodecVersion = 0x00420000;
constexpr u32 kHandleMagic = 0xEF58AD41, kStreamMagic = 0xED37BC51; // libpngdec's
constexpr u32 kHandleSize = 0x450, kHandleExtraSize = 0xA80, kStreamSize = 0x98;
constexpr u32 kInitSpaceAllocated = 0x4604;
// The largest interlaced image decode_parts holds whole before its parts go out (D-595): the PS3's main memory.
constexpr u64 kPartsImageMax = 0x10000000;

// The handle (pspocket's layout but for the magic and the state at +0x18, libpngdec's).
namespace hd {
constexpr u32 magic = 0x00, malloc_fn = 0x04, malloc_arg = 0x08, free_fn = 0x0C, free_arg = 0x10, extra = 0x14,
              state = 0x18, busy = 0x1C, stream = 0x20;
}
// The stream (the magic, the handle, the state at +0x3C as libpngdec's). From +0x40, pspocket's: what the Ext functions
// hand the game's callbacks (D-595): CellPngDecStrmInfo, CellPngDecStrmParam, CellPngDecDispParam (when the game gives
// none) and CellPngDecDispInfo (0x40 bytes, to the block's end).
namespace st {
constexpr u32 magic = 0x00, handle = 0x04, state = 0x3C;
constexpr u32 strm_info = 0x40, strm_param = 0x48, disp_param = 0x50, disp_info = 0x58;
}
enum State : u32 { OPENED = 1, HEADER = 3, PARAMETERS = 4, DECODED = 5, FAILED = 6 };

enum : s32 { GRAYSCALE = 1, RGB = 2, PALETTE = 4, GRAYSCALE_ALPHA = 9, RGBA = 10, ARGB = 20 };
constexpr bool has_alpha(s32 space) { return space == GRAYSCALE_ALPHA || space == RGBA || space == ARGB; }

// A stream's libpng side, on the host.
struct Stream {
    std::vector<u8> data;
    usize cursor = 0;
    png_structp png = nullptr;
    png_infop info = nullptr;
    u32 width = 0, height = 0, passes = 1;
    u64 row_bytes = 0;     // the game's row (outputWidthByte)
    u64 png_row_bytes = 0; // libpng's row, which the game's comes from
    s32 out_mode = 0;
    // Colour to gray, done here as libpng 1.2.8 does it (libpng 1.6 rounds, and works after png_set_strip_16):
    // libpng gives RGB or RGBA at the image's bit depth, each pixel becomes gray (and alpha).
    bool to_gray = false, gray_alpha = false, fixed_alpha = false;
    u32 src_channels = 0, src_bytes = 1, dst_bytes = 1;
    u8 alpha_value = 0;
    float file_gamma = 0; // libpng 1.2.8's png_ptr->gamma: gAMA's, or sRGB's 0.45455; 0 when neither
    std::vector<u32> blocks; // the game's memory the chunk queries' answers use, freed at Close
    // tRNS as read with the header: libpng 1.6 clears it once it has expanded it into alpha, libpng 1.2.8 kept it.
    bool trns = false;
    std::vector<u8> trns_alphas;
    png_color_16 trns_color{};
    // The Ext functions (D-595). Stream input: the game's callback (CellPngDecCbCtrlStrm; 0: the source holds the whole
    // PNG), the stream's guest block, and while a call that reads runs, the context the callback is called on; ended
    // once it gives nothing more, with CB_PARAM kept when what it gave was not guest memory.
    u32 strm_fn = 0, strm_arg = 0, strm_block = 0;
    PPUContext* strm_ctx = nullptr;
    bool strm_ended = false;
    u32 strm_error = 0;
    // Partial output: the lines a display callback gets at a time (CellPngDecExtInParam's outputCounts; 0: the whole
    // image into DecodeData's buffer), and what ExtSetParameter was given and answered.
    u32 output_counts = 0, command = 0, out_components = 0, out_depth = 0;
    ~Stream()
    {
        if (png) png_destroy_read_struct(&png, info ? &info : nullptr, nullptr);
    }
};

struct PngDecs {
    std::mutex m;
    std::map<u32, std::shared_ptr<Stream>> streams;
    bool select_logged = false, flip_logged = false; // the Ext functions' once-a-process notes (D-595)
};

PngDecs& decs() { return HleProcess::current()->state<PngDecs>(); }

std::shared_ptr<Stream> stream_of(u32 s)
{
    PngDecs& d = decs();
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

// The handle the game passes, or 0 (not guest memory, not a live handle).
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

// Stream input (cellPngDecExtOpen's CellPngDecCbCtrlStrm, D-595; RPCS3 pngDecodeData's loop over cbCtrlStrmFunc): the
// game's stream callback asked for the PNG's next bytes until `need` are held, as libpng reads them (the header's
// chunks at ReadHeader, the rest at DecodeData). The callback gets CellPngDecStrmInfo (decodedStrmSize: the bytes of
// the stream given so far, all of them read by then) and CellPngDecStrmParam, where it puts the next part (strmPtr,
// strmSize), copied at once so that it may reuse its buffer. A negative answer or no bytes ends the stream (libpng then
// says what it lacks: STREAM_FORMAT); bytes that are not guest memory end it too and make the call CB_PARAM. Nothing
// here lives past the return: read_data may longjmp right after.
bool stream_more(Stream& s, usize need)
{
    if (!s.strm_fn || !s.strm_ctx || s.strm_ended) return false;
    PPUContext& ctx = *s.strm_ctx;
    u8* base = guest_base(ctx);
    const u32 info = s.strm_block + st::strm_info, param = s.strm_block + st::strm_param;
    while (s.data.size() < need) {
        ps_write32(base, info, static_cast<u32>(s.data.size()));
        ps_write32(base, param, 0);
        ps_write32(base, param + 4, 0);
        const auto r = static_cast<s32>(static_cast<u32>(call_guest(ctx, s.strm_fn, {info, param, s.strm_arg})));
        const u32 ptr = ps_read32(base, param), size = ps_read32(base, param + 4);
        if (r < 0 || !size) {
            PS_LOG_DEBUG(kChan, "the stream callback ends the stream after {} bytes ({:#x})", s.data.size(), static_cast<u32>(r));
            s.strm_ended = true;
            return false;
        }
        if (!readable(ctx, ptr, size)) {
            PS_LOG_WARN(kChan, "the stream callback gave {} bytes at {:#x}, not guest memory", size, ptr);
            s.strm_error = CELL_PNGDEC_ERROR_CB_PARAM;
            s.strm_ended = true;
            return false;
        }
        s.data.insert(s.data.end(), base + ptr, base + ptr + size);
    }
    return true;
}

// ---- libpng, each call behind its own setjmp (libpng reports errors by longjmp)

void read_data(png_structp png, png_bytep out, size_t length)
{
    auto* s = static_cast<Stream*>(png_get_io_ptr(png));
    if (s->cursor + length > s->data.size() && !stream_more(*s, s->cursor + length)) png_error(png, "the stream ends early");
    std::memcpy(out, s->data.data() + s->cursor, length);
    s->cursor += length;
}

void png_failed(png_structp png, png_const_charp message)
{
    PS_LOG_WARN(kChan, "libpng: {}", message ? message : "(no message)");
    png_longjmp(png, 1);
}

void png_warned(png_structp, png_const_charp message) { PS_LOG_DEBUG(kChan, "libpng: {}", message ? message : ""); }

bool read_info(Stream& s)
{
    if (setjmp(png_jmpbuf(s.png))) return false;
    png_read_info(s.png, s.info);
    return true;
}

bool update_info(Stream& s)
{
    if (setjmp(png_jmpbuf(s.png))) return false;
    png_read_update_info(s.png, s.info);
    return true;
}

// The image as libpng gives it (every pass combined), into `rows`.
bool read_rows(Stream& s, std::vector<u8>& rows)
{
    if (setjmp(png_jmpbuf(s.png))) return false;
    for (u32 pass = 0; pass < s.passes; pass++)
        for (u32 y = 0; y < s.height; y++) png_read_row(s.png, rows.data() + y * s.png_row_bytes, nullptr);
    png_read_end(s.png, s.info);
    return true;
}

// The next row of an image that is not interlaced, and what follows the image (the parts of decode_parts, D-595).
bool read_row(Stream& s, u8* row)
{
    if (setjmp(png_jmpbuf(s.png))) return false;
    png_read_row(s.png, row, nullptr);
    return true;
}

bool read_end(Stream& s)
{
    if (setjmp(png_jmpbuf(s.png))) return false;
    png_read_end(s.png, s.info);
    return true;
}

// libpng 1.2.8's png_do_rgb_to_gray: (6969 R + 23432 G + 2367 B) >> 15, a gray pixel kept as it is. The coefficients
// are what png_set_rgb_to_gray(png, 1, 0.2127, 0.7151) gives, the only natural ones that fit every pixel measured
// (run 106; libpng 1.6's defaults are 6968, 23434, 2366, and it rounds). With the image's gamma known, through 1.2.8's
// gamma_to_1 and gamma_from_1 tables (png_build_gamma_table with no screen gamma), as measured (run 106). At 16 bits
// (then png_do_strip_16's high byte when 8 bits are asked for); a 16-bit image with gamma goes through the same curves
// computed in floating point (libpng 1.2.8's 16-bit tables are not reproduced: not measured).
void rgb_to_gray_row(const Stream& s, const u8* in, u8* out)
{
    constexpr u32 rc = 6969, gc = 23432, bc = 2367;
    const double g = static_cast<double>(s.file_gamma);
    std::array<u8, 256> to_1{}, from_1{};
    if (g > 0)
        for (u32 i = 0; i < 256; i++) {
            to_1[i] = static_cast<u8>(std::pow(i / 255.0, 1.0 / g) * 255.0 + .5);
            from_1[i] = static_cast<u8>(std::pow(i / 255.0, g) * 255.0 + .5);
        }
    const auto gray = [&](u32 r, u32 gr, u32 b) -> u32 {
        if (r == gr && r == b) return r;
        if (g <= 0) return (rc * r + gc * gr + bc * b) >> 15;
        if (s.src_bytes == 1) return from_1[(rc * to_1[r] + gc * to_1[gr] + bc * to_1[b]) >> 15];
        const auto lin = [&](u32 v) { return static_cast<u32>(std::pow(v / 65535.0, 1.0 / g) * 65535.0 + .5); };
        const u32 y = (rc * lin(r) + gc * lin(gr) + bc * lin(b)) >> 15;
        return static_cast<u32>(std::pow(y / 65535.0, g) * 65535.0 + .5);
    };
    const auto sample = [&](const u8* p) { return s.src_bytes == 2 ? u32{p[0]} << 8 | p[1] : u32{p[0]}; };
    const auto put = [&](u8*& o, u32 v) { // v at the source's depth
        if (s.dst_bytes == 2) {
            *o++ = static_cast<u8>(v >> 8);
            *o++ = static_cast<u8>(v);
        } else {
            *o++ = static_cast<u8>(s.src_bytes == 2 ? v >> 8 : v);
        }
    };
    u8* o = out;
    for (u32 x = 0; x < s.width; x++) {
        const u8* p = in + x * s.src_channels * s.src_bytes;
        const u32 r = sample(p), gg = sample(p + s.src_bytes), b = sample(p + 2 * s.src_bytes);
        put(o, gray(r, gg, b));
        if (s.gray_alpha) {
            if (s.fixed_alpha) {
                for (u32 k = 0; k < s.dst_bytes; k++) *o++ = s.alpha_value;
            } else {
                put(o, s.src_channels == 4 ? sample(p + 3 * s.src_bytes) : (s.src_bytes == 2 ? 0xFFFFu : 0xFFu));
            }
        }
    }
}

// CellPngDecInfo's colour space for the PNG's colour type.
s32 color_space(int type)
{
    switch (type) {
    case PNG_COLOR_TYPE_GRAY: return GRAYSCALE;
    case PNG_COLOR_TYPE_RGB: return RGB;
    case PNG_COLOR_TYPE_PALETTE: return PALETTE;
    case PNG_COLOR_TYPE_GRAY_ALPHA: return GRAYSCALE_ALPHA;
    default: return RGBA;
    }
}

// The chunks present (RPCS3's bit order, libpngdec's): gAMA, sBIT, cHRM, PLTE, tRNS, bKGD, hIST, pHYs, oFFs, tIME, pCAL,
// sRGB, iCCP, sPLT, sCAL, then IDAT once decoded.
u32 chunk_information(const Stream& s, bool idat)
{
    constexpr u32 flags[15] = {PNG_INFO_gAMA, PNG_INFO_sBIT, PNG_INFO_cHRM, PNG_INFO_PLTE, PNG_INFO_tRNS, PNG_INFO_bKGD,
        PNG_INFO_hIST, PNG_INFO_pHYs, PNG_INFO_oFFs, PNG_INFO_tIME, PNG_INFO_pCAL, PNG_INFO_sRGB, PNG_INFO_iCCP,
        PNG_INFO_sPLT, PNG_INFO_sCAL};
    u32 bits = 0;
    for (u32 i = 0; i < 15; i++)
        if (png_get_valid(s.png, s.info, flags[i])) bits |= 1u << i;
    return bits | (idat ? 1u << 15 : 0);
}

// While a call that reads the PNG runs (ReadHeader, DecodeData and their Ext forms): the context a stream opened with a
// stream callback calls it on (D-595). Nothing for any other stream.
class Reading {
public:
    Reading(PPUContext& ctx, u32 sa) : s_(stream_of(sa))
    {
        if (s_ && s_->strm_fn) s_->strm_ctx = &ctx;
        else s_ = nullptr;
    }
    ~Reading()
    {
        if (s_) s_->strm_ctx = nullptr;
    }
    Reading(const Reading&) = delete;
    Reading& operator=(const Reading&) = delete;
    // The first `n` bytes held before the header is looked at (the source's buffer may be shorter than the signature).
    void need(usize n) const
    {
        if (s_ && s_->data.size() < n) stream_more(*s_, n);
    }
    // A read cut short by what the stream callback gave answers CB_PARAM.
    s32 result(s32 r) const
    {
        return s_ && s_->strm_error && r == error(CELL_PNGDEC_ERROR_STREAM_FORMAT) ? error(s_->strm_error) : r;
    }

private:
    std::shared_ptr<Stream> s_;
};

// ---- the API

s32 create(PPUContext& ctx, u32 handle_out, u32 in, u32 out)
{
    u8* base = guest_base(ctx);
    if (!readable(ctx, handle_out, 4) || !readable(ctx, in, 0x1C) || !readable(ctx, out, 4)) return error(CELL_PNGDEC_ERROR_ARG);
    // CellPngDecThreadInParam: spuThreadEnable, ppuThreadPriority, spuThreadPriority, malloc, its argument, free, its
    // argument.
    const u32 malloc_fn = ps_read32(base, in + 12), free_fn = ps_read32(base, in + 20);
    if (!malloc_fn || !free_fn) return error(CELL_PNGDEC_ERROR_ARG);
    const u32 malloc_arg = ps_read32(base, in + 16), free_arg = ps_read32(base, in + 24);
    const u32 h = static_cast<u32>(call_guest(ctx, malloc_fn, {kHandleSize, malloc_arg}));
    if (!h || !readable(ctx, h, kHandleSize)) return error(CELL_PNGDEC_ERROR_FATAL);
    std::memset(base + h, 0, kHandleSize);
    ps_write32(base, h + hd::malloc_fn, malloc_fn);
    ps_write32(base, h + hd::malloc_arg, malloc_arg);
    ps_write32(base, h + hd::free_fn, free_fn);
    ps_write32(base, h + hd::free_arg, free_arg);
    const u32 extra = guest_malloc(ctx, h, kHandleExtraSize);
    if (!extra) {
        guest_free(ctx, h, h);
        return error(CELL_PNGDEC_ERROR_FATAL);
    }
    ps_write32(base, h + hd::extra, extra);
    ps_write32(base, h + hd::magic, kHandleMagic);
    ps_write32(base, handle_out, h);
    ps_write32(base, out, kCodecVersion); // CellPngDecThreadOutParam
    return lv2::CELL_OK;
}

s32 cellPngDecCreate(PPUContext& ctx, u32 handle_out, u32 thread_in, u32 thread_out)
{
    const std::string call = std::format("cellPngDecCreate({:#x}, {:#x}, {:#x})", handle_out, thread_in, thread_out);
    return finish(create(ctx, handle_out, thread_in, thread_out), call);
}

s32 destroy(PPUContext& ctx, u32 h)
{
    if (!live_handle(ctx, h)) return error(CELL_PNGDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (ps_read32(base, h + hd::busy)) return error(CELL_PNGDEC_ERROR_SEQ);
    ps_write32(base, h + hd::magic, 0);
    guest_free(ctx, h, ps_read32(base, h + hd::extra));
    // The free callback is read before the block goes.
    const u32 free_fn = ps_read32(base, h + hd::free_fn), free_arg = ps_read32(base, h + hd::free_arg);
    call_guest(ctx, free_fn, {h, free_arg});
    return lv2::CELL_OK;
}

s32 cellPngDecDestroy(PPUContext& ctx, u32 handle)
{
    return finish(destroy(ctx, handle), std::format("cellPngDecDestroy({:#x})", handle));
}

// The source's bytes (CellPngDecSrc: srcSelect, fileName, fileOffset (8 bytes at +8), fileSize, streamPtr, streamSize,
// spuThreadEnable), or an error.
s32 read_source(PPUContext& ctx, u32 src, std::vector<u8>& out)
{
    u8* base = guest_base(ctx);
    const s32 select = static_cast<s32>(ps_read32(base, src));
    if (select == 1) { // a buffer
        const u32 ptr = ps_read32(base, src + 20), size = ps_read32(base, src + 24);
        if (!ptr || !size) return error(CELL_PNGDEC_ERROR_ARG);
        if (!readable(ctx, ptr, size)) {
            log_call_site(ctx, base, std::format("cellPngDecOpen: the buffer ({:#x}, {} bytes) is not guest memory", ptr, size));
            return error(CELL_PNGDEC_ERROR_ARG);
        }
        out.assign(base + ptr, base + ptr + size);
        return lv2::CELL_OK;
    }
    if (select != 0) return error(CELL_PNGDEC_ERROR_ARG);
    const GuestString name = read_cstr(thread_of(ctx), ps_read32(base, src + 4), 1024);
    if (!name.terminated) return error(CELL_PNGDEC_ERROR_ARG);
    std::vector<u8> file;
    if (!thread_of(ctx).kernel.vfs_read_file(name.text, file)) {
        PS_LOG_WARN(kChan, "cellPngDecOpen: {} cannot be read", name.text);
        return error(CELL_PNGDEC_ERROR_OPEN_FILE);
    }
    const u64 offset = ps_read64(base, src + 8);
    const u32 size = ps_read32(base, src + 16);
    if (offset > file.size()) return error(CELL_PNGDEC_ERROR_OPEN_FILE);
    const usize end = size ? std::min<usize>(file.size(), offset + size) : file.size();
    out.assign(file.begin() + static_cast<std::ptrdiff_t>(offset), file.begin() + static_cast<std::ptrdiff_t>(end));
    return lv2::CELL_OK;
}

s32 open(PPUContext& ctx, u32 h, u32 stream_out, u32 src, u32 open_info)
{
    if (!live_handle(ctx, h)) return error(CELL_PNGDEC_ERROR_ARG);
    if (!readable(ctx, stream_out, 4)) return error(CELL_PNGDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (ps_read32(base, h + hd::busy)) return error(CELL_PNGDEC_ERROR_SEQ);
    ps_write32(base, h + hd::busy, 1); // from here an error leaves the handle busy, as libpngdec does
    if (!readable(ctx, src, 0x20) || !readable(ctx, open_info, 4)) return error(CELL_PNGDEC_ERROR_ARG);
    auto s = std::make_shared<Stream>();
    if (const s32 r = read_source(ctx, src, s->data); r != lv2::CELL_OK) return r;
    const u32 sa = guest_malloc(ctx, h, kStreamSize);
    if (!sa || !readable(ctx, sa, kStreamSize)) return error(CELL_PNGDEC_ERROR_FATAL);
    s->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, s.get(), png_failed, png_warned);
    s->info = s->png ? png_create_info_struct(s->png) : nullptr;
    if (!s->png || !s->info) {
        guest_free(ctx, h, sa);
        return error(CELL_PNGDEC_ERROR_FATAL);
    }
    png_set_read_fn(s->png, s.get(), read_data);
    png_set_keep_unknown_chunks(s->png, PNG_HANDLE_CHUNK_ALWAYS, nullptr, 0);
    std::memset(base + sa, 0, kStreamSize);
    ps_write32(base, sa + st::magic, kStreamMagic);
    ps_write32(base, sa + st::handle, h);
    set_state(base, h, sa, OPENED);
    ps_write32(base, h + hd::stream, sa);
    {
        PngDecs& d = decs();
        std::lock_guard lock(d.m);
        d.streams[sa] = s;
    }
    ps_write32(base, open_info, kInitSpaceAllocated); // CellPngDecOpnInfo
    ps_write32(base, stream_out, sa);
    return lv2::CELL_OK;
}

s32 cellPngDecOpen(PPUContext& ctx, u32 handle, u32 stream_out, u32 src, u32 open_info)
{
    const std::string call = std::format("cellPngDecOpen({:#x}, {:#x}, {:#x}, {:#x})", handle, stream_out, src, open_info);
    return finish(open(ctx, handle, stream_out, src, open_info), call);
}

// The stream of a call: the handle and the stream checked (ARG), the stream in `state` (SEQ).
s32 checked(PPUContext& ctx, u32 h, u32 s, u32 state, std::shared_ptr<Stream>& out)
{
    if (!live_handle(ctx, h)) return error(CELL_PNGDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (!ps_read32(base, h + hd::stream)) return error(CELL_PNGDEC_ERROR_SEQ);
    if (!live_stream(ctx, h, s) || ps_read32(base, h + hd::stream) != s) return error(CELL_PNGDEC_ERROR_ARG);
    if (ps_read32(base, s + st::state) != state) return error(CELL_PNGDEC_ERROR_SEQ);
    out = stream_of(s);
    if (!out) PS_FATAL(kChan, "stream {:#x} has no host side", s);
    return lv2::CELL_OK;
}

s32 close(PPUContext& ctx, u32 h, u32 s)
{
    if (!live_handle(ctx, h)) return error(CELL_PNGDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (!ps_read32(base, h + hd::stream)) return error(CELL_PNGDEC_ERROR_SEQ);
    if (!live_stream(ctx, h, s) || ps_read32(base, h + hd::stream) != s) return error(CELL_PNGDEC_ERROR_ARG);
    std::shared_ptr<Stream> stream;
    {
        PngDecs& d = decs();
        std::lock_guard lock(d.m);
        const auto it = d.streams.find(s);
        if (it != d.streams.end()) {
            stream = it->second;
            d.streams.erase(it);
        }
    }
    if (stream)
        for (const u32 block : stream->blocks) guest_free(ctx, h, block);
    ps_write32(base, s + st::magic, 0);
    guest_free(ctx, h, s);
    ps_write32(base, h + hd::stream, 0);
    ps_write32(base, h + hd::busy, 0);
    ps_write32(base, h + hd::state, 0);
    return lv2::CELL_OK;
}

s32 cellPngDecClose(PPUContext& ctx, u32 handle, u32 stream)
{
    return finish(close(ctx, handle, stream), std::format("cellPngDecClose({:#x}, {:#x})", handle, stream));
}

s32 read_header(PPUContext& ctx, u32 h, u32 sa, u32 info)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, OPENED, s); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    if (!readable(ctx, info, 0x1C)) return error(CELL_PNGDEC_ERROR_ARG);
    if (s->data.size() >= 8 && png_sig_cmp(s->data.data(), 0, 8) != 0) {
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_HEADER);
    }
    if (s->data.size() < 8 || !read_info(*s)) {
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_STREAM_FORMAT);
    }
    // CellPngDecInfo: width, height, components, colour space, bit depth, interlace, chunks.
    s->width = png_get_image_width(s->png, s->info);
    s->height = png_get_image_height(s->png, s->info);
    ps_write32(base, info + 0x00, s->width);
    ps_write32(base, info + 0x04, s->height);
    ps_write32(base, info + 0x08, png_get_channels(s->png, s->info));
    ps_write32(base, info + 0x0C, static_cast<u32>(color_space(png_get_color_type(s->png, s->info))));
    ps_write32(base, info + 0x10, png_get_bit_depth(s->png, s->info));
    ps_write32(base, info + 0x14, png_get_interlace_type(s->png, s->info) == PNG_INTERLACE_ADAM7 ? 1 : 0);
    ps_write32(base, info + 0x18, chunk_information(*s, false));
    png_bytep alphas = nullptr;
    int alpha_count = 0;
    png_color_16p color = nullptr;
    png_fixed_point gamma = 0;
    if (png_get_valid(s->png, s->info, PNG_INFO_sRGB)) s->file_gamma = 0.45455f;
    else if (png_get_gAMA_fixed(s->png, s->info, &gamma)) s->file_gamma = static_cast<float>(gamma / 100000.0);
    if (png_get_tRNS(s->png, s->info, &alphas, &alpha_count, &color)) {
        s->trns = true;
        if (alphas && alpha_count > 0) s->trns_alphas.assign(alphas, alphas + alpha_count);
        if (color) s->trns_color = *color;
    }
    set_state(base, h, sa, HEADER);
    return lv2::CELL_OK;
}

// read_header, with a stream callback asked for the header's bytes as libpng reads them (D-595).
s32 read_header_streamed(PPUContext& ctx, u32 handle, u32 stream, u32 info)
{
    const Reading reading(ctx, stream);
    reading.need(8);
    return reading.result(read_header(ctx, handle, stream, info));
}

s32 cellPngDecReadHeader(PPUContext& ctx, u32 handle, u32 stream, u32 info)
{
    const std::string call = std::format("cellPngDecReadHeader({:#x}, {:#x}, {:#x})", handle, stream, info);
    return finish(read_header_streamed(ctx, handle, stream, info), call);
}

// libpngdec's transformations for the output asked for (RPCS3 pngDecSetParameter, with libpngdec's refusals).
s32 set_parameter(PPUContext& ctx, u32 h, u32 sa, u32 in, u32 out)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, HEADER, s); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    if (!readable(ctx, in, 0x1C) || !readable(ctx, out, 0x28)) return error(CELL_PNGDEC_ERROR_ARG);
    // CellPngDecInParam: commandPtr, outputMode, outputColorSpace, outputBitDepth, outputPackFlag, outputAlphaSelect,
    // outputColorAlpha.
    const s32 mode = static_cast<s32>(ps_read32(base, in + 4)), space = static_cast<s32>(ps_read32(base, in + 8));
    const u32 depth = ps_read32(base, in + 12), pack = ps_read32(base, in + 16), alpha_select = ps_read32(base, in + 20);
    const u32 alpha = ps_read32(base, in + 24);
    const int type = png_get_color_type(s->png, s->info), in_depth = png_get_bit_depth(s->png, s->info);
    const bool trns = png_get_valid(s->png, s->info, PNG_INFO_tRNS) != 0;
    const bool in_alpha = (type & PNG_COLOR_MASK_ALPHA) != 0;
    const auto refuse = [&](std::string_view why) {
        PS_LOG_WARN(kChan, "cellPngDecSetParameter: {} (colour type {}, {} bits; output {} at {} bits, alpha {})", why, type,
            in_depth, space, depth, alpha_select);
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_ARG);
    };
    if (space != GRAYSCALE && space != RGB && space != PALETTE && space != GRAYSCALE_ALPHA && space != RGBA && space != ARGB)
        return refuse("an unknown colour space");
    if ((depth != 8 && depth != 16) || (depth == 16 && in_depth != 16)) return refuse("a bit depth the image cannot give");
    if (space == PALETTE && type != PNG_COLOR_TYPE_PALETTE) return refuse("a palette from an image without one");
    if (has_alpha(space) && alpha_select == 0 && !in_alpha && !trns) return refuse("the image's alpha, which it has not");
    if ((mode != 0 && mode != 1) || pack > 1 || alpha_select > 1) return refuse("an unknown mode");
    png_structp png = s->png;
    const bool gray_in = type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA;
    const bool gray_out = space == GRAYSCALE || space == GRAYSCALE_ALPHA;
    s->to_gray = !gray_in && gray_out;
    // libpngdec's order: 16 bits stripped after the colour conversions (done here for colour to gray).
    if (in_depth == 16 && depth == 8 && !s->to_gray) png_set_strip_16(png); // libpngdec predates png_set_scale_16
    if (space == PALETTE) {
        if (in_depth < 8) png_set_packing(png);
    } else if (gray_in && gray_out) {
        if (in_depth < 8) png_set_packing(png); // the samples as they are, a byte each
    } else {
        if (type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
        if (gray_in && in_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
        if (gray_in) png_set_gray_to_rgb(png);
    }
    if (s->to_gray) {
        // RGB(A) at the image's depth from libpng; gray, and alpha as asked, here.
        s->gray_alpha = space == GRAYSCALE_ALPHA;
        s->fixed_alpha = s->gray_alpha && alpha_select == 1;
        s->alpha_value = static_cast<u8>(alpha);
        if (s->gray_alpha && !s->fixed_alpha && !in_alpha && trns) png_set_tRNS_to_alpha(png);
        if ((!s->gray_alpha || s->fixed_alpha) && (in_alpha || trns)) png_set_strip_alpha(png);
    } else if (has_alpha(space)) {
        if (alpha_select == 1) {
            if (in_alpha || trns) png_set_strip_alpha(png); // the fixed alpha replaces the image's (tRNS too)
            png_set_add_alpha(png, alpha, space == ARGB ? PNG_FILLER_BEFORE : PNG_FILLER_AFTER);
        } else {
            if (!in_alpha && trns) png_set_tRNS_to_alpha(png);
            if (space == ARGB) png_set_swap_alpha(png);
        }
    } else if (in_alpha || trns) {
        png_set_strip_alpha(png); // also the alpha a palette's tRNS gives when it is expanded
    }
    s->passes = static_cast<u32>(png_set_interlace_handling(png));
    if (!update_info(*s)) {
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_STREAM_FORMAT);
    }
    s->png_row_bytes = png_get_rowbytes(png, s->info);
    s->out_mode = mode;
    u32 components = png_get_channels(png, s->info);
    s->row_bytes = s->png_row_bytes;
    if (s->to_gray) {
        s->src_channels = components;
        s->src_bytes = png_get_bit_depth(png, s->info) == 16 ? 2 : 1;
        s->dst_bytes = depth == 16 ? 2 : 1;
        components = s->gray_alpha ? 2 : 1;
        s->row_bytes = u64{s->width} * components * s->dst_bytes;
    }
    // CellPngDecOutParam: outputWidthByte (8 bytes), width, height, components, bit depth, mode, colour space,
    // useMemorySpace.
    ps_write64(base, out + 0x00, s->row_bytes);
    ps_write32(base, out + 0x08, s->width);
    ps_write32(base, out + 0x0C, s->height);
    ps_write32(base, out + 0x10, components);
    ps_write32(base, out + 0x14, depth);
    ps_write32(base, out + 0x18, static_cast<u32>(mode));
    ps_write32(base, out + 0x1C, static_cast<u32>(space));
    ps_write32(base, out + 0x20, 0x8441 + 2 * s->width * components);
    set_state(base, h, sa, PARAMETERS);
    return lv2::CELL_OK;
}

s32 cellPngDecSetParameter(PPUContext& ctx, u32 handle, u32 stream, u32 in_param, u32 out_param)
{
    const std::string call =
        std::format("cellPngDecSetParameter({:#x}, {:#x}, {:#x}, {:#x})", handle, stream, in_param, out_param);
    return finish(set_parameter(ctx, handle, stream, in_param, out_param), call);
}

s32 decode_data(PPUContext& ctx, u32 h, u32 sa, u32 data, u32 ctrl, u32 out)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, PARAMETERS, s); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    if (!readable(ctx, ctrl, 8) || !readable(ctx, out, 0x10)) return error(CELL_PNGDEC_ERROR_ARG);
    const u64 pitch = ps_read64(base, ctrl); // CellPngDecDataCtrlParam: outputBytesPerLine
    if (pitch < s->row_bytes) {
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_ARG);
    }
    const u64 size = s->height ? pitch * (s->height - 1) + s->row_bytes : 0;
    if (size > 0xFFFFFFFFull || !readable(ctx, data, static_cast<u32>(size))) {
        log_call_site(ctx, base, std::format("cellPngDecDecodeData: the image ({:#x}, {} bytes) is not guest memory", data, size));
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_ARG);
    }
    // CellPngDecDataOutInfo: chunkInformation, numText, numUnknownChunk, status (1: stopped).
    ps_write32(base, out + 12, 1);
    std::vector<u8> rows(s->png_row_bytes * s->height);
    if (!read_rows(*s, rows)) {
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_STREAM_FORMAT);
    }
    for (u32 y = 0; y < s->height; y++) {
        u8* line = base + data + static_cast<u32>((s->out_mode == 1 ? s->height - 1 - y : y) * pitch);
        const u8* in = rows.data() + y * s->png_row_bytes;
        if (s->to_gray) rgb_to_gray_row(*s, in, line);
        else std::memcpy(line, in, s->png_row_bytes);
    }
    png_unknown_chunkp unknowns = nullptr;
    ps_write32(base, out + 0, chunk_information(*s, true) | (s->trns ? 1u << 4 : 0)); // tRNS, even once expanded
    ps_write32(base, out + 4, static_cast<u32>(png_get_text(s->png, s->info, nullptr, nullptr)));
    ps_write32(base, out + 8, static_cast<u32>(png_get_unknown_chunks(s->png, s->info, &unknowns)));
    ps_write32(base, out + 12, 0);
    set_state(base, h, sa, DECODED);
    return lv2::CELL_OK;
}

s32 cellPngDecDecodeData(PPUContext& ctx, u32 handle, u32 stream, u32 data, u32 ctrl, u32 out)
{
    const std::string call =
        std::format("cellPngDecDecodeData({:#x}, {:#x}, {:#x}, {:#x}, {:#x})", handle, stream, data, ctrl, out);
    const Reading reading(ctx, stream);
    return finish(reading.result(decode_data(ctx, handle, stream, data, ctrl, out)), call);
}

// ---- the Ext functions (D-595): made from RPCS3 rpcs3/Emu/Cell/Modules/cellPngDec.cpp (pngDecCreate, pngDecOpen,
// cellPngDecExtReadHeader, pngDecSetParameter and pngDecodeData with their ext arguments; the structures from
// cellPngDec.h) on the decoder above, unverified (D-594): not compared with Sony's libpngdec, which serves a game
// importing one wherever the system software is installed.

// CellPngDecExtThreadInParam (spurs, priority[8], maxContention) and CellPngDecExtThreadOutParam (reserved) are
// libpngdec's SPU side on the game's SPURS. pspocket decodes on the calling thread: both are only checked for being
// guest memory (RPCS3 checks nothing and stops on an extThreadOutParam), and the decoder is Create's, the same memory.
s32 cellPngDecExtCreate(PPUContext& ctx, u32 handle_out, u32 thread_in, u32 thread_out, u32 ext_in, u32 ext_out)
{
    const std::string call = std::format("cellPngDecExtCreate({:#x}, {:#x}, {:#x}, {:#x}, {:#x})", handle_out, thread_in,
        thread_out, ext_in, ext_out);
    if ((ext_in && !readable(ctx, ext_in, 16)) || (ext_out && !readable(ctx, ext_out, 4)))
        return finish(error(CELL_PNGDEC_ERROR_ARG), call);
    return finish(create(ctx, handle_out, thread_in, thread_out), call);
}

// Open with stream input: CellPngDecCbCtrlStrm (cbCtrlStrmFunc, cbCtrlStrmArg; a function of 0 is no stream input).
// The source's buffer is the stream's first part, the rest comes from the callback as libpng reads it (stream_more).
// Only from a buffer: a stream callback with a file source is STREAM_FORMAT (RPCS3), answered before the handle is
// taken. CellPngDecOpnParam's selectChunk (RPCS3 stops on one that is not 0) is noted once: every chunk is read and
// kept, so the chunk queries answer for chunks a game did not select too.
s32 ext_open(PPUContext& ctx, u32 h, u32 stream_out, u32 src, u32 open_info, u32 cb_strm, u32 opn_param)
{
    if (!live_handle(ctx, h)) return error(CELL_PNGDEC_ERROR_ARG);
    if ((cb_strm && !readable(ctx, cb_strm, 8)) || (opn_param && !readable(ctx, opn_param, 4)))
        return error(CELL_PNGDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    const u32 strm_fn = cb_strm ? ps_read32(base, cb_strm) : 0, strm_arg = cb_strm ? ps_read32(base, cb_strm + 4) : 0;
    if (strm_fn && readable(ctx, src, 4) && ps_read32(base, src) != 1) {
        PS_LOG_WARN(kChan, "cellPngDecExtOpen: a stream callback with a source that is not a buffer");
        return error(CELL_PNGDEC_ERROR_STREAM_FORMAT);
    }
    if (const u32 select = opn_param ? ps_read32(base, opn_param) : 0) {
        PngDecs& d = decs();
        std::lock_guard lock(d.m);
        if (!d.select_logged)
            PS_LOG_WARN(kChan, "cellPngDecExtOpen: selectChunk {:#x}: every chunk is read and kept (logged once)", select);
        d.select_logged = true;
    }
    if (const s32 r = open(ctx, h, stream_out, src, open_info); r != lv2::CELL_OK || !strm_fn) return r;
    const u32 sa = ps_read32(base, stream_out);
    if (const std::shared_ptr<Stream> s = stream_of(sa)) {
        s->strm_fn = strm_fn;
        s->strm_arg = strm_arg;
        s->strm_block = sa;
    }
    return lv2::CELL_OK;
}

s32 cellPngDecExtOpen(PPUContext& ctx, u32 handle, u32 stream_out, u32 src, u32 open_info, u32 cb_strm, u32 opn_param)
{
    const std::string call = std::format("cellPngDecExtOpen({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x})", handle,
        stream_out, src, open_info, cb_strm, opn_param);
    return finish(ext_open(ctx, handle, stream_out, src, open_info, cb_strm, opn_param), call);
}

// ReadHeader, and CellPngDecExtInfo's reserved word (8 bytes) set to 0 first, when the game gives one (RPCS3).
s32 cellPngDecExtReadHeader(PPUContext& ctx, u32 handle, u32 stream, u32 info, u32 ext_info)
{
    const std::string call = std::format("cellPngDecExtReadHeader({:#x}, {:#x}, {:#x}, {:#x})", handle, stream, info,
        ext_info);
    if (ext_info) {
        std::shared_ptr<Stream> s;
        if (const s32 r = checked(ctx, handle, stream, OPENED, s); r != lv2::CELL_OK) return finish(r, call);
        if (!readable(ctx, ext_info, 8)) return finish(error(CELL_PNGDEC_ERROR_ARG), call);
        ps_write64(guest_base(ctx), ext_info, 0);
    }
    return finish(read_header_streamed(ctx, handle, stream, info), call);
}

// SetParameter, and the parts: CellPngDecExtInParam (bufferMode, outputCounts, spuMode) and CellPngDecExtOutParam
// (outputWidthByte, 8 bytes; outputHeight), both optional (RPCS3). bufferMode must be CELL_PNGDEC_LINE_MODE (1), else
// ARG, refused before the output is set (pspocket's way with refused parameters: the stream fails). outputCounts lines
// go to each display callback, an interlaced image's one at a time (RPCS3); 0 gives the whole image to DecodeData's
// buffer, interlaced or not. extOutParam: the row and the lines of a part (the image's height for 0). spuMode does
// nothing here.
s32 ext_set_parameter(PPUContext& ctx, u32 h, u32 sa, u32 in, u32 out, u32 ext_in, u32 ext_out)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, HEADER, s); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    if ((ext_in && !readable(ctx, ext_in, 12)) || (ext_out && !readable(ctx, ext_out, 12))) return error(CELL_PNGDEC_ERROR_ARG);
    if (ext_in && ps_read32(base, ext_in) != 1) {
        PS_LOG_WARN(kChan, "cellPngDecExtSetParameter: buffer mode {}", ps_read32(base, ext_in));
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_ARG);
    }
    if (const s32 r = set_parameter(ctx, h, sa, in, out); r != lv2::CELL_OK) return r;
    s->command = ps_read32(base, in); // CellPngDecInParam's commandPtr: CELL_PNGDEC_STOP stops between parts
    s->out_components = ps_read32(base, out + 0x10);
    s->out_depth = ps_read32(base, out + 0x14);
    s->output_counts = 0;
    if (ext_in) {
        // RPCS3 makes an interlaced image's 1 even when 0 was asked, against its own 0 (the whole image, no callbacks):
        // here 0 stays 0.
        const u32 counts = ps_read32(base, ext_in + 4);
        s->output_counts = counts && s->passes > 1 ? 1 : counts;
        if (ext_out) {
            ps_write64(base, ext_out, s->row_bytes);
            ps_write32(base, ext_out + 8, s->output_counts ? std::min(s->output_counts, s->height) : s->height);
        }
    }
    return lv2::CELL_OK;
}

s32 cellPngDecExtSetParameter(PPUContext& ctx, u32 handle, u32 stream, u32 in_param, u32 out_param, u32 ext_in,
    u32 ext_out)
{
    const std::string call = std::format("cellPngDecExtSetParameter({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x})", handle,
        stream, in_param, out_param, ext_in, ext_out);
    return finish(ext_set_parameter(ctx, handle, stream, in_param, out_param, ext_in, ext_out), call);
}

// Partial output (RPCS3 pngDecodeData with cbCtrlDisp and outputCounts, its row callback pngDecRowCallback): the image
// goes out outputCounts lines at a time, each part written at the frame's line pitch (outputBytesPerLine) from the
// address CellPngDecDispParam's nextOutputImage gives, then handed to the display callback (CellPngDecCbCtrlDisp:
// cbCtrlDispFunc, cbCtrlDispArg) with CellPngDecDispInfo, which may point nextOutputImage elsewhere for the next part.
// What is pspocket's own, where RPCS3 has none or goes wrong:
// - every part has its callback, the last one too (RPCS3 calls back as the next part starts, so never for the last),
//   with the part's own height and nextOutputStartY the next part's first line (the image's height after the last);
// - the callback is given the game's own CellPngDecDispParam when it passes one (RPCS3 a copy), else the stream's,
//   which starts at `data`; a first nextOutputImage of 0 means `data` too;
// - a non-interlaced image decodes a row at a time, its stream callback called as libpng reads, so the stream and
//   display callbacks interleave; an interlaced one decodes whole first (every pass), then goes out a line at a time,
//   each line once, complete: scanPassCount is 0 throughout (RPCS3 reports libpng's passes);
// - CELL_PNGDEC_STOP at the parameters' commandPtr after a callback stops the decoding: status STOP, CELL_OK;
// - a part's place that is not guest memory: ARG for the first, CB_PARAM for one a callback gave; the stream fails;
// - the row order (outputMode) is top to bottom in parts (RPCS3 too), noted once.
s32 decode_parts(PPUContext& ctx, u32 h, u32 sa, Stream& s, u32 data, u32 ctrl, u32 out, u32 cb_disp, u32 disp_param)
{
    u8* base = guest_base(ctx);
    if (!readable(ctx, ctrl, 8) || !readable(ctx, out, 0x10) || !readable(ctx, cb_disp, 8) || !ps_read32(base, cb_disp) ||
        (disp_param && !readable(ctx, disp_param, 4)))
        return error(CELL_PNGDEC_ERROR_ARG);
    const u64 pitch = ps_read64(base, ctrl);
    if (pitch < s.row_bytes) {
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_ARG);
    }
    const u32 disp_fn = ps_read32(base, cb_disp), disp_arg = ps_read32(base, cb_disp + 4);
    const u32 param = disp_param ? disp_param : sa + st::disp_param;
    u32 image = disp_param ? ps_read32(base, disp_param) : 0;
    if (!image) image = data;
    if (!disp_param) ps_write32(base, param, image);
    // A part of n lines at `at`, as guest memory.
    const auto fits = [&](u32 at, u32 n) {
        const u64 size = pitch * (n - 1) + s.row_bytes;
        return size <= 0xFFFFFFFFull && readable(ctx, at, static_cast<u32>(size));
    };
    if (s.height && !fits(image, std::min(s.output_counts, s.height))) {
        log_call_site(ctx, base, std::format("cellPngDecExtDecodeData: the first part's place ({:#x}) is not guest memory", image));
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_ARG);
    }
    if (s.out_mode == 1) {
        PngDecs& d = decs();
        std::lock_guard lock(d.m);
        if (!d.flip_logged) PS_LOG_WARN(kChan, "cellPngDecExtDecodeData: parts go top to bottom (logged once)");
        d.flip_logged = true;
    }
    const bool interlaced = s.passes > 1;
    if (interlaced && s.png_row_bytes * s.height > kPartsImageMax) { // DecodeData bounds it by the game's buffer
        PS_LOG_WARN(kChan, "cellPngDecExtDecodeData: an interlaced {}x{} image in parts", s.width, s.height);
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_FATAL);
    }
    ps_write32(base, out + 12, 1); // CellPngDecDataOutInfo's status: stopped, until the end
    std::vector<u8> rows(s.png_row_bytes * (interlaced ? s.height : 1));
    if (interlaced && !read_rows(s, rows)) {
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_STREAM_FORMAT);
    }
    const u32 info = sa + st::disp_info;
    u32 start = 0;
    for (u32 y = 0; y < s.height; y++) {
        const u8* in = rows.data();
        if (interlaced) {
            in += y * s.png_row_bytes;
        } else if (!read_row(s, rows.data())) {
            set_state(base, h, sa, FAILED);
            return error(CELL_PNGDEC_ERROR_STREAM_FORMAT);
        }
        u8* line = base + image + static_cast<u32>((y - start) * pitch);
        if (s.to_gray) rgb_to_gray_row(s, in, line);
        else std::memcpy(line, in, s.png_row_bytes);
        const u32 lines = y + 1 - start;
        if (lines < s.output_counts && y + 1 < s.height) continue;
        // CellPngDecDispInfo: outputFrameWidthByte (8 bytes), outputFrameHeight, outputStartXByte (8 bytes at +0x10),
        // outputStartY, outputWidthByte (8 bytes at +0x20), outputHeight, outputBitDepth, outputComponents,
        // nextOutputStartY, scanPassCount, outputImage.
        ps_write64(base, info + 0x00, pitch);
        ps_write32(base, info + 0x08, s.height);
        ps_write64(base, info + 0x10, 0);
        ps_write32(base, info + 0x18, start);
        ps_write64(base, info + 0x20, s.row_bytes);
        ps_write32(base, info + 0x28, lines);
        ps_write32(base, info + 0x2C, s.out_depth);
        ps_write32(base, info + 0x30, s.out_components);
        ps_write32(base, info + 0x34, y + 1);
        ps_write32(base, info + 0x38, 0);
        ps_write32(base, info + 0x3C, image);
        call_guest(ctx, disp_fn, {info, param, disp_arg});
        start = y + 1;
        if (start == s.height) break;
        if (s.command && readable(ctx, s.command, 4) && ps_read32(base, s.command) == 1) { // CELL_PNGDEC_STOP
            set_state(base, h, sa, DECODED);
            return lv2::CELL_OK;
        }
        image = ps_read32(base, param);
        if (!fits(image, std::min(s.output_counts, s.height - start))) {
            log_call_site(ctx, base, std::format("cellPngDecExtDecodeData: the display callback's next place ({:#x}) is "
                                                 "not guest memory", image));
            set_state(base, h, sa, FAILED);
            return error(CELL_PNGDEC_ERROR_CB_PARAM);
        }
    }
    if (!interlaced && !read_end(s)) {
        set_state(base, h, sa, FAILED);
        return error(CELL_PNGDEC_ERROR_STREAM_FORMAT);
    }
    png_unknown_chunkp unknowns = nullptr;
    ps_write32(base, out + 0, chunk_information(s, true) | (s.trns ? 1u << 4 : 0)); // as decode_data
    ps_write32(base, out + 4, static_cast<u32>(png_get_text(s.png, s.info, nullptr, nullptr)));
    ps_write32(base, out + 8, static_cast<u32>(png_get_unknown_chunks(s.png, s.info, &unknowns)));
    ps_write32(base, out + 12, 0);
    set_state(base, h, sa, DECODED);
    return lv2::CELL_OK;
}

// DecodeData, in parts when there is a display callback and ExtSetParameter set outputCounts (RPCS3), else whole into
// `data`.
s32 ext_decode_data(PPUContext& ctx, u32 h, u32 sa, u32 data, u32 ctrl, u32 out, u32 cb_disp, u32 disp_param)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, PARAMETERS, s); r != lv2::CELL_OK) return r;
    if (!cb_disp || !s->output_counts) return decode_data(ctx, h, sa, data, ctrl, out);
    return decode_parts(ctx, h, sa, *s, data, ctrl, out, cb_disp, disp_param);
}

s32 cellPngDecExtDecodeData(PPUContext& ctx, u32 handle, u32 stream, u32 data, u32 ctrl, u32 out, u32 cb_disp,
    u32 disp_param)
{
    const std::string call = std::format("cellPngDecExtDecodeData({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x})",
        handle, stream, data, ctrl, out, cb_disp, disp_param);
    const Reading reading(ctx, stream);
    return finish(reading.result(ext_decode_data(ctx, handle, stream, data, ctrl, out, cb_disp, disp_param)), call);
}

// ---- the chunk queries: after DecodeData only (SEQ); no place for the answer is ARG and a chunk not there SEQ, both
// putting the stream in the error state (libpngdec).

template <class Answer>
s32 query(PPUContext& ctx, u32 h, u32 sa, u32 out, u32 out_size, Answer answer, const char* name)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, DECODED, s); r != lv2::CELL_OK) return finish(r, name);
    u8* base = guest_base(ctx);
    if (!readable(ctx, out, out_size)) {
        set_state(base, h, sa, FAILED);
        return finish(error(CELL_PNGDEC_ERROR_ARG), name);
    }
    if (!answer(*s, base)) {
        set_state(base, h, sa, FAILED);
        return finish(error(CELL_PNGDEC_ERROR_SEQ), name);
    }
    return finish(lv2::CELL_OK, name);
}

// libpng 1.2.8 keeps gAMA and cHRM as floats: the doubles a game reads are those floats.
void write_double(u8* base, u32 at, double v) { ps_write64(base, at, std::bit_cast<u64>(static_cast<double>(static_cast<float>(v)))); }

// A copy of `size` bytes in the game's memory, freed with the stream; 0 when the game's malloc fails.
u32 guest_copy(PPUContext& ctx, u32 h, Stream& s, const void* p, u32 size)
{
    const u32 a = guest_malloc(ctx, h, std::max<u32>(size, 1));
    if (!a) return 0;
    s.blocks.push_back(a);
    std::memcpy(guest_base(ctx) + a, p, size);
    return a;
}

u32 guest_string(PPUContext& ctx, u32 h, Stream& s, const char* str)
{
    return str ? guest_copy(ctx, h, s, str, static_cast<u32>(std::strlen(str)) + 1) : 0;
}

s32 cellPngDecGetTextChunk(PPUContext& ctx, u32 h, u32 sa, u32 num_out, u32 info_out)
{
    if (!readable(ctx, info_out, 4)) num_out = 0; // one check for both
    return query(ctx, h, sa, num_out, 4, [&](Stream& s, u8* base) {
        png_textp text = nullptr;
        const int n = png_get_text(s.png, s.info, &text, nullptr);
        if (n <= 0) return false;
        // CellPngTextInfo (24 bytes): txtType (0 tEXt, 1 zTXt, 2 iTXt), keyword, text, textLength, languageTag,
        // translatedKeyword.
        const u32 array = guest_malloc(ctx, h, 24 * static_cast<u32>(n));
        if (!array) return false;
        s.blocks.push_back(array);
        for (int i = 0; i < n; i++) {
            const png_text& t = text[i];
            const u32 at = array + 24 * static_cast<u32>(i);
            const bool itxt = t.compression == PNG_ITXT_COMPRESSION_NONE || t.compression == PNG_ITXT_COMPRESSION_zTXt;
            ps_write32(base, at + 0, itxt ? 2 : t.compression == PNG_TEXT_COMPRESSION_zTXt ? 1 : 0);
            ps_write32(base, at + 4, guest_string(ctx, h, s, t.key));
            ps_write32(base, at + 8, guest_string(ctx, h, s, t.text));
            ps_write32(base, at + 12, t.text ? static_cast<u32>(std::strlen(t.text)) : 0);
            ps_write32(base, at + 16, itxt ? guest_string(ctx, h, s, t.lang) : 0);
            ps_write32(base, at + 20, itxt ? guest_string(ctx, h, s, t.lang_key) : 0);
        }
        ps_write32(base, num_out, static_cast<u32>(n));
        ps_write32(base, info_out, array);
        return true;
    }, "cellPngDecGetTextChunk");
}

s32 cellPngDecGetPLTE(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 8, [&](Stream& s, u8* base) {
        png_colorp palette = nullptr;
        int n = 0;
        if (!png_get_PLTE(s.png, s.info, &palette, &n)) return false;
        // CellPngPLTE: paletteEntriesNumber, paletteEntries (red, green, blue bytes).
        std::vector<u8> rgb;
        for (int i = 0; i < n; i++) rgb.insert(rgb.end(), {palette[i].red, palette[i].green, palette[i].blue});
        const u32 entries = guest_copy(ctx, h, s, rgb.data(), static_cast<u32>(rgb.size()));
        if (!entries) return false;
        ps_write32(base, out, static_cast<u32>(n));
        ps_write32(base, out + 4, entries);
        return true;
    }, "cellPngDecGetPLTE");
}

s32 cellPngDecGettRNS(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 16, [&](Stream& s, u8* base) {
        if (!s.trns) return false;
        // CellPngTRNS: alphaForPaletteIndex, alphaForPaletteIndexNumber, red, green, blue, gray (16 bits each).
        const bool palette = !s.trns_alphas.empty();
        const auto n = static_cast<u32>(s.trns_alphas.size());
        u32 copy = 0;
        if (palette && !(copy = guest_copy(ctx, h, s, s.trns_alphas.data(), n))) return false;
        ps_write32(base, out + 0, copy);
        ps_write32(base, out + 4, n);
        ps_write16(base, out + 8, palette ? 0 : s.trns_color.red);
        ps_write16(base, out + 10, palette ? 0 : s.trns_color.green);
        ps_write16(base, out + 12, palette ? 0 : s.trns_color.blue);
        ps_write16(base, out + 14, palette ? 0 : s.trns_color.gray);
        return true;
    }, "cellPngDecGettRNS");
}

// With sRGB present, libpng 1.2.8 gives sRGB's gamma and chromaticities, whatever gAMA and cHRM say
// (png_set_sRGB_gAMA_and_cHRM); libpng 1.6 keeps the chunks' own.
bool has_srgb(const Stream& s) { return png_get_valid(s.png, s.info, PNG_INFO_sRGB) != 0; }

s32 cellPngDecGetgAMA(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 8, [&](Stream& s, u8* base) {
        png_fixed_point gamma = 0;
        if (!png_get_gAMA_fixed(s.png, s.info, &gamma)) return false;
        write_double(base, out, has_srgb(s) ? 0.45455 : gamma / 100000.0);
        return true;
    }, "cellPngDecGetgAMA");
}

s32 cellPngDecGetcHRM(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 64, [&](Stream& s, u8* base) {
        png_fixed_point v[8] = {};
        if (!png_get_cHRM_fixed(s.png, s.info, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7])) return false;
        // CellPngCHRM: white x, y, red x, y, green x, y, blue x, y.
        constexpr double kSrgb[8] = {0.3127, 0.3290, 0.64, 0.33, 0.30, 0.60, 0.15, 0.06};
        for (u32 i = 0; i < 8; i++) write_double(base, out + 8 * i, has_srgb(s) ? kSrgb[i] : v[i] / 100000.0);
        return true;
    }, "cellPngDecGetcHRM");
}

s32 cellPngDecGetsRGB(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 4, [&](Stream& s, u8* base) {
        int intent = 0;
        if (!png_get_sRGB(s.png, s.info, &intent)) return false;
        ps_write32(base, out, static_cast<u32>(intent));
        return true;
    }, "cellPngDecGetsRGB");
}

s32 cellPngDecGetpHYs(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 12, [&](Stream& s, u8* base) {
        png_uint_32 x = 0, y = 0;
        int unit = 0;
        if (!png_get_pHYs(s.png, s.info, &x, &y, &unit)) return false;
        ps_write32(base, out, x);
        ps_write32(base, out + 4, y);
        ps_write32(base, out + 8, static_cast<u32>(unit));
        return true;
    }, "cellPngDecGetpHYs");
}

s32 cellPngDecGetoFFs(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 12, [&](Stream& s, u8* base) {
        png_int_32 x = 0, y = 0;
        int unit = 0;
        if (!png_get_oFFs(s.png, s.info, &x, &y, &unit)) return false;
        ps_write32(base, out, static_cast<u32>(x));
        ps_write32(base, out + 4, static_cast<u32>(y));
        ps_write32(base, out + 8, static_cast<u32>(unit));
        return true;
    }, "cellPngDecGetoFFs");
}

s32 cellPngDecGetsBIT(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 20, [&](Stream& s, u8* base) {
        png_color_8p bits = nullptr;
        if (!png_get_sBIT(s.png, s.info, &bits)) return false;
        // CellPngSBIT: red, green, blue, gray, alpha.
        const u32 v[5] = {bits->red, bits->green, bits->blue, bits->gray, bits->alpha};
        for (u32 i = 0; i < 5; i++) ps_write32(base, out + 4 * i, v[i]);
        return true;
    }, "cellPngDecGetsBIT");
}

s32 cellPngDecGetbKGD(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 20, [&](Stream& s, u8* base) {
        png_color_16p color = nullptr;
        if (!png_get_bKGD(s.png, s.info, &color)) return false;
        // CellPngBKGD: paletteIndex (a byte; the three after it untouched), red, green, blue, gray.
        ps_write8(base, out, color->index);
        const u32 v[4] = {color->red, color->green, color->blue, color->gray};
        for (u32 i = 0; i < 4; i++) ps_write32(base, out + 4 + 4 * i, v[i]);
        return true;
    }, "cellPngDecGetbKGD");
}

s32 cellPngDecGettIME(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 7, [&](Stream& s, u8* base) {
        png_timep t = nullptr;
        if (!png_get_tIME(s.png, s.info, &t)) return false;
        // CellPngTIME: year (16 bits), month, day, hour, minute, second.
        ps_write16(base, out, t->year);
        const u8 v[5] = {t->month, t->day, t->hour, t->minute, t->second};
        for (u32 i = 0; i < 5; i++) ps_write8(base, out + 2 + i, v[i]);
        return true;
    }, "cellPngDecGettIME");
}

s32 cellPngDecGetUnknownChunks(PPUContext& ctx, u32 h, u32 sa, u32 chunks_out, u32 num_out)
{
    if (!readable(ctx, num_out, 4)) chunks_out = 0; // one check for both
    return query(ctx, h, sa, chunks_out, 4, [&](Stream& s, u8* base) {
        png_unknown_chunkp chunks = nullptr;
        const int n = png_get_unknown_chunks(s.png, s.info, &chunks);
        if (n <= 0) return false;
        // CellPngUnknownChunk (20 bytes): chunkType (5 bytes), chunkData, length, location.
        const u32 array = guest_malloc(ctx, h, 20 * static_cast<u32>(n));
        if (!array) return false;
        s.blocks.push_back(array);
        for (int i = 0; i < n; i++) {
            const png_unknown_chunk& c = chunks[i];
            const u32 at = array + 20 * static_cast<u32>(i);
            std::memset(base + at, 0, 8);
            std::memcpy(base + at, c.name, 4);
            ps_write32(base, at + 8, guest_copy(ctx, h, s, c.data, static_cast<u32>(c.size)));
            ps_write32(base, at + 12, static_cast<u32>(c.size));
            ps_write32(base, at + 16, c.location);
        }
        ps_write32(base, chunks_out, array);
        ps_write32(base, num_out, static_cast<u32>(n));
        return true;
    }, "cellPngDecGetUnknownChunks");
}

s32 cellPngDecGetiCCP(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 12, [&](Stream& s, u8* base) {
        png_charp name = nullptr;
        int compression = 0;
        png_bytep profile = nullptr;
        png_uint_32 length = 0;
        if (!png_get_iCCP(s.png, s.info, &name, &compression, &profile, &length)) return false;
        // CellPngICCP: profileName, profile, profileLength.
        ps_write32(base, out, guest_string(ctx, h, s, name));
        ps_write32(base, out + 4, guest_copy(ctx, h, s, profile, length));
        ps_write32(base, out + 8, length);
        return true;
    }, "cellPngDecGetiCCP");
}

s32 cellPngDecGethIST(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 8, [&](Stream& s, u8* base) {
        png_uint_16p hist = nullptr;
        if (!png_get_hIST(s.png, s.info, &hist)) return false;
        png_colorp palette = nullptr;
        int n = 0;
        png_get_PLTE(s.png, s.info, &palette, &n);
        // CellPngHIST: histgramEntries (16 bits each), histgramEntriesNumber (one per palette entry).
        std::vector<u8> be;
        for (int i = 0; i < n; i++) be.insert(be.end(), {static_cast<u8>(hist[i] >> 8), static_cast<u8>(hist[i])});
        ps_write32(base, out, guest_copy(ctx, h, s, be.data(), static_cast<u32>(be.size())));
        ps_write32(base, out + 4, static_cast<u32>(n));
        return true;
    }, "cellPngDecGethIST");
}

s32 cellPngDecGetsPLT(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 8, [&](Stream& s, u8* base) {
        png_sPLT_tp palettes = nullptr;
        const int n = png_get_sPLT(s.png, s.info, &palettes);
        if (n <= 0) return false;
        // CellPngSPLT: sPLTentries, sPLTentriesNumber. CellPngSPLTentry (16 bytes): paletteName, sampleDepth (a byte),
        // paletteEntries, paletteEntriesNumber. CellPngPaletteEntries (10 bytes): red, green, blue, alpha, frequency.
        const u32 array = guest_malloc(ctx, h, 16 * static_cast<u32>(n));
        if (!array) return false;
        s.blocks.push_back(array);
        for (int i = 0; i < n; i++) {
            const png_sPLT_t& p = palettes[i];
            std::vector<u8> e;
            for (int k = 0; k < p.nentries; k++) {
                const png_sPLT_entry& x = p.entries[k];
                for (const u16 v : {x.red, x.green, x.blue, x.alpha, x.frequency})
                    e.insert(e.end(), {static_cast<u8>(v >> 8), static_cast<u8>(v)});
            }
            const u32 at = array + 16 * static_cast<u32>(i);
            std::memset(base + at, 0, 16);
            ps_write32(base, at, guest_string(ctx, h, s, p.name));
            ps_write8(base, at + 4, p.depth);
            ps_write32(base, at + 8, guest_copy(ctx, h, s, e.data(), static_cast<u32>(e.size())));
            ps_write32(base, at + 12, static_cast<u32>(p.nentries));
        }
        ps_write32(base, out, array);
        ps_write32(base, out + 4, static_cast<u32>(n));
        return true;
    }, "cellPngDecGetsPLT");
}

s32 cellPngDecGetpCAL(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 28, [&](Stream& s, u8* base) {
        png_charp purpose = nullptr, units = nullptr;
        png_charpp params = nullptr;
        png_int_32 x0 = 0, x1 = 0;
        int type = 0, n = 0;
        if (!png_get_pCAL(s.png, s.info, &purpose, &x0, &x1, &type, &n, &units, &params)) return false;
        // CellPngPCAL: calibrationName, x0, x1, equationType, numberOfParameters, unitName, parameter (pointers).
        std::vector<u32> pointers;
        for (int i = 0; i < n; i++) pointers.push_back(guest_string(ctx, h, s, params[i]));
        std::vector<u8> be;
        for (const u32 p : pointers)
            be.insert(be.end(), {static_cast<u8>(p >> 24), static_cast<u8>(p >> 16), static_cast<u8>(p >> 8), static_cast<u8>(p)});
        ps_write32(base, out, guest_string(ctx, h, s, purpose));
        ps_write32(base, out + 4, static_cast<u32>(x0));
        ps_write32(base, out + 8, static_cast<u32>(x1));
        ps_write32(base, out + 12, static_cast<u32>(type));
        ps_write32(base, out + 16, static_cast<u32>(n));
        ps_write32(base, out + 20, guest_string(ctx, h, s, units));
        ps_write32(base, out + 24, guest_copy(ctx, h, s, be.data(), static_cast<u32>(be.size())));
        return true;
    }, "cellPngDecGetpCAL");
}

s32 cellPngDecGetsCAL(PPUContext& ctx, u32 h, u32 sa, u32 out)
{
    return query(ctx, h, sa, out, 24, [&](Stream& s, u8* base) {
        int unit = 0;
        double width = 0, height = 0;
        if (!png_get_sCAL(s.png, s.info, &unit, &width, &height)) return false;
        // CellPngSCAL: unitSpecifier, pixelWidth and pixelHeight (doubles at +8, +16).
        ps_write32(base, out, static_cast<u32>(unit));
        ps_write64(base, out + 8, std::bit_cast<u64>(width));
        ps_write64(base, out + 16, std::bit_cast<u64>(height));
        return true;
    }, "cellPngDecGetsCAL");
}

} // namespace

void register_cellPngDec(Registry& r)
{
    r.add("cellPngDec", {
        PS_HLE_FUNC(0x157D30C5, cellPngDecCreate),
        PS_HLE_FUNC(0x820DAE1A, cellPngDecDestroy),
        PS_HLE_FUNC(0xD2BC5BFD, cellPngDecOpen),
        PS_HLE_FUNC(0x5B3D1FF1, cellPngDecClose),
        PS_HLE_FUNC(0x9CCDCC95, cellPngDecReadHeader),
        PS_HLE_FUNC(0xE97C9BD4, cellPngDecSetParameter),
        PS_HLE_FUNC(0x2310F155, cellPngDecDecodeData),
        PS_HLE_FUNC(0xB40CA175, cellPngDecGetTextChunk),
        PS_HLE_FUNC(0xE163977F, cellPngDecGetPLTE),
        PS_HLE_FUNC(0xB96FB26E, cellPngDecGettRNS),
        PS_HLE_FUNC(0xB153629C, cellPngDecGetgAMA),
        PS_HLE_FUNC(0x7A062D26, cellPngDecGetcHRM),
        PS_HLE_FUNC(0xE4416E82, cellPngDecGetsRGB),
        PS_HLE_FUNC(0x3D50016A, cellPngDecGetpHYs),
        PS_HLE_FUNC(0x27C921B5, cellPngDecGetoFFs),
        PS_HLE_FUNC(0x30CB334A, cellPngDecGetsBIT),
        PS_HLE_FUNC(0x7585A275, cellPngDecGetbKGD),
        PS_HLE_FUNC(0x35A6846C, cellPngDecGettIME),
        PS_HLE_FUNC(0x609EC7D5, cellPngDecGetUnknownChunks),
        PS_HLE_FUNC(0xF44B6C30, cellPngDecGetiCCP),
        PS_HLE_FUNC(0xB905EBB7, cellPngDecGethIST),
        PS_HLE_FUNC(0xA5CDF57E, cellPngDecGetsPLT),
        PS_HLE_FUNC(0xB4FE75E1, cellPngDecGetpCAL),
        PS_HLE_FUNC(0xC41E1198, cellPngDecGetsCAL),
        // Made from RPCS3, not compared with Sony's libpngdec (D-594, D-595).
        PS_HLE_UNVERIFIED(0x48436B2D, cellPngDecExtCreate),
        PS_HLE_UNVERIFIED(0x0C515302, cellPngDecExtOpen),
        PS_HLE_UNVERIFIED(0x8B33F863, cellPngDecExtReadHeader),
        PS_HLE_UNVERIFIED(0x9E9D7D42, cellPngDecExtSetParameter),
        PS_HLE_UNVERIFIED(0x726FC1D0, cellPngDecExtDecodeData),
    });
}

#else

void register_cellPngDec(Registry&) {} // without libpng, games get Sony's libpngdec (D-175)

#endif

} // namespace pspocket::hle
