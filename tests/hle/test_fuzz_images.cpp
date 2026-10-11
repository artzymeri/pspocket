// Mutation fuzzing of the image decoders (D-603, tests/common/mutation_fuzz.hpp): pspocket's own JPEG decoder alone
// and through cellJpgDec, cellGifDec's own GIF decoder and cellPngDec on libpng, each over damaged files the tests'
// writers made (jpeg_writer.hpp, gif_writer.hpp, png_writer.hpp). A file comes from a buffer flush against unmapped
// memory and decodes into a frame flush against it too, at random output settings: every stream must open, decode
// or be refused, never read past its file or write past its frame. cellJpgDec's Ext functions (D-617) too: the file
// fed by a stream callback in parts, decoded in parts to a display callback. Longer: PSPOCKET_FUZZ_ITERS=<n>
// build/host/tests/hle/test_hle "[fuzz]".

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "gif_writer.hpp"
#include "guest.hpp"
#include "hle_fuzz_util.hpp"
#include "imgdec_ext_util.hpp"
#include "jpeg_decoder.hpp" // runtime/hle/src
#include "jpeg_writer.hpp"
#include "png_writer.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::fuzzing;
namespace gifw = pspocket::test::gif;
namespace jpegw = pspocket::test::jpeg;
namespace pngw = pspocket::test::png;

namespace {

constexpr u32 kHeap = 0x01000000, kHeapSize = 0x00800000;
constexpr u32 kArgs = kData + 0x4000;
constexpr u32 kHandleOut = kArgs, kThreadIn = kArgs + 0x10, kThreadOut = kArgs + 0x30, kSrc = kArgs + 0x40,
              kStreamOut = kArgs + 0x60, kOpenInfo = kArgs + 0x68, kInfo = kArgs + 0x80, kInParam = kArgs + 0xA0,
              kOutParam = kArgs + 0xC0, kCtrl = kArgs + 0xF0, kDataOut = kArgs + 0x100;
const GuestHeap kGuestHeap{kData, kData + 0x200, kData + 0x208, kData + 0x1000, kHeap, kHeapSize};

// One of the three decoders' API: its library (and its functions' prefix) and how its CellXxxDecInParam is written.
struct Decoder {
    const char* library;
    void (*in_param)(u8* b, fuzz::Rng& rng);
    u32 records; // DecodeData calls a stream may take (a GIF's records)
};

// CellJpgDecInParam: commandPtr, downScale, method, outputMode, outputColorSpace, outputColorAlpha.
void jpg_in(u8* b, fuzz::Rng& rng)
{
    static constexpr u32 kScales[] = {1, 2, 4, 8, 3, 0, 16};
    static constexpr u32 kSpaces[] = {1, 2, 3, 10, 11, 20, 30, 31, 40, 41, 0, 99};
    std::memset(b + kInParam, 0, 0x18);
    ps_write32(b, kInParam + 4, kScales[rng.below(std::size(kScales))]);
    ps_write32(b, kInParam + 8, rng.one_in(3) ? 5 : rng.one_in(8) ? 1 : 0);
    ps_write32(b, kInParam + 12, static_cast<u32>(rng.below(2)));
    ps_write32(b, kInParam + 16, kSpaces[rng.below(std::size(kSpaces))]);
    ps_write8(b, kInParam + 20, rng.byte());
}

// CellPngDecInParam: commandPtr, outputMode, outputColorSpace, outputBitDepth, outputPackFlag, outputAlphaSelect,
// outputColorAlpha.
void png_in(u8* b, fuzz::Rng& rng)
{
    static constexpr u32 kSpaces[] = {1, 2, 4, 9, 10, 20, 0, 7};
    std::memset(b + kInParam, 0, 0x1C);
    ps_write32(b, kInParam + 4, static_cast<u32>(rng.below(2)));
    ps_write32(b, kInParam + 8, kSpaces[rng.below(std::size(kSpaces))]);
    ps_write32(b, kInParam + 12, rng.one_in(3) ? 16 : 8);
    ps_write32(b, kInParam + 16, static_cast<u32>(rng.below(2)));
    ps_write32(b, kInParam + 20, static_cast<u32>(rng.below(2)));
    ps_write32(b, kInParam + 24, static_cast<u32>(rng.below(256)));
}

// CellGifDecInParam: commandPtr, colorSpace, outputColorAlpha1, outputColorAlpha2.
void gif_in(u8* b, fuzz::Rng& rng)
{
    static constexpr u32 kSpaces[] = {10, 20, 2, 0};
    std::memset(b + kInParam, 0, 0x10);
    ps_write32(b, kInParam + 4, kSpaces[rng.below(std::size(kSpaces))]);
    ps_write8(b, kInParam + 8, rng.byte());
    ps_write8(b, kInParam + 9, rng.byte());
}

const Decoder kJpg{"cellJpgDec", jpg_in, 1};
const Decoder kPng{"cellPngDec", png_in, 1};
const Decoder kGif{"cellGifDec", gif_in, 8};

// A decoder created once (SPU threads off and on), then streams opened over each input.
struct ImageRun {
    InterpretMisses interpret;
    Fixture f;
    GuestThread t{f, 0, "ppu"};
    u8* b = f.base();
    const Decoder& d;
    u32 handle = 0, heap_after_create = 0;

    ImageRun(const Decoder& dec, bool spu) : d(dec)
    {
        REQUIRE(f.hle->link_internal_functions({d.library}));
        REQUIRE(f.hle->main_memory().reserve(kHeap, kHeapSize));
        REQUIRE(f.mem.commit(kHeap, kHeapSize));
        REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
        add_guarded_areas(f);
        kGuestHeap.write(b);
        std::memset(b + kArgs, 0, 0x400);
        const u32 tin[7] = {spu ? 1u : 0u, 1000, 200, kGuestHeap.malloc_opd, kGuestHeap.ctl, kGuestHeap.free_opd,
            kGuestHeap.ctl};
        for (u32 i = 0; i < 7; i++) ps_write32(b, kThreadIn + 4 * i, tin[i]);
        create();
    }
    ~ImageRun() { call("Destroy", {handle}); }

    // A decoder made at the heap's start: at first, and after an Open it refused, which leaves libpngdec's and
    // libgifdec's handle busy for good (later Opens and Destroy are SEQ, as Sony's).
    void create()
    {
        kGuestHeap.reset(b, kHeap);
        REQUIRE(call("Create", {kHandleOut, kThreadIn, kThreadOut}) == 0);
        handle = ps_read32(b, kHandleOut);
        heap_after_create = kGuestHeap.next(b);
    }

    // The library's function `what` (cellJpgDecOpen for "Open"), linked as a game's import.
    u32 call(const char* what, std::initializer_list<u64> args)
    {
        const std::string name = std::string(d.library) + what;
        const u32 descriptor = f.hle->link_function(d.library, recomp::nid_of(name), name);
        REQUIRE(descriptor != 0);
        return static_cast<u32>(hle::call_guest(t.ctx, descriptor, args));
    }

    // What a stream came to: its header read, and the image (or a record of it) decoded.
    struct Outcome {
        bool header = false, decoded = false;
    };

    // One stream over `file`: open (from a buffer), header, parameters (random ones: some the image cannot give, which
    // are refused), every record decoded into a frame of its size, close.
    Outcome stream(const std::vector<u8>& file, fuzz::Rng& rng)
    {
        kGuestHeap.reset(b, heap_after_create);
        const u32 at = place_input(b, file);
        std::memset(b + kSrc, 0, 0x20);
        ps_write32(b, kSrc + 0, 1); // CELL_XXXDEC_BUFFER
        ps_write32(b, kSrc + 20, at);
        ps_write32(b, kSrc + 24, static_cast<u32>(file.size()));
        Outcome outcome;
        if (call("Open", {handle, kStreamOut, kSrc, kOpenInfo}) != 0) {
            call("Destroy", {handle});
            create();
            return outcome;
        }
        const u32 stream = ps_read32(b, kStreamOut);
        bool& decoded = outcome.decoded;
        if (call("ReadHeader", {handle, stream, kInfo}) == 0) {
            outcome.header = true;
            d.in_param(b, rng);
            if (call("SetParameter", {handle, stream, kInParam, kOutParam}) == 0) {
                const u64 width_bytes = ps_read64(b, kOutParam);
                const u32 height = ps_read32(b, kOutParam + 12);
                // The frame: the image's rows, or a little wider; pitch * height bytes, the last row ending at the guard.
                const u64 pitch = width_bytes + (rng.one_in(2) ? 0 : 16 * rng.below(3));
                if (pitch && height && pitch * height <= kAreaSize && pitch <= 0xFFFFFFFF) {
                    const u32 frame = place_output(b, static_cast<u32>(pitch * height));
                    ps_write64(b, kCtrl, pitch);
                    for (u32 k = 0; k < d.records; k++) {
                        std::memset(b + kDataOut, 0xEE, 0x10);
                        if (call("DecodeData", {handle, stream, frame, kCtrl, kDataOut}) != 0) break;
                        decoded = true;
                        const u32 record = ps_read32(b, kDataOut); // a GIF's: 1 an image, 2 an extension, else the end
                        if (record != 1 && record != 2) break;
                    }
                }
            }
        }
        call("Close", {handle, stream});
        CHECK_FALSE(kGuestHeap.overrun(b));
        return outcome;
    }
};

// PNG chunks with their CRC-32 made right again after the damage (libpng refuses a bad CRC at once), as long as the
// chunk lengths still lead from one to the next.
void fix_png_crcs(std::vector<u8>& d)
{
    usize at = 8;
    while (at + 12 <= d.size()) {
        const u32 len = static_cast<u32>(fuzz::load_field(d, at, 4));
        if (len > d.size() - at - 12) break;
        const u32 crc = pngw::crc32(d.data() + at + 4, len + 4);
        ps_st32(d.data() + at + 8 + len, crc);
        at += 12 + len;
    }
}

// The fields of a PNG: every chunk's length, and IHDR's fields.
std::vector<fuzz::Field> png_fields(const std::vector<u8>& d)
{
    std::vector<fuzz::Field> f = {{16, 4}, {20, 4}, {24, 1}, {25, 1}, {26, 1}, {27, 1}, {28, 1}};
    for (usize at = 8; at + 12 <= d.size();) {
        f.push_back({at, 4});
        at += 12 + static_cast<usize>(fuzz::load_field(d, at, 4));
    }
    return f;
}

// The fields of a JPEG: every marker segment's length, SOF's sizes and components, SOS's component selectors.
std::vector<fuzz::Field> jpeg_fields(const std::vector<u8>& d)
{
    std::vector<fuzz::Field> f;
    for (usize at = 2; at + 4 <= d.size();) {
        if (d[at] != 0xFF) break;
        const u8 marker = d[at + 1];
        const usize len = static_cast<usize>(fuzz::load_field(d, at + 2, 2));
        f.push_back({at + 2, 2});
        if ((marker == 0xC0 || marker == 0xC2) && at + 2 + len <= d.size())
            fuzz::append(f, {{at + 4, 1}, {at + 5, 2}, {at + 7, 2}, {at + 9, 1}, {at + 10, 1}, {at + 11, 1}, {at + 12, 1}});
        if (marker == 0xDD) f.push_back({at + 4, 2}); // the restart interval
        if (marker == 0xDA) {
            if (at + 2 + len <= d.size()) fuzz::append(f, fuzz::fields(at + 4, (len - 2) / 2, 2));
            break; // entropy-coded data follows
        }
        at += 2 + len;
    }
    return f;
}

// The fields of a GIF: the screen descriptor and every image descriptor's place, size and flags (little-endian).
std::vector<fuzz::Field> gif_fields(const std::vector<u8>& d)
{
    std::vector<fuzz::Field> f = {{6, 2, true}, {8, 2, true}, {10, 1}, {11, 1}};
    for (usize at = 13; at + 10 <= d.size(); at++)
        if (d[at] == 0x2C) fuzz::append(f, {{at + 1, 2, true}, {at + 3, 2, true}, {at + 5, 2, true}, {at + 7, 2, true}, {at + 9, 1}});
    return f;
}

std::vector<fuzz::Sample> jpeg_samples()
{
    std::vector<jpegw::Image> images;
    images.push_back(jpegw::pattern(37, 21, false, 2, 2, 0)); // 4:2:0
    images.push_back(jpegw::pattern(37, 21, false, 2, 1, 1)); // 4:2:2
    images.push_back(jpegw::pattern(29, 18, true, 1, 1, 3));  // gray
    jpegw::Image r = jpegw::pattern(29, 18, false, 2, 2, 4);
    r.restart_interval = 2;
    images.push_back(r);
    jpegw::Image a = jpegw::pattern(16, 16, false, 1, 1, 5);
    a.app = 2; // Adobe APP14
    images.push_back(a);
    jpegw::Image pg = jpegw::pattern(23, 17, true, 1, 1, 7);
    pg.progressive = true;
    images.push_back(pg);
    jpegw::Image pc = jpegw::pattern(29, 18, false, 2, 2, 8);
    pc.progressive = true;
    images.push_back(pc);
    std::vector<fuzz::Sample> v;
    for (usize i = 0; i < images.size(); i++) {
        std::vector<u8> bytes = jpegw::encode(images[i]);
        auto fields = jpeg_fields(bytes);
        v.push_back({std::format("jpeg {}", i), std::move(bytes), std::move(fields)});
    }
    return v;
}

std::vector<fuzz::Sample> png_samples()
{
    std::vector<pngw::Image> images;
    images.push_back(pngw::pattern(13, 7, 6, 8, 0));  // RGBA
    images.push_back(pngw::pattern(9, 11, 2, 16, 1)); // RGB, 16 bits
    images.push_back(pngw::pattern(17, 5, 3, 4, 2));  // a palette of 4 bits
    images.push_back(pngw::pattern(10, 10, 0, 1, 3)); // gray, 1 bit
    images.push_back(pngw::pattern(8, 9, 4, 8, 4));   // gray and alpha
    pngw::Image i = pngw::pattern(19, 13, 6, 8, 5);
    i.interlaced = true;
    images.push_back(i);
    pngw::Image t = pngw::pattern(12, 6, 3, 8, 6);
    t.trns = {0, 64, 128, 255};
    t.text = {{"Comment", "a synthetic image"}};
    images.push_back(t);
    std::vector<fuzz::Sample> v;
    for (usize k = 0; k < images.size(); k++) {
        std::vector<u8> bytes = pngw::encode(images[k]);
        auto fields = png_fields(bytes);
        v.push_back({std::format("png {}", k), std::move(bytes), std::move(fields)});
    }
    return v;
}

std::vector<fuzz::Sample> gif_samples()
{
    const auto frame = [](u32 left, u32 top, u32 w, u32 h, int bits, u32 seed) {
        gifw::Frame fr;
        fr.left = left;
        fr.top = top;
        fr.width = w;
        fr.height = h;
        fr.indices = gifw::indices(w, h, bits, seed);
        return fr;
    };
    std::vector<gifw::File> files;
    {
        gifw::File f; // a plain 4-bit image with a graphic control extension
        f.width = 13;
        f.height = 9;
        f.global_bits = 4;
        f.global = gifw::palette(4, 0);
        f.frames.push_back(frame(0, 0, 13, 9, 4, 0));
        gifw::Extension e;
        e.label = 0xF9;
        e.blocks.push_back({1, 10, 0, 3});
        f.frames[0].extensions.push_back(e);
        files.push_back(f);
    }
    {
        gifw::File f; // interlaced, 256 colours, a comment
        f.width = 11;
        f.height = 17;
        f.global_bits = 8;
        f.global = gifw::palette(8, 1);
        f.frames.push_back(frame(0, 0, 11, 17, 8, 1));
        f.frames[0].interlaced = true;
        gifw::Extension c;
        c.blocks.push_back({'h', 'i'});
        f.frames[0].extensions.push_back(c);
        files.push_back(f);
    }
    {
        gifw::File f; // two frames, the second smaller with a local palette and early clear codes
        f.width = 16;
        f.height = 12;
        f.global_bits = 2;
        f.global = gifw::palette(2, 2);
        f.frames.push_back(frame(0, 0, 16, 12, 2, 2));
        gifw::Frame fr = frame(2, 3, 9, 5, 3, 3);
        fr.local_bits = 3;
        fr.local = gifw::palette(3, 3);
        fr.clear_every = 7;
        fr.block_size = 17;
        f.frames.push_back(fr);
        files.push_back(f);
    }
    std::vector<fuzz::Sample> v;
    for (usize k = 0; k < files.size(); k++) {
        std::vector<u8> bytes = gifw::encode(files[k]);
        auto fields = gif_fields(bytes);
        v.push_back({std::format("gif {}", k), std::move(bytes), std::move(fields)});
    }
    return v;
}

} // namespace

TEST_CASE("fuzz: pspocket's JPEG decoder alone, every output", "[hle][fuzz]")
{
    namespace jpeg = pspocket::hle::jpeg;
    fuzz::Tally tally("jpeg decoder");
    fuzz::run(tally, jpeg_samples(), 800, [](const std::vector<u8>& input, fuzz::Rng& rng) {
        jpeg::Decoder d;
        std::string error;
        if (!d.read_header(input.data(), input.size(), error)) return false;
        const int scale = 1 << rng.below(4);
        const bool fast = rng.one_in(2);
        (void)d.memory_estimate(scale, fast, d.header().width / static_cast<u32>(scale), 3);
        if (static_cast<u64>(d.header().width) * d.header().height > (1u << 22)) return true; // a header alone
        if (!d.decode(error)) return true;
        (void)d.mean();
        static constexpr jpeg::ColorSpace kSpaces[] = {jpeg::ColorSpace::RGB, jpeg::ColorSpace::Gray, jpeg::ColorSpace::YCbCr};
        const jpeg::ColorSpace space = kSpaces[rng.below(3)];
        (void)d.lines_written(scale, fast, space, d.header().height);
        jpeg::Decoder::Output out;
        (void)d.output(scale, fast, space, out, error);
        return true;
    });
}

TEST_CASE("fuzz: cellJpgDec, cellPngDec and cellGifDec over damaged files", "[hle][fuzz]")
{
    struct Target {
        const Decoder& d;
        std::string name;
        std::vector<fuzz::Sample> samples;
        bool fix_crcs;
    };
    const std::vector<Target> targets = {{kJpg, "cellJpgDec", jpeg_samples(), false},
        {kPng, "cellPngDec", png_samples(), true}, {kGif, "cellGifDec", gif_samples(), false}};
    // Accepted: the header was read. The decodes are counted apart, as many inputs come with parameters the image
    // cannot give.
    for (const Target& target : targets) {
        for (const bool spu : {false, true}) {
            ImageRun run(target.d, spu);
            fuzz::Tally tally(target.name + (spu ? " (spu)" : ""));
            u64 decoded = 0;
            fuzz::run(tally, target.samples, spu ? 100 : 400, [&](const std::vector<u8>& input, fuzz::Rng& rng) {
                std::vector<u8> file = input;
                if (target.fix_crcs && rng.one_in(2)) fix_png_crcs(file);
                const ImageRun::Outcome o = run.stream(file, rng);
                decoded += o.decoded;
                return o.header;
            }, kAreaSize);
            std::printf("fuzz %-24s %8llu decoded\n", tally.name().c_str(), static_cast<unsigned long long>(decoded));
            CHECK(decoded > 0);
        }
    }
}

TEST_CASE("fuzz: cellJpgDec's Ext functions, damaged files from a stream callback, decoded in parts (D-617)",
    "[hle][fuzz]")
{
    // The source's buffer holds the first bytes, the stream callback gives the rest (imgdec_ext_util.hpp's, reading
    // the file where it lies, flush against unmapped memory); the display callback takes the parts one after another
    // in a frame flush against it too, at most 32 of them.
    namespace ext = pspocket::hle::test::imgdec_ext;
    constexpr u32 kExtCode = kData + 0x2000, kStrmCtl = kData + 0x2800, kDispCtl = kData + 0x8000;
    constexpr u32 kCbStrm = kArgs + 0x200, kExtIn = kArgs + 0x210, kExtOut = kArgs + 0x220, kCbDisp = kArgs + 0x230,
                  kExtInfo = kArgs + 0x240;
    ImageRun run(kJpg, false);
    u8* b = run.b;
    const ext::Callbacks cbs = ext::write_callbacks(b, kExtCode, 0x3C);
    fuzz::Tally tally("cellJpgDec (Ext)");
    u64 decoded = 0, parts = 0;
    fuzz::run(tally, jpeg_samples(), 300, [&](const std::vector<u8>& input, fuzz::Rng& rng) {
        if (input.empty()) return false;
        kGuestHeap.reset(b, run.heap_after_create);
        const u32 at = place_input(b, input), size = static_cast<u32>(input.size());
        const u32 first = std::min<u32>(size, 1 + static_cast<u32>(rng.below(48)));
        static constexpr u32 kChunks[] = {37, 512, 4093};
        std::memset(b + kSrc, 0, 0x20);
        ps_write32(b, kSrc + 0, 1);
        ps_write32(b, kSrc + 20, at);
        ps_write32(b, kSrc + 24, first);
        std::memset(b + kStrmCtl, 0, 0x40);
        ps_write32(b, kStrmCtl + ext::strm::source, at);
        ps_write32(b, kStrmCtl + ext::strm::size, size);
        ps_write32(b, kStrmCtl + ext::strm::offset, first);
        ps_write32(b, kStrmCtl + ext::strm::chunk, size > 0x4000 ? 4093 : kChunks[rng.below(3)]); // its log fits
        ps_write32(b, kCbStrm, cbs.stream_opd);
        ps_write32(b, kCbStrm + 4, kStrmCtl);
        if (run.call("ExtOpen", {run.handle, kStreamOut, kSrc, kOpenInfo, kCbStrm}) != 0) {
            run.call("Destroy", {run.handle});
            run.create();
            return false;
        }
        const u32 stream = ps_read32(b, kStreamOut);
        bool header = false;
        if (run.call("ExtReadHeader", {run.handle, stream, kInfo, kExtInfo}) == 0) {
            header = true;
            jpg_in(b, rng);
            const u32 height = ps_read32(b, kInfo + 4);
            ps_write32(b, kExtIn, 1); // CELL_JPGDEC_LINE_MODE
            ps_write32(b, kExtIn + 4, std::max(1u, (height + 31) / 32));
            ps_write32(b, kExtIn + 8, 0);
            if (run.call("ExtSetParameter", {run.handle, stream, kInParam, kOutParam, kExtIn, kExtOut}) == 0) {
                const u64 width_bytes = ps_read64(b, kOutParam);
                const u32 out_height = ps_read32(b, kOutParam + 12);
                const u64 pitch = width_bytes + (rng.one_in(2) ? 0 : 16 * rng.below(3));
                if (pitch && out_height && pitch * out_height <= kAreaSize) {
                    const u32 frame = place_output(b, static_cast<u32>(pitch * out_height));
                    ps_write64(b, kCtrl, pitch);
                    std::memset(b + kDispCtl, 0, 0x40); // mode 0: each part's place just past the last
                    ps_write32(b, kCbDisp, cbs.display_opd);
                    ps_write32(b, kCbDisp + 4, kDispCtl);
                    std::memset(b + kDataOut, 0xEE, 0x10);
                    const bool display = !rng.one_in(4);
                    if (run.call("ExtDecodeData", {run.handle, stream, frame, kCtrl, kDataOut, display ? kCbDisp : 0, 0}) == 0)
                        decoded++;
                    parts += ps_read32(b, kDispCtl + ext::disp::count);
                }
            }
        }
        run.call("Close", {run.handle, stream});
        CHECK_FALSE(kGuestHeap.overrun(b));
        return header;
    }, kAreaSize);
    std::printf("fuzz %-24s %8llu decoded, %llu parts\n", tally.name().c_str(), static_cast<unsigned long long>(decoded),
        static_cast<unsigned long long>(parts));
    CHECK(decoded > 0);
    CHECK(parts > 0);
}
