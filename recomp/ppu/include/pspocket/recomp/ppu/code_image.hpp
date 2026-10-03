#pragma once

// Input of the PPU analyzer and lifter: guest memory contents (segments) plus seeds.
//
// Today ppu-lift builds a CodeImage from a raw binary (--image/--base). The ELF/PRX loader (recomp/common) will
// fill the same structures: one Segment per PT_LOAD (executable flag from p_flags), seeds from the entry point,
// exports and .opd descriptors (each with its TOC), and nothing else changes.

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::recomp::ppu {

struct Segment {
    u32 addr = 0;
    std::vector<u8> bytes; // big-endian guest bytes
    bool executable = true;
};

class CodeImage {
public:
    CodeImage() = default;
    // One executable segment at base (the raw-image path).
    CodeImage(u32 base, std::vector<u8> bytes) { add_segment(Segment{base, std::move(bytes), true}); }

    void add_segment(Segment segment);
    const std::vector<Segment>& segments() const { return segments_; }

    // True when [addr, addr + size) lies inside one segment.
    bool contains(u32 addr, u32 size = 4) const;
    bool is_code(u32 addr) const; // inside an executable segment, 4-byte aligned
    std::optional<u32> read32(u32 addr) const;
    std::optional<u64> read64(u32 addr) const;

private:
    const Segment* find(u32 addr, u32 size) const;
    std::vector<Segment> segments_;
};

// A function entry the analyzer must lift: the ELF entry, an export, an .opd descriptor, a known indirect-call
// target, or anything a manifest or a runtime log names. toc is r2 at entry when known (.opd descriptors carry it).
struct Seed {
    u32 addr = 0;
    std::optional<u32> toc;
    std::string name;
};

// Parses "0x10200" / "10200" (hex) and "0x10200:0x28000" (address:toc). Returns nullopt on malformed text.
std::optional<Seed> parse_seed(std::string_view text);

// Reads a seeds file: one seed per line in parse_seed() syntax, an optional name after whitespace, '#' comments.
std::optional<std::vector<Seed>> read_seeds_file(const std::string& path, std::string& error);

// .opd descriptors (8 bytes each: function address, TOC; PS3 lv2 ABI uses 32-bit pointers) -> seeds.
std::vector<Seed> seeds_from_opd(const CodeImage& image, u32 opd_addr, u32 opd_size);

} // namespace pspocket::recomp::ppu
