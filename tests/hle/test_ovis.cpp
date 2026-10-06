// cellOvis (D-195): SPU overlay tables, as libsre computes and fills them, over synthetic SPU ELF headers built here
// (no game data), and the overlapping segments it drops from a sys_spu_image, with the swap that leaves one word behind.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <initializer_list>

#include "hle_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kElf = kData + 0x1000, kTable = kData + 0x8000, kSegs = kData + 0x10000, kImage = kData + 0x10800,
              kCount = kData + 0x10900;
constexpr u64 kInval = 0xFFFFFFFF80410402ull, kAlign = 0xFFFFFFFF80410416ull;

struct Segment {
    u32 type, offset, paddr, filesz;
};

// A big-endian ELF with the given program headers at 0x34 (32 bytes each) and each segment's data a byte pattern.
void write_elf(u8* b, std::initializer_list<Segment> segs)
{
    std::memset(b + kElf, 0, 0x4000);
    const u8 ident[8] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
    std::memcpy(b + kElf, ident, 8);
    ps_write32(b, kElf + 28, 0x34);
    ps_write16(b, kElf + 42, 0x20);
    ps_write16(b, kElf + 44, static_cast<u16>(segs.size()));
    u32 i = 0;
    for (const Segment& s : segs) {
        const u32 ph = kElf + 0x34 + 0x20 * i++;
        ps_write32(b, ph + 0, s.type);
        ps_write32(b, ph + 4, s.offset);
        ps_write32(b, ph + 8, s.paddr);
        ps_write32(b, ph + 12, s.paddr);
        ps_write32(b, ph + 16, s.filesz);
        ps_write32(b, ph + 20, s.filesz);
        for (u32 k = 0; k < s.filesz && s.offset + k < 0x4000; k++) b[kElf + s.offset + k] = static_cast<u8>(0x40 + i * 16 + k);
    }
}

// A sys_spu_segment (24 bytes): type, ls, size, the word at +0xC, the address at +0x10.
void write_segment(u8* b, u32 index, s32 type, u32 ls, u32 size, u32 word_c, u64 addr)
{
    const u32 at = kSegs + 0x18 * index;
    ps_write32(b, at, static_cast<u32>(type));
    ps_write32(b, at + 4, ls);
    ps_write32(b, at + 8, size);
    ps_write32(b, at + 0xC, word_c);
    ps_write64(b, at + 0x10, addr);
}

} // namespace

TEST_CASE("cellOvis: the overlay table's size and contents, as libsre's", "[hle][ovis]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();

    // Not an ELF, or not big-endian: INVAL.
    std::memset(b + kElf, 0, 0x40);
    CHECK(t.call("cellOvisGetOverlayTableSize", kElf) == kInval);
    CHECK(t.call("cellOvisInitializeOverlayTable", kTable, kElf) == kInval);
    write_elf(b, {{1, 0x100, 0x3000, 0x80}});
    b[kElf + 5] = 1; // little-endian
    CHECK(t.call("cellOvisGetOverlayTableSize", kElf) == kInval);

    // Segments only in local store: no table.
    write_elf(b, {{1, 0x100, 0x3000, 0x80}, {1, 0x200, 0x3F000, 0x1000}});
    CHECK(t.call("cellOvisGetOverlayTableSize", kElf) == 0);

    // Overlays past local store: the table reaches the end of the last one with data (an empty one, and one that is
    // not PT_LOAD, don't count).
    write_elf(b, {{1, 0x100, 0x3000, 0x80}, {1, 0x200, 0x40000, 0x300}, {1, 0x600, 0x40300, 0x80},
                     {1, 0x700, 0x48000, 0}, {4, 0x700, 0x50000, 0x100}});
    CHECK(t.call("cellOvisGetOverlayTableSize", kElf) == 0x380);

    // Initialize: the table 128-aligned (libsre's 0x80410416), then each overlay at its place; local store segments
    // are not copied.
    CHECK(t.call("cellOvisInitializeOverlayTable", kTable + 0x40, kElf) == kAlign);
    std::memset(b + kTable, 0xEE, 0x400);
    REQUIRE(t.call("cellOvisInitializeOverlayTable", kTable, kElf) == 0);
    CHECK(std::memcmp(b + kTable, b + kElf + 0x200, 0x300) == 0);
    CHECK(std::memcmp(b + kTable + 0x300, b + kElf + 0x600, 0x80) == 0);
    CHECK(b[kTable + 0x380] == 0xEE);
}

TEST_CASE("cellOvis: overlapping segments are swapped to the end and no longer counted, as libsre's", "[hle][ovis]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    // A at 0x3000..0x4000; B inside A; C apart; D an INFO segment over A (INFO segments never overlap).
    write_segment(b, 0, 1, 0x3000, 0x1000, 0xA0, 0xAAAA);
    write_segment(b, 1, 1, 0x3800, 0x100, 0xB0, 0xBBBB);
    write_segment(b, 2, 1, 0x5000, 0x100, 0xC0, 0xCCCC);
    write_segment(b, 3, 4, 0x3000, 0x10, 0xD0, 0xDDDD);
    ps_write32(b, kCount, 4);
    t.call("cellOvisInvalidateOverlappedSegments", kSegs, kCount);
    CHECK(ps_read32(b, kCount) == 3);
    // B swapped places with D, the last counted one; the word at +0xC stays where it was (libsre's swap skips it).
    CHECK(ps_read32(b, kSegs + 0x18 * 1) == 4);
    CHECK(ps_read64(b, kSegs + 0x18 * 1 + 0x10) == 0xDDDD);
    CHECK(ps_read32(b, kSegs + 0x18 * 1 + 0xC) == 0xB0);
    CHECK(ps_read32(b, kSegs + 0x18 * 3 + 4) == 0x3800);
    CHECK(ps_read64(b, kSegs + 0x18 * 3 + 0x10) == 0xBBBB);
    CHECK(ps_read32(b, kSegs + 0x18 * 3 + 0xC) == 0xD0);
    CHECK(ps_read64(b, kSegs + 0x18 * 2 + 0x10) == 0xCCCC);

    // FixSpuSegments does the same for a sys_spu_image (segs at +8, nsegs at +0xC).
    write_segment(b, 0, 1, 0x3000, 0x1000, 0, 1);
    write_segment(b, 1, 1, 0x3FF0, 0x20, 0, 2); // overlaps A's last bytes
    ps_write32(b, kImage + 8, kSegs);
    ps_write32(b, kImage + 0xC, 2);
    t.call("cellOvisFixSpuSegments", kImage);
    CHECK(ps_read32(b, kImage + 0xC) == 1);
    write_segment(b, 1, 1, 0x4000, 0x20, 0, 2); // adjacent: no overlap
    ps_write32(b, kImage + 0xC, 2);
    t.call("cellOvisFixSpuSegments", kImage);
    CHECK(ps_read32(b, kImage + 0xC) == 2);
}
