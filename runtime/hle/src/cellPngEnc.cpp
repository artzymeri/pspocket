// cellPngEnc: the PNG encoder (libpngenc), pspocket's own (D-566), made to answer and write what Sony's libpngenc does
// (D-913, compared call for call with libpngenc.sprx, tests/hle/test_pngenc_compare.cpp). RPCS3's
// (rpcs3/Emu/Cell/Modules/cellPngEnc.cpp/.h, cellPng.h) gave the structures and error codes.
//
// What libpngenc is: libpng 1.2.8 on zlib 1.2.3 behind a command queue. Open makes an encoder in the game's work memory
// (its handle the memory's first 128-byte boundary); EncodePicture checks the picture, its parameters and every
// ancillary chunk, then queues a command (four at most: BUSY); a thread of its own takes the commands while fewer than
// four streams wait, encodes each and queues its stream information (GetStreamInfo takes the oldest). Reset turns the
// commands waiting into skipped streams and skips the next picture too. Here the commands are encoded as soon as there is
// room, on the caller's thread, which is what libpngenc's thread does when it keeps up.
//
// The PNG is libpng 1.2.8's, written here (a libpng of today orders and formats some chunks otherwise, and checks
// others): the chunks in 1.2.8's order (IHDR; gAMA sRGB iCCP sBIT cHRM and the unknown chunks placed before PLTE; PLTE
// tRNS bKGD hIST oFFs pCAL sCAL pHYs tIME sPLT, the texts, the unknown chunks placed after PLTE; IDAT in 2048-byte
// chunks; the unknown chunks placed after IDAT; IEND), each as 1.2.8 writes it. libpngenc installs no warning handler,
// so every warning of libpng's is an error: the stream fails (FATAL) where libpng would have left a chunk out (a keyword
// with spaces to tidy, a PLTE on a gray picture, a tRNS out of range or on an alpha picture, a cHRM point past 0.8, a
// palette background past the palette).
// The compressed data (IDAT, zTXt, iCCP) is zlib's of this build, with libpng 1.2.8's settings and filter choice: zlib's
// version may give other bytes for the same pixels (D-597).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
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
#include <zlib.h>
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
    CELL_PNGENC_ERROR_FATAL = 0x80611296,
    CELL_PNGENC_ERROR_STREAM_SKIP = 0x806112A2,
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
// libpng's filter bits.
constexpr u32 kFilterNone = 0x08, kFilterSub = 0x10, kFilterUp = 0x20, kFilterAvg = 0x40, kFilterPaeth = 0x80;
constexpr u32 kQueueDepth = 4, kStreamQueue = 8, kVersionUpper = 0x270000, kVersionLower = 0;
// libpng's unknown chunk locations.
constexpr u32 kHaveIhdr = 0x01, kHavePlte = 0x02, kAfterIdat = 0x08;
constexpr usize kZbufSize = 2048; // libpngenc's zlib output buffer: the size of its IDAT chunks

// CellPngEncConfig {maxWidth, maxHeight, maxBitDepth, enableSpu (b8, +12), addMemSize (+16), exParamList, exParamNum}.
struct Config {
    u32 max_w = 0, max_h = 0, max_depth = 0, add_mem = 0;
    bool spu = false;
};

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

// The work memory libpngenc asks for, less its 0x1780 bytes of encoder state.
u32 mem_size(const Config& c) { return c.add_mem + (c.spu ? 0x78200 : 0x47A00) + (c.max_depth >> 1) * c.max_w * 7; }

// ------------------------------------------------------------------------------------------------- the queues

// A command as EncodePicture copies it: the picture, the encode parameters and the output parameters.
struct Command {
    u32 width = 0, height = 0, pitch = 0, space = 0, depth = 0, addr = 0;
    bool packed = false;
    u64 user_data = 0;
    u32 encode_space = 0, level = 0, filters = 0, chunk_list = 0, chunk_count = 0;
    u32 location = 0, file_name = 0, stream = 0, limit = 0;
};

struct Result {
    s32 state = 0;
    u32 location = 0, file_name = 0, addr = 0, limit = 0, size = 0, lines = 0;
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
    r.user_data = c.user_data;
    return r;
}

struct Encoder {
    Config cfg;
    std::deque<Command> commands; // waiting to be encoded (four at most)
    std::deque<Result> streams;   // waiting to be taken (eight at most)
    bool skip_next = false;       // Reset leaves libpngenc's abort flag up: the next picture is skipped
    bool pumping = false;         // a thread is encoding this encoder's commands
    u64 generation = 0;           // which Open this is (Open again over the same memory makes a new one)
    u64 resets = 0;               // Resets so far (a blocking wait ends with RESET at one)
};

struct State {
    std::mutex mutex;
    std::map<u32, Encoder> encoders; // by handle (the work memory's first 128-byte boundary)
    u64 opens = 0;
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

std::string guest_cstr(PPUContext& ctx, u32 addr, u32 max = 0x10000)
{
    if (!addr) return {};
    return read_cstr(thread_of(ctx), addr, max).text;
}

// ------------------------------------------------------------------------------------------------- the checks

u32 components(u32 space)
{
    switch (space) {
    case GRAYSCALE:
    case PALETTE: return 1;
    case GRAYSCALE_ALPHA: return 2;
    case RGB: return 3;
    case RGBA:
    case ARGB: return 4;
    default: return 0;
    }
}

// libpngenc's measure of a string (its copy and a header): strlen + 0x11 rounded down to 16, times `k`.
u32 string_cost(PPUContext& ctx, u32 s, u32 k)
{
    return ((static_cast<u32>(guest_cstr(ctx, s).size()) + 0x11) & ~0xFu) * k;
}

// Its measure of a buffer: rounded up to 16, and the 2 KB pages past the first 2 KB once more.
u32 buffer_cost(u32 n) { return ((n + 0xF) & ~0xFu) + (n >= 0x800 ? (n & ~0x7FFu) : 0); }

// EncodePicture's checks of the ancillary chunks (each type's fields, what they need of the picture and the palette
// given before them) and the memory they take of addMemSize. False: ARG.
bool chunks_ok(PPUContext& ctx, const Command& c, const Config& cfg)
{
    u8* base = gb(ctx);
    u32 palette = 0, memory = 0;
    if (c.chunk_count) {
        if (!c.chunk_list || !in_guest(ctx, c.chunk_list, u64{c.chunk_count} * 8)) return false;
        for (u32 i = 0; i < c.chunk_count; i++) {
            const u32 type = ps_read32(base, c.chunk_list + 8 * i), p = ps_read32(base, c.chunk_list + 8 * i + 4);
            if (type > CHUNK_UNKNOWN || !p || !in_guest(ctx, p, 8)) return false;
            const auto w = [&](u32 off) { return ps_read32(base, p + off); };
            const auto d = [&](u32 off) { return ps_readf64(base, p + off); };
            switch (type) {
            case CHUNK_PLTE: // CellPngPLTE {paletteEntriesNumber, paletteEntries}
                if (!w(0) || w(0) > (1u << std::min(c.depth, 31u)) || !w(4)) return false;
                palette = w(0);
                break;
            case CHUNK_TRNS: // CellPngTRNS {alphaForPaletteIndex, number, red, green, blue, gray (u16)}
                if (c.encode_space == RGBA || c.encode_space == ARGB) return false;
                if (c.encode_space == PALETTE && (!w(0) || !w(4) || w(4) > palette)) return false;
                break;
            case CHUNK_CHRM: // eight doubles: the white point and the three primaries, each x and y
                if (d(0) > 0.8 || d(8) > 0.8) return false;
                for (u32 k = 0; k < 4; k++)
                    if (d(16 * k) < 0.0 || d(16 * k + 8) < 0.0 || d(16 * k) + d(16 * k + 8) > 1.0) return false;
                break;
            case CHUNK_GAMA:
                if (d(0) <= 0.0 || d(0) > 21474.83) return false;
                break;
            case CHUNK_ICCP: // CellPngICCP {profileName, iccProfile, iccProfileLength}
                if (!w(0) || !w(4) || !w(8)) return false;
                memory += string_cost(ctx, w(0), 2) + buffer_cost(w(8));
                break;
            case CHUNK_SBIT: { // CellPngSBIT {red, green, blue, gray, alpha}
                const u32 limit = c.encode_space == PALETTE ? 8 : c.depth;
                const auto bits_ok = [&](u32 v) { return v && v <= limit; };
                if (c.encode_space == PALETTE || !(c.encode_space & 1)) {
                    if (!bits_ok(w(0)) || !bits_ok(w(4)) || !bits_ok(w(8))) return false;
                } else if (!bits_ok(w(12))) {
                    return false;
                }
                if (c.encode_space > 8 && !bits_ok(w(16))) return false;
                break;
            }
            case CHUNK_SRGB:
                if (w(0) > 3) return false;
                break;
            case CHUNK_TEXT: { // CellPngTextInfo {txtType, keyword, text, textLength, languageTag, translatedKeyword}
                const u32 kind = w(0), key = w(4), text = w(8), lang = w(16), translated = w(20);
                if (kind > 2 || !key || guest_cstr(ctx, key, 2).empty()) return false;
                if (kind == 2 && (!lang || guest_cstr(ctx, lang, 2).empty())) return false;
                memory += string_cost(ctx, key, 2) + 0x120;
                if (text)
                    memory += kind == 0 ? string_cost(ctx, text, 1)
                                        : buffer_cost(static_cast<u32>(guest_cstr(ctx, text).size()) + 2);
                if (kind == 2) {
                    memory += string_cost(ctx, lang, 2);
                    if (translated) memory += string_cost(ctx, translated, 1);
                }
                break;
            }
            case CHUNK_BKGD: { // CellPngBKGD {paletteIndex (u8), red, green, blue, gray}
                const u32 limit = (1u << std::min(c.depth, 31u)) - 1;
                if (c.encode_space == PALETTE) {
                    const u32 index = ps_read8(base, p);
                    if (index > palette || index > limit) return false;
                } else if (c.encode_space == GRAYSCALE || c.encode_space == GRAYSCALE_ALPHA) {
                    if (w(16) > limit) return false;
                } else if ((w(4) | w(8) | w(12)) > limit) {
                    return false;
                }
                break;
            }
            case CHUNK_HIST: // CellPngHIST {histgramEntries, histgramEntriesNumber}
                if (!palette || !w(4) || w(4) > palette || !w(0)) return false;
                break;
            case CHUNK_PHYS: // CellPngPHYS {xAxis, yAxis, unitSpecifier}
            case CHUNK_OFFS: // CellPngOFFS {xPosition, yPosition, unitSpecifier}
                if (w(8) > 1) return false;
                break;
            case CHUNK_SPLT: { // {CellPngSPLT list, number}: {paletteName, sampleDepth (u8), paletteEntries, number}
                const u32 list = w(0), n = w(4);
                if (!list) return false;
                for (u32 k = 0; k < n; k++) {
                    const u32 e = list + 16 * k;
                    if (!in_guest(ctx, e, 16)) return false;
                    const u32 name = ps_read32(base, e), depth = ps_read8(base, e + 4);
                    const u32 entries = ps_read32(base, e + 8), count = ps_read32(base, e + 12);
                    if (!name || (depth != 8 && depth != 16) || !entries || !count) return false;
                    if (depth == 8)
                        for (u32 j = 0; j < count; j++) {
                            const u32 at = entries + 10 * j;
                            if ((ps_read16(base, at) | ps_read16(base, at + 2) | ps_read16(base, at + 4) |
                                    ps_read16(base, at + 6)) > 0xFF)
                                return false;
                        }
                    memory += string_cost(ctx, name, 2) + ((count * 10 + 0xF) & ~0xFu);
                }
                memory += n * 0x20;
                break;
            }
            case CHUNK_TIME: { // CellPngTIME {year (u16), month, day, hour, minute, second}
                const u8 month = ps_read8(base, p + 2), day = ps_read8(base, p + 3), hour = ps_read8(base, p + 4);
                const u8 minute = ps_read8(base, p + 5), second = ps_read8(base, p + 6);
                if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60)
                    return false;
                break;
            }
            case CHUNK_PCAL: { // CellPngPCAL {calibrationName, x0, x1, equationType, number, unitName, parameter}
                const u32 n = w(16), params = w(24);
                if (!w(0) || !n || w(12) > 3 || !w(20) || !params) return false;
                for (u32 k = 0; k < n; k++) {
                    const u32 param = ps_read32(base, params + 4 * k);
                    if (!param) return false;
                    memory += string_cost(ctx, param, 1);
                }
                memory += string_cost(ctx, w(0), 2) + string_cost(ctx, w(20), 2) + ((n * 4 + 0xF) & ~0xFu) * 2;
                break;
            }
            case CHUNK_SCAL: // CellPngSCAL {unitSpecifier, pixelWidth (f64, +8), pixelHeight}
                if (w(0) > 2 || d(8) < 0.0 || d(16) < 0.0) return false;
                break;
            case CHUNK_UNKNOWN: // CellPngUnknownChunk {chunkType[5], chunkData (+8), length, location}
                if (!w(8) || !w(12) || (w(16) & ~(kHaveIhdr | kHavePlte | kAfterIdat))) return false;
                memory += ((w(12) + 0xF) & ~0xFu) + 0x50;
                break;
            }
        }
    }
    if (c.encode_space == PALETTE && !palette) return false;
    return memory <= cfg.add_mem;
}

// ------------------------------------------------------------------------------------------------- libpng 1.2.8

// What failed, as libpngenc maps it into a stream's state.
enum class Failure { None, Fatal, Overflow, FileOpen };

struct Abort {
    Failure why;
};

// libpng's chunk writer onto the stream: the game's buffer (a write that does not fit fails, nothing of it written) or
// a file's bytes.
struct Sink {
    PPUContext& ctx;
    bool to_buffer = false;
    u32 addr = 0, limit = 0;
    std::vector<u8> bytes; // a file's
    u64 written = 0;

    void write(const u8* p, usize n)
    {
        if (to_buffer) {
            if (written + n > limit) throw Abort{Failure::Overflow};
            std::memcpy(gb(ctx) + addr + written, p, n);
        } else {
            bytes.insert(bytes.end(), p, p + n);
        }
        written += n;
    }
};

struct ChunkWriter {
    Sink& sink;
    u32 crc = 0;
    void start(const char* type, usize length)
    {
        u8 head[8];
        ps_st32(head, static_cast<u32>(length));
        std::memcpy(head + 4, type, 4);
        sink.write(head, 8);
        crc = static_cast<u32>(crc32(0, head + 4, 4));
    }
    void data(const u8* p, usize n)
    {
        if (!n) return;
        sink.write(p, n);
        crc = static_cast<u32>(crc32(crc, p, static_cast<uInt>(n)));
    }
    void data(const std::string& s, bool with_nul)
    {
        data(reinterpret_cast<const u8*>(s.c_str()), s.size() + (with_nul ? 1 : 0));
    }
    void end()
    {
        u8 tail[4];
        ps_st32(tail, crc);
        sink.write(tail, 4);
    }
    void chunk(const char* type, const std::vector<u8>& body)
    {
        start(type, body.size());
        data(body.data(), body.size());
        end();
    }
};

[[noreturn]] void warning() { throw Abort{Failure::Fatal}; } // libpngenc's libpng: every warning ends the stream

// png_check_keyword: printable Latin-1 (char unsigned on the PPU), no leading, trailing or doubled spaces, 1 to 79
// characters; each tidying is a warning.
std::string checked_keyword(const std::string& key)
{
    if (key.empty() || key.size() > 79) warning();
    for (const char ch : key) {
        const u8 c = static_cast<u8>(ch);
        if (c < 0x20 || (c > 0x7E && c < 0xA1)) warning();
    }
    if (key.front() == ' ' || key.back() == ' ' || key.find("  ") != std::string::npos) warning();
    return key;
}

std::vector<u8> deflate_all(const u8* p, usize n, int level, int strategy)
{
    std::vector<u8> out;
    z_stream z{};
    if (deflateInit2(&z, level, Z_DEFLATED, 15, 8, strategy) != Z_OK) warning();
    z.next_in = const_cast<u8*>(p);
    z.avail_in = static_cast<uInt>(n);
    u8 buf[kZbufSize];
    int r = Z_OK;
    while (r != Z_STREAM_END) {
        z.next_out = buf;
        z.avail_out = sizeof(buf);
        r = deflate(&z, Z_FINISH);
        if (r != Z_OK && r != Z_STREAM_END) {
            deflateEnd(&z);
            warning();
        }
        out.insert(out.end(), buf, buf + (sizeof(buf) - z.avail_out));
    }
    deflateEnd(&z);
    return out;
}

// sCAL's numbers as libpngenc prints them ("%d.%06de%+03d", its own scaling and rounding).
std::string scal_number(double v)
{
    double a = std::fabs(v), scale = 1.0;
    int e = 0;
    if (a >= 10.0) {
        do {
            e++;
            a /= 10.0;
            scale *= 10.0;
        } while (a >= 10.0);
    }
    while (a < 1.0 && a != 0.0) {
        a *= 10.0;
        e--;
        scale /= 10.0;
    }
    double d = v / scale, whole = 1.0;
    if (v < 0.0) d = -d;
    int i = static_cast<int>(d + 5e-07);
    if (i == 10) {
        e++;
        i = 1;
        d /= 10.0;
    } else {
        whole = static_cast<double>(i);
    }
    int f = static_cast<int>(std::fma(d - whole, 1000000.0, 0.5));
    if (f == 1000000) {
        f = 0;
        if (i == 9) {
            e++;
            i = 1;
        } else {
            i++;
        }
    }
    if (v < 0.0) i = -i;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d.%06de%+03d", i, f, e);
    return buf;
}

// A float of libpng's (gAMA, cHRM) in 1/100000ths: the PPU's fused multiply-add, truncated.
u32 fixed_100000(float v) { return static_cast<u32>(static_cast<s64>(std::fma(static_cast<double>(v), 100000.0, 0.5))); }

struct Text {
    int compression = -1; // libpng's: -1 tEXt, 0 zTXt, 1 iTXt uncompressed
    std::string key, text, lang, lang_key;
    bool has_lang_key = false;
};

struct Unknown {
    std::string name;
    std::vector<u8> data;
    u32 location = 0;
};

struct SpltEntry {
    u16 red, green, blue, alpha, frequency;
};

struct Splt {
    std::string name;
    u8 depth = 0;
    std::vector<SpltEntry> entries;
};

// png_info as libpngenc's chunk setters leave it.
struct Info {
    std::vector<u8> palette; // three bytes an entry
    bool has_plte = false;
    std::vector<u8> trans; // the palette's alphas
    u16 trans_red = 0, trans_green = 0, trans_blue = 0, trans_gray = 0, num_trans = 0;
    bool has_trns = false;
    float chrm[8]{};
    bool has_chrm = false;
    float gamma = 0;
    bool has_gama = false;
    std::string iccp_name;
    std::vector<u8> iccp_profile;
    bool has_iccp = false;
    u8 sbit[5]{}; // red, green, blue, gray, alpha
    bool has_sbit = false;
    u8 srgb = 0;
    bool has_srgb = false;
    std::vector<Text> texts;
    u8 bkgd_index = 0;
    u16 bkgd_red = 0, bkgd_green = 0, bkgd_blue = 0, bkgd_gray = 0;
    bool has_bkgd = false;
    std::vector<u16> hist;
    bool has_hist = false;
    u32 phys_x = 0, phys_y = 0;
    u8 phys_unit = 0;
    bool has_phys = false;
    s32 offs_x = 0, offs_y = 0;
    u8 offs_unit = 0;
    bool has_offs = false;
    u8 time[7]{};
    bool has_time = false;
    u8 scal_unit = 0;
    double scal_w = 0, scal_h = 0;
    bool has_scal = false;
    std::string pcal_purpose, pcal_units;
    s32 pcal_x0 = 0, pcal_x1 = 0;
    u8 pcal_type = 0;
    std::vector<std::string> pcal_params;
    bool has_pcal = false;
    std::vector<Splt> splt;
    std::vector<Unknown> unknown;
};

// libpngenc's setter of one ancillary chunk onto png_info: its own checks (FATAL) and libpng 1.2.8's png_set_*.
void set_chunk(PPUContext& ctx, const Command& c, u32 type, u32 p, Info& info)
{
    u8* base = gb(ctx);
    const auto w = [&](u32 off) { return ps_read32(base, p + off); };
    const auto d = [&](u32 off) { return ps_readf64(base, p + off); };
    switch (type) {
    case CHUNK_PLTE: {
        const u32 n = w(0), entries = w(4);
        if (!n || !entries) warning();
        if (n > 256) warning(); // png_set_PLTE: "Invalid palette length" (an error on a palette picture)
        info.palette.assign(base + entries, base + entries + 3 * n);
        info.has_plte = true;
        break;
    }
    case CHUNK_TRNS: {
        const u32 alphas = w(0), n = w(4);
        if (c.encode_space == PALETTE && (!n || !alphas)) warning();
        info.trans.clear();
        if (alphas && n <= 256) info.trans.assign(base + alphas, base + alphas + n);
        info.trans_red = ps_read16(base, p + 8);
        info.trans_green = ps_read16(base, p + 10);
        info.trans_blue = ps_read16(base, p + 12);
        info.trans_gray = ps_read16(base, p + 14);
        info.num_trans = static_cast<u16>(n ? n : 1); // png_set_tRNS: with trans_values, none counts as one
        info.has_trns = true;
        break;
    }
    case CHUNK_CHRM:
        for (u32 k = 0; k < 8; k++) info.chrm[k] = static_cast<float>(d(8 * k));
        info.has_chrm = true;
        break;
    case CHUNK_GAMA:
        if (d(0) > 21474.83 || d(0) == 0.0) warning(); // "Limiting gamma", "Setting gamma=0"
        info.gamma = static_cast<float>(d(0));
        info.has_gama = true;
        break;
    case CHUNK_ICCP: {
        const u32 name = w(0), profile = w(4), length = w(8);
        if (!name || !profile || !length) warning();
        info.iccp_name = guest_cstr(ctx, name);
        info.iccp_profile.assign(base + profile, base + profile + length);
        info.has_iccp = true;
        break;
    }
    case CHUNK_SBIT:
        for (u32 k = 0; k < 5; k++) info.sbit[k] = static_cast<u8>(w(4 * k));
        info.has_sbit = true;
        break;
    case CHUNK_SRGB:
        info.srgb = static_cast<u8>(w(0));
        info.has_srgb = true;
        break;
    case CHUNK_TEXT: {
        const u32 kind = w(0), key = w(4), text = w(8);
        if (kind > 2 || !key) warning();
        Text t;
        t.key = guest_cstr(ctx, key);
        t.text = guest_cstr(ctx, text);
        // png_set_text: no text (or an empty one) is written uncompressed, as tEXt or as an uncompressed iTXt.
        t.compression = kind == 0 ? -1 : kind == 1 ? 0 : 1;
        if (t.text.empty() && t.compression == 0) t.compression = -1;
        if (kind == 2) {
            t.lang = guest_cstr(ctx, w(16));
            t.has_lang_key = w(20) != 0;
            t.lang_key = guest_cstr(ctx, w(20));
        }
        info.texts.push_back(std::move(t));
        break;
    }
    case CHUNK_BKGD:
        info.bkgd_index = ps_read8(base, p);
        info.bkgd_red = static_cast<u16>(w(4));
        info.bkgd_green = static_cast<u16>(w(8));
        info.bkgd_blue = static_cast<u16>(w(12));
        info.bkgd_gray = static_cast<u16>(w(16));
        info.has_bkgd = true;
        break;
    case CHUNK_HIST: {
        const u32 palette = static_cast<u32>(info.palette.size() / 3);
        if (!palette || !w(4) || w(4) != palette || !w(0)) warning();
        info.hist.resize(palette);
        for (u32 k = 0; k < palette; k++) info.hist[k] = ps_read16(base, w(0) + 2 * k);
        info.has_hist = true;
        break;
    }
    case CHUNK_PHYS:
        info.phys_x = w(0);
        info.phys_y = w(4);
        info.phys_unit = static_cast<u8>(w(8));
        info.has_phys = true;
        break;
    case CHUNK_SPLT: {
        const u32 list = w(0), n = w(4);
        if (!list) warning();
        for (u32 k = 0; k < n; k++) {
            const u32 e = list + 16 * k, name = ps_read32(base, e), entries = ps_read32(base, e + 8);
            if (!name || !entries) warning();
            Splt s;
            s.name = guest_cstr(ctx, name);
            s.depth = ps_read8(base, e + 4);
            const u32 count = ps_read32(base, e + 12);
            for (u32 j = 0; j < count; j++) {
                const u32 at = entries + 10 * j;
                s.entries.push_back({ps_read16(base, at), ps_read16(base, at + 2), ps_read16(base, at + 4),
                    ps_read16(base, at + 6), ps_read16(base, at + 8)});
            }
            info.splt.push_back(std::move(s));
        }
        break;
    }
    case CHUNK_TIME:
        std::memcpy(info.time, base + p, 7);
        info.has_time = true;
        break;
    case CHUNK_OFFS:
        info.offs_x = static_cast<s32>(w(0));
        info.offs_y = static_cast<s32>(w(4));
        info.offs_unit = static_cast<u8>(w(8));
        info.has_offs = true;
        break;
    case CHUNK_PCAL: {
        const u32 n = w(16), params = w(24);
        if (!w(0) || !w(20) || !params || !n) warning();
        info.pcal_params.clear();
        for (u32 k = 0; k < n; k++) {
            const u32 param = ps_read32(base, params + 4 * k);
            if (!param) warning();
            info.pcal_params.push_back(guest_cstr(ctx, param));
        }
        info.pcal_purpose = guest_cstr(ctx, w(0));
        info.pcal_x0 = static_cast<s32>(w(4));
        info.pcal_x1 = static_cast<s32>(w(8));
        info.pcal_type = static_cast<u8>(w(12));
        info.pcal_units = guest_cstr(ctx, w(20));
        info.has_pcal = true;
        break;
    }
    case CHUNK_SCAL:
        info.scal_unit = static_cast<u8>(w(0));
        info.scal_w = d(8);
        info.scal_h = d(16);
        info.has_scal = true;
        break;
    case CHUNK_UNKNOWN: {
        const u32 location = w(16);
        if (location != kHaveIhdr && location != kHavePlte && location != kAfterIdat) warning();
        Unknown u;
        u.name.assign(reinterpret_cast<const char*>(base + p), 4);
        u.data.assign(base + w(8), base + w(8) + w(12));
        u.location = location;
        info.unknown.push_back(std::move(u));
        break;
    }
    default: warning();
    }
}

int png_color_type(u32 space)
{
    switch (space) {
    case GRAYSCALE: return 0;
    case RGB: return 2;
    case PALETTE: return 3;
    case GRAYSCALE_ALPHA: return 4;
    default: return 6; // RGBA, and ARGB written as RGBA
    }
}

// The chunks png_write_info writes, in libpng 1.2.8's order, with its checks (warnings).
void write_info(ChunkWriter& out, const Command& c, const Info& info)
{
    const int color = png_color_type(c.encode_space);
    const u32 depth = c.depth;
    std::vector<u8> b;
    const auto u16be = [&](u16 v) {
        b.push_back(static_cast<u8>(v >> 8));
        b.push_back(static_cast<u8>(v));
    };
    const auto u32be = [&](u32 v) {
        for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<u8>(v >> s));
    };
    static constexpr u8 kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.sink.write(kSignature, 8);
    b.clear();
    u32be(c.width);
    u32be(c.height);
    b.push_back(static_cast<u8>(depth));
    b.push_back(static_cast<u8>(color));
    b.push_back(0);
    b.push_back(0);
    b.push_back(0);
    out.chunk("IHDR", b);

    const auto unknowns = [&](u32 phase) {
        for (const Unknown& u : info.unknown) {
            const bool here = phase == kAfterIdat ? (u.location & kAfterIdat) != 0
                : phase == kHavePlte              ? (u.location & kHavePlte) && !(u.location & kAfterIdat)
                                                  : u.location && !(u.location & (kHavePlte | kAfterIdat));
            if (here) out.chunk(u.name.c_str(), u.data);
        }
    };

    if (info.has_gama) {
        if (info.gamma < 0.0f) warning();
        b.clear();
        u32be(fixed_100000(info.gamma));
        out.chunk("gAMA", b);
    }
    if (info.has_srgb) out.chunk("sRGB", {info.srgb});
    if (info.has_iccp) {
        const std::string name = checked_keyword(info.iccp_name);
        const std::vector<u8> z =
            deflate_all(info.iccp_profile.data(), info.iccp_profile.size(), static_cast<int>(c.level),
                c.filters == kFilterNone ? Z_DEFAULT_STRATEGY : Z_FILTERED);
        out.start("iCCP", name.size() + 2 + z.size());
        out.data(name, true);
        const u8 method = 0;
        out.data(&method, 1);
        out.data(z.data(), z.size());
        out.end();
    }
    if (info.has_sbit) {
        b.clear();
        const u8 maxbits = static_cast<u8>(color == 3 ? 8 : depth);
        const auto ok = [&](u8 v) { return v != 0 && v <= maxbits; };
        if (color & 2) {
            if (!ok(info.sbit[0]) || !ok(info.sbit[1]) || !ok(info.sbit[2])) warning();
            b.insert(b.end(), {info.sbit[0], info.sbit[1], info.sbit[2]});
        } else {
            if (!ok(info.sbit[3])) warning();
            b.push_back(info.sbit[3]);
        }
        if (color & 4) {
            if (!ok(info.sbit[4])) warning();
            b.push_back(info.sbit[4]);
        }
        out.chunk("sBIT", b);
    }
    if (info.has_chrm) {
        b.clear();
        for (u32 k = 0; k < 4; k++) {
            const double x = static_cast<double>(info.chrm[2 * k]), y = static_cast<double>(info.chrm[2 * k + 1]);
            if (x < 0.0 || x > 0.8 || y < 0.0 || y > 0.8 || x + y > 1.0) warning(); // "Invalid cHRM ... point"
            u32be(fixed_100000(info.chrm[2 * k]));
            u32be(fixed_100000(info.chrm[2 * k + 1]));
        }
        out.chunk("cHRM", b);
    }
    unknowns(kHaveIhdr);

    const u32 num_palette = static_cast<u32>(info.palette.size() / 3);
    if (info.has_plte) {
        if (!(color & 2)) warning(); // "Ignoring request to write a PLTE chunk in grayscale PNG"
        out.chunk("PLTE", info.palette);
    }
    if (info.has_trns) {
        b.clear();
        if (color == 3) {
            if (info.num_trans > num_palette) warning(); // "Invalid number of transparent colors specified"
            b.resize(info.num_trans, 0);
            std::copy_n(info.trans.begin(), std::min<usize>(info.trans.size(), b.size()), b.begin());
        } else if (color == 0) {
            if (info.trans_gray >= (1u << depth)) warning(); // out of range for the bit depth
            u16be(info.trans_gray);
        } else if (color == 2) {
            u16be(info.trans_red);
            u16be(info.trans_green);
            u16be(info.trans_blue);
            if (depth == 8 && (b[0] | b[2] | b[4])) warning(); // a 16-bit tRNS at 8 bits
        } else {
            warning(); // "Can't write tRNS with an alpha channel"
        }
        out.chunk("tRNS", b);
    }
    if (info.has_bkgd) {
        b.clear();
        if (color == 3) {
            // "Invalid background palette index" past the palette (its last + 1 passes), and past the bit depth.
            if (info.bkgd_index > num_palette || info.bkgd_index >= (1u << depth)) warning();
            b.push_back(info.bkgd_index);
        } else if (color & 2) {
            u16be(info.bkgd_red);
            u16be(info.bkgd_green);
            u16be(info.bkgd_blue);
            if (depth == 8 && (b[0] | b[2] | b[4])) warning();
        } else {
            if (info.bkgd_gray >= (1u << depth)) warning();
            u16be(info.bkgd_gray);
        }
        out.chunk("bKGD", b);
    }
    if (info.has_hist) {
        b.clear();
        for (const u16 h : info.hist) u16be(h);
        out.chunk("hIST", b);
    }
    if (info.has_offs) {
        b.clear();
        u32be(static_cast<u32>(info.offs_x));
        u32be(static_cast<u32>(info.offs_y));
        b.push_back(info.offs_unit);
        out.chunk("oFFs", b);
    }
    if (info.has_pcal) {
        const std::string purpose = checked_keyword(info.pcal_purpose);
        const usize n = info.pcal_params.size();
        usize total = purpose.size() + 1 + 10 + info.pcal_units.size() + (n ? 1 : 0);
        for (usize k = 0; k < n; k++) total += info.pcal_params[k].size() + (k + 1 == n ? 0 : 1);
        out.start("pCAL", total);
        out.data(purpose, true);
        b.clear();
        u32be(static_cast<u32>(info.pcal_x0));
        u32be(static_cast<u32>(info.pcal_x1));
        b.push_back(info.pcal_type);
        b.push_back(static_cast<u8>(n));
        out.data(b.data(), b.size());
        out.data(info.pcal_units, n != 0);
        for (usize k = 0; k < n; k++) out.data(info.pcal_params[k], k + 1 != n);
        out.end();
    }
    if (info.has_scal) {
        const std::string wv = scal_number(info.scal_w), hv = scal_number(info.scal_h);
        b.assign(1, info.scal_unit);
        b.insert(b.end(), wv.begin(), wv.end());
        b.push_back(0);
        b.insert(b.end(), hv.begin(), hv.end());
        out.chunk("sCAL", b);
    }
    if (info.has_phys) {
        b.clear();
        u32be(info.phys_x);
        u32be(info.phys_y);
        b.push_back(info.phys_unit);
        out.chunk("pHYs", b);
    }
    if (info.has_time) {
        const u8* t = info.time;
        if (t[2] > 12 || t[2] < 1 || t[3] > 31 || t[3] < 1 || t[4] > 23 || t[6] > 60) warning();
        out.chunk("tIME", std::vector<u8>(t, t + 7));
    }
    for (const Splt& s : info.splt) {
        const std::string name = checked_keyword(s.name);
        const usize entry = s.depth == 8 ? 6 : 10;
        out.start("sPLT", name.size() + 2 + entry * s.entries.size());
        out.data(name, true);
        out.data(&s.depth, 1);
        for (const SpltEntry& e : s.entries) {
            b.clear();
            if (s.depth == 8)
                b.insert(b.end(), {static_cast<u8>(e.red), static_cast<u8>(e.green), static_cast<u8>(e.blue),
                                      static_cast<u8>(e.alpha)});
            else {
                u16be(e.red);
                u16be(e.green);
                u16be(e.blue);
                u16be(e.alpha);
            }
            u16be(e.frequency);
            out.data(b.data(), b.size());
        }
        out.end();
    }
    for (const Text& t : info.texts) {
        const std::string key = checked_keyword(t.key);
        if (t.compression > 0) { // iTXt, uncompressed
            const std::string lang = checked_keyword(t.lang);
            out.start("iTXt", key.size() + 5 + lang.size() + t.lang_key.size() + t.text.size());
            out.data(key, true);
            const u8 flags[2] = {0, 0};
            out.data(flags, 2);
            out.data(lang, true);
            out.data(t.lang_key, true);
            out.data(t.text, false);
            out.end();
        } else if (t.compression == 0) { // zTXt (a text that is there)
            const std::vector<u8> z = deflate_all(reinterpret_cast<const u8*>(t.text.data()), t.text.size(),
                static_cast<int>(c.level), c.filters == kFilterNone ? Z_DEFAULT_STRATEGY : Z_FILTERED);
            out.start("zTXt", key.size() + 2 + z.size());
            out.data(key, true);
            const u8 method = 0;
            out.data(&method, 1);
            out.data(z.data(), z.size());
            out.end();
        } else {
            out.start("tEXt", key.size() + 1 + t.text.size());
            out.data(key, true);
            out.data(t.text, false);
            out.end();
        }
    }
    unknowns(kHavePlte);
}

// One row of the picture as libpng 1.2.8 gets it from libpngenc, in the encoded layout: the alpha dropped (RGBA, ARGB
// to RGB; gray and alpha to gray), ARGB's alpha moved last, samples below 8 bits packed (png_do_pack: a 1-bit sample
// is any byte but 0, wider ones their low bits) unless they come packed.
void build_row(const u8* in, const Command& c, std::vector<u8>& out)
{
    const u32 w = c.width, d = c.depth;
    if (d < 8) {
        const u32 bytes = (w * d + 7) / 8;
        out.assign(bytes, 0);
        if (c.packed) {
            std::memcpy(out.data(), in, bytes);
            return;
        }
        for (u32 x = 0; x < w; x++) {
            const u32 v = d == 1 ? (in[x] != 0) : (in[x] & ((1u << d) - 1));
            const u32 bit = x * d;
            out[bit / 8] |= static_cast<u8>(v << (8 - d - bit % 8));
        }
        return;
    }
    const u32 bps = d / 8, in_comp = components(c.space), out_comp = components(c.encode_space);
    out.resize(static_cast<usize>(w) * out_comp * bps);
    for (u32 x = 0; x < w; x++) {
        const u8* src = in + static_cast<usize>(x) * in_comp * bps;
        u8* dst = out.data() + static_cast<usize>(x) * out_comp * bps;
        if (c.space == ARGB) { // A R G B: R G B, then A when it is kept
            std::memcpy(dst, src + bps, 3 * bps);
            if (out_comp == 4) std::memcpy(dst + 3 * bps, src, bps);
        } else {
            std::memcpy(dst, src, static_cast<usize>(out_comp) * bps); // the same, or the alpha left off the end
        }
    }
}

int paeth(int a, int b, int c)
{
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// png_write_find_filter (libpng 1.2.8): one filter of the mask as it is; of several, the one whose filtered row has the
// least sum of bytes taken as signed, the earlier on a tie (None, Sub, Up, Average, Paeth).
void filter_row(const std::vector<u8>& row, const std::vector<u8>& prev, u32 bpp, u32 mask, std::vector<u8>& out)
{
    const usize n = row.size();
    const auto make = [&](u32 f, std::vector<u8>& v) {
        v.resize(n + 1);
        v[0] = static_cast<u8>(f);
        for (usize i = 0; i < n; i++) {
            const int a = i >= bpp ? row[i - bpp] : 0, b = prev[i], cc = i >= bpp ? prev[i - bpp] : 0;
            const int pred = f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? paeth(a, b, cc) : 0;
            v[i + 1] = static_cast<u8>(row[i] - pred);
        }
    };
    const auto sum = [&](const std::vector<u8>& v) {
        u64 s = 0;
        for (usize i = 1; i < v.size(); i++) s += v[i] < 128 ? v[i] : 256 - v[i];
        return s;
    };
    static constexpr u32 kBits[5] = {kFilterNone, kFilterSub, kFilterUp, kFilterAvg, kFilterPaeth};
    u32 count = 0, only = 0;
    for (u32 f = 0; f < 5; f++)
        if (mask & kBits[f]) {
            count++;
            only = f;
        }
    if (count == 1) {
        make(only, out);
        return;
    }
    std::vector<u8> v;
    u64 best = ~u64{0};
    for (u32 f = 0; f < 5; f++) {
        if (!(mask & kBits[f])) continue;
        make(f, v);
        const u64 s = sum(v);
        if (s < best) {
            best = s;
            out = v;
        }
    }
}

// The rows into IDAT chunks as libpng 1.2.8 writes them: deflate row by row into a 2048-byte buffer, an IDAT each time
// it fills and the rest at the end (the first one's zlib header with the smallest window that holds the image, libpng
// 1.2's tidying); `lines` counts the rows written before a write failed.
void write_image(PPUContext& ctx, ChunkWriter& out, const Command& c, u32& lines)
{
    u8* base = gb(ctx);
    const u32 out_comp = c.depth < 8 ? 1 : components(c.encode_space);
    const u32 bpp = std::max(1u, out_comp * c.depth / 8);
    const int level = static_cast<int>(c.level);
    z_stream z{};
    if (deflateInit2(&z, level, Z_DEFLATED, 15, 8, c.filters == kFilterNone ? Z_DEFAULT_STRATEGY : Z_FILTERED) != Z_OK)
        warning();
    struct Ender {
        z_stream& z;
        ~Ender() { deflateEnd(&z); }
    } ender{z};
    u8 zbuf[kZbufSize];
    z.next_out = zbuf;
    z.avail_out = sizeof(zbuf);
    bool first = true;
    const u32 channels = c.depth < 8 ? 1 : components(c.encode_space);
    const auto idat = [&](usize n) {
        if (first && n >= 2 && c.height < 16384 && c.width < 16384) {
            const u32 unpacked = c.height * ((c.width * channels * c.depth + 15) >> 3);
            u32 cinfo = zbuf[0] >> 4, half = 1u << (cinfo + 7);
            while (unpacked <= half && half >= 256) {
                cinfo--;
                half >>= 1;
            }
            const u8 cmf = static_cast<u8>((zbuf[0] & 0x0F) | (cinfo << 4));
            if (cmf != zbuf[0]) {
                zbuf[0] = cmf;
                zbuf[1] &= 0xE0;
                zbuf[1] = static_cast<u8>(zbuf[1] + (0x1F - ((cmf << 8) + zbuf[1]) % 0x1F));
            }
        }
        first = false;
        out.start("IDAT", n);
        out.data(zbuf, n);
        out.end();
        z.next_out = zbuf;
        z.avail_out = sizeof(zbuf);
    };
    std::vector<u8> row, prev, filtered;
    for (u32 y = 0; y < c.height; y++) {
        build_row(base + c.addr + static_cast<usize>(y) * c.pitch, c, row);
        if (prev.size() != row.size()) prev.assign(row.size(), 0);
        filter_row(row, prev, bpp, c.filters, filtered);
        z.next_in = filtered.data();
        z.avail_in = static_cast<uInt>(filtered.size());
        do {
            if (deflate(&z, Z_NO_FLUSH) != Z_OK) warning();
            if (!z.avail_out) idat(sizeof(zbuf));
        } while (z.avail_in);
        if (y + 1 == c.height) { // png_write_finish_row: the compressor flushed with the last row
            int r;
            do {
                r = deflate(&z, Z_FINISH);
                if (r == Z_OK) {
                    if (!z.avail_out) idat(sizeof(zbuf));
                } else if (r != Z_STREAM_END) {
                    warning();
                }
            } while (r != Z_STREAM_END);
            if (z.avail_out < sizeof(zbuf)) idat(sizeof(zbuf) - z.avail_out);
        }
        prev.swap(row);
        lines = y + 1;
    }
}

// The stream to a file: made or emptied, then written. False when it cannot be.
bool open_file(PPUContext& ctx, u32 path, u32& fd)
{
    u8* base = gb(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    StackVar out(ctx, base, 8);
    constexpr s32 kWriteCreateTrunc = 0x1 | 0x40 | 0x200, kMode = 0x180;
    const s32 r = lv2::sys_fs_open(t, path, kWriteCreateTrunc, out.addr(), kMode, 0, 0);
    if (r != lv2::CELL_OK) {
        PS_LOG_WARN(kChan, "cellPngEncEncodePicture: {} cannot be made ({})", read_cstr(t, path, 0x400).text,
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

// One command encoded, as libpngenc's thread does it: its checks at the start (the picture's rows against its own
// colour space), the file, the chunks, png_write_info, the rows, png_write_end.
Result encode(PPUContext& ctx, const Command& c)
{
    Result r = result_of(c, 0);
    const u64 row_bytes =
        (u64{c.width} * components(c.space) * (c.depth < 8 && !c.packed ? 8 : c.depth) + 7) / 8;
    if (c.pitch < row_bytes) {
        r.state = error(CELL_PNGENC_ERROR_FATAL);
        return r;
    }
    u32 fd = 0;
    if (c.location == LOCATION_FILE && !open_file(ctx, c.file_name, fd)) {
        r.state = error(CELL_PNGENC_ERROR_STREAM_FILE_OPEN);
        return r;
    }
    Sink sink{ctx, false, 0, 0, {}, 0};
    sink.to_buffer = c.location == LOCATION_BUFFER;
    sink.addr = c.stream;
    sink.limit = c.limit;
    ChunkWriter out{sink};
    u32 lines = 0;
    Failure failed = Failure::None;
    try {
        Info info;
        u8* base = gb(ctx);
        for (u32 i = 0; i < c.chunk_count; i++)
            set_chunk(ctx, c, ps_read32(base, c.chunk_list + 8 * i), ps_read32(base, c.chunk_list + 8 * i + 4), info);
        write_info(out, c, info);
        write_image(ctx, out, c, lines);
        for (const Unknown& u : info.unknown)
            if (u.location & kAfterIdat) out.chunk(u.name.c_str(), u.data);
        out.chunk("IEND", {});
    } catch (const Abort& a) {
        failed = a.why;
    }
    if (c.location == LOCATION_FILE) {
        const bool ok = write_file(ctx, fd, sink.bytes);
        if (failed == Failure::None && !ok) failed = Failure::Fatal;
    }
    switch (failed) {
    case Failure::None:
        r.size = static_cast<u32>(sink.written);
        r.lines = c.height;
        break;
    case Failure::Overflow:
        r.state = error(CELL_PNGENC_ERROR_STREAM_OVERFLOW);
        r.lines = lines;
        break;
    default: r.state = error(CELL_PNGENC_ERROR_FATAL); break;
    }
    return r;
}

// Encodes the commands waiting while fewer than four streams wait (libpngenc's thread), one thread at a time.
void pump(PPUContext& ctx, State& s, u32 handle)
{
    u64 generation = 0;
    {
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (e == s.encoders.end() || e->second.pumping) return;
        e->second.pumping = true;
        generation = e->second.generation;
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
            skip = enc.skip_next;
            enc.skip_next = false;
        }
        const Result r = skip ? result_of(c, error(CELL_PNGENC_ERROR_STREAM_SKIP)) : encode(ctx, c);
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (e == s.encoders.end() || e->second.generation != generation) return;
        Encoder& enc = e->second;
        if (!enc.commands.empty()) enc.commands.pop_front();
        enc.streams.push_back(r);
    }
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

// Open(config, CellPngEncResource {memAddr, memSize, ppuThreadPriority, spuThreadPriority}, handle *) and
// OpenEx(config, CellPngEncResourceEx {memAddr, memSize, ppuThreadPriority, spurs, priority[8]}, handle *): libpngenc's
// checks, then an encoder at the memory's first 128-byte boundary (one there already is made anew).
s32 open(PPUContext& ctx, u32 config, u32 resource, u32 handle, bool ex)
{
    u8* base = gb(ctx);
    Config c;
    if (!handle || !in_guest(ctx, handle, 4) || !config || !in_guest(ctx, config, 28) || !read_config(base, config, c) ||
        !resource || !in_guest(ctx, resource, ex ? 24 : 16))
        return error(CELL_PNGENC_ERROR_ARG);
    const u32 mem = ps_read32(base, resource), size = ps_read32(base, resource + 4);
    const s32 ppu_prio = static_cast<s32>(ps_read32(base, resource + 8));
    if (!mem || !size || ppu_prio < 0 || ppu_prio > 0xBFF) return error(CELL_PNGENC_ERROR_ARG);
    if (ex) {
        for (u32 i = 0; i < 8; i++)
            if (ps_read8(base, resource + 16 + i) > 15) return error(CELL_PNGENC_ERROR_ARG);
    } else {
        const s32 spu_prio = static_cast<s32>(ps_read32(base, resource + 12));
        if (spu_prio < 0 || spu_prio > 0xFF) return error(CELL_PNGENC_ERROR_ARG);
    }
    if (size < mem_size(c) + 0x1780) return error(CELL_PNGENC_ERROR_ARG);
    const u32 h = (mem + 0x7F) & ~0x7Fu;
    State& s = state();
    std::lock_guard lock(s.mutex);
    Encoder e;
    e.cfg = c;
    e.generation = ++s.opens;
    s.encoders[h] = std::move(e);
    ps_write32(base, handle, h);
    PS_LOG_INFO(kChan, "cellPngEncOpen{}: handle {:#x}, up to {}x{} at {} bits", ex ? "Ex" : "", h, c.max_w, c.max_h,
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

// Reset: with fewer than five streams waiting, the commands waiting become skipped streams (eight at most) and the next
// picture is skipped as well.
s32 cellPngEncReset(PPUContext&, u32 handle)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    const auto e = s.encoders.find(handle);
    if (!handle || e == s.encoders.end()) return error(CELL_PNGENC_ERROR_ARG);
    Encoder& enc = e->second;
    if (enc.streams.size() >= 5) return error(CELL_PNGENC_ERROR_BUSY);
    for (const Command& c : enc.commands)
        if (enc.streams.size() < kStreamQueue) enc.streams.push_back(result_of(c, error(CELL_PNGENC_ERROR_STREAM_SKIP)));
    enc.commands.clear();
    enc.skip_next = true;
    enc.resets++;
    return lv2::CELL_OK;
}

// cellPngEncWaitForInput(handle, bool block): room for a command (fewer than four waiting).
s32 cellPngEncWaitForInput(PPUContext& ctx, u32 handle, u32 block)
{
    State& s = state();
    u64 resets = 0;
    bool first = true;
    for (;;) {
        {
            std::lock_guard lock(s.mutex);
            const auto e = s.encoders.find(handle);
            if (!handle || e == s.encoders.end()) return first ? error(CELL_PNGENC_ERROR_ARG) : error(CELL_PNGENC_ERROR_RESET);
            if (first) resets = e->second.resets;
            first = false;
            if (e->second.resets != resets) return error(CELL_PNGENC_ERROR_RESET);
            if (e->second.commands.size() < kQueueDepth) return lv2::CELL_OK;
            if (!(block & 0xFF)) return error(CELL_PNGENC_ERROR_BUSY);
        }
        nap(ctx);
    }
}

// cellPngEncEncodePicture(handle, CellPngEncPicture *, CellPngEncEncodeParam *, CellPngEncOutputParam *).
s32 cellPngEncEncodePicture(PPUContext& ctx, u32 handle, u32 picture, u32 param, u32 output)
{
    u8* base = gb(ctx);
    State& s = state();
    Config cfg;
    {
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (!handle || e == s.encoders.end()) return error(CELL_PNGENC_ERROR_ARG);
        cfg = e->second.cfg;
    }
    if (!picture || !in_guest(ctx, picture, 40) || !param || !in_guest(ctx, param, 24)) return error(CELL_PNGENC_ERROR_ARG);
    // CellPngEncPicture {width, height, pitchWidth, colorSpace, bitDepth, packedPixel (b8, +20), pictureAddr (+24),
    // userData (u64, +32)}; CellPngEncEncodeParam {enableSpu (b8), encodeColorSpace (+4), compressionLevel, filterType,
    // ancillaryChunkList (+16), ancillaryChunkNum}; CellPngEncOutputParam {location, streamFileName, streamAddr,
    // limitSize}.
    Command c;
    c.width = ps_read32(base, picture);
    c.height = ps_read32(base, picture + 4);
    c.pitch = ps_read32(base, picture + 8);
    c.space = ps_read32(base, picture + 12);
    c.depth = ps_read32(base, picture + 16);
    c.packed = ps_read8(base, picture + 20) != 0;
    c.addr = ps_read32(base, picture + 24);
    c.user_data = ps_read64(base, picture + 32);
    const bool spu = ps_read8(base, param) != 0;
    c.encode_space = ps_read32(base, param + 4);
    c.level = ps_read32(base, param + 8);
    c.filters = ps_read32(base, param + 12);
    c.chunk_list = ps_read32(base, param + 16);
    c.chunk_count = ps_read32(base, param + 20);
    const auto refused = [&](const char* why) {
        PS_LOG_WARN(kChan, "cellPngEncEncodePicture: {}x{} pitch {} colour space {} at {} bits{} to {}: {}", c.width,
            c.height, c.pitch, c.space, c.depth, c.packed ? " packed" : "", c.encode_space, why);
        return error(CELL_PNGENC_ERROR_ARG);
    };
    if (!c.width || c.width > cfg.max_w || !c.height || c.height > cfg.max_h) return refused("its size");
    if (spu && c.width > (0xA00u >> (cfg.max_depth >> 4))) return refused("too wide for the SPUs");
    if (c.depth > cfg.max_depth || (c.packed && c.depth > 7) || !c.addr) return refused("its depth or address");
    // The colour spaces: the same, or RGBA and ARGB to RGB, gray and alpha to gray; the depths each takes.
    u32 comps = 0;
    bool same = c.space == c.encode_space;
    const bool depth_8_16 = c.depth == 8 || c.depth == 16;
    switch (c.space) {
    case GRAYSCALE:
        if (c.depth != 1 && c.depth != 2 && c.depth != 4 && !depth_8_16) return refused("its depth");
        comps = 1;
        break;
    case RGB:
        if (!depth_8_16) return refused("its depth");
        comps = 3;
        break;
    case PALETTE:
        if (c.depth != 1 && c.depth != 2 && c.depth != 4 && c.depth != 8) return refused("its depth");
        comps = 1;
        break;
    case GRAYSCALE_ALPHA:
        comps = 2;
        if (c.encode_space == GRAYSCALE) same = true, comps = 1;
        if (!depth_8_16) return refused("its depth");
        break;
    case RGBA:
    case ARGB:
        comps = 4;
        if (c.encode_space == RGB) same = true, comps = 3;
        if (!depth_8_16) return refused("its depth");
        break;
    default: same = false; break;
    }
    if (!same) return refused("its colour space");
    // The row against the encoded colour space's samples (libpngenc's measure; the encoder checks the picture's own).
    const u32 bits = c.depth < 8 && !c.packed ? 8 : c.depth;
    if (c.pitch < (u64{comps} * bits * c.width + 7) / 8) return refused("its pitch");
    if ((!cfg.spu && spu) || c.level > 9 || !(c.filters & 0xF8) || (c.filters & ~0xF8u)) return refused("its parameters");
    if (!chunks_ok(ctx, c, cfg)) return refused("its ancillary chunks");
    if (!output || !in_guest(ctx, output, 16)) return error(CELL_PNGENC_ERROR_ARG);
    c.location = ps_read32(base, output);
    c.file_name = ps_read32(base, output + 4);
    c.stream = ps_read32(base, output + 8);
    c.limit = ps_read32(base, output + 12);
    if (c.location == LOCATION_FILE ? !c.file_name : c.location != LOCATION_BUFFER || !c.stream || c.limit <= 0x30)
        return refused("its output");
    // The picture's rows (as the encoder reads them) and the buffer must be guest memory (libpngenc reads and writes
    // whatever is there).
    if (!in_guest(ctx, c.addr, u64{c.pitch} * (c.height - 1) + (u64{components(c.space)} * bits * c.width + 7) / 8) ||
        (c.location == LOCATION_BUFFER && !in_guest(ctx, c.stream, c.limit))) {
        log_call_site(ctx, base, "cellPngEncEncodePicture: the picture or the stream is not in guest memory");
        return error(CELL_PNGENC_ERROR_ARG);
    }
    {
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (e == s.encoders.end()) return error(CELL_PNGENC_ERROR_ARG);
        if (e->second.commands.size() >= kQueueDepth) return error(CELL_PNGENC_ERROR_BUSY);
        e->second.commands.push_back(c);
    }
    pump(ctx, s, handle);
    return lv2::CELL_OK;
}

// cellPngEncWaitForOutput(handle, uint32_t *streamInfoNum, bool block): the streams waiting.
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
            if (!handle || e == s.encoders.end())
                return first ? error(CELL_PNGENC_ERROR_ARG) : error(CELL_PNGENC_ERROR_RESET);
            if (!num || !in_guest(ctx, num, 4)) return error(CELL_PNGENC_ERROR_ARG);
            if (first) resets = e->second.resets;
            first = false;
            const u32 n = static_cast<u32>(e->second.streams.size());
            if (e->second.resets != resets) {
                ps_write32(base, num, n);
                return error(CELL_PNGENC_ERROR_RESET);
            }
            if (n || !(block & 0xFF)) {
                ps_write32(base, num, n);
                return n ? lv2::CELL_OK : error(CELL_PNGENC_ERROR_EMPTY);
            }
        }
        nap(ctx);
    }
}

// cellPngEncGetStreamInfo(handle, CellPngEncStreamInfo *): {state, location, streamFileName, streamAddr, limitSize,
// streamSize, processedLine, (reserved), userData (u64, +32)}, the oldest; taking it lets the next command be encoded.
s32 cellPngEncGetStreamInfo(PPUContext& ctx, u32 handle, u32 info)
{
    u8* base = gb(ctx);
    State& s = state();
    {
        std::lock_guard lock(s.mutex);
        const auto e = s.encoders.find(handle);
        if (!handle || e == s.encoders.end() || !info || !in_guest(ctx, info, 40)) return error(CELL_PNGENC_ERROR_ARG);
        if (e->second.streams.empty()) return error(CELL_PNGENC_ERROR_EMPTY);
        const Result r = e->second.streams.front();
        e->second.streams.pop_front();
        const u32 words[] = {static_cast<u32>(r.state), r.location, r.file_name, r.addr, r.limit, r.size, r.lines, 0};
        for (u32 i = 0; i < std::size(words); i++) ps_write32(base, info + 4 * i, words[i]);
        ps_write64(base, info + 32, r.user_data);
    }
    pump(ctx, s, handle);
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

void register_cellPngEnc(Registry&) {} // without libpng (and its zlib), games get Sony's libpngenc (D-175)

#endif

} // namespace pspocket::hle
