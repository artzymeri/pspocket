// cellPngEnc: the PNG encoder (libpngenc), pspocket's own on libpng (D-566).
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellPngEnc.cpp/.h and cellPng.h: the structures, the error codes, and its
// checks of the configuration, the resources and the picture, with the memory libpngenc asks for (0x47A00, or 0x78200
// with SPUs, plus 7 x maxWidth x maxBitDepth / 2, plus addMemSize, plus 0x1780), its command queue depth (4) and
// version (0x270000); RPCS3 encodes nothing. No Sony module was at hand to compare with: the library is firmware-first
// (D-563).
// pspocket's:
// - The handle is the work memory the game gives Open (resource.memAddr), where libpngenc keeps itself; nothing is
//   written there.
// - EncodePicture encodes the picture at once, on the calling thread, and queues its result (at most 4 waiting: then
//   BUSY); WaitForInput says there is room (waiting for it when asked to block); WaitForOutput gives the number of
//   results waiting (waiting for one when asked to block, else EMPTY); GetStreamInfo takes the oldest (EMPTY without
//   one); Reset drops them. A result's state is CELL_OK, STREAM_OVERFLOW (a buffer too small: what fits is written),
//   STREAM_FILE_OPEN (the file cannot be made) or STREAM_ABORT (libpng refused).
// - The picture (width, height, pitchWidth bytes a row, its colour space and bit depth, below 8 bits one sample a byte
//   unless packedPixel) becomes a PNG of the encode colour space: the same, or with the alpha dropped (RGBA or ARGB to
//   RGB, gray and alpha to gray), or added opaque (RGB to RGBA, gray to gray and alpha); ARGB is written as RGBA (PNG
//   has no ARGB). Bit depths as PNG has them (gray 1-16, palette 1-8, the others 8 or 16), 16-bit samples big-endian.
//   Not interlaced; compressionLevel is zlib's, filterType libpng's filter set (none when 0).
// - The ancillary chunks given with their cellPng.h structures (those pspocket's cellPngDec answers with): PLTE, tRNS,
//   cHRM, gAMA, iCCP, sBIT, sRGB, tEXt/zTXt/iTXt, bKGD, hIST, pHYs, tIME, oFFs and sCAL. sPLT, pCAL and unknown chunks
//   are left out (logged once). A palette picture needs a PLTE.
// - Output to the game's buffer or to a file (made or emptied, through lv2's file calls).

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
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
#include "pspocket/lv2/object.hpp"
#include "pspocket/lv2/syscalls.hpp"

#if PSPOCKET_HAVE_LIBPNG
#include <csetjmp>

#include <png.h>
#endif

namespace pspocket::hle {

#if PSPOCKET_HAVE_LIBPNG

namespace {

constexpr std::string_view kModule = "cellPngEnc";
constexpr const char* kChan = "cellPngEnc";

enum : u32 {
    CELL_PNGENC_ERROR_ARG = 0x80611291,
    CELL_PNGENC_ERROR_BUSY = 0x80611293,
    CELL_PNGENC_ERROR_EMPTY = 0x80611294,
    CELL_PNGENC_ERROR_RESET = 0x80611295,
    CELL_PNGENC_ERROR_STREAM_ABORT = 0x806112A1,
    CELL_PNGENC_ERROR_STREAM_OVERFLOW = 0x806112A3,
    CELL_PNGENC_ERROR_STREAM_FILE_OPEN = 0x806112A4,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

enum : u32 { GRAYSCALE = 1, RGB = 2, PALETTE = 4, GRAYSCALE_ALPHA = 9, RGBA = 10, ARGB = 20 };
enum : u32 { LOCATION_FILE = 0, LOCATION_BUFFER = 1 };
enum : u32 {
    CHUNK_PLTE, CHUNK_TRNS, CHUNK_CHRM, CHUNK_GAMA, CHUNK_ICCP, CHUNK_SBIT, CHUNK_SRGB, CHUNK_TEXT, CHUNK_BKGD,
    CHUNK_HIST, CHUNK_PHYS, CHUNK_SPLT, CHUNK_TIME, CHUNK_OFFS, CHUNK_PCAL, CHUNK_SCAL, CHUNK_UNKNOWN,
};
constexpr u32 kFilterAll = 0xF8, kFilterNone = 0x08;
constexpr u32 kQueueDepth = 4, kVersionUpper = 0x270000, kVersionLower = 0;

// CellPngEncConfig {maxWidth, maxHeight, maxBitDepth, enableSpu (b8, +12), addMemSize (+16), exParamList, exParamNum}.
struct Config {
    u32 max_w = 0, max_h = 0, max_depth = 0, add_mem = 0;
    bool spu = false;
};

// RPCS3's check_config and get_mem_size.
bool read_config(u8* base, u32 cfg, Config& c)
{
    c.max_w = ps_read32(base, cfg);
    c.max_h = ps_read32(base, cfg + 4);
    c.max_depth = ps_read32(base, cfg + 8);
    c.spu = ps_read8(base, cfg + 12) != 0;
    c.add_mem = ps_read32(base, cfg + 16);
    const u32 ex_num = ps_read32(base, cfg + 24);
    return c.max_w && c.max_w <= 1000000 && c.max_h && c.max_h <= 1000000 && (c.max_depth == 8 || c.max_depth == 16) &&
           static_cast<s32>(c.add_mem) >= 0 && ex_num == 0;
}

u32 mem_size(const Config& c) { return c.add_mem + (c.spu ? 0x78200 : 0x47A00) + (c.max_depth >> 1) * c.max_w * 7; }

struct Result {
    s32 state = 0;
    u32 location = 0, file_name = 0, addr = 0, limit = 0, size = 0, lines = 0;
    u64 user_data = 0;
};

struct Encoder {
    Config cfg;
    std::deque<Result> results;
    u64 resets = 0;
};

struct State {
    std::mutex mutex;
    std::map<u32, Encoder> encoders; // by handle (the work memory)
    bool chunk_logged = false;
};

State& state() { return HleProcess::current()->state<State>(); }

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

bool in_guest(PPUContext& ctx, u32 addr, u64 size)
{
    if (!addr || u64{addr} + size > 0x100000000ull) return false;
    lv2::PpuThread& t = thread_of(ctx);
    for (u64 at = 0; at < size; at += 0x1000)
        if (!guest_readable(t, static_cast<u32>(addr + at), static_cast<u32>(std::min<u64>(size - at, 0x1000))))
            return false;
    return true;
}

// ------------------------------------------------------------------------------------------------- the picture

int png_color_type(u32 space)
{
    switch (space) {
    case GRAYSCALE: return PNG_COLOR_TYPE_GRAY;
    case RGB: return PNG_COLOR_TYPE_RGB;
    case PALETTE: return PNG_COLOR_TYPE_PALETTE;
    case GRAYSCALE_ALPHA: return PNG_COLOR_TYPE_GRAY_ALPHA;
    case RGBA:
    case ARGB: return PNG_COLOR_TYPE_RGB_ALPHA;
    default: return -1;
    }
}

u32 components(u32 space)
{
    switch (space) {
    case GRAYSCALE:
    case PALETTE: return 1;
    case GRAYSCALE_ALPHA: return 2;
    case RGB: return 3;
    default: return 4;
    }
}

bool depth_ok(u32 space, u32 depth)
{
    switch (space) {
    case GRAYSCALE: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case PALETTE: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case RGB:
    case RGBA:
    case ARGB:
    case GRAYSCALE_ALPHA: return depth == 8 || depth == 16;
    default: return false;
    }
}

// The conversions from the picture's colour space to the encoded one: the same, alpha dropped, or alpha added.
bool conversion_ok(u32 from, u32 to)
{
    const auto rgbx = [](u32 s) { return s == RGB || s == RGBA || s == ARGB; };
    if (from == to) return true;
    if (rgbx(from) && rgbx(to)) return true;
    return (from == GRAYSCALE && to == GRAYSCALE_ALPHA) || (from == GRAYSCALE_ALPHA && to == GRAYSCALE);
}

struct Picture {
    u32 width = 0, height = 0, pitch = 0, space = 0, depth = 0, addr = 0;
    bool packed = false;
};

// One row of the picture in the encoded colour space's PNG layout.
void build_row(const u8* in, const Picture& p, u32 space, std::vector<u8>& out)
{
    const u32 w = p.width, d = p.depth;
    if (d < 8) { // gray or palette: PNG packs the samples MSB first
        const u32 bytes = (w * d + 7) / 8;
        out.assign(bytes, 0);
        if (p.packed) {
            std::memcpy(out.data(), in, bytes);
            return;
        }
        const u32 mask = (1u << d) - 1;
        for (u32 x = 0; x < w; x++) {
            const u32 bit = x * d;
            out[bit / 8] |= static_cast<u8>((in[x] & mask) << (8 - d - bit % 8));
        }
        return;
    }
    const u32 bps = d / 8, in_comp = components(p.space), out_comp = components(space);
    out.resize(static_cast<usize>(w) * out_comp * bps);
    const u8 opaque = 0xFF;
    for (u32 x = 0; x < w; x++) {
        const u8* src = in + static_cast<usize>(x) * in_comp * bps;
        u8* dst = out.data() + static_cast<usize>(x) * out_comp * bps;
        // The picture's components as gray/alpha or R, G, B, A (each bps bytes); a missing alpha is opaque.
        const u8* comp[4] = {nullptr, nullptr, nullptr, nullptr};
        if (p.space == ARGB) {
            comp[0] = src + bps;
            comp[1] = src + 2 * bps;
            comp[2] = src + 3 * bps;
            comp[3] = src;
        } else {
            for (u32 c = 0; c < in_comp; c++) comp[c] = src + c * bps;
        }
        const auto put = [&](u32 slot, const u8* from) {
            if (from) std::memcpy(dst + slot * bps, from, bps);
            else std::memset(dst + slot * bps, opaque, bps);
        };
        switch (space) {
        case GRAYSCALE: put(0, comp[0]); break;
        case GRAYSCALE_ALPHA:
            put(0, comp[0]);
            put(1, p.space == GRAYSCALE_ALPHA ? comp[1] : nullptr);
            break;
        case RGB:
            put(0, comp[0]);
            put(1, comp[1]);
            put(2, comp[2]);
            break;
        default: // RGBA, and ARGB written as RGBA
            put(0, comp[0]);
            put(1, comp[1]);
            put(2, comp[2]);
            put(3, (p.space == RGBA || p.space == ARGB) ? comp[3] : nullptr);
            break;
        }
    }
}

// ------------------------------------------------------------------------------------------------- libpng

struct Writer {
    std::vector<u8> bytes;
};

void write_data(png_structp png, png_bytep data, size_t length)
{
    auto* w = static_cast<Writer*>(png_get_io_ptr(png));
    w->bytes.insert(w->bytes.end(), data, data + length);
}

void flush_data(png_structp) {}

void png_failed(png_structp png, png_const_charp message)
{
    PS_LOG_WARN(kChan, "libpng: {}", message ? message : "(no message)");
    png_longjmp(png, 1);
}

void png_warned(png_structp, png_const_charp message) { PS_LOG_DEBUG(kChan, "libpng: {}", message ? message : ""); }

std::string guest_cstr(PPUContext& ctx, u32 addr, u32 max = 0x10000)
{
    if (!addr) return {};
    return read_cstr(thread_of(ctx), addr, max).text;
}

// What libpng keeps pointers to until the image is written.
struct ChunkData {
    std::vector<png_color> palette;
    std::vector<u8> trans_alpha;
    png_color_16 trans_color{}, background{};
    png_color_8 sig_bit{};
    std::vector<std::string> strings;
    std::vector<png_text> texts;
    std::vector<u8> profile;
    std::vector<png_uint_16> hist;
    png_time time{};
};

// The ancillary chunks onto libpng's info (behind the caller's setjmp).
void set_chunks(PPUContext& ctx, png_structp png, png_infop info, u32 list, u32 count, ChunkData& d, bool& palette)
{
    u8* base = gb(ctx);
    State& s = state();
    for (u32 i = 0; i < count; i++) {
        const u32 type = ps_read32(base, list + 8 * i), at = ps_read32(base, list + 8 * i + 4);
        if (!at) continue;
        switch (type) {
        case CHUNK_PLTE: {
            const u32 n = std::min(ps_read32(base, at), 256u), entries = ps_read32(base, at + 4);
            d.palette.resize(n);
            for (u32 k = 0; k < n; k++)
                d.palette[k] = {ps_read8(base, entries + 3 * k), ps_read8(base, entries + 3 * k + 1),
                    ps_read8(base, entries + 3 * k + 2)};
            png_set_PLTE(png, info, d.palette.data(), static_cast<int>(n));
            palette = true;
            break;
        }
        case CHUNK_TRNS: {
            const u32 alphas = ps_read32(base, at), n = std::min(ps_read32(base, at + 4), 256u);
            d.trans_alpha.resize(n);
            for (u32 k = 0; k < n; k++) d.trans_alpha[k] = ps_read8(base, alphas + k);
            d.trans_color.red = ps_read16(base, at + 8);
            d.trans_color.green = ps_read16(base, at + 10);
            d.trans_color.blue = ps_read16(base, at + 12);
            d.trans_color.gray = ps_read16(base, at + 14);
            png_set_tRNS(png, info, n ? d.trans_alpha.data() : nullptr, static_cast<int>(n), &d.trans_color);
            break;
        }
        case CHUNK_CHRM: {
            f64 v[8];
            for (u32 k = 0; k < 8; k++) v[k] = ps_readf64(base, at + 8 * k);
            png_set_cHRM(png, info, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
            break;
        }
        case CHUNK_GAMA: png_set_gAMA(png, info, ps_readf64(base, at)); break;
        case CHUNK_ICCP: {
            const u32 len = ps_read32(base, at + 8), prof = ps_read32(base, at + 4);
            if (!len || !in_guest(ctx, prof, len)) break;
            d.profile.assign(base + prof, base + prof + len);
            d.strings.push_back(guest_cstr(ctx, ps_read32(base, at), 80));
            png_set_iCCP(png, info, d.strings.back().c_str(), 0, d.profile.data(), len);
            break;
        }
        case CHUNK_SBIT:
            d.sig_bit.red = static_cast<png_byte>(ps_read32(base, at));
            d.sig_bit.green = static_cast<png_byte>(ps_read32(base, at + 4));
            d.sig_bit.blue = static_cast<png_byte>(ps_read32(base, at + 8));
            d.sig_bit.gray = static_cast<png_byte>(ps_read32(base, at + 12));
            d.sig_bit.alpha = static_cast<png_byte>(ps_read32(base, at + 16));
            png_set_sBIT(png, info, &d.sig_bit);
            break;
        case CHUNK_SRGB: png_set_sRGB(png, info, static_cast<int>(ps_read32(base, at))); break;
        case CHUNK_TEXT: {
            // CellPngTextInfo {txtType, keyword, text, textLength, languageTag, translatedKeyword}.
            const s32 kind = static_cast<s32>(ps_read32(base, at));
            d.strings.reserve(d.strings.size() + 4);
            d.strings.push_back(guest_cstr(ctx, ps_read32(base, at + 4), 80));
            const u32 text = ps_read32(base, at + 8), text_len = ps_read32(base, at + 12);
            d.strings.push_back(text_len && in_guest(ctx, text, text_len)
                    ? std::string(reinterpret_cast<const char*>(base + text), text_len)
                    : guest_cstr(ctx, text));
            d.strings.push_back(guest_cstr(ctx, ps_read32(base, at + 16)));
            d.strings.push_back(guest_cstr(ctx, ps_read32(base, at + 20)));
            png_text t{};
            t.compression = kind == 1 ? PNG_TEXT_COMPRESSION_zTXt
                            : kind == 2 ? PNG_ITXT_COMPRESSION_NONE
                                        : PNG_TEXT_COMPRESSION_NONE;
            const usize n = d.strings.size();
            t.key = d.strings[n - 4].data();
            t.text = d.strings[n - 3].data();
            t.text_length = d.strings[n - 3].size();
            t.lang = d.strings[n - 2].data();
            t.lang_key = d.strings[n - 1].data();
            d.texts.push_back(t);
            break;
        }
        case CHUNK_BKGD:
            d.background.index = ps_read8(base, at);
            d.background.red = static_cast<png_uint_16>(ps_read32(base, at + 4));
            d.background.green = static_cast<png_uint_16>(ps_read32(base, at + 8));
            d.background.blue = static_cast<png_uint_16>(ps_read32(base, at + 12));
            d.background.gray = static_cast<png_uint_16>(ps_read32(base, at + 16));
            png_set_bKGD(png, info, &d.background);
            break;
        case CHUNK_HIST: {
            const u32 entries = ps_read32(base, at), n = std::min(ps_read32(base, at + 4), 256u);
            d.hist.resize(n);
            for (u32 k = 0; k < n; k++) d.hist[k] = ps_read16(base, entries + 2 * k);
            if (n) png_set_hIST(png, info, d.hist.data());
            break;
        }
        case CHUNK_PHYS:
            png_set_pHYs(png, info, ps_read32(base, at), ps_read32(base, at + 4),
                static_cast<int>(ps_read32(base, at + 8)));
            break;
        case CHUNK_TIME:
            d.time = {ps_read16(base, at), ps_read8(base, at + 2), ps_read8(base, at + 3), ps_read8(base, at + 4),
                ps_read8(base, at + 5), ps_read8(base, at + 6)};
            png_set_tIME(png, info, &d.time);
            break;
        case CHUNK_OFFS:
            png_set_oFFs(png, info, static_cast<png_int_32>(ps_read32(base, at)),
                static_cast<png_int_32>(ps_read32(base, at + 4)), static_cast<int>(ps_read32(base, at + 8)));
            break;
        case CHUNK_SCAL:
            png_set_sCAL(png, info, static_cast<int>(ps_read32(base, at)), ps_readf64(base, at + 8),
                ps_readf64(base, at + 16));
            break;
        default: {
            std::lock_guard lock(s.mutex);
            if (!s.chunk_logged) {
                s.chunk_logged = true;
                PS_LOG_INFO(kChan, "cellPngEncEncodePicture: ancillary chunk type {} (sPLT, pCAL, unknown) left out",
                    type);
            }
            break;
        }
        }
    }
    if (!d.texts.empty()) png_set_text(png, info, d.texts.data(), static_cast<int>(d.texts.size()));
}

// The PNG of a picture, or false when libpng refused it.
bool encode(PPUContext& ctx, const Picture& p, u32 space, u32 level, u32 filters, u32 chunk_list, u32 chunk_count,
    std::vector<u8>& out)
{
    u8* base = gb(ctx);
    Writer w;
    ChunkData d;
    std::vector<u8> row;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, png_failed, png_warned);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        return false;
    }
    volatile bool ok = false; // set past the last libpng call, read after a longjmp
    if (!setjmp(png_jmpbuf(png))) {
        png_set_write_fn(png, &w, write_data, flush_data);
        png_set_IHDR(png, info, p.width, p.height, static_cast<int>(p.depth), png_color_type(space), PNG_INTERLACE_NONE,
            PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
        png_set_compression_level(png, static_cast<int>(level));
        png_set_filter(png, PNG_FILTER_TYPE_BASE, static_cast<int>(filters ? filters & kFilterAll : kFilterNone));
        bool palette = false;
        set_chunks(ctx, png, info, chunk_list, chunk_count, d, palette);
        if (space == PALETTE && !palette) png_error(png, "a palette picture without a PLTE chunk");
        png_write_info(png, info);
        for (u32 y = 0; y < p.height; y++) {
            build_row(base + p.addr + static_cast<usize>(y) * p.pitch, p, space, row);
            png_write_row(png, row.data());
        }
        png_write_end(png, info);
        ok = true;
    }
    png_destroy_write_struct(&png, &info);
    if (ok) out = std::move(w.bytes);
    return ok;
}

// The stream to a file: made or emptied, written, closed. False when it cannot be.
bool write_file(PPUContext& ctx, u32 path, const std::vector<u8>& bytes)
{
    u8* base = gb(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    StackVar fd(ctx, base, 8);
    constexpr s32 kWriteCreateTrunc = 0x1 | 0x40 | 0x200, kMode = 0x180;
    const s32 r = lv2::sys_fs_open(t, path, kWriteCreateTrunc, fd.addr(), kMode, 0, 0);
    if (r != lv2::CELL_OK) {
        PS_LOG_WARN(kChan, "cellPngEncEncodePicture: {} cannot be made ({})", read_cstr(t, path, 0x400).text,
            lv2::error_name(r));
        return false;
    }
    const u32 f = ps_read32(base, fd.addr());
    bool ok = true;
    {
        constexpr u32 kChunk = 0x1000;
        StackVar buf(ctx, base, kChunk + 16);
        const u32 nwrite = buf.addr() + kChunk;
        for (usize at = 0; ok && at < bytes.size(); at += kChunk) {
            const u32 n = static_cast<u32>(std::min<usize>(kChunk, bytes.size() - at));
            std::memcpy(base + buf.addr(), bytes.data() + at, n);
            ok = lv2::sys_fs_write(t, f, buf.addr(), n, nwrite) == lv2::CELL_OK && ps_read64(base, nwrite) == n;
        }
    }
    lv2::sys_fs_close(t, f);
    return ok;
}

// Waits a millisecond, the thread still answering the process's end.
void nap(PPUContext& ctx)
{
    if (!lv2::sleep_us(thread_of(ctx).waiter, 1000)) throw lv2::ThreadExit{};
}

// ------------------------------------------------------------------------------------------------- the calls

// cellPngEncQueryAttr(config, attr): CellPngEncAttr {memSize, cmdQueueDepth (u8), versionUpper (+8), versionLower}.
s32 cellPngEncQueryAttr(PPUContext& ctx, u32 config, u32 attr)
{
    u8* base = gb(ctx);
    Config c;
    if (!attr || !config || !in_guest(ctx, config, 28) || !in_guest(ctx, attr, 16) || !read_config(base, config, c))
        return error(CELL_PNGENC_ERROR_ARG);
    ps_write32(base, attr, mem_size(c) + 0x1780);
    ps_write8(base, attr + 4, kQueueDepth);
    ps_write32(base, attr + 8, kVersionUpper);
    ps_write32(base, attr + 12, kVersionLower);
    return lv2::CELL_OK;
}

// Open and OpenEx: RPCS3's checks (priorities, the memory's address and size), then the encoder at the memory.
s32 open(PPUContext& ctx, u32 config, u32 resource, u32 handle, bool ex)
{
    u8* base = gb(ctx);
    Config c;
    if (!handle || !in_guest(ctx, handle, 4) || !config || !in_guest(ctx, config, 28) ||
        !read_config(base, config, c) || !resource || !in_guest(ctx, resource, ex ? 24 : 16))
        return error(CELL_PNGENC_ERROR_ARG);
    const u32 mem = ps_read32(base, resource), size = ps_read32(base, resource + 4);
    const s32 ppu_prio = static_cast<s32>(ps_read32(base, resource + 8));
    bool priorities = ppu_prio >= 0 && ppu_prio <= 0xBFF;
    if (ex) {
        for (u32 i = 0; i < 8; i++) priorities = priorities && ps_read8(base, resource + 16 + i) <= 15;
    } else {
        const s32 spu_prio = static_cast<s32>(ps_read32(base, resource + 12));
        priorities = priorities && spu_prio >= 0 && spu_prio <= 0xFF;
    }
    if (!mem || !size || !priorities || size < mem_size(c) + 0x1780) return error(CELL_PNGENC_ERROR_ARG);
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.encoders.try_emplace(mem, Encoder{c, {}, 0}).second) return error(CELL_PNGENC_ERROR_ARG); // already open
    ps_write32(base, handle, mem);
    PS_LOG_INFO(kChan, "cellPngEncOpen{}: handle {:#x}, up to {}x{} at {} bits", ex ? "Ex" : "", mem, c.max_w, c.max_h,
        c.max_depth);
    return lv2::CELL_OK;
}

s32 cellPngEncOpen(PPUContext& ctx, u32 config, u32 resource, u32 handle)
{
    return open(ctx, config, resource, handle, false);
}

s32 cellPngEncOpenEx(PPUContext& ctx, u32 config, u32 resource, u32 handle)
{
    return open(ctx, config, resource, handle, true);
}

s32 cellPngEncClose(PPUContext&, u32 handle)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!handle || !s.encoders.erase(handle)) return error(CELL_PNGENC_ERROR_ARG);
    return lv2::CELL_OK;
}

s32 cellPngEncReset(PPUContext&, u32 handle)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    const auto e = s.encoders.find(handle);
    if (!handle || e == s.encoders.end()) return error(CELL_PNGENC_ERROR_ARG);
    e->second.results.clear();
    e->second.resets++;
    return lv2::CELL_OK;
}

// cellPngEncWaitForInput(handle, bool block): room for a picture (fewer than 4 results waiting).
s32 cellPngEncWaitForInput(PPUContext& ctx, u32 handle, u32 block)
{
    State& s = state();
    for (;;) {
        {
            std::lock_guard lock(s.mutex);
            const auto e = s.encoders.find(handle);
            if (!handle || e == s.encoders.end()) return error(CELL_PNGENC_ERROR_ARG);
            if (e->second.results.size() < kQueueDepth) return lv2::CELL_OK;
            if (!(block & 0xFF)) return error(CELL_PNGENC_ERROR_BUSY);
        }
        nap(ctx);
    }
}

// cellPngEncEncodePicture(handle, picture, encodeParam, outputParam).
s32 cellPngEncEncodePicture(PPUContext& ctx, u32 handle, u32 picture, u32 param, u32 output)
{
    u8* base = gb(ctx);
    State& s = state();
    Config c;
    {
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (!handle || e == s.encoders.end()) return error(CELL_PNGENC_ERROR_ARG);
        c = e->second.cfg;
    }
    if (!picture || !in_guest(ctx, picture, 40) || !param || !in_guest(ctx, param, 24) || !output ||
        !in_guest(ctx, output, 16))
        return error(CELL_PNGENC_ERROR_ARG);
    // CellPngEncPicture {width, height, pitchWidth, colorSpace, bitDepth, packedPixel (b8, +20), pictureAddr (+24),
    // userData (u64, +32)}.
    Picture p;
    p.width = ps_read32(base, picture);
    p.height = ps_read32(base, picture + 4);
    p.pitch = ps_read32(base, picture + 8);
    p.space = ps_read32(base, picture + 12);
    p.depth = ps_read32(base, picture + 16);
    p.packed = ps_read8(base, picture + 20) != 0;
    p.addr = ps_read32(base, picture + 24);
    const u64 user_data = ps_read64(base, picture + 32);
    // CellPngEncEncodeParam {enableSpu (b8), encodeColorSpace (+4), compressionLevel, filterType, ancillaryChunkList
    // (+16), ancillaryChunkNum}; CellPngEncOutputParam {location, streamFileName, streamAddr, limitSize}.
    const u32 space = ps_read32(base, param + 4), level = ps_read32(base, param + 8);
    const u32 filters = ps_read32(base, param + 12);
    const u32 chunk_list = ps_read32(base, param + 16), chunk_count = ps_read32(base, param + 20);
    const u32 location = ps_read32(base, output), file_name = ps_read32(base, output + 4);
    const u32 stream = ps_read32(base, output + 8), limit = ps_read32(base, output + 12);
    const u64 row_bytes = p.depth < 8 && !p.packed ? p.width : (u64{p.width} * components(p.space) * p.depth + 7) / 8;
    if (!p.width || !p.height || (p.packed && p.depth >= 8) || !p.addr || p.space > ARGB || p.width > c.max_w ||
        p.height > c.max_h || p.depth > c.max_depth || !depth_ok(p.space, p.depth) || !depth_ok(space, p.depth) ||
        !conversion_ok(p.space, space) || p.pitch < row_bytes || level > 9 || (filters & ~kFilterAll) ||
        (chunk_count && !in_guest(ctx, chunk_list, u64{chunk_count} * 8)) ||
        !in_guest(ctx, p.addr, u64{p.pitch} * (p.height - 1) + row_bytes) ||
        (location == LOCATION_FILE ? !file_name : location != LOCATION_BUFFER || !stream || !limit)) {
        PS_LOG_WARN(kChan, "cellPngEncEncodePicture: {}x{} pitch {} colour space {} at {} bits{} to {}, level {}, "
                           "filters {:#x}, location {}: refused",
            p.width, p.height, p.pitch, p.space, p.depth, p.packed ? " packed" : "", space, level, filters, location);
        return error(CELL_PNGENC_ERROR_ARG);
    }
    {
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (e == s.encoders.end()) return error(CELL_PNGENC_ERROR_ARG);
        if (e->second.results.size() >= kQueueDepth) return error(CELL_PNGENC_ERROR_BUSY);
    }

    Result r;
    r.location = location;
    r.file_name = location == LOCATION_FILE ? file_name : 0;
    r.addr = location == LOCATION_BUFFER ? stream : 0;
    r.limit = limit;
    r.user_data = user_data;
    std::vector<u8> png;
    if (!encode(ctx, p, space, level, filters, chunk_list, chunk_count, png)) {
        r.state = error(CELL_PNGENC_ERROR_STREAM_ABORT);
    } else if (location == LOCATION_BUFFER) {
        const u32 n = static_cast<u32>(std::min<usize>(png.size(), limit));
        if (in_guest(ctx, stream, n)) std::memcpy(base + stream, png.data(), n);
        r.size = n;
        r.lines = p.height;
        if (png.size() > limit) r.state = error(CELL_PNGENC_ERROR_STREAM_OVERFLOW);
    } else if (write_file(ctx, file_name, png)) {
        r.size = static_cast<u32>(png.size());
        r.lines = p.height;
    } else {
        r.state = error(CELL_PNGENC_ERROR_STREAM_FILE_OPEN);
    }
    std::lock_guard lock(s.mutex);
    const auto e = s.encoders.find(handle);
    if (e == s.encoders.end()) return error(CELL_PNGENC_ERROR_ARG); // closed meanwhile
    e->second.results.push_back(r);
    return lv2::CELL_OK;
}

// cellPngEncWaitForOutput(handle, uint32_t *streamInfoNum, bool block): the results waiting.
s32 cellPngEncWaitForOutput(PPUContext& ctx, u32 handle, u32 num, u32 block)
{
    u8* base = gb(ctx);
    State& s = state();
    u64 resets = 0;
    bool first = true;
    for (;;) {
        {
            std::lock_guard lock(s.mutex);
            const auto e = s.encoders.find(handle);
            if (!handle || e == s.encoders.end() || !num || !in_guest(ctx, num, 4)) return error(CELL_PNGENC_ERROR_ARG);
            if (first) resets = e->second.resets;
            first = false;
            if (e->second.resets != resets) return error(CELL_PNGENC_ERROR_RESET);
            const u32 n = static_cast<u32>(e->second.results.size());
            if (n || !(block & 0xFF)) {
                ps_write32(base, num, n);
                return n ? lv2::CELL_OK : error(CELL_PNGENC_ERROR_EMPTY);
            }
        }
        nap(ctx);
    }
}

// cellPngEncGetStreamInfo(handle, CellPngEncStreamInfo *): {state, location, streamFileName, streamAddr, limitSize,
// streamSize, processedLine, userData (u64, +32)}.
s32 cellPngEncGetStreamInfo(PPUContext& ctx, u32 handle, u32 info)
{
    u8* base = gb(ctx);
    State& s = state();
    std::lock_guard lock(s.mutex);
    const auto e = s.encoders.find(handle);
    if (!handle || e == s.encoders.end() || !info || !in_guest(ctx, info, 40)) return error(CELL_PNGENC_ERROR_ARG);
    if (e->second.results.empty()) return error(CELL_PNGENC_ERROR_EMPTY);
    const Result r = e->second.results.front();
    e->second.results.pop_front();
    const u32 words[] = {static_cast<u32>(r.state), r.location, r.file_name, r.addr, r.limit, r.size, r.lines, 0};
    for (u32 i = 0; i < std::size(words); i++) ps_write32(base, info + 4 * i, words[i]);
    ps_write64(base, info + 32, r.user_data);
    return lv2::CELL_OK;
}

} // namespace

void register_cellPngEnc(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x496CFCD0, cellPngEncQueryAttr),
        PS_HLE_FUNC(0x19256DC5, cellPngEncOpen),
        PS_HLE_FUNC(0xC82558CE, cellPngEncOpenEx),
        PS_HLE_FUNC(0x117CD726, cellPngEncClose),
        PS_HLE_FUNC(0x662BD637, cellPngEncWaitForInput),
        PS_HLE_FUNC(0x5B546CA4, cellPngEncEncodePicture),
        PS_HLE_FUNC(0x90EF2963, cellPngEncWaitForOutput),
        PS_HLE_FUNC(0x585269BC, cellPngEncGetStreamInfo),
        PS_HLE_FUNC(0x6AC91DE3, cellPngEncReset),
    });
    r.firmware_first(kModule);
}

#else

void register_cellPngEnc(Registry&) {} // without libpng, games get Sony's libpngenc (D-175)

#endif

} // namespace pspocket::hle
