// cellJpgEnc: the JPEG encoder (libjpgenc), pspocket's own on its baseline encoder (jpeg_encoder.cpp, D-615, D-616).
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellJpgEnc.cpp/.h: the error codes and the enums (the colour spaces, the
// sampling formats, the DCT methods, the compression modes, the locations), with stub bodies and no structures. No
// Sony module was at hand to compare with: the library is firmware-first (D-563).
// The structures are inferred (D-616), each from cellPngEnc's (libpngenc is the same SDK family and design; RPCS3
// cellPngEnc.h) with what JPEG has in place of what PNG has, to check on the Mac:
// - CellJpgEncConfig {maxWidth, maxHeight, enableSpu (b8, +8), addMemSize (+12), exParamList, exParamNum}: PNG's but
//   for its maxBitDepth (JPEG's samples are 8 bits).
// - CellJpgEncAttr, CellJpgEncResource, CellJpgEncResourceEx, CellJpgEncOutputParam and CellJpgEncStreamInfo: PNG's.
// - CellJpgEncPicture {width, height, pitchWidth, colorSpace, pictureAddr (+16), userData (u64, +24)}: PNG's but for
//   its bitDepth and packedPixel. CellJpgEncPicture2 (EncodePicture2) adds samplingFormat at +16 (pictureAddr +20): a
//   YCbCr picture's planes (see read_picture).
// - CellJpgEncEncodeParam {enableSpu (b8), encodeColorSpace (+4), samplingFormat (+8), dctMethod (+12),
//   compressionMode (+16), compressionModeParam (+20)}: PNG's first two, then the enums RPCS3 gives in its order.
// - The enums: RPCS3 writes them as one, so that its values run on from ARGB (20); each is an enum of its own from 0
//   here, as libpngenc's location is (FILE 0, BUFFER 1): sampling formats YCbCr444 0, 422 1, 420 2, 411 3, FULL 4;
//   compression modes CONSTANT_QUALITY 0, STREAM_SIZE_LIMIT 1. The DCT methods are RPCS3's (QUALITY 0, FAST 5, as
//   libjpgdec's methods are).
// pspocket's, as cellPngEnc (D-566) has it: the handle is the work memory the game gives Open (nothing is written
// there); EncodePicture encodes on the calling thread and queues the result, at most 4 waiting (then BUSY);
// WaitForInput, WaitForOutput, GetStreamInfo and Reset as cellPngEnc's.

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "jpeg_encoder.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/object.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kModule = "cellJpgEnc";
constexpr const char* kChan = "cellJpgEnc";

enum : u32 {
    CELL_JPGENC_ERROR_ARG = 0x80611191,
    CELL_JPGENC_ERROR_BUSY = 0x80611193,
    CELL_JPGENC_ERROR_EMPTY = 0x80611194,
    CELL_JPGENC_ERROR_RESET = 0x80611195,
    CELL_JPGENC_ERROR_STREAM_ABORT = 0x806111A1,
    CELL_JPGENC_ERROR_STREAM_OVERFLOW = 0x806111A3,
    CELL_JPGENC_ERROR_STREAM_FILE_OPEN = 0x806111A4,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

enum : u32 { GRAYSCALE = 1, RGB = 2, YCBCR = 3, RGBA = 10, ARGB = 20 };
enum : u32 { SAMPLING_444 = 0, SAMPLING_422 = 1, SAMPLING_420 = 2, SAMPLING_411 = 3, SAMPLING_FULL = 4 };
enum : u32 { DCT_QUALITY = 0, DCT_FAST = 5 };
enum : u32 { CONSTANT_QUALITY = 0, STREAM_SIZE_LIMIT = 1 };
enum : u32 { LOCATION_FILE = 0, LOCATION_BUFFER = 1 };
constexpr u32 kQueueDepth = 4, kVersionUpper = 0x270000, kVersionLower = 0; // libpngenc's (D-616)
constexpr u32 kMaxDimension = 0xFFFF;                                     // SOF's 16-bit fields

// CellJpgEncConfig {maxWidth, maxHeight, enableSpu (b8, +8), addMemSize (+12), exParamList (+16), exParamNum (+20)}.
struct Config {
    u32 max_w = 0, max_h = 0, add_mem = 0;
    bool spu = false;
};

// RPCS3 cellPngEnc's check_config, JPEG's dimensions: both 1 to 65535, addMemSize not negative, no extended
// parameters.
bool read_config(u8* base, u32 cfg, Config& c)
{
    c.max_w = ps_read32(base, cfg);
    c.max_h = ps_read32(base, cfg + 4);
    c.spu = ps_read8(base, cfg + 8) != 0;
    c.add_mem = ps_read32(base, cfg + 12);
    const u32 ex_num = ps_read32(base, cfg + 20);
    return c.max_w && c.max_w <= kMaxDimension && c.max_h && c.max_h <= kMaxDimension &&
           static_cast<s32>(c.add_mem) >= 0 && ex_num == 0;
}

// The work memory asked for (pspocket's own measure, D-616: libjpgenc's is not known): a fixed part, more with SPUs,
// and 64 bytes a column (the MCU rows of 16 lines a baseline encoder keeps of each component and their coefficients).
u32 mem_size(const Config& c) { return c.add_mem + (c.spu ? 0x40000 : 0x10000) + c.max_w * 0x40; }

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
    bool spu_logged = false;
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

bool known_space(u32 space)
{
    return space == GRAYSCALE || space == RGB || space == YCBCR || space == RGBA || space == ARGB;
}

u32 pixel_bytes(u32 space)
{
    switch (space) {
    case GRAYSCALE: return 1;
    case RGB:
    case YCBCR: return 3;
    default: return 4;
    }
}

// ------------------------------------------------------------------------------------------------- the picture

struct Picture {
    u32 width = 0, height = 0, pitch = 0, space = 0, sampling = SAMPLING_FULL, addr = 0;
    u64 user_data = 0;
    // A planar picture's chroma planes (EncodePicture2): their subsampling, place and pitch.
    u32 hs = 1, vs = 1, cb = 0, cr = 0, chroma_pitch = 0;
};

// The bytes a picture covers from its address, or 0 when it cannot be laid out.
u64 picture_extent(const Picture& p)
{
    if (p.sampling == SAMPLING_FULL) return u64{p.pitch} * (p.height - 1) + u64{p.width} * pixel_bytes(p.space);
    const u64 rows = (p.height + p.vs - 1) / p.vs;
    return u64{p.pitch} * p.height + 2 * u64{p.chroma_pitch} * rows;
}

// CellJpgEncPicture {width, height, pitchWidth, colorSpace, pictureAddr (+16), userData (u64, +24)}: the pixels
// interleaved at full size (gray 1 byte; RGB R G B; YCbCr Y Cb Cr; RGBA R G B A; ARGB A R G B; an alpha is not
// encoded). CellJpgEncPicture2 {width, height, pitchWidth, colorSpace, samplingFormat (+16), pictureAddr (+20),
// userData (+24)}: with FULL the same pixels; with a YCbCr sampling format a YCbCr picture's planes one after the other
// from pictureAddr, as cellVdec's YUV420_PLANAR lays them out: Y (height rows, pitchWidth bytes apart), then Cb, then
// Cr (each the height over the vertical subsampling, rounded up, rows of pitchWidth over the horizontal subsampling,
// rounded up, bytes apart). Inferred (D-616). Why it is refused, or nullptr.
const char* read_picture(PPUContext& ctx, u32 at, bool two, Picture& p)
{
    u8* base = gb(ctx);
    p.width = ps_read32(base, at);
    p.height = ps_read32(base, at + 4);
    p.pitch = ps_read32(base, at + 8);
    p.space = ps_read32(base, at + 12);
    p.sampling = two ? ps_read32(base, at + 16) : SAMPLING_FULL;
    p.addr = ps_read32(base, at + (two ? 20 : 16));
    p.user_data = ps_read64(base, at + 24);
    if (!p.width || !p.height) return "no width or height";
    if (!known_space(p.space)) return "an unknown colour space";
    if (p.sampling > SAMPLING_FULL) return "an unknown sampling format";
    if (p.sampling != SAMPLING_FULL) {
        if (p.space != YCBCR) return "planes of a picture that is not YCbCr";
        p.hs = p.sampling == SAMPLING_411 ? 4 : p.sampling == SAMPLING_444 ? 1 : 2;
        p.vs = p.sampling == SAMPLING_420 ? 2 : 1;
        p.chroma_pitch = (p.pitch + p.hs - 1) / p.hs;
    }
    if (u64{p.pitch} < u64{p.width} * (p.sampling == SAMPLING_FULL ? pixel_bytes(p.space) : 1))
        return "a pitch shorter than a row";
    if (!p.addr || !in_guest(ctx, p.addr, picture_extent(p))) return "pixels that are not guest memory";
    if (p.sampling != SAMPLING_FULL) { // within guest memory, so within 32 bits
        p.cb = p.addr + p.pitch * p.height;
        p.cr = p.cb + p.chroma_pitch * ((p.height + p.vs - 1) / p.vs);
    }
    return nullptr;
}

// The picture's samples at full size, as the JPEG's components: Y (gray: the sample; colour: T.871's luma), and Cb
// and Cr for a colour JPEG (a gray picture's are 128). A planar picture's chroma is repeated over the samples it
// covers (and averaged back by the encoder: the same values when the JPEG's sampling is the picture's).
jpeg::Planes planes_of(const u8* base, const Picture& p, bool gray_jpeg)
{
    jpeg::Planes out;
    out.width = p.width;
    out.height = p.height;
    out.components = gray_jpeg ? 1 : 3;
    const usize n = usize{p.width} * p.height;
    out.y.resize(n);
    if (!gray_jpeg) {
        out.cb.resize(n);
        out.cr.resize(n);
    }
    for (u32 y = 0; y < p.height; y++) {
        const u8* row = base + p.addr + usize{y} * p.pitch;
        for (u32 x = 0; x < p.width; x++) {
            const usize i = usize{y} * p.width + x;
            u8 luma = 0, cb = 128, cr = 128;
            if (p.sampling != SAMPLING_FULL) {
                luma = row[x];
                const usize c = usize{y / p.vs} * p.chroma_pitch + x / p.hs;
                cb = base[p.cb + c];
                cr = base[p.cr + c];
            } else {
                const u8* px = row + usize{x} * pixel_bytes(p.space);
                switch (p.space) {
                case GRAYSCALE: luma = px[0]; break;
                case YCBCR:
                    luma = px[0];
                    cb = px[1];
                    cr = px[2];
                    break;
                case ARGB: px++; [[fallthrough]];
                default: // RGB, RGBA, and ARGB past its alpha
                    if (gray_jpeg) luma = jpeg::rgb_to_y(px[0], px[1], px[2]);
                    else jpeg::rgb_to_ycbcr(px[0], px[1], px[2], luma, cb, cr);
                    break;
                }
            }
            out.y[i] = luma;
            if (!gray_jpeg) {
                out.cb[i] = cb;
                out.cr[i] = cr;
            }
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------- output

// The stream to a file: made or emptied, written, closed. False when it cannot be.
bool write_file(PPUContext& ctx, u32 path, const u8* bytes, usize size)
{
    u8* base = gb(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    StackVar fd(ctx, base, 8);
    constexpr s32 kWriteCreateTrunc = 0x1 | 0x40 | 0x200, kMode = 0x180;
    const s32 r = lv2::sys_fs_open(t, path, kWriteCreateTrunc, fd.addr(), kMode, 0, 0);
    if (r != lv2::CELL_OK) {
        PS_LOG_WARN(kChan, "cellJpgEncEncodePicture: {} cannot be made ({})", read_cstr(t, path, 0x400).text,
            lv2::error_name(r));
        return false;
    }
    const u32 f = ps_read32(base, fd.addr());
    bool ok = true;
    {
        constexpr u32 kChunk = 0x1000;
        StackVar buf(ctx, base, kChunk + 16);
        const u32 nwrite = buf.addr() + kChunk;
        for (usize at = 0; ok && at < size; at += kChunk) {
            const u32 n = static_cast<u32>(std::min<usize>(kChunk, size - at));
            std::memcpy(base + buf.addr(), bytes + at, n);
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

// cellJpgEncQueryAttr(config, attr): CellJpgEncAttr {memSize, cmdQueueDepth (u8), versionUpper (+8), versionLower}.
s32 cellJpgEncQueryAttr(PPUContext& ctx, u32 config, u32 attr)
{
    u8* base = gb(ctx);
    Config c;
    if (!attr || !config || !in_guest(ctx, config, 24) || !in_guest(ctx, attr, 16) || !read_config(base, config, c))
        return error(CELL_JPGENC_ERROR_ARG);
    ps_write32(base, attr, mem_size(c));
    ps_write8(base, attr + 4, kQueueDepth);
    ps_write32(base, attr + 8, kVersionUpper);
    ps_write32(base, attr + 12, kVersionLower);
    return lv2::CELL_OK;
}

// Open and OpenEx: RPCS3 cellPngEnc's checks (priorities, the memory's address and size), then the encoder at the
// memory. CellJpgEncResource {memAddr, memSize, ppuThreadPriority, spuThreadPriority}; CellJpgEncResourceEx {memAddr,
// memSize, ppuThreadPriority, spurs, priority[8] (+16)}.
s32 open(PPUContext& ctx, u32 config, u32 resource, u32 handle, bool ex)
{
    u8* base = gb(ctx);
    Config c;
    if (!handle || !in_guest(ctx, handle, 4) || !config || !in_guest(ctx, config, 24) ||
        !read_config(base, config, c) || !resource || !in_guest(ctx, resource, ex ? 24 : 16))
        return error(CELL_JPGENC_ERROR_ARG);
    const u32 mem = ps_read32(base, resource), size = ps_read32(base, resource + 4);
    const s32 ppu_prio = static_cast<s32>(ps_read32(base, resource + 8));
    bool priorities = ppu_prio >= 0 && ppu_prio <= 0xBFF;
    if (ex) {
        for (u32 i = 0; i < 8; i++) priorities = priorities && ps_read8(base, resource + 16 + i) <= 15;
    } else {
        const s32 spu_prio = static_cast<s32>(ps_read32(base, resource + 12));
        priorities = priorities && spu_prio >= 0 && spu_prio <= 0xFF;
    }
    if (!mem || !size || !priorities || size < mem_size(c)) return error(CELL_JPGENC_ERROR_ARG);
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.encoders.try_emplace(mem, Encoder{c, {}, 0}).second) return error(CELL_JPGENC_ERROR_ARG); // already open
    ps_write32(base, handle, mem);
    PS_LOG_INFO(kChan, "cellJpgEncOpen{}: handle {:#x}, up to {}x{}", ex ? "Ex" : "", mem, c.max_w, c.max_h);
    return lv2::CELL_OK;
}

s32 cellJpgEncOpen(PPUContext& ctx, u32 config, u32 resource, u32 handle)
{
    return open(ctx, config, resource, handle, false);
}

s32 cellJpgEncOpenEx(PPUContext& ctx, u32 config, u32 resource, u32 handle)
{
    return open(ctx, config, resource, handle, true);
}

s32 cellJpgEncClose(PPUContext&, u32 handle)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (!handle || !s.encoders.erase(handle)) return error(CELL_JPGENC_ERROR_ARG);
    return lv2::CELL_OK;
}

s32 cellJpgEncReset(PPUContext&, u32 handle)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    const auto e = s.encoders.find(handle);
    if (!handle || e == s.encoders.end()) return error(CELL_JPGENC_ERROR_ARG);
    e->second.results.clear();
    e->second.resets++;
    return lv2::CELL_OK;
}

// cellJpgEncWaitForInput(handle, bool block): room for a picture (fewer than 4 results waiting).
s32 cellJpgEncWaitForInput(PPUContext& ctx, u32 handle, u32 block)
{
    State& s = state();
    for (;;) {
        {
            std::lock_guard lock(s.mutex);
            const auto e = s.encoders.find(handle);
            if (!handle || e == s.encoders.end()) return error(CELL_JPGENC_ERROR_ARG);
            if (e->second.results.size() < kQueueDepth) return lv2::CELL_OK;
            if (!(block & 0xFF)) return error(CELL_JPGENC_ERROR_BUSY);
        }
        nap(ctx);
    }
}

// The JPEG for the picture: at the quality asked for; in the stream size limit mode at the highest quality up to it
// whose stream fits `limit` (the qualities searched by halves; each stream's size is measured, so the one taken fits),
// else quality 1's, too large. `fits`: whether it fits the limit.
std::vector<u8> compress(const jpeg::Planes& planes, jpeg::EncodeSettings settings, bool size_limit, u32 limit,
    bool& fits)
{
    fits = true;
    if (!size_limit) return jpeg::encode(planes, settings);
    const int highest = settings.quality;
    std::vector<u8> best = jpeg::encode(planes, settings);
    if (best.size() <= limit) return best;
    int lo = 1, hi = highest - 1, chosen = 0;
    while (lo <= hi) {
        settings.quality = (lo + hi) / 2;
        std::vector<u8> s = jpeg::encode(planes, settings);
        if (s.size() <= limit) {
            chosen = settings.quality;
            best = std::move(s);
            lo = settings.quality + 1;
        } else {
            hi = settings.quality - 1;
        }
    }
    if (chosen) {
        PS_LOG_DEBUG(kChan, "cellJpgEncEncodePicture: quality {} for {} bytes ({} asked)", chosen, limit, highest);
        return best;
    }
    fits = false;
    settings.quality = 1;
    return jpeg::encode(planes, settings);
}

// cellJpgEncEncodePicture(handle, picture, encodeParam, outputParam), and EncodePicture2 with CellJpgEncPicture2.
// CellJpgEncEncodeParam {enableSpu (b8), encodeColorSpace (+4), samplingFormat (+8), dctMethod (+12),
// compressionMode (+16), compressionModeParam (+20)}; CellJpgEncOutputParam {location, streamFileName, streamAddr,
// limitSize}. encodeColorSpace GRAYSCALE gives a gray JPEG, the other colour spaces a YCbCr one (JFIF) at the
// sampling format's chroma (FULL as 4:4:4); compressionModeParam is the quality (1 to 100), in the stream size limit
// mode the highest, lowered until the stream fits limitSize. Inferred (D-616).
s32 encode_picture(PPUContext& ctx, u32 handle, u32 picture, u32 param, u32 output, bool two)
{
    const char* call = two ? "cellJpgEncEncodePicture2" : "cellJpgEncEncodePicture";
    u8* base = gb(ctx);
    State& s = state();
    Config c;
    {
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (!handle || e == s.encoders.end()) return error(CELL_JPGENC_ERROR_ARG);
        c = e->second.cfg;
    }
    if (!picture || !in_guest(ctx, picture, 32) || !param || !in_guest(ctx, param, 24) || !output ||
        !in_guest(ctx, output, 16))
        return error(CELL_JPGENC_ERROR_ARG);
    const bool spu = ps_read8(base, param) != 0;
    const u32 space = ps_read32(base, param + 4), sampling = ps_read32(base, param + 8);
    const u32 method = ps_read32(base, param + 12), mode = ps_read32(base, param + 16);
    const u32 quality = ps_read32(base, param + 20);
    const u32 location = ps_read32(base, output), file_name = ps_read32(base, output + 4);
    const u32 stream = ps_read32(base, output + 8), limit = ps_read32(base, output + 12);
    Picture p;
    const char* why = read_picture(ctx, picture, two, p);
    const auto refuse_if = [&](bool bad, const char* reason) {
        if (!why && bad) why = reason;
    };
    refuse_if(p.width > c.max_w || p.height > c.max_h, "a picture larger than the encoder's maxWidth or maxHeight");
    refuse_if(!known_space(space), "an unknown encode colour space");
    refuse_if(sampling > SAMPLING_FULL, "an unknown sampling format");
    refuse_if(method != DCT_QUALITY && method != DCT_FAST, "an unknown DCT method");
    refuse_if(mode != CONSTANT_QUALITY && mode != STREAM_SIZE_LIMIT, "an unknown compression mode");
    refuse_if(quality < 1 || quality > 100, "a quality that is not 1 to 100");
    refuse_if(location == LOCATION_FILE ? !file_name : location != LOCATION_BUFFER || !stream || !limit,
        "no file name, or no buffer, or an unknown location");
    refuse_if(mode == STREAM_SIZE_LIMIT && !limit, "the stream size limit mode without a limit");
    if (why) {
        PS_LOG_WARN(kChan, "{}: {} ({}x{} pitch {} colour space {} sampling {} to colour space {} sampling {}, DCT {}, "
                           "mode {}, quality {}, location {}): refused",
            call, why, p.width, p.height, p.pitch, p.space, p.sampling, space, sampling, method, mode, quality,
            location);
        return error(CELL_JPGENC_ERROR_ARG);
    }
    {
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (e == s.encoders.end()) return error(CELL_JPGENC_ERROR_ARG);
        if (e->second.results.size() >= kQueueDepth) return error(CELL_JPGENC_ERROR_BUSY);
        if (spu && !s.spu_logged) PS_LOG_INFO(kChan, "{}: encoded on the calling thread, not on SPUs (logged once)", call);
        s.spu_logged = s.spu_logged || spu;
    }

    Result r;
    r.location = location;
    r.file_name = location == LOCATION_FILE ? file_name : 0;
    r.addr = location == LOCATION_BUFFER ? stream : 0;
    r.limit = limit;
    r.user_data = p.user_data;
    jpeg::EncodeSettings settings;
    settings.quality = static_cast<int>(quality);
    settings.fast = method == DCT_FAST;
    settings.sampling = sampling == SAMPLING_422   ? jpeg::Sampling::YCbCr422
                        : sampling == SAMPLING_420 ? jpeg::Sampling::YCbCr420
                        : sampling == SAMPLING_411 ? jpeg::Sampling::YCbCr411
                                                   : jpeg::Sampling::YCbCr444;
    bool fits = true;
    const std::vector<u8> jpg =
        compress(planes_of(base, p, space == GRAYSCALE), settings, mode == STREAM_SIZE_LIMIT, limit, fits);
    if (jpg.empty()) {
        r.state = error(CELL_JPGENC_ERROR_STREAM_ABORT);
    } else {
        // What fits the buffer (or, in the stream size limit mode, the limit) is written.
        const bool cut = location == LOCATION_BUFFER || mode == STREAM_SIZE_LIMIT;
        const usize n = cut ? std::min<usize>(jpg.size(), limit) : jpg.size();
        if (location == LOCATION_BUFFER) {
            if (in_guest(ctx, stream, n)) std::memcpy(base + stream, jpg.data(), n);
            r.size = static_cast<u32>(n);
            r.lines = p.height;
        } else if (write_file(ctx, file_name, jpg.data(), n)) {
            r.size = static_cast<u32>(n);
            r.lines = p.height;
        } else {
            r.state = error(CELL_JPGENC_ERROR_STREAM_FILE_OPEN);
        }
        if (!r.state && (!fits || n < jpg.size())) r.state = error(CELL_JPGENC_ERROR_STREAM_OVERFLOW);
    }
    std::lock_guard lock(s.mutex);
    const auto e = s.encoders.find(handle);
    if (e == s.encoders.end()) return error(CELL_JPGENC_ERROR_ARG); // closed meanwhile
    e->second.results.push_back(r);
    return lv2::CELL_OK;
}

s32 cellJpgEncEncodePicture(PPUContext& ctx, u32 handle, u32 picture, u32 param, u32 output)
{
    return encode_picture(ctx, handle, picture, param, output, false);
}

s32 cellJpgEncEncodePicture2(PPUContext& ctx, u32 handle, u32 picture, u32 param, u32 output)
{
    return encode_picture(ctx, handle, picture, param, output, true);
}

// cellJpgEncWaitForOutput(handle, uint32_t *streamInfoNum, bool block): the results waiting.
s32 cellJpgEncWaitForOutput(PPUContext& ctx, u32 handle, u32 num, u32 block)
{
    u8* base = gb(ctx);
    State& s = state();
    u64 resets = 0;
    bool first = true;
    for (;;) {
        {
            std::lock_guard lock(s.mutex);
            const auto e = s.encoders.find(handle);
            if (!handle || e == s.encoders.end() || !num || !in_guest(ctx, num, 4)) return error(CELL_JPGENC_ERROR_ARG);
            if (first) resets = e->second.resets;
            first = false;
            if (e->second.resets != resets) return error(CELL_JPGENC_ERROR_RESET);
            const u32 n = static_cast<u32>(e->second.results.size());
            if (n || !(block & 0xFF)) {
                ps_write32(base, num, n);
                return n ? lv2::CELL_OK : error(CELL_JPGENC_ERROR_EMPTY);
            }
        }
        nap(ctx);
    }
}

// cellJpgEncGetStreamInfo(handle, CellJpgEncStreamInfo *): {state, location, streamFileName, streamAddr, limitSize,
// streamSize, processedLine, userData (u64, +32)}.
s32 cellJpgEncGetStreamInfo(PPUContext& ctx, u32 handle, u32 info)
{
    u8* base = gb(ctx);
    State& s = state();
    std::lock_guard lock(s.mutex);
    const auto e = s.encoders.find(handle);
    if (!handle || e == s.encoders.end() || !info || !in_guest(ctx, info, 40)) return error(CELL_JPGENC_ERROR_ARG);
    if (e->second.results.empty()) return error(CELL_JPGENC_ERROR_EMPTY);
    const Result r = e->second.results.front();
    e->second.results.pop_front();
    const u32 words[] = {static_cast<u32>(r.state), r.location, r.file_name, r.addr, r.limit, r.size, r.lines, 0};
    for (u32 i = 0; i < std::size(words); i++) ps_write32(base, info + 4 * i, words[i]);
    ps_write64(base, info + 32, r.user_data);
    return lv2::CELL_OK;
}

} // namespace

void register_cellJpgEnc(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0x12D9B6C5, cellJpgEncQueryAttr),
        PS_HLE_FUNC(0xA4BFAE51, cellJpgEncOpen),
        PS_HLE_FUNC(0x6F2D371C, cellJpgEncOpenEx),
        PS_HLE_FUNC(0x969FC5F7, cellJpgEncClose),
        PS_HLE_FUNC(0x2AE79BE8, cellJpgEncWaitForInput),
        PS_HLE_FUNC(0xA9E81214, cellJpgEncEncodePicture),
        PS_HLE_FUNC(0x636DC89E, cellJpgEncEncodePicture2),
        PS_HLE_FUNC(0x9B4E3A74, cellJpgEncWaitForOutput),
        PS_HLE_FUNC(0x4262E880, cellJpgEncGetStreamInfo),
        PS_HLE_FUNC(0x0CF2B78B, cellJpgEncReset),
    });
    r.firmware_first(kModule);
}

} // namespace pspocket::hle
