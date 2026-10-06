// cellGifDec: the GIF decoder games use for their images (D-202), pspocket's own, written here to answer as Sony's
// libgifdec does. libgifdec is giflib (its 12-byte colour maps and its private state with the LZW tables show in what
// it allocates) behind the same small API as libjpgdec: a handle made with the game's malloc and free callbacks, one
// stream at a time (a file or a buffer), its screen descriptor, the output wanted (RGBA or ARGB, two alphas), then the
// file's records one DecodeData at a time: an extension (its sub-blocks handed back in a buffer), an image (the whole
// logical screen), or the trailer.
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellGifDec.cpp/.h (the structures). Sony's behaviour is measured call by call
// in tests/hle/test_gifdec.cpp against the user's libgifdec (run 108), with SPU threads off and on:
// - An image record is the logical screen: every pixel the background index looked up in the image's colour table
//   (the local one if it has one, else the global one), the image drawn at its place; a pixel whose index (the
//   background's included) is the last graphic control extension's transparent one gets alpha1 (its colour kept),
//   every other alpha2. The transparency stays for later images until another graphic control extension. No earlier
//   image shows.
// - useMemorySpace is width x height + 4 x height + 40 (the index raster and its row pointers).
// - A call out of order answers SEQ and leaves the stream refusing everything but Close. Once the parameters are set,
//   DecodeData writes the status (0) first; a stop asked through commandPtr writes 1 and decodes nothing; an image past
//   the screen, a missing colour table, bad or cut LZW data, a missing trailer: STREAM_FORMAT, the game's buffer left
//   as it was.
// - Memory: the game's allocator gives what libgifdec takes (Create 0x380, plus 0x33880 with SPU threads; Open
//   0x140, 0x100 (the extension buffer), 0x5C and 0x6148; a colour table 12 bytes plus 3 per colour; per image the
//   raster and its row pointers), all given back by Close and Destroy.
// What is not pspocket's yet: the Ext functions; a game that imports one gets Sony's whole libgifdec (D-175).

#include <algorithm>
#include <array>
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

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellGifDec";

enum : u32 {
    CELL_GIFDEC_ERROR_OPEN_FILE = 0x80611300,
    CELL_GIFDEC_ERROR_STREAM_FORMAT = 0x80611301,
    CELL_GIFDEC_ERROR_SEQ = 0x80611302,
    CELL_GIFDEC_ERROR_ARG = 0x80611303,
    CELL_GIFDEC_ERROR_FATAL = 0x80611304,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr u32 kCodecVersion = 0x00240000;
constexpr u32 kHandleMagic = 0x47494644, kStreamMagic = 0x47494653; // pspocket's own ("GIFD", "GIFS")
constexpr u32 kHandleSize = 0x380, kSpuBlock = 0x33880;
constexpr u32 kStreamSize = 0x140, kExtBufferSize = 0x100;
constexpr u32 kOpenBlocks[2] = {0x5C, 0x6148}; // giflib's file and private state (the LZW tables)
constexpr u32 kInitSpaceAllocated = 0x6D70;
constexpr u32 RGBA = 10, ARGB = 20;
enum : u32 { RECORD_IMAGE = 1, RECORD_EXTENSION = 2, RECORD_TERMINATE = 3 };

namespace hd {
constexpr u32 magic = 0x00, malloc_fn = 0x04, malloc_arg = 0x08, free_fn = 0x0C, free_arg = 0x10, busy = 0x1C,
              stream = 0x20, spu_block = 0x24;
}
namespace st {
constexpr u32 magic = 0x00, handle = 0x04, state = 0x08;
}
enum State : u32 { OPENED = 1, HEADER = 2, PARAMETERS = 3, FAILED = 6 };

struct ColorTable {
    u32 entries = 0; // 0: none
    std::array<std::array<u8, 3>, 256> colors{};
};

struct Stream {
    std::vector<u8> data;
    usize pos = 0;
    // the logical screen descriptor
    u32 width = 0, height = 0, background = 0;
    ColorTable global;
    // the parameters
    u32 command = 0, space = RGBA;
    u8 alpha1 = 0, alpha2 = 0;
    // the last graphic control extension's transparency (it stays for every image after it until the next one)
    bool transparent = false;
    u32 transparent_index = 0;
    bool terminated = false;
    u32 ext_buffer = 0, ext_capacity = 0;
    std::vector<u32> blocks; // what the stream took from the game's allocator, in order
};

struct GifDecs {
    std::mutex m;
    std::map<u32, std::shared_ptr<Stream>> streams;
};

GifDecs& decs() { return HleProcess::current()->state<GifDecs>(); }

std::shared_ptr<Stream> stream_of(u32 s)
{
    GifDecs& d = decs();
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

// A block from the game's allocator, kept to be given back at Close.
u32 take(PPUContext& ctx, u32 h, Stream& s, u32 size)
{
    const u32 p = guest_malloc(ctx, h, size);
    if (p) s.blocks.push_back(p);
    return p;
}

s32 create(PPUContext& ctx, u32 handle_out, u32 in, u32 out)
{
    u8* base = guest_base(ctx);
    if (!readable(ctx, handle_out, 4) || !readable(ctx, in, 0x1C) || !readable(ctx, out, 4)) return error(CELL_GIFDEC_ERROR_ARG);
    // CellGifDecThreadInParam: spuThreadEnable, ppuThreadPriority, spuThreadPriority, malloc, its argument, free, its
    // argument.
    const u32 malloc_fn = ps_read32(base, in + 12), free_fn = ps_read32(base, in + 20);
    if (!malloc_fn || !free_fn) return error(CELL_GIFDEC_ERROR_ARG);
    const u32 malloc_arg = ps_read32(base, in + 16), free_arg = ps_read32(base, in + 24);
    const u32 h = static_cast<u32>(call_guest(ctx, malloc_fn, {kHandleSize, malloc_arg}));
    if (!h || !readable(ctx, h, kHandleSize)) return error(CELL_GIFDEC_ERROR_FATAL);
    std::memset(base + h, 0, kHandleSize);
    if (ps_read32(base, in) != 0) { // spuThreadEnable: libgifdec's SPURS memory, taken the same
        const u32 p = static_cast<u32>(call_guest(ctx, malloc_fn, {kSpuBlock, malloc_arg}));
        if (!p) {
            call_guest(ctx, free_fn, {h, free_arg});
            return error(CELL_GIFDEC_ERROR_FATAL);
        }
        ps_write32(base, h + hd::spu_block, p);
    }
    ps_write32(base, h + hd::malloc_fn, malloc_fn);
    ps_write32(base, h + hd::malloc_arg, malloc_arg);
    ps_write32(base, h + hd::free_fn, free_fn);
    ps_write32(base, h + hd::free_arg, free_arg);
    ps_write32(base, h + hd::magic, kHandleMagic);
    ps_write32(base, handle_out, h);
    ps_write32(base, out, kCodecVersion); // CellGifDecThreadOutParam
    return lv2::CELL_OK;
}

s32 cellGifDecCreate(PPUContext& ctx, u32 handle_out, u32 thread_in, u32 thread_out)
{
    const std::string call = std::format("cellGifDecCreate({:#x}, {:#x}, {:#x})", handle_out, thread_in, thread_out);
    return finish(create(ctx, handle_out, thread_in, thread_out), call);
}

s32 destroy(PPUContext& ctx, u32 h)
{
    if (!live_handle(ctx, h)) return error(CELL_GIFDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (ps_read32(base, h + hd::busy)) return error(CELL_GIFDEC_ERROR_SEQ);
    ps_write32(base, h + hd::magic, 0);
    const u32 free_fn = ps_read32(base, h + hd::free_fn), free_arg = ps_read32(base, h + hd::free_arg);
    if (const u32 p = ps_read32(base, h + hd::spu_block)) call_guest(ctx, free_fn, {p, free_arg});
    call_guest(ctx, free_fn, {h, free_arg});
    return lv2::CELL_OK;
}

s32 cellGifDecDestroy(PPUContext& ctx, u32 handle)
{
    return finish(destroy(ctx, handle), std::format("cellGifDecDestroy({:#x})", handle));
}

// The source's bytes (CellGifDecSrc: srcSelect, fileName, fileOffset (8 bytes at +8), fileSize, streamPtr,
// streamSize, spuThreadEnable).
s32 read_source(PPUContext& ctx, u32 src, std::vector<u8>& out)
{
    u8* base = guest_base(ctx);
    const u32 select = ps_read32(base, src);
    if (select == 1) {
        const u32 ptr = ps_read32(base, src + 20), size = ps_read32(base, src + 24);
        if (!ptr || !size) return error(CELL_GIFDEC_ERROR_ARG);
        if (!readable(ctx, ptr, size)) {
            log_call_site(ctx, base, std::format("cellGifDecOpen: the buffer ({:#x}, {} bytes) is not guest memory", ptr, size));
            return error(CELL_GIFDEC_ERROR_ARG);
        }
        out.assign(base + ptr, base + ptr + size);
        return lv2::CELL_OK;
    }
    if (select != 0) return error(CELL_GIFDEC_ERROR_ARG);
    const GuestString name = read_cstr(thread_of(ctx), ps_read32(base, src + 4), 1024);
    if (!name.terminated) return error(CELL_GIFDEC_ERROR_ARG);
    std::vector<u8> file;
    if (!thread_of(ctx).kernel.vfs_read_file(name.text, file)) {
        PS_LOG_WARN(kChan, "cellGifDecOpen: {} cannot be read", name.text);
        return error(CELL_GIFDEC_ERROR_OPEN_FILE);
    }
    const u64 offset = ps_read64(base, src + 8);
    const u32 size = ps_read32(base, src + 16);
    if (offset > file.size()) return error(CELL_GIFDEC_ERROR_OPEN_FILE);
    const usize end = size ? std::min<usize>(file.size(), offset + size) : file.size();
    out.assign(file.begin() + static_cast<std::ptrdiff_t>(offset), file.begin() + static_cast<std::ptrdiff_t>(end));
    return lv2::CELL_OK;
}

s32 open(PPUContext& ctx, u32 h, u32 stream_out, u32 src, u32 open_info)
{
    if (!live_handle(ctx, h)) return error(CELL_GIFDEC_ERROR_ARG);
    if (!readable(ctx, stream_out, 4)) return error(CELL_GIFDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (ps_read32(base, h + hd::busy)) return error(CELL_GIFDEC_ERROR_SEQ);
    ps_write32(base, h + hd::busy, 1);
    if (!readable(ctx, src, 0x20) || !readable(ctx, open_info, 4)) return error(CELL_GIFDEC_ERROR_ARG);
    auto s = std::make_shared<Stream>();
    if (const s32 r = read_source(ctx, src, s->data); r != lv2::CELL_OK) return r;
    const u32 sa = take(ctx, h, *s, kStreamSize);
    if (!sa || !readable(ctx, sa, kStreamSize)) return error(CELL_GIFDEC_ERROR_FATAL);
    s->ext_buffer = take(ctx, h, *s, kExtBufferSize);
    s->ext_capacity = kExtBufferSize;
    for (const u32 size : kOpenBlocks)
        if (!take(ctx, h, *s, size)) return error(CELL_GIFDEC_ERROR_FATAL);
    if (!s->ext_buffer) return error(CELL_GIFDEC_ERROR_FATAL);
    std::memset(base + sa, 0, kStreamSize);
    ps_write32(base, sa + st::magic, kStreamMagic);
    ps_write32(base, sa + st::handle, h);
    ps_write32(base, sa + st::state, OPENED);
    ps_write32(base, h + hd::stream, sa);
    {
        GifDecs& d = decs();
        std::lock_guard lock(d.m);
        d.streams[sa] = s;
    }
    ps_write32(base, open_info, kInitSpaceAllocated); // CellGifDecOpnInfo
    ps_write32(base, stream_out, sa);
    return lv2::CELL_OK;
}

s32 cellGifDecOpen(PPUContext& ctx, u32 handle, u32 stream_out, u32 src, u32 open_info)
{
    const std::string call = std::format("cellGifDecOpen({:#x}, {:#x}, {:#x}, {:#x})", handle, stream_out, src, open_info);
    return finish(open(ctx, handle, stream_out, src, open_info), call);
}

// The handle and stream checked; a wrong state answers SEQ and fails the stream (libgifdec refuses everything after).
s32 checked(PPUContext& ctx, u32 h, u32 sa, std::initializer_list<State> states, std::shared_ptr<Stream>& out)
{
    if (!live_handle(ctx, h)) return error(CELL_GIFDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (!ps_read32(base, h + hd::stream)) return error(CELL_GIFDEC_ERROR_SEQ);
    if (!readable(ctx, sa, kStreamSize) || ps_read32(base, sa + st::magic) != kStreamMagic ||
        ps_read32(base, sa + st::handle) != h || ps_read32(base, h + hd::stream) != sa)
        return error(CELL_GIFDEC_ERROR_ARG);
    const u32 state = ps_read32(base, sa + st::state);
    if (std::find(states.begin(), states.end(), static_cast<State>(state)) == states.end()) {
        ps_write32(base, sa + st::state, FAILED);
        return error(CELL_GIFDEC_ERROR_SEQ);
    }
    out = stream_of(sa);
    if (!out) PS_FATAL(kChan, "stream {:#x} has no host side", sa);
    return lv2::CELL_OK;
}

s32 fail(u8* base, u32 sa, u32 code)
{
    ps_write32(base, sa + st::state, FAILED);
    return error(code);
}

s32 close(PPUContext& ctx, u32 h, u32 sa)
{
    if (!live_handle(ctx, h)) return error(CELL_GIFDEC_ERROR_ARG);
    u8* base = guest_base(ctx);
    if (!ps_read32(base, h + hd::stream)) return error(CELL_GIFDEC_ERROR_SEQ);
    if (!readable(ctx, sa, kStreamSize) || ps_read32(base, sa + st::magic) != kStreamMagic ||
        ps_read32(base, h + hd::stream) != sa)
        return error(CELL_GIFDEC_ERROR_ARG);
    std::shared_ptr<Stream> s;
    {
        GifDecs& d = decs();
        std::lock_guard lock(d.m);
        if (const auto it = d.streams.find(sa); it != d.streams.end()) {
            s = it->second;
            d.streams.erase(it);
        }
    }
    ps_write32(base, sa + st::magic, 0);
    if (s)
        for (auto it = s->blocks.rbegin(); it != s->blocks.rend(); ++it) guest_free(ctx, h, *it);
    ps_write32(base, h + hd::stream, 0);
    ps_write32(base, h + hd::busy, 0);
    return lv2::CELL_OK;
}

s32 cellGifDecClose(PPUContext& ctx, u32 handle, u32 stream)
{
    return finish(close(ctx, handle, stream), std::format("cellGifDecClose({:#x}, {:#x})", handle, stream));
}

// A colour table of 2^bits entries at the stream's position, taking giflib's 12-byte map and its colours from the
// game's allocator first (as libgifdec does).
bool read_table(PPUContext& ctx, u32 h, Stream& s, int bits, ColorTable& t)
{
    const u32 n = 1u << bits;
    take(ctx, h, s, 12);
    take(ctx, h, s, 3 * n);
    if (s.pos + 3 * n > s.data.size()) return false;
    t.entries = n;
    for (u32 i = 0; i < n; i++, s.pos += 3) t.colors[i] = {s.data[s.pos], s.data[s.pos + 1], s.data[s.pos + 2]};
    return true;
}

s32 read_header(PPUContext& ctx, u32 h, u32 sa, u32 info)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, {OPENED}, s); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    if (!readable(ctx, info, 0x20)) return fail(base, sa, CELL_GIFDEC_ERROR_ARG);
    const std::vector<u8>& d = s->data;
    if (d.size() < 13 || d[0] != 'G' || d[1] != 'I' || d[2] != 'F') return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
    s->width = u32{d[6]} | u32{d[7]} << 8;
    s->height = u32{d[8]} | u32{d[9]} << 8;
    const u8 packed = d[10];
    s->background = d[11];
    s->pos = 13;
    if ((packed & 0x80) && !read_table(ctx, h, *s, (packed & 7) + 1, s->global))
        return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
    // CellGifDecInfo: SWidth, SHeight, the global table's flag, colour resolution, sort flag, size (bits), background,
    // pixel aspect ratio.
    const u32 fields[8] = {s->width, s->height, u32{packed} >> 7, ((u32{packed} >> 4) & 7) + 1, (u32{packed} >> 3) & 1,
        (u32{packed} & 7) + 1, s->background, d[12]};
    for (u32 i = 0; i < 8; i++) ps_write32(base, info + 4 * i, fields[i]);
    ps_write32(base, sa + st::state, HEADER);
    return lv2::CELL_OK;
}

s32 cellGifDecReadHeader(PPUContext& ctx, u32 handle, u32 stream, u32 info)
{
    const std::string call = std::format("cellGifDecReadHeader({:#x}, {:#x}, {:#x})", handle, stream, info);
    return finish(read_header(ctx, handle, stream, info), call);
}

s32 set_parameter(PPUContext& ctx, u32 h, u32 sa, u32 in, u32 out)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, {HEADER}, s); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    if (!readable(ctx, in, 0x0C) || !readable(ctx, out, 0x20)) return fail(base, sa, CELL_GIFDEC_ERROR_ARG);
    // CellGifDecInParam: commandPtr, colorSpace, outputColorAlpha1, outputColorAlpha2.
    const u32 space = ps_read32(base, in + 4);
    if (space != RGBA && space != ARGB) {
        PS_LOG_WARN(kChan, "cellGifDecSetParameter: colour space {}", space);
        return fail(base, sa, CELL_GIFDEC_ERROR_ARG);
    }
    s->command = ps_read32(base, in);
    s->space = space;
    s->alpha1 = ps_read8(base, in + 8);
    s->alpha2 = ps_read8(base, in + 9);
    // CellGifDecOutParam: outputWidthByte (8 bytes), width, height, components, bit depth, colour space, useMemorySpace.
    ps_write64(base, out + 0x00, u64{s->width} * 4);
    ps_write32(base, out + 0x08, s->width);
    ps_write32(base, out + 0x0C, s->height);
    ps_write32(base, out + 0x10, 4);
    ps_write32(base, out + 0x14, 8);
    ps_write32(base, out + 0x18, space);
    ps_write32(base, out + 0x1C, s->width * s->height + 4 * s->height + 40);
    ps_write32(base, sa + st::state, PARAMETERS);
    return lv2::CELL_OK;
}

s32 cellGifDecSetParameter(PPUContext& ctx, u32 handle, u32 stream, u32 in_param, u32 out_param)
{
    const std::string call =
        std::format("cellGifDecSetParameter({:#x}, {:#x}, {:#x}, {:#x})", handle, stream, in_param, out_param);
    return finish(set_parameter(ctx, handle, stream, in_param, out_param), call);
}

// ---- the image data: giflib's LZW decoding (dgif_lib.c DGifDecompressLine, DGifDecompressInput,
// DGifBufferedInput), written here: codes from the low bit, the code width growing with the table up to 12 bits and
// kept there, a code past the next free one a defect, the end code before the image is full an error.
class Lzw {
public:
    Lzw(const std::vector<u8>& data, usize& pos, int bits) : data_(data), pos_(pos), bits_(bits)
    {
        clear_ = 1 << bits;
        eof_ = clear_ + 1;
        running_code_ = eof_ + 1;
        running_bits_ = bits + 1;
        max_code1_ = 1 << running_bits_;
        prefix_.fill(kNoCode);
    }
    // The next n pixels; false for a defect or cut data.
    bool line(u8* out, u32 n)
    {
        u32 i = 0;
        while (stack_ptr_ && i < n) out[i++] = stack_[--stack_ptr_];
        while (i < n) {
            int code = 0;
            if (!input(code)) return false;
            if (code == eof_) return false; // D_GIF_ERR_EOF_TOO_SOON
            if (code == clear_) {
                prefix_.fill(kNoCode);
                running_code_ = eof_ + 1;
                running_bits_ = bits_ + 1;
                max_code1_ = 1 << running_bits_;
                last_code_ = kNoCode;
                continue;
            }
            if (code < clear_) {
                out[i++] = static_cast<u8>(code);
            } else {
                int prefix = 0;
                if (prefix_[static_cast<usize>(code)] == kNoCode) {
                    if (code != running_code_ - 2) return false; // D_GIF_ERR_IMAGE_DEFECT
                    prefix = last_code_;
                    suffix_[static_cast<usize>(running_code_ - 2)] = stack_[stack_ptr_++] = prefix_char(last_code_);
                } else {
                    prefix = code;
                }
                int j = 0;
                while (j++ <= kMaxCode && prefix > clear_ && prefix <= kMaxCode) {
                    stack_[stack_ptr_++] = suffix_[static_cast<usize>(prefix)];
                    prefix = prefix_[static_cast<usize>(prefix)];
                }
                if (j >= kMaxCode || prefix > kMaxCode) return false;
                stack_[stack_ptr_++] = static_cast<u8>(prefix);
                while (stack_ptr_ && i < n) out[i++] = stack_[--stack_ptr_];
            }
            if (last_code_ != kNoCode && running_code_ - 2 <= kMaxCode && prefix_[static_cast<usize>(running_code_ - 2)] == kNoCode) {
                prefix_[static_cast<usize>(running_code_ - 2)] = last_code_;
                suffix_[static_cast<usize>(running_code_ - 2)] =
                    code == running_code_ - 2 ? prefix_char(last_code_) : prefix_char(code);
            }
            last_code_ = code;
        }
        return true;
    }
    // Skips what is left of the image data, up to its block terminator (DGifGetCodeNext).
    bool skip_rest()
    {
        if (block_left_ == 0 && ended_) return true;
        pos_ += block_left_;
        block_left_ = 0;
        for (;;) {
            if (pos_ >= data_.size()) return false;
            const u32 size = data_[pos_++];
            if (size == 0) return true;
            if (pos_ + size > data_.size()) return false;
            pos_ += size;
        }
    }

private:
    static constexpr int kMaxCode = 4095, kNoCode = 4098;
    u8 prefix_char(int code) const
    {
        int i = 0;
        while (code > clear_ && i++ <= kMaxCode) {
            if (code > kMaxCode) return static_cast<u8>(kNoCode); // giflib stores its NO_SUCH_CODE in a byte
            code = prefix_[static_cast<usize>(code)];
        }
        return static_cast<u8>(code);
    }
    bool byte(u8& b) // DGifBufferedInput
    {
        if (block_left_ == 0) {
            if (pos_ >= data_.size()) return false;
            block_left_ = data_[pos_++];
            if (block_left_ == 0) { // an empty block inside the data: a defect
                ended_ = true;
                return false;
            }
            if (pos_ + block_left_ > data_.size()) return false;
        }
        b = data_[pos_++];
        block_left_--;
        return true;
    }
    bool input(int& code) // DGifDecompressInput
    {
        if (running_bits_ > 12) return false;
        while (shift_state_ < running_bits_) {
            u8 b = 0;
            if (!byte(b)) return false;
            shift_dword_ |= u32{b} << shift_state_;
            shift_state_ += 8;
        }
        code = static_cast<int>(shift_dword_ & ((1u << running_bits_) - 1));
        shift_dword_ >>= running_bits_;
        shift_state_ -= running_bits_;
        if (running_code_ < kMaxCode + 2 && ++running_code_ > max_code1_ && running_bits_ < 12) {
            max_code1_ <<= 1;
            running_bits_++;
        }
        return true;
    }

    const std::vector<u8>& data_;
    usize& pos_;
    int bits_, clear_ = 0, eof_ = 0, running_code_ = 0, running_bits_ = 0, max_code1_ = 0, last_code_ = kNoCode;
    u32 shift_dword_ = 0;
    int shift_state_ = 0;
    u32 block_left_ = 0;
    bool ended_ = false;
    std::array<int, 4096> prefix_{};
    std::array<u8, 4096> suffix_{};
    std::array<u8, 4096 + 8> stack_{};
    u32 stack_ptr_ = 0;
};

// The next record into the game's buffer and its CellGifDecDataOutInfo.
s32 next_record(PPUContext& ctx, u32 h, u32 sa, Stream& s, u32 data, u64 pitch, u32 out)
{
    u8* base = guest_base(ctx);
    const std::vector<u8>& d = s.data;
    if (s.pos >= d.size()) return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT); // no trailer
    const u8 kind = d[s.pos++];
    if (kind == 0x3B) { // the trailer
        s.terminated = true;
        ps_write32(base, out + 0, RECORD_TERMINATE);
        return lv2::CELL_OK;
    }
    if (kind == 0x21) { // an extension: its label, then its sub-blocks (with the terminator) in the buffer
        if (s.pos >= d.size()) return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
        const u8 label = d[s.pos++];
        const usize start = s.pos;
        u32 length = 0;
        // Each sub-block that does not fit makes libgifdec take a larger buffer, in whole 0x100s, the bytes so far
        // copied over (measured: 0x200 then 0x300 for 620 bytes).
        const auto room = [&](u32 needed) {
            if (needed <= s.ext_capacity) return true;
            const u32 capacity = (needed + 0xFF) & ~0xFFu;
            const u32 p = take(ctx, h, s, capacity);
            if (!p) return false;
            std::memcpy(base + p, base + s.ext_buffer, length);
            s.ext_buffer = p;
            s.ext_capacity = capacity;
            return true;
        };
        for (;;) {
            if (s.pos >= d.size()) return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
            const u32 size = d[s.pos];
            if (s.pos + 1 + size > d.size()) return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
            if (!room(length + 1 + size)) return fail(base, sa, CELL_GIFDEC_ERROR_FATAL);
            std::memcpy(base + s.ext_buffer + length, d.data() + s.pos, 1 + size);
            length += 1 + size;
            s.pos += 1 + size;
            if (size == 0) break;
        }
        if (label == 0xF9 && length >= 5 && d[start] >= 4) { // the graphic control extension
            s.transparent = (d[start + 1] & 1) != 0;
            s.transparent_index = d[start + 4];
        }
        ps_write32(base, out + 0, RECORD_EXTENSION);
        ps_write8(base, out + 4, label);
        ps_write32(base, out + 8, s.ext_buffer);
        return lv2::CELL_OK;
    }
    if (kind != 0x2C) return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
    // An image: its descriptor, the screen's raster and row pointers, its colour table, its LZW data.
    if (s.pos + 9 > d.size()) return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
    const u32 left = u32{d[s.pos]} | u32{d[s.pos + 1]} << 8, top = u32{d[s.pos + 2]} | u32{d[s.pos + 3]} << 8;
    const u32 w = u32{d[s.pos + 4]} | u32{d[s.pos + 5]} << 8, ht = u32{d[s.pos + 6]} | u32{d[s.pos + 7]} << 8;
    const u8 packed = d[s.pos + 8];
    s.pos += 9;
    take(ctx, h, s, 4 * s.height);
    take(ctx, h, s, s.width * s.height);
    if (left + w > s.width || top + ht > s.height) {
        PS_LOG_WARN(kChan, "cellGifDecDecodeData: an image of {}x{} at ({}, {}) on a screen of {}x{}", w, ht, left, top,
            s.width, s.height);
        return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
    }
    ColorTable local;
    if ((packed & 0x80) && !read_table(ctx, h, s, (packed & 7) + 1, local))
        return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
    const ColorTable& table = local.entries ? local : s.global;
    if (!table.entries) return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
    if (s.pos >= d.size()) return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
    const int code_size = d[s.pos++];
    if (code_size < 1 || code_size > 8) return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
    std::vector<u8> pixels(usize{w} * ht);
    {
        Lzw lzw(d, s.pos, code_size);
        std::vector<u32> rows;
        if (packed & 0x40) { // interlaced: every 8th row from 0, every 8th from 4, every 4th from 2, every 2nd from 1
            const u32 start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
            for (int p = 0; p < 4; p++)
                for (u32 y = start[p]; y < ht; y += step[p]) rows.push_back(y);
        } else {
            for (u32 y = 0; y < ht; y++) rows.push_back(y);
        }
        for (const u32 y : rows)
            if (w && !lzw.line(pixels.data() + usize{y} * w, w)) return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
        if (!lzw.skip_rest()) return fail(base, sa, CELL_GIFDEC_ERROR_STREAM_FORMAT);
    }
    // The logical screen: the background index in this image's table with alpha2, the image at its place.
    const auto color = [&](u32 index) {
        return index < table.entries ? table.colors[index] : std::array<u8, 3>{0, 0, 0};
    };
    for (u32 y = 0; y < s.height; y++) {
        u8* o = base + data + static_cast<u32>(y * pitch);
        for (u32 x = 0; x < s.width; x++, o += 4) {
            const bool inside = x >= left && x < left + w && y >= top && y < top + ht;
            const u32 index = inside ? pixels[usize{y - top} * w + (x - left)] : s.background;
            const std::array<u8, 3> c = color(index);
            const u8 a = s.transparent && index == s.transparent_index ? s.alpha1 : s.alpha2;
            if (s.space == RGBA) {
                o[0] = c[0];
                o[1] = c[1];
                o[2] = c[2];
                o[3] = a;
            } else {
                o[0] = a;
                o[1] = c[0];
                o[2] = c[1];
                o[3] = c[2];
            }
        }
    }
    ps_write32(base, out + 0, RECORD_IMAGE);
    ps_write8(base, out + 4, 0);
    ps_write32(base, out + 8, s.ext_buffer);
    return lv2::CELL_OK;
}

s32 decode_data(PPUContext& ctx, u32 h, u32 sa, u32 data, u32 ctrl, u32 out)
{
    std::shared_ptr<Stream> s;
    if (const s32 r = checked(ctx, h, sa, {PARAMETERS}, s); r != lv2::CELL_OK) return r;
    u8* base = guest_base(ctx);
    if (!readable(ctx, ctrl, 8) || !readable(ctx, out, 0x10)) return fail(base, sa, CELL_GIFDEC_ERROR_ARG);
    // CellGifDecDataOutInfo: recordType, outExtension (label, a pointer), status. The status is written first.
    ps_write32(base, out + 12, 0);
    if (s->command && readable(ctx, s->command, 4) && ps_read32(base, s->command) == 1) {
        ps_write32(base, out + 12, 1); // CELL_GIFDEC_DEC_STATUS_STOP
        return lv2::CELL_OK;
    }
    if (s->terminated) return fail(base, sa, CELL_GIFDEC_ERROR_SEQ);
    const u64 pitch = ps_read64(base, ctrl); // CellGifDecDataCtrlParam: outputBytesPerLine
    if (pitch < u64{s->width} * 4) return fail(base, sa, CELL_GIFDEC_ERROR_ARG);
    const u64 size = s->height ? pitch * (s->height - 1) + u64{s->width} * 4 : 0;
    if (size > 0xFFFFFFFFull || !readable(ctx, data, static_cast<u32>(size))) {
        log_call_site(ctx, base, std::format("cellGifDecDecodeData: the image ({:#x}, {} bytes) is not guest memory", data, size));
        return fail(base, sa, CELL_GIFDEC_ERROR_ARG);
    }
    return next_record(ctx, h, sa, *s, data, pitch, out);
}

s32 cellGifDecDecodeData(PPUContext& ctx, u32 handle, u32 stream, u32 data, u32 ctrl, u32 out)
{
    const std::string call =
        std::format("cellGifDecDecodeData({:#x}, {:#x}, {:#x}, {:#x}, {:#x})", handle, stream, data, ctrl, out);
    return finish(decode_data(ctx, handle, stream, data, ctrl, out), call);
}

} // namespace

void register_cellGifDec(Registry& r)
{
    r.add("cellGifDec", {
        PS_HLE_FUNC(0xB60D42A5, cellGifDecCreate),
        PS_HLE_FUNC(0xE74B2CB1, cellGifDecDestroy),
        PS_HLE_FUNC(0x75745079, cellGifDecOpen),
        PS_HLE_FUNC(0x116A7DA9, cellGifDecClose),
        PS_HLE_FUNC(0xF0DA95DE, cellGifDecReadHeader),
        PS_HLE_FUNC(0x41A90DC4, cellGifDecSetParameter),
        PS_HLE_FUNC(0x44B1BC61, cellGifDecDecodeData),
    });
}

} // namespace pspocket::hle
