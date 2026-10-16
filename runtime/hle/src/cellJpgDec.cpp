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
// The Ext functions (D-617) are made on this decoder as cellPngDec's and cellGifDec's are (D-595), libjpgdec being
// their sibling, without Sony's module to compare them with: they are unverified (D-594), so a game importing one
// still gets Sony's whole libjpgdec where the system software is installed. They add stream input (the JPEG's next
// bytes from the game's callback as they are needed) and partial output (outputCounts lines at a time to the game's
// display callback); see ext_open and decode_parts. RPCS3 has bare todos for them and no structures: those are
// libpngdec's (RPCS3 cellPngDec.h), inferred for libjpgdec. What is not pspocket's yet: six exports with no known
// names; a game that imports one gets Sony's whole libjpgdec (D-175).

#include <algorithm>
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
    CELL_JPGDEC_ERROR_CB_PARAM = 0x80611109,
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
// The stream (the magic, the handle, the state at +0x5A4 as libjpgdec's). From +0x40, pspocket's: what the Ext
// functions hand the game's callbacks (D-617): CellJpgDecStrmInfo, CellJpgDecStrmParam, CellJpgDecDispParam (when the
// game gives none) and CellJpgDecDispInfo (0x40 bytes).
namespace st {
constexpr u32 magic = 0x00, handle = 0x04, state = 0x5A4;
constexpr u32 strm_info = 0x40, strm_param = 0x48, disp_param = 0x50, disp_info = 0x58;
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
    // The Ext functions (D-617). Stream input: the game's stream callback (CellJpgDecCbCtrlStrm; 0: the source holds
    // the whole JPEG), ended once it gives nothing more, CB_PARAM kept when what it gave was not guest memory. Partial
    // output: the lines a display callback gets at a time (CellJpgDecExtInParam's outputCounts; 0: the whole image into
    // DecodeData's buffer) and the parameters' commandPtr.
    u32 strm_fn = 0, strm_arg = 0;
    bool strm_ended = false;
    u32 strm_error = 0;
    u32 output_counts = 0, command = 0;
};

struct JpgDecs {
    std::mutex m;
    std::map<u32, std::shared_ptr<Stream>> streams;
    bool flip_logged = false; // the Ext functions' once-a-process note (D-617)
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

// Whether [addr, addr + size) is guest memory, page by page (the parts and the stream's bytes may span many pages).
bool in_guest(PPUContext& ctx, u32 addr, u64 size)
{
    if (!addr || u64{addr} + size > 0x100000000ull) return false;
    lv2::PpuThread& t = thread_of(ctx);
    for (u64 at = 0; at < size; at += 0x1000)
        if (!guest_readable(t, static_cast<u32>(addr + at), static_cast<u32>(std::min<u64>(size - at, 0x1000))))
            return false;
    return true;
}

// ---- stream input (cellJpgDecExtOpen's CellJpgDecCbCtrlStrm, D-617; as cellPngDec's and cellGifDec's, D-595):
// before ReadHeader and DecodeData read, the bytes they will read are asked of the game's stream callback, so the
// decoder reads them as when the source held the whole JPEG: for ReadHeader the marker segments to the first scan's
// header, for DecodeData the rest to EOI (each scan's entropy-coded data and the segments between scans). The
// callback gets CellJpgDecStrmInfo (decodedStrmSize: the bytes of the stream given so far) and CellJpgDecStrmParam,
// where it puts the next part (strmPtr, strmSize), copied at once so that it may reuse its buffer. A negative answer
// or no bytes ends the stream (the decoder then finds the JPEG cut: STREAM_FORMAT); bytes that are not guest memory
// end it too and make the call CB_PARAM. Nothing is asked for past EOI.

// Whether `need` bytes are held, the stream callback asked for the rest. A stream is not taken past the PS3's main
// memory (a callback that never ends it gives a JPEG cut there).
bool more(PPUContext& ctx, u32 sa, Stream& s, usize need)
{
    constexpr usize kStreamLimit = 0x10000000;
    u8* base = guest_base(ctx);
    const u32 info = sa + st::strm_info, param = sa + st::strm_param;
    while (s.data.size() < need) {
        if (!s.strm_fn || s.strm_ended || s.data.size() >= kStreamLimit) return false;
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
        if (!in_guest(ctx, ptr, size)) {
            PS_LOG_WARN(kChan, "the stream callback gave {} bytes at {:#x}, not guest memory", size, ptr);
            s.strm_error = CELL_JPGDEC_ERROR_CB_PARAM;
            s.strm_ended = true;
            return false;
        }
        s.data.insert(s.data.end(), base + ptr, base + ptr + size);
    }
    return true;
}

// The bytes the decoder reads: the marker segments, each by its length, to the first scan's header (bytes between
// them skipped as the decoder skips them); with `to_eoi` on through the scans to EOI: in a scan's entropy-coded data a
// 0xFF is followed by a stuffed 0, a restart marker, a fill byte or the next marker.
void take_stream(PPUContext& ctx, u32 sa, Stream& s, bool to_eoi)
{
    const auto have = [&](usize n) { return more(ctx, sa, s, n); };
    if (!have(2)) return;
    usize pos = 2;
    bool scans = false;
    for (;;) {
        if (!have(pos + 2)) return;
        if (s.data[pos] != 0xFF) {
            pos++;
            continue;
        }
        const u8 m = s.data[pos + 1];
        if (m == 0xFF) {
            pos++;
            continue;
        }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || (scans && m == 0x00)) {
            pos += 2;
            continue;
        }
        if (m == 0xD9) return;
        if (!have(pos + 4)) return;
        const usize length = usize{s.data[pos + 2]} << 8 | s.data[pos + 3];
        if (length < 2 || !have(pos + 2 + length)) return;
        pos += 2 + length;
        if (m == 0xDA) {
            if (!to_eoi) return;
            scans = true;
        }
    }
}

// The stream behind `sa` when it has a stream callback and handle `h` has it in `state`: the reading calls take its
// bytes first.
std::shared_ptr<Stream> streamed(PPUContext& ctx, u32 h, u32 sa, State state)
{
    if (!live_handle(ctx, h) || !live_stream(ctx, h, sa) || ps_read32(guest_base(ctx), sa + st::state) != state) return nullptr;
    std::shared_ptr<Stream> s = stream_of(sa);
    return s && s->strm_fn ? s : nullptr;
}

// A read cut short by what the stream callback gave answers CB_PARAM.
s32 streamed_result(const std::shared_ptr<Stream>& s, s32 r)
{
    const bool cut = r == error(CELL_JPGDEC_ERROR_STREAM_FORMAT) || r == error(CELL_JPGDEC_ERROR_HEADER);
    return s && s->strm_error && cut ? error(s->strm_error) : r;
}

// Before DecodeData reads a streamed JPEG: the rest of it, and the decoder pointed at the bytes now held (the same
// header, read again: the bytes may have moved). False when it cannot be (the stream then fails).
bool stream_for_decode(PPUContext& ctx, u32 h, u32 sa)
{
    const std::shared_ptr<Stream> s = streamed(ctx, h, sa, PARAMETERS);
    if (!s) return true;
    take_stream(ctx, sa, *s, true);
    std::string why;
    s->decoder = jpeg::Decoder{};
    if (s->decoder.read_header(s->data.data(), s->data.size(), why)) return true;
    PS_LOG_WARN(kChan, "cellJpgDecDecodeData: the stream's header read again: {}", why);
    set_state(guest_base(ctx), h, sa, FAILED);
    return false;
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

// read_header, with a stream callback asked for the header's bytes first (D-617).
s32 read_header_streamed(PPUContext& ctx, u32 handle, u32 stream, u32 info)
{
    const std::shared_ptr<Stream> s = streamed(ctx, handle, stream, OPENED);
    if (s) take_stream(ctx, stream, *s, false);
    return streamed_result(s, read_header(ctx, handle, stream, info));
}

s32 cellJpgDecReadHeader(PPUContext& ctx, u32 handle, u32 stream, u32 info)
{
    const std::string call = std::format("cellJpgDecReadHeader({:#x}, {:#x}, {:#x})", handle, stream, info);
    return finish(read_header_streamed(ctx, handle, stream, info), call);
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

// The decoded image's row y in the stream's output colour space, into the game's line at o.
void write_row(const Stream& s, const jpeg::Decoder::Output& img, u32 y, u8* o)
{
    const u8* p = img.pixels.data() + usize{y} * img.width * img.components;
    for (u32 x = 0; x < s.width; x++, p += img.components) {
        const u8 r = p[0], g = img.components == 3 ? p[1] : p[0], b = img.components == 3 ? p[2] : p[0];
        switch (s.space) {
        case RGBA: *o++ = r; *o++ = g; *o++ = b; *o++ = s.alpha; break;
        case ARGB: *o++ = s.alpha; *o++ = r; *o++ = g; *o++ = b; break;
        case GRAY_TO_ALPHA_RGBA: o[3] = r; o += 4; break; // only the alpha: the gray sample
        case GRAY_TO_ALPHA_ARGB: o[0] = r; o += 4; break;
        case GRAYSCALE: *o++ = r; break;
        case UPSAMPLE_ONLY:
            if (s.components == 1) {
                *o++ = r;
                break;
            }
            [[fallthrough]];
        default: *o++ = r; *o++ = g; *o++ = b; break;
        }
    }
}

// The colour space the decoder gives for the stream's output colour space.
jpeg::ColorSpace decoder_space(const Stream& s)
{
    const bool gray_out = s.space == GRAYSCALE || s.space == GRAY_TO_ALPHA_RGBA || s.space == GRAY_TO_ALPHA_ARGB ||
        (s.space == UPSAMPLE_ONLY && s.decoder.header().color_space == jpeg::ColorSpace::Gray);
    const bool ycc_out = s.space == YCBCR || (s.space == UPSAMPLE_ONLY && !gray_out);
    const bool gray_in = s.decoder.header().color_space == jpeg::ColorSpace::Gray;
    return gray_out || gray_in ? jpeg::ColorSpace::Gray : ycc_out ? jpeg::ColorSpace::YCbCr : jpeg::ColorSpace::RGB;
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
    jpeg::Decoder::Output img;
    const jpeg::ColorSpace want = decoder_space(*s);
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
    for (u32 y = 0; y < lines; y++)
        write_row(*s, img, y, base + data + static_cast<u32>((s->mode == 1 ? s->height - 1 - y : y) * pitch));
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
    if (!stream_for_decode(ctx, handle, stream)) return finish(error(CELL_JPGDEC_ERROR_STREAM_FORMAT), call);
    return finish(streamed_result(stream_of(stream), decode_data(ctx, handle, stream, data, ctrl, out)), call);
}

// ---- the Ext functions (D-617): made on the decoder above as cellPngDec's and cellGifDec's are (D-595: from RPCS3
// rpcs3/Emu/Cell/Modules/cellPngDec.cpp, whose structures, rpcs3/Emu/Cell/Modules/cellPngDec.h, are taken for
// libjpgdec's: RPCS3's cellJpgDec.cpp has bare todos for these five and cellJpgDec.h none of their structures).
// Unverified (D-594): not compared with Sony's libjpgdec, which serves a game importing one wherever the system
// software is installed.

// CellJpgDecExtThreadInParam (spurs, priority[8], maxContention) and CellJpgDecExtThreadOutParam (reserved) are
// libjpgdec's SPU side on the game's SPURS. pspocket decodes on the calling thread: both are only checked for being
// guest memory when given, and the decoder is Create's, the same memory.
s32 cellJpgDecExtCreate(PPUContext& ctx, u32 handle_out, u32 thread_in, u32 thread_out, u32 ext_in, u32 ext_out)
{
    const std::string call = std::format("cellJpgDecExtCreate({:#x}, {:#x}, {:#x}, {:#x}, {:#x})", handle_out, thread_in,
        thread_out, ext_in, ext_out);
    if ((ext_in && !readable(ctx, ext_in, 16)) || (ext_out && !readable(ctx, ext_out, 4)))
        return finish(error(CELL_JPGDEC_ERROR_ARG), call);
    return finish(create(ctx, handle_out, thread_in, thread_out), call);
}

// Open with stream input: CellJpgDecCbCtrlStrm (cbCtrlStrmFunc, cbCtrlStrmArg; a function of 0 is no stream input).
// The source's buffer is the stream's first part, the rest comes from the callback as the decoder needs it
// (take_stream). Only from a buffer: a stream callback with a file source is STREAM_FORMAT (as RPCS3's cellPngDec has
// it), answered before the handle is taken. A sixth argument (libpngdec's CellPngDecOpnParam; a JPEG has no chunks to
// select) is not read.
s32 ext_open(PPUContext& ctx, u32 h, u32 stream_out, u32 src, u32 open_info, u32 cb_strm)
{
    if (!live_handle(ctx, h)) return error(CELL_JPGDEC_ERROR_ARG);
    if (cb_strm && !readable(ctx, cb_strm, 8)) return error(CELL_JPGDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    const u32 strm_fn = cb_strm ? ps_read32(base, cb_strm) : 0, strm_arg = cb_strm ? ps_read32(base, cb_strm + 4) : 0;
    if (strm_fn && readable(ctx, src, 4) && ps_read32(base, src) != 1) {
        PS_LOG_WARN(kChan, "cellJpgDecExtOpen: a stream callback with a source that is not a buffer");
        return error(CELL_JPGDEC_ERROR_STREAM_FORMAT);
    }
    if (const s32 r = open(ctx, h, stream_out, src, open_info); r != lv2::CELL_OK || !strm_fn) return r;
    if (const std::shared_ptr<Stream> s = stream_of(ps_read32(base, stream_out))) {
        s->strm_fn = strm_fn;
        s->strm_arg = strm_arg;
    }
    return lv2::CELL_OK;
}

s32 cellJpgDecExtOpen(PPUContext& ctx, u32 handle, u32 stream_out, u32 src, u32 open_info, u32 cb_strm)
{
    const std::string call = std::format("cellJpgDecExtOpen({:#x}, {:#x}, {:#x}, {:#x}, {:#x})", handle, stream_out, src,
        open_info, cb_strm);
    return finish(ext_open(ctx, handle, stream_out, src, open_info, cb_strm), call);
}

// ReadHeader, and CellJpgDecExtInfo's reserved word (8 bytes) set to 0 first, when the game gives one (as RPCS3's
// cellPngDecExtReadHeader does).
s32 cellJpgDecExtReadHeader(PPUContext& ctx, u32 handle, u32 stream, u32 info, u32 ext_info)
{
    const std::string call = std::format("cellJpgDecExtReadHeader({:#x}, {:#x}, {:#x}, {:#x})", handle, stream, info,
        ext_info);
    if (ext_info) {
        std::shared_ptr<Stream> s;
        if (const s32 r = checked(ctx, handle, stream, OPENED, s); r != lv2::CELL_OK) return finish(r, call);
        if (!readable(ctx, ext_info, 8)) return finish(error(CELL_JPGDEC_ERROR_ARG), call);
        ps_write64(guest_base(ctx), ext_info, 0);
    }
    return finish(read_header_streamed(ctx, handle, stream, info), call);
}

// SetParameter, and the parts: CellJpgDecExtInParam (bufferMode, outputCounts, spuMode) and CellJpgDecExtOutParam
// (outputWidthByte, 8 bytes; outputHeight), both optional. bufferMode must be CELL_JPGDEC_LINE_MODE (1), else ARG,
// refused before the output is set (the stream fails, as for any refused parameter). outputCounts lines go to each
// display callback; 0 gives the whole image to DecodeData's buffer. extOutParam: the row and the lines of a part (the
// image's height for 0). spuMode does nothing here.
s32 ext_set_parameter(PPUContext& ctx, u32 h, u32 sa, u32 in, u32 out, u32 ext_in, u32 ext_out)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, HEADER, s); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    if ((ext_in && !readable(ctx, ext_in, 12)) || (ext_out && !readable(ctx, ext_out, 12))) return error(CELL_JPGDEC_ERROR_ARG);
    if (ext_in && ps_read32(base, ext_in) != 1) {
        PS_LOG_WARN(kChan, "cellJpgDecExtSetParameter: buffer mode {}", ps_read32(base, ext_in));
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_ARG);
    }
    if (const s32 r = set_parameter(ctx, h, sa, in, out); r != lv2::CELL_OK) return r;
    s->command = ps_read32(base, in); // CellJpgDecInParam's commandPtr: CELL_JPGDEC_STOP stops between parts
    s->output_counts = ext_in ? ps_read32(base, ext_in + 4) : 0;
    if (ext_in && ext_out) {
        ps_write64(base, ext_out, u64{s->width} * s->components);
        ps_write32(base, ext_out + 8, s->output_counts ? std::min(s->output_counts, s->height) : s->height);
    }
    return lv2::CELL_OK;
}

s32 cellJpgDecExtSetParameter(PPUContext& ctx, u32 handle, u32 stream, u32 in_param, u32 out_param, u32 ext_in,
    u32 ext_out)
{
    const std::string call = std::format("cellJpgDecExtSetParameter({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x})", handle,
        stream, in_param, out_param, ext_in, ext_out);
    return finish(ext_set_parameter(ctx, handle, stream, in_param, out_param, ext_in, ext_out), call);
}

// Partial output, as cellPngDec's (D-595, decode_parts there): the image goes out outputCounts lines at a time, each
// part written at the frame's line pitch (outputBytesPerLine) from the address CellJpgDecDispParam's nextOutputImage
// gives, then handed to the display callback (CellJpgDecCbCtrlDisp: cbCtrlDispFunc, cbCtrlDispArg) with
// CellJpgDecDispInfo, which may point nextOutputImage elsewhere for the next part. The image is decoded whole first
// (the decoder decodes every scan before its output), then goes out top to bottom, each line once: scanPassCount is 0
// throughout, and the row order (outputMode) is top to bottom in parts (noted once). Every part has its callback, the
// last one too, with the part's own height and nextOutputStartY the next part's first line. The callback is given the
// game's own CellJpgDecDispParam when it passes one, else the stream's, which starts at `data` (as a first
// nextOutputImage of 0 does). CELL_JPGDEC_STOP (1) at the parameters' commandPtr after a callback stops the rest:
// status STOP (outputLines the lines given), CELL_OK. A part's place that is not guest memory: ARG for the first,
// CB_PARAM for one a callback gave; the stream fails. A JPEG that ends early gives its decoded lines in parts, then
// STREAM_FORMAT, as DecodeData does; damaged data the whole image, then STREAM_FORMAT.
s32 decode_parts(PPUContext& ctx, u32 h, u32 sa, Stream& s, u32 data, u32 ctrl, u32 out, u32 cb_disp, u32 disp_param)
{
    u8* base = guest_base(ctx);
    if (!readable(ctx, ctrl, 8) || !readable(ctx, out, 0x0C) || !readable(ctx, cb_disp, 8) || !ps_read32(base, cb_disp) ||
        (disp_param && !readable(ctx, disp_param, 4)))
        return error(CELL_JPGDEC_ERROR_ARG);
    const u64 pitch = ps_read64(base, ctrl), row = u64{s.width} * s.components;
    if (pitch < row) {
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_ARG);
    }
    const u32 disp_fn = ps_read32(base, cb_disp), disp_arg = ps_read32(base, cb_disp + 4);
    const u32 param = disp_param ? disp_param : sa + st::disp_param;
    u32 image = disp_param ? ps_read32(base, disp_param) : 0;
    if (!image) image = data;
    if (!disp_param) ps_write32(base, param, image);
    // A part of n lines at `at`, as guest memory.
    const auto fits = [&](u32 at, u32 n) {
        const u64 size = pitch * (n - 1) + row;
        return size <= 0xFFFFFFFFull && in_guest(ctx, at, size);
    };
    if (s.height && !fits(image, std::min(s.output_counts, s.height))) {
        log_call_site(ctx, base, std::format("cellJpgDecExtDecodeData: the first part's place ({:#x}) is not guest memory", image));
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_ARG);
    }
    // The image is decoded whole before its parts go out (DecodeData bounds it by the game's buffer; parts have none
    // that holds it): no larger than the PS3's main memory (D-619).
    constexpr u64 kPartsImageMax = 0x10000000;
    if (row * s.height > kPartsImageMax) {
        PS_LOG_WARN(kChan, "cellJpgDecExtDecodeData: a {}x{} image in parts", s.width, s.height);
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_FATAL);
    }
    if (s.mode == 1) {
        JpgDecs& d = decs();
        std::lock_guard lock(d.m);
        if (!d.flip_logged) PS_LOG_WARN(kChan, "cellJpgDecExtDecodeData: parts go top to bottom (logged once)");
        d.flip_logged = true;
    }
    std::string why;
    jpeg::Decoder::Output img;
    const jpeg::ColorSpace want = decoder_space(s);
    if (!s.decoder.decode(why) || !s.decoder.output(static_cast<int>(s.scale), s.method == 5, want, img, why)) {
        PS_LOG_WARN(kChan, "cellJpgDecExtDecodeData: {}", why);
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_STREAM_FORMAT);
    }
    const bool ran_out = s.decoder.ran_out();
    const u32 lines = s.decoder.lines_written(static_cast<int>(s.scale), s.method == 5, want, s.height);
    ps_write32(base, out + 8, 1); // CellJpgDecDataOutInfo's status: stopped, until the end
    const u32 info = sa + st::disp_info;
    for (u32 start = 0; start < lines;) {
        const u32 n = std::min(s.output_counts, lines - start);
        for (u32 i = 0; i < n; i++) write_row(s, img, start + i, base + image + static_cast<u32>(i * pitch));
        // CellJpgDecDispInfo (CellPngDecDispInfo's layout): outputFrameWidthByte (8 bytes), outputFrameHeight,
        // outputStartXByte (8 bytes at +0x10), outputStartY, outputWidthByte (8 bytes at +0x20), outputHeight,
        // outputBitDepth, outputComponents, nextOutputStartY, scanPassCount, outputImage.
        ps_write64(base, info + 0x00, pitch);
        ps_write32(base, info + 0x08, s.height);
        ps_write64(base, info + 0x10, 0);
        ps_write32(base, info + 0x18, start);
        ps_write64(base, info + 0x20, row);
        ps_write32(base, info + 0x28, n);
        ps_write32(base, info + 0x2C, 8);
        ps_write32(base, info + 0x30, s.components);
        ps_write32(base, info + 0x34, start + n);
        ps_write32(base, info + 0x38, 0);
        ps_write32(base, info + 0x3C, image);
        call_guest(ctx, disp_fn, {info, param, disp_arg});
        start += n;
        if (start == lines) break;
        if (s.command && readable(ctx, s.command, 4) && ps_read32(base, s.command) == 1) { // CELL_JPGDEC_STOP
            ps_write32(base, out + 4, start);
            set_state(base, h, sa, DECODED);
            return lv2::CELL_OK;
        }
        image = ps_read32(base, param);
        if (!fits(image, std::min(s.output_counts, lines - start))) {
            log_call_site(ctx, base, std::format("cellJpgDecExtDecodeData: the display callback's next place ({:#x}) is "
                                                 "not guest memory", image));
            set_state(base, h, sa, FAILED);
            return error(CELL_JPGDEC_ERROR_CB_PARAM);
        }
    }
    ps_write32(base, out + 4, lines);
    if (ran_out) {
        PS_LOG_WARN(kChan, "cellJpgDecExtDecodeData: the file ends before its image does ({} of {} lines)", lines, s.height);
        set_state(base, h, sa, FAILED);
        return error(CELL_JPGDEC_ERROR_STREAM_FORMAT);
    }
    ps_write32(base, out + 0, std::bit_cast<u32>(s.decoder.mean()));
    ps_write32(base, out + 8, 0);
    set_state(base, h, sa, DECODED);
    if (s.decoder.warnings()) {
        PS_LOG_WARN(kChan, "cellJpgDecExtDecodeData: damaged data ({} libjpeg warnings)", s.decoder.warnings());
        return error(CELL_JPGDEC_ERROR_STREAM_FORMAT);
    }
    return lv2::CELL_OK;
}

// DecodeData, in parts when there is a display callback and ExtSetParameter set outputCounts, else whole into `data`.
s32 ext_decode_data(PPUContext& ctx, u32 h, u32 sa, u32 data, u32 ctrl, u32 out, u32 cb_disp, u32 disp_param)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, PARAMETERS, s); r != lv2::CELL_OK) return r;
    if (!cb_disp || !s->output_counts) return decode_data(ctx, h, sa, data, ctrl, out);
    return decode_parts(ctx, h, sa, *s, data, ctrl, out, cb_disp, disp_param);
}

s32 cellJpgDecExtDecodeData(PPUContext& ctx, u32 handle, u32 stream, u32 data, u32 ctrl, u32 out, u32 cb_disp,
    u32 disp_param)
{
    const std::string call = std::format("cellJpgDecExtDecodeData({:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x}, {:#x})",
        handle, stream, data, ctrl, out, cb_disp, disp_param);
    if (!stream_for_decode(ctx, handle, stream)) return finish(error(CELL_JPGDEC_ERROR_STREAM_FORMAT), call);
    return finish(streamed_result(stream_of(stream), ext_decode_data(ctx, handle, stream, data, ctrl, out, cb_disp,
        disp_param)), call);
}

// libjpgdec's unnamed 0xDB9AFFCE (D-1066): it sets r8 = r9 = 0 and r10 = 1, then branches into the body
// cellJpgDecExtDecodeData shares with cellJpgDecDecodeData (r10 = 1 is the Ext entry): cellJpgDecExtDecodeData without
// a display callback (scripts/nid_audit.py).
s32 cellJpgDec_DB9AFFCE(PPUContext& ctx, u32 handle, u32 stream, u32 data, u32 ctrl, u32 out)
{
    return cellJpgDecExtDecodeData(ctx, handle, stream, data, ctrl, out, 0, 0);
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
        // Made as cellPngDec's and cellGifDec's are, not compared with Sony's libjpgdec (D-594, D-617).
        PS_HLE_UNVERIFIED(0x8B300F66, cellJpgDecExtCreate),
        PS_HLE_UNVERIFIED(0xA9F703E3, cellJpgDecExtOpen),
        PS_HLE_UNVERIFIED(0xB91EB3D2, cellJpgDecExtReadHeader),
        PS_HLE_UNVERIFIED(0x65CBBB16, cellJpgDecExtSetParameter),
        PS_HLE_UNVERIFIED(0x716F8792, cellJpgDecExtDecodeData),
        PS_HLE_UNVERIFIED(0xDB9AFFCE, cellJpgDec_DB9AFFCE),
    });
}

} // namespace pspocket::hle
