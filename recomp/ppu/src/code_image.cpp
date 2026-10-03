#include "pspocket/recomp/ppu/code_image.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

#include "pspocket/core/be.h"

namespace pspocket::recomp::ppu {

void CodeImage::add_segment(Segment segment)
{
    segments_.push_back(std::move(segment));
    std::sort(segments_.begin(), segments_.end(), [](const Segment& a, const Segment& b) { return a.addr < b.addr; });
}

const Segment* CodeImage::find(u32 addr, u32 size) const
{
    for (const Segment& s : segments_) {
        if (addr >= s.addr && u64{addr} + size <= u64{s.addr} + s.bytes.size()) return &s;
    }
    return nullptr;
}

bool CodeImage::contains(u32 addr, u32 size) const { return find(addr, size) != nullptr; }

bool CodeImage::is_code(u32 addr) const
{
    const Segment* s = find(addr, 4);
    return s && s->executable && (addr & 3) == 0;
}

std::optional<u32> CodeImage::read32(u32 addr) const
{
    const Segment* s = find(addr, 4);
    if (!s) return std::nullopt;
    return ps_ld32(s->bytes.data() + (addr - s->addr));
}

std::optional<u64> CodeImage::read64(u32 addr) const
{
    const Segment* s = find(addr, 8);
    if (!s) return std::nullopt;
    return ps_ld64(s->bytes.data() + (addr - s->addr));
}

namespace {

std::optional<u32> parse_hex(std::string_view t)
{
    if (t.starts_with("0x") || t.starts_with("0X")) t.remove_prefix(2);
    if (t.empty()) return std::nullopt;
    u32 v = 0;
    const auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), v, 16);
    if (ec != std::errc{} || ptr != t.data() + t.size()) return std::nullopt;
    return v;
}

} // namespace

std::optional<Seed> parse_seed(std::string_view text)
{
    Seed seed;
    const usize colon = text.find(':');
    const auto addr = parse_hex(text.substr(0, colon));
    if (!addr) return std::nullopt;
    seed.addr = *addr;
    if (colon != std::string_view::npos) {
        const auto toc = parse_hex(text.substr(colon + 1));
        if (!toc) return std::nullopt;
        seed.toc = *toc;
    }
    return seed;
}

std::optional<std::vector<Seed>> read_seeds_file(const std::string& path, std::string& error)
{
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return std::nullopt;
    }
    std::vector<Seed> seeds;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        lineno++;
        if (const usize hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream fields(line);
        std::string addr, name;
        if (!(fields >> addr)) continue;
        fields >> name;
        auto seed = parse_seed(addr);
        if (!seed) {
            error = path + ":" + std::to_string(lineno) + ": bad seed '" + addr + "'";
            return std::nullopt;
        }
        seed->name = name;
        seeds.push_back(*seed);
    }
    return seeds;
}

std::vector<Seed> seeds_from_opd(const CodeImage& image, u32 opd_addr, u32 opd_size)
{
    std::vector<Seed> seeds;
    for (u32 off = 0; off + 8 <= opd_size; off += 8) {
        const auto fn = image.read32(opd_addr + off);
        const auto toc = image.read32(opd_addr + off + 4);
        if (!fn || !toc || !image.is_code(*fn)) continue;
        seeds.push_back(Seed{*fn, *toc, {}});
    }
    return seeds;
}

} // namespace pspocket::recomp::ppu
