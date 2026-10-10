#pragma once

// What the image decoders' Ext tests share (D-595): a game's stream callback and display callback as guest code, each
// keeping a record of its calls in a control block the test reads.

#include <cstring>
#include <utility>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

namespace pspocket::hle::test::imgdec_ext {

namespace as = pspocket::recomp::ppu::as;

// The stream callback (strmInfo r3, strmParam r4, its control block r5) gives the next `chunk` bytes of a source at
// each call, none at its end. Control block: +0 the source's address, +4 its size, +8 the next offset, +12 the chunk
// size, +16 the calls so far, +0x18 the address of a word to note at each call (0: none; the display callback's count,
// to see the two interleave), +0x20 + 8 n what call n saw: the decodedStrmSize it was given, then that word.
namespace strm {
constexpr u32 source = 0, size = 4, offset = 8, chunk = 12, calls = 16, sample = 0x18, log = 0x20;
}

// The display callback (dispInfo r3, dispParam r4, its control block r5) copies each dispInfo (0x40 bytes) into an
// entry of 0x50 bytes from +0x40, with the dispParam pointer it was given at +0x40 and that block's nextOutputImage at
// +0x44; counts its calls at +0; then, by the mode at +4: 0 points nextOutputImage just past the part, so that the parts
// follow each other in one frame; 1 copies the part (outputHeight lines of the frame's pitch, in words) into the frame
// at +8 at its line, nextOutputImage left as it is (one buffer for every part); 2 sets nextOutputImage to 0. At the
// call numbered as +12 says (1: the first; 0: none) it stores 1 at the word +16 points at (CELL_PNGDEC_STOP,
// CELL_GIFDEC_STOP at the commandPtr).
namespace disp {
constexpr u32 count = 0, mode = 4, frame = 8, stop_at = 12, command = 16, entries = 0x40, entry_size = 0x50;
}

struct Callbacks {
    u32 stream_opd = 0, display_opd = 0;
};

// Both callbacks' code at `code` (under 0x200 bytes) and their descriptors after it. `image_at`: where dispInfo keeps
// outputImage (0x3C in CellPngDecDispInfo, 0x38 in CellGifDecDispInfo).
inline Callbacks write_callbacks(u8* b, u32 code, u32 image_at)
{
    as::Assembler a(code);
    const u32 stream_entry = a.pc();
    a.emit(as::lwz(6, strm::offset, 5));
    a.emit(as::lwz(7, strm::size, 5));
    a.emit(as::lwz(8, strm::chunk, 5));
    a.emit(as::subf(9, 6, 7)); // what is left
    a.emit(as::cmplw(0, 9, 8));
    const auto fits = a.new_label();
    a.ble(0, fits);
    a.emit(as::mr(9, 8));
    a.bind(fits);
    a.emit(as::lwz(10, strm::source, 5));
    a.emit(as::add(10, 10, 6));
    a.emit(as::stw(10, 0, 4)); // strmPtr
    a.emit(as::stw(9, 4, 4));  // strmSize
    a.emit(as::add(6, 6, 9));
    a.emit(as::stw(6, strm::offset, 5));
    a.emit(as::lwz(11, strm::calls, 5));
    a.emit(as::slwi(12, 11, 3));
    a.emit(as::add(12, 12, 5));
    a.emit(as::lwz(10, 0, 3)); // decodedStrmSize
    a.emit(as::stw(10, strm::log, 12));
    a.emit(as::lwz(10, strm::sample, 5));
    a.emit(as::cmpwi(0, 10, 0));
    const auto no_sample = a.new_label();
    a.beq(0, no_sample);
    a.emit(as::lwz(10, 0, 10));
    a.bind(no_sample);
    a.emit(as::stw(10, strm::log + 4, 12));
    a.emit(as::addi(11, 11, 1));
    a.emit(as::stw(11, strm::calls, 5));
    a.emit(as::li(3, 0));
    a.emit(as::blr());

    const u32 display_entry = a.pc();
    a.emit(as::lwz(6, disp::count, 5));
    a.emit(as::mulli(7, 6, disp::entry_size));
    a.emit(as::add(7, 7, 5));
    a.emit(as::addi(7, 7, disp::entries));
    a.emit(as::li(8, 16));
    a.emit(as::mtctr(8));
    a.emit(as::addi(9, 3, -4));
    a.emit(as::addi(10, 7, -4));
    const auto copy_info = a.here();
    a.emit(as::lwzu(11, 4, 9));
    a.emit(as::stwu(11, 4, 10));
    a.bdnz(copy_info);
    a.emit(as::stw(4, 0x40, 7));
    a.emit(as::lwz(11, 0, 4));
    a.emit(as::stw(11, 0x44, 7));
    a.emit(as::addi(6, 6, 1));
    a.emit(as::stw(6, disp::count, 5));
    a.emit(as::lwz(8, disp::stop_at, 5));
    a.emit(as::cmpw(0, 6, 8));
    const auto no_stop = a.new_label();
    a.bne(0, no_stop);
    a.emit(as::lwz(9, disp::command, 5));
    a.emit(as::li(10, 1));
    a.emit(as::stw(10, 0, 9));
    a.bind(no_stop);
    a.emit(as::lwz(8, disp::mode, 5));
    a.emit(as::cmpwi(0, 8, 1));
    const auto copy = a.new_label();
    a.beq(0, copy);
    a.emit(as::cmpwi(0, 8, 2));
    const auto advance = a.new_label();
    a.bne(0, advance);
    a.emit(as::li(9, 0));
    a.emit(as::stw(9, 0, 4));
    a.emit(as::li(3, 0));
    a.emit(as::blr());
    a.bind(advance);
    a.emit(as::lwz(9, static_cast<s32>(image_at), 3)); // outputImage
    a.emit(as::lwz(10, 0x28, 3));                      // outputHeight
    a.emit(as::lwz(11, 0x04, 3));                      // outputFrameWidthByte's low word
    a.emit(as::mullw(10, 10, 11));
    a.emit(as::add(9, 9, 10));
    a.emit(as::stw(9, 0, 4));
    a.emit(as::li(3, 0));
    a.emit(as::blr());
    a.bind(copy);
    a.emit(as::lwz(9, disp::frame, 5));
    a.emit(as::lwz(10, 0x18, 3)); // outputStartY
    a.emit(as::lwz(11, 0x04, 3)); // the pitch
    a.emit(as::mullw(10, 10, 11));
    a.emit(as::add(9, 9, 10));
    a.emit(as::lwz(10, 0x28, 3));
    a.emit(as::mullw(10, 10, 11));
    a.emit(as::srwi(10, 10, 2));
    a.emit(as::mtctr(10));
    a.emit(as::lwz(12, static_cast<s32>(image_at), 3));
    a.emit(as::addi(12, 12, -4));
    a.emit(as::addi(9, 9, -4));
    const auto copy_part = a.here();
    a.emit(as::lwzu(11, 4, 12));
    a.emit(as::stwu(11, 4, 9));
    a.bdnz(copy_part);
    a.emit(as::li(3, 0));
    a.emit(as::blr());

    const std::vector<u8> bytes = a.finish();
    std::memcpy(b + code, bytes.data(), bytes.size());
    const u32 opds = (code + static_cast<u32>(bytes.size()) + 15) & ~15u;
    ps_write32(b, opds, stream_entry);
    ps_write32(b, opds + 4, 0);
    ps_write32(b, opds + 8, display_entry);
    ps_write32(b, opds + 12, 0);
    return {opds, opds + 8};
}

// A display callback's call n, as it recorded it.
struct Part {
    u64 frame_width_byte = 0, start_x_byte = 0, width_byte = 0;
    u32 frame_height = 0, start_y = 0, height = 0, bit_depth = 0, components = 0;
    u32 word_34 = 0, word_38 = 0, word_3c = 0; // PNG: nextOutputStartY, scanPassCount, outputImage; GIF: scanPassCount,
                                               // outputImage, interlaceFlag
    u32 param = 0, next_image = 0;
};

// What the stream callback's calls saw: (decodedStrmSize, the noted word).
inline std::vector<std::pair<u32, u32>> stream_calls(u8* b, u32 control)
{
    std::vector<std::pair<u32, u32>> out;
    for (u32 n = 0; n < ps_read32(b, control + strm::calls); n++)
        out.emplace_back(ps_read32(b, control + strm::log + 8 * n), ps_read32(b, control + strm::log + 8 * n + 4));
    return out;
}

inline std::vector<Part> parts(u8* b, u32 control)
{
    std::vector<Part> out;
    for (u32 n = 0; n < ps_read32(b, control + disp::count); n++) {
        const u32 e = control + disp::entries + disp::entry_size * n;
        Part p;
        p.frame_width_byte = ps_read64(b, e + 0x00);
        p.frame_height = ps_read32(b, e + 0x08);
        p.start_x_byte = ps_read64(b, e + 0x10);
        p.start_y = ps_read32(b, e + 0x18);
        p.width_byte = ps_read64(b, e + 0x20);
        p.height = ps_read32(b, e + 0x28);
        p.bit_depth = ps_read32(b, e + 0x2C);
        p.components = ps_read32(b, e + 0x30);
        p.word_34 = ps_read32(b, e + 0x34);
        p.word_38 = ps_read32(b, e + 0x38);
        p.word_3c = ps_read32(b, e + 0x3C);
        p.param = ps_read32(b, e + 0x40);
        p.next_image = ps_read32(b, e + 0x44);
        out.push_back(p);
    }
    return out;
}

} // namespace pspocket::hle::test::imgdec_ext
