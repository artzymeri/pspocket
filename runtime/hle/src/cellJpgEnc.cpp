// cellJpgEnc: the JPEG encoder (libjpgenc), pspocket's own (D-616), made to answer and write what Sony's libjpgenc does
// (D-913, compared call for call with libjpgenc.sprx, tests/hle/test_jpgenc_compare.cpp). RPCS3's
// (rpcs3/Emu/Cell/Modules/cellJpgEnc.cpp/.h) gave the error codes.
//
// What libjpgenc is: IJG's libjpeg 6b behind a command queue, the design of libpngenc (D-913: cellPngEnc's). Open makes
// an encoder in the game's work memory (its handle the memory's first 128-byte boundary) for one colour space and
// sampling; EncodePicture and EncodePicture2 check the picture and its parameters, then queue a command (four at most:
// BUSY); its thread takes the commands while fewer than four streams wait, encodes each and queues its stream
// information (GetStreamInfo takes the oldest). Reset turns the commands waiting into skipped streams and skips the next
// picture too. Here the commands are encoded as soon as there is room, on the caller's thread.
//
// The structures: CellJpgEncConfig {maxWidth, maxHeight, encodeColorSpace (gray 1, RGB 2, YCbCr 3), samplingFormat
// (4:4:4 0, 4:2:2 1, 4:2:0 2, 4:1:1 3, none 4 for gray and RGB), enableSpu (b8, +16), exParamList, exParamNum};
// CellJpgEncAttr {memSize, cmdQueueDepth (u8), versionUpper, versionLower}; CellJpgEncResource {memAddr, memSize,
// ppuThreadPriority, spuThreadPriority} and ResourceEx {memAddr, memSize, ppuThreadPriority, spurs, priority[8]};
// CellJpgEncPicture {width, height, colorSpace, pictureAddr, userData (u64, +16)} (rows of the picture's own width);
// CellJpgEncPicture2 {width, height, pitchWidth, colorSpace, pictureAddr, (4 bytes), userData (u64, +24)};
// CellJpgEncEncodeParam {enableSpu (b8), restartInterval, dctMethod (QUALITY 0, FAST 5), compressionMode (CONSTANT_QUALITY
// 0, STREAM_SIZE_LIMIT 1), compressionModeParam (the quality, 0 to 100), exParamList, exParamNum}, each extended
// parameter {marker, data, size}, a marker segment written after the stream's SOI (its marker code the low byte);
// CellJpgEncOutputParam {location (FILE 0, BUFFER 1), streamFileName, streamAddr, limitSize}; CellJpgEncStreamInfo
// {state, location, streamFileName, streamAddr, limitSize, streamSize, processedLine, quality, userData (u64, +32)}.
// The stream is jpeg_encoder.cpp's (libjpeg 6b, without JFIF or Adobe markers, as libjpgenc's).

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
    CELL_JPGENC_ERROR_FATAL = 0x80611196,
    CELL_JPGENC_ERROR_STREAM_SKIP = 0x806111A2,
    CELL_JPGENC_ERROR_STREAM_OVERFLOW = 0x806111A3,
    CELL_JPGENC_ERROR_STREAM_FILE_OPEN = 0x806111A4,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

enum : u32 { GRAYSCALE = 1, RGB = 2, YCBCR = 3, RGBA = 10, ARGB = 20 };
enum : u32 { SAMPLING_444 = 0, SAMPLING_422 = 1, SAMPLING_420 = 2, SAMPLING_411 = 3, SAMPLING_NONE = 4 };
enum : u32 { DCT_QUALITY = 0, DCT_FAST = 5 };
enum : u32 { CONSTANT_QUALITY = 0, STREAM_SIZE_LIMIT = 1 };
enum : u32 { LOCATION_FILE = 0, LOCATION_BUFFER = 1 };
constexpr u32 kQueueDepth = 4, kStreamQueue = 8, kVersionUpper = 0x270000, kVersionLower = 4;

struct Config {
    u32 max_w = 0, max_h = 0, space = 0, sampling = 0;
    bool spu = false;
};

// libjpgenc's checks of a configuration: sizes 1 to 65535, a YCbCr encoder at 4:4:4 to 4:1:1, a gray or RGB one
// without sampling, no extended parameters.
bool read_config(u8* base, u32 cfg, Config& c)
{
    c.max_w = ps_read32(base, cfg);
    c.max_h = ps_read32(base, cfg + 4);
    c.space = ps_read32(base, cfg + 8);
    c.sampling = ps_read32(base, cfg + 12);
    c.spu = ps_read8(base, cfg + 16) != 0;
    const u32 ex_num = ps_read32(base, cfg + 24);
    if (!c.max_w || !c.max_h || c.max_w > 0xFFFF || c.max_h > 0xFFFF || !c.space || c.space > 3 || c.sampling > 4)
        return false;
    if (c.sampling == SAMPLING_NONE ? c.space == YCBCR : c.space != YCBCR) return false;
    return ex_num == 0;
}

// The work memory libjpgenc asks for, less its 0x1780 bytes of encoder state: a fixed part (more with SPUs) and its
// rows' buffers, by the encoded colour space and sampling, for the width rounded up to 16 (u32 arithmetic, as it).
u32 mem_size(const Config& c)
{
    const u32 w = (c.max_w + 15) & ~15u;
    u32 n = c.spu ? 0x388A0 : 0x1B630;
    if (c.space == GRAYSCALE) n += w * 12;
    else if (c.space == RGB) n += w * 30;
    else
        switch (c.sampling) {
        case SAMPLING_444: n += w * 30; break;
        case SAMPLING_422: n += w * 22; break;
        case SAMPLING_411: n += w * 18; break;
        default: n += w * 33; break; // 4:2:0
        }
    return n;
}

// ------------------------------------------------------------------------------------------------- the queues

// A command as EncodePicture2 copies it: the picture, the encode parameters and the output parameters.
struct Command {
    u32 width = 0, height = 0, pitch = 0, space = 0, addr = 0;
    u64 user_data = 0;
    bool spu = false;
    u32 restart = 0, dct = 0, mode = 0, quality = 0, ex_list = 0, ex_num = 0;
    u32 location = 0, file_name = 0, stream = 0, limit = 0;
};

struct Result {
    s32 state = 0;
    u32 location = 0, file_name = 0, addr = 0, limit = 0, size = 0, lines = 0, quality = 0;
    u64 user_data = 0;
};

Result result_of(const Command& c, s32 state)
{
    Result r;
    r.state = state;
    r.location = c.location;
    r.file_name = c.file_name;
    r.addr = c.stream;
    r.limit = c.limit;
    r.quality = c.quality;
    r.user_data = c.user_data;
    return r;
}

struct Encoder {
    Config cfg;
    std::deque<Command> commands; // waiting to be encoded (four at most)
    std::deque<Result> streams;   // waiting to be taken (eight at most)
    bool skip_next = false;       // Reset leaves libjpgenc's abort flag up: the next picture is skipped
    bool pumping = false;
    bool in_flight = false;       // the first command is being encoded (it keeps its place until its stream is queued)
    bool reset_pending = false;   // a Reset came while it was: the others are skipped once its stream is queued
    u64 generation = 0, resets = 0;
};

struct State {
    std::mutex mutex;
    std::map<u32, Encoder> encoders; // by handle
    u64 opens = 0;
    bool spu_logged = false, limit_logged = false;
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

// The bytes of a pixel EncodePicture2 takes from each colour space (0: none it takes) for an encoder of `encoder`'s:
// gray only for a gray encoder, YCbCr not for an RGB one.
u32 pixel_bytes(u32 space, u32 encoder)
{
    switch (space) {
    case GRAYSCALE: return encoder == GRAYSCALE ? 1 : 0;
    case RGB: return 3;
    case YCBCR: return encoder == RGB ? 0 : 3;
    case RGBA:
    case ARGB: return 4;
    default: return 0;
    }
}

// ------------------------------------------------------------------------------------------------- encoding

// The stream to the game's buffer (libjpeg's destination: bytes until the buffer is full, then the encoder stops) or
// collected for a file.
struct Output {
    PPUContext& ctx;
    bool to_buffer = false;
    u32 addr = 0, limit = 0;
    std::vector<u8> bytes;
    u64 written = 0;

    bool write(const u8* p, usize n)
    {
        if (!to_buffer) {
            bytes.insert(bytes.end(), p, p + n);
            written += n;
            return true;
        }
        const u64 room = limit - written, take = std::min<u64>(room, n);
        std::memcpy(gb(ctx) + addr + written, p, take);
        written += take;
        return written < limit; // libjpeg calls empty_output_buffer as the buffer fills: libjpgenc's stops there
    }
};

bool open_file(PPUContext& ctx, u32 path, u32& fd)
{
    u8* base = gb(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    StackVar out(ctx, base, 8);
    constexpr s32 kWriteCreateTrunc = 0x1 | 0x40 | 0x200, kMode = 0x180;
    const s32 r = lv2::sys_fs_open(t, path, kWriteCreateTrunc, out.addr(), kMode, 0, 0);
    if (r != lv2::CELL_OK) {
        PS_LOG_WARN(kChan, "cellJpgEncEncodePicture: {} cannot be made ({})", read_cstr(t, path, 0x400).text,
            lv2::error_name(r));
        return false;
    }
    fd = ps_read32(base, out.addr());
    return true;
}

bool write_file(PPUContext& ctx, u32 fd, const std::vector<u8>& bytes)
{
    u8* base = gb(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    bool ok = true;
    {
        constexpr u32 kChunk = 0x1000;
        StackVar buf(ctx, base, kChunk + 16);
        const u32 nwrite = buf.addr() + kChunk;
        for (usize at = 0; ok && at < bytes.size(); at += kChunk) {
            const u32 n = static_cast<u32>(std::min<usize>(kChunk, bytes.size() - at));
            std::memcpy(base + buf.addr(), bytes.data() + at, n);
            ok = lv2::sys_fs_write(t, fd, buf.addr(), n, nwrite) == lv2::CELL_OK && ps_read64(base, nwrite) == n;
        }
    }
    lv2::sys_fs_close(t, fd);
    return ok;
}

// The libjpeg settings of a command on an encoder: libjpgenc's jpeg_set_defaults, jpeg_set_colorspace (the encoder's),
// its sampling, jpeg_set_quality(quality, TRUE), the restart interval, FAST as JDCT_IFAST and QUALITY as JDCT_FLOAT,
// no JFIF or Adobe marker.
jpeg::Settings settings_of(const Config& cfg, const Command& c, u32 quality)
{
    jpeg::Settings s;
    s.width = c.width;
    s.height = c.height;
    s.input = c.space == GRAYSCALE ? jpeg::Space::Gray : c.space == YCBCR ? jpeg::Space::YCbCr : jpeg::Space::RGB;
    s.output = cfg.space == GRAYSCALE ? jpeg::Space::Gray : cfg.space == RGB ? jpeg::Space::RGB : jpeg::Space::YCbCr;
    static constexpr int kH[4] = {1, 2, 2, 4}, kV[4] = {1, 1, 2, 1};
    s.h_samp = cfg.space == YCBCR ? kH[cfg.sampling & 3] : 1;
    s.v_samp = cfg.space == YCBCR ? kV[cfg.sampling & 3] : 1;
    s.quality = static_cast<int>(quality);
    s.dct = c.dct == DCT_FAST ? jpeg::Dct::Fast : jpeg::Dct::Float;
    s.restart_interval = c.restart;
    return s;
}

enum class Failure { None, Fatal, Overflow };

// One encode of a command at `quality` into `out`: the extended parameters' segments after SOI, the rows (RGBA and
// ARGB with their alpha left out, as libjpgenc gives libjpeg RGB), EOI. `lines`: the rows written.
Failure encode_once(PPUContext& ctx, const Config& cfg, const Command& c, u32 quality, Output& out, u32& lines)
{
    u8* base = gb(ctx);
    jpeg::Encoder e(settings_of(cfg, c, quality), [&](const u8* p, usize n) { return out.write(p, n); });
    lines = 0;
    if (!e.valid()) return Failure::Fatal;
    if (!e.start()) return Failure::Overflow;
    for (u32 i = 0; i < c.ex_num; i++) {
        const u32 entry = c.ex_list + 12 * i, data = ps_read32(base, entry + 4), size = ps_read32(base, entry + 8);
        if (!e.marker(static_cast<u8>(ps_read32(base, entry)), base + data, size)) return Failure::Overflow;
    }
    std::vector<u8> rgb;
    for (u32 y = 0; y < c.height; y++) {
        const u8* row = base + c.addr + static_cast<usize>(y) * c.pitch;
        if (c.space == RGBA || c.space == ARGB) {
            rgb.resize(static_cast<usize>(c.width) * 3);
            const u32 skip = c.space == ARGB ? 1 : 0;
            for (u32 x = 0; x < c.width; x++) std::memcpy(rgb.data() + 3 * x, row + 4 * x + skip, 3);
            row = rgb.data();
        }
        if (!e.write_row(row)) {
            lines = e.rows();
            return e.bad_coefficient() ? Failure::Fatal : Failure::Overflow;
        }
    }
    lines = c.height;
    return e.finish() ? Failure::None : Failure::Overflow;
}

// One command encoded, as libjpgenc's thread does it.
Result encode(PPUContext& ctx, State& s, const Config& cfg, const Command& c)
{
    Result r = result_of(c, 0);
    u32 fd = 0;
    if (c.location == LOCATION_FILE && !open_file(ctx, c.file_name, fd)) {
        r.state = error(CELL_JPGENC_ERROR_STREAM_FILE_OPEN);
        return r;
    }
    if (c.spu) {
        std::lock_guard lock(s.mutex);
        if (!s.spu_logged) {
            s.spu_logged = true;
            PS_LOG_WARN(kChan, "cellJpgEncEncodePicture: encoded on the PPU's libjpeg (libjpgenc's SPU encoder rounds "
                               "otherwise: its streams differ, D-913)");
        }
    }
    Output out{ctx, c.location == LOCATION_BUFFER, c.stream, c.limit, {}, 0};
    u32 lines = 0, quality = c.quality;
    Failure failed = Failure::None;
    if (c.mode == STREAM_SIZE_LIMIT) {
        // libjpgenc chooses the quality from tables of its own (stream sizes measured by quality); pspocket takes the
        // highest quality up to the one given whose stream fits the buffer, the qualities searched by halves.
        {
            std::lock_guard lock(s.mutex);
            if (!s.limit_logged) {
                s.limit_logged = true;
                PS_LOG_INFO(kChan, "cellJpgEncEncodePicture: the stream size limit mode chooses its quality by a "
                                   "search, not libjpgenc's tables (D-913)");
            }
        }
        u32 lo = 0, hi = c.quality;
        while (lo < hi) {
            const u32 mid = (lo + hi + 1) / 2;
            Output trial{ctx, true, c.stream, c.limit, {}, 0};
            u32 trial_lines = 0;
            if (encode_once(ctx, cfg, c, mid, trial, trial_lines) == Failure::None) lo = mid;
            else hi = mid - 1;
        }
        quality = lo;
    }
    failed = encode_once(ctx, cfg, c, quality, out, lines);
    if (c.location == LOCATION_FILE) {
        const bool ok = write_file(ctx, fd, out.bytes);
        if (failed == Failure::None && !ok) failed = Failure::Fatal;
    }
    r.quality = quality;
    switch (failed) {
    case Failure::None:
        r.size = static_cast<u32>(out.written);
        r.lines = c.height;
        break;
    case Failure::Overflow:
        r.state = error(CELL_JPGENC_ERROR_STREAM_OVERFLOW);
        r.lines = lines;
        break;
    default: r.state = error(CELL_JPGENC_ERROR_FATAL); break;
    }
    return r;
}

// Encodes the commands waiting while fewer than four streams wait (libjpgenc's thread), one thread at a time.
// Reset's work: the commands waiting become skipped streams (eight streams at most).
void skip_waiting(Encoder& enc)
{
    for (const Command& c : enc.commands)
        if (enc.streams.size() < kStreamQueue) enc.streams.push_back(result_of(c, error(CELL_JPGENC_ERROR_STREAM_SKIP)));
    enc.commands.clear();
}

void pump(PPUContext& ctx, State& s, u32 handle)
{
    u64 generation = 0;
    Config cfg;
    {
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (e == s.encoders.end() || e->second.pumping) return;
        e->second.pumping = true;
        generation = e->second.generation;
        cfg = e->second.cfg;
    }
    for (;;) {
        Command c;
        bool skip = false;
        {
            std::lock_guard lock(s.mutex);
            const auto e = s.encoders.find(handle);
            if (e == s.encoders.end() || e->second.generation != generation) return;
            Encoder& enc = e->second;
            if (enc.commands.empty() || enc.streams.size() >= kQueueDepth) {
                enc.pumping = false;
                return;
            }
            c = enc.commands.front();
            enc.in_flight = true;
            skip = enc.skip_next;
            enc.skip_next = false;
        }
        const Result r = skip ? result_of(c, error(CELL_JPGENC_ERROR_STREAM_SKIP)) : encode(ctx, s, cfg, c);
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (e == s.encoders.end() || e->second.generation != generation) return;
        Encoder& enc = e->second;
        if (enc.in_flight && !enc.commands.empty()) enc.commands.pop_front();
        enc.in_flight = false;
        enc.streams.push_back(r);
        if (enc.reset_pending) {
            enc.reset_pending = false;
            skip_waiting(enc);
        }
    }
}

void nap(PPUContext& ctx)
{
    if (!lv2::sleep_us(thread_of(ctx).waiter, 1000)) throw lv2::ThreadExit{};
}

// ------------------------------------------------------------------------------------------------- the calls

s32 cellJpgEncQueryAttr(PPUContext& ctx, u32 config, u32 attr)
{
    u8* base = gb(ctx);
    Config c;
    if (!attr || !config || !in_guest(ctx, config, 28) || !in_guest(ctx, attr, 16) || !read_config(base, config, c))
        return error(CELL_JPGENC_ERROR_ARG);
    ps_write32(base, attr, mem_size(c) + 0x1780);
    ps_write8(base, attr + 4, kQueueDepth);
    ps_write32(base, attr + 8, kVersionUpper);
    ps_write32(base, attr + 12, kVersionLower);
    return lv2::CELL_OK;
}

s32 open(PPUContext& ctx, u32 config, u32 resource, u32 handle, bool ex)
{
    u8* base = gb(ctx);
    Config c;
    if (!handle || !in_guest(ctx, handle, 4) || !config || !in_guest(ctx, config, 28) || !read_config(base, config, c) ||
        !resource || !in_guest(ctx, resource, ex ? 24 : 16))
        return error(CELL_JPGENC_ERROR_ARG);
    const u32 mem = ps_read32(base, resource), size = ps_read32(base, resource + 4);
    const s32 ppu_prio = static_cast<s32>(ps_read32(base, resource + 8));
    if (!mem || !size || ppu_prio < 0 || ppu_prio > 0xBFF) return error(CELL_JPGENC_ERROR_ARG);
    if (ex) {
        for (u32 i = 0; i < 8; i++)
            if (ps_read8(base, resource + 16 + i) > 15) return error(CELL_JPGENC_ERROR_ARG);
    } else {
        const s32 spu_prio = static_cast<s32>(ps_read32(base, resource + 12));
        if (spu_prio < 0 || spu_prio > 0xFF) return error(CELL_JPGENC_ERROR_ARG);
    }
    if (size < mem_size(c) + 0x1780) return error(CELL_JPGENC_ERROR_ARG);
    const u32 h = (mem + 0x7F) & ~0x7Fu;
    State& s = state();
    std::lock_guard lock(s.mutex);
    Encoder e;
    e.cfg = c;
    e.generation = ++s.opens;
    s.encoders[h] = std::move(e);
    ps_write32(base, handle, h);
    PS_LOG_INFO(kChan, "cellJpgEncOpen{}: handle {:#x}, up to {}x{}, colour space {}, sampling {}", ex ? "Ex" : "", h,
        c.max_w, c.max_h, c.space, c.sampling);
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
    Encoder& enc = e->second;
    if (enc.streams.size() >= 5) return error(CELL_JPGENC_ERROR_BUSY);
    // A command being encoded finishes as it is and its stream is queued first (libpngenc's thread takes the Reset
    // between commands); the others become skipped streams then.
    if (enc.in_flight) enc.reset_pending = true;
    else skip_waiting(enc);
    enc.skip_next = true;
    enc.resets++;
    return lv2::CELL_OK;
}

s32 cellJpgEncWaitForInput(PPUContext& ctx, u32 handle, u32 block)
{
    State& s = state();
    u64 resets = 0;
    bool first = true;
    for (;;) {
        {
            std::lock_guard lock(s.mutex);
            const auto e = s.encoders.find(handle);
            if (!handle || e == s.encoders.end()) return first ? error(CELL_JPGENC_ERROR_ARG) : error(CELL_JPGENC_ERROR_RESET);
            if (first) resets = e->second.resets;
            first = false;
            if (e->second.resets != resets) return error(CELL_JPGENC_ERROR_RESET);
            if (e->second.commands.size() < kQueueDepth) return lv2::CELL_OK;
            if (!(block & 0xFF)) return error(CELL_JPGENC_ERROR_BUSY);
        }
        nap(ctx);
    }
}

// EncodePicture2's checks (ARG), then the command queued (BUSY with four waiting).
s32 encode_picture(PPUContext& ctx, u32 handle, const Command& picture, u32 param, u32 output)
{
    u8* base = gb(ctx);
    State& s = state();
    Config cfg;
    {
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (!handle || e == s.encoders.end()) return error(CELL_JPGENC_ERROR_ARG);
        cfg = e->second.cfg;
    }
    Command c = picture;
    const auto refused = [&](const char* why) {
        PS_LOG_WARN(kChan, "cellJpgEncEncodePicture: {}x{} pitch {} colour space {}: {}", c.width, c.height, c.pitch,
            c.space, why);
        return error(CELL_JPGENC_ERROR_ARG);
    };
    if (!c.width || c.width > cfg.max_w || !c.height || c.height > cfg.max_h || !c.addr || c.space > 20)
        return refused("its size, address or colour space");
    const u32 bpp = pixel_bytes(c.space, cfg.space);
    if (!bpp || c.pitch < bpp * c.width) return refused("its colour space or pitch");
    if (!param || !in_guest(ctx, param, 28)) return refused("its parameters");
    c.spu = ps_read8(base, param) != 0;
    c.restart = ps_read32(base, param + 4);
    c.dct = ps_read32(base, param + 8);
    c.mode = ps_read32(base, param + 12);
    c.quality = ps_read32(base, param + 16);
    c.ex_list = ps_read32(base, param + 20);
    c.ex_num = ps_read32(base, param + 24);
    if (c.spu && !cfg.spu) return refused("on the SPUs, not opened for them");
    if ((c.dct != DCT_QUALITY && c.dct != DCT_FAST) || (c.spu && c.dct == DCT_QUALITY) || c.mode > 1 ||
        c.quality > 100 || c.restart > 0xFFFF)
        return refused("its parameters");
    u32 extra = 0;
    if (c.ex_num) {
        if (!c.ex_list || !in_guest(ctx, c.ex_list, u64{c.ex_num} * 12)) return refused("its extended parameters");
        for (u32 i = 0; i < c.ex_num; i++) {
            const u32 data = ps_read32(base, c.ex_list + 12 * i + 4), size = ps_read32(base, c.ex_list + 12 * i + 8);
            if (!data || size - 1 >= 0xFFFD || !in_guest(ctx, data, size)) return refused("an extended parameter");
            extra += size + 4;
        }
    }
    if (!output || !in_guest(ctx, output, 16)) return refused("its output");
    c.location = ps_read32(base, output);
    c.file_name = ps_read32(base, output + 4);
    c.stream = ps_read32(base, output + 8);
    c.limit = ps_read32(base, output + 12);
    if (c.location > LOCATION_BUFFER) return refused("its output");
    if (c.mode == STREAM_SIZE_LIMIT && c.location != LOCATION_BUFFER) return refused("its output");
    if (c.location == LOCATION_FILE ? !c.file_name : !c.stream || c.limit < extra + 0x1000) return refused("its output");
    // The picture's rows and the buffer must be guest memory (libjpgenc reads and writes whatever is there).
    if (!in_guest(ctx, c.addr, u64{c.pitch} * (c.height - 1) + u64{bpp} * c.width) ||
        (c.location == LOCATION_BUFFER && !in_guest(ctx, c.stream, c.limit))) {
        log_call_site(ctx, base, "cellJpgEncEncodePicture: the picture or the stream is not in guest memory");
        return error(CELL_JPGENC_ERROR_ARG);
    }
    {
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (e == s.encoders.end()) return error(CELL_JPGENC_ERROR_ARG);
        if (e->second.commands.size() >= kQueueDepth) return error(CELL_JPGENC_ERROR_BUSY);
        e->second.commands.push_back(c);
    }
    pump(ctx, s, handle);
    return lv2::CELL_OK;
}

// cellJpgEncEncodePicture2(handle, CellJpgEncPicture2 *, CellJpgEncEncodeParam *, CellJpgEncOutputParam *).
s32 cellJpgEncEncodePicture2(PPUContext& ctx, u32 handle, u32 picture, u32 param, u32 output)
{
    u8* base = gb(ctx);
    if (!picture || !in_guest(ctx, picture, 32)) return error(CELL_JPGENC_ERROR_ARG);
    Command c;
    c.width = ps_read32(base, picture);
    c.height = ps_read32(base, picture + 4);
    c.pitch = ps_read32(base, picture + 8);
    c.space = ps_read32(base, picture + 12);
    c.addr = ps_read32(base, picture + 16);
    c.user_data = ps_read64(base, picture + 24);
    return encode_picture(ctx, handle, c, param, output);
}

// cellJpgEncEncodePicture(handle, CellJpgEncPicture *, ...): EncodePicture2 with rows of the picture's own width.
s32 cellJpgEncEncodePicture(PPUContext& ctx, u32 handle, u32 picture, u32 param, u32 output)
{
    u8* base = gb(ctx);
    if (!picture || !in_guest(ctx, picture, 24)) return error(CELL_JPGENC_ERROR_ARG);
    Command c;
    c.width = ps_read32(base, picture);
    c.height = ps_read32(base, picture + 4);
    c.space = ps_read32(base, picture + 8);
    c.addr = ps_read32(base, picture + 12);
    c.user_data = ps_read64(base, picture + 16);
    const u32 bpp = c.space == GRAYSCALE ? 1 : c.space == RGB || c.space == YCBCR ? 3
                                          : c.space == RGBA || c.space == ARGB    ? 4
                                                                                   : 0;
    c.pitch = c.width * bpp;
    return encode_picture(ctx, handle, c, param, output);
}

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
            if (!handle || e == s.encoders.end())
                return first ? error(CELL_JPGENC_ERROR_ARG) : error(CELL_JPGENC_ERROR_RESET);
            if (!num || !in_guest(ctx, num, 4)) return error(CELL_JPGENC_ERROR_ARG);
            if (first) resets = e->second.resets;
            first = false;
            const u32 n = static_cast<u32>(e->second.streams.size());
            if (e->second.resets != resets) {
                ps_write32(base, num, n);
                return error(CELL_JPGENC_ERROR_RESET);
            }
            if (n || !(block & 0xFF)) {
                ps_write32(base, num, n);
                return n ? lv2::CELL_OK : error(CELL_JPGENC_ERROR_EMPTY);
            }
        }
        nap(ctx);
    }
}

s32 cellJpgEncGetStreamInfo(PPUContext& ctx, u32 handle, u32 info)
{
    u8* base = gb(ctx);
    State& s = state();
    {
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (!handle || e == s.encoders.end() || !info || !in_guest(ctx, info, 40)) return error(CELL_JPGENC_ERROR_ARG);
        if (e->second.streams.empty()) return error(CELL_JPGENC_ERROR_EMPTY);
        const Result r = e->second.streams.front();
        e->second.streams.pop_front();
        const u32 words[] = {static_cast<u32>(r.state), r.location, r.file_name, r.addr, r.limit, r.size, r.lines,
            r.quality};
        for (u32 i = 0; i < std::size(words); i++) ps_write32(base, info + 4 * i, words[i]);
        ps_write64(base, info + 32, r.user_data);
    }
    pump(ctx, s, handle);
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
