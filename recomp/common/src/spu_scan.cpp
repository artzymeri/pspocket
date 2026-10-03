#include "pspocket/recomp/spu_scan.hpp"

#include <algorithm>
#include <cstring>
#include <set>

#include "pspocket/core/be.h"
#include "pspocket/recomp/bytes.hpp"

namespace pspocket::recomp {

namespace {

constexpr u64 kGuestSpace = 0x1'0000'0000ull;

bool looks_like_spu_elf(std::span<const u8> d)
{
    return d.size() >= elf::kHeaderSize32 && d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F' &&
           d[4] == elf::kClass32 && d[5] == elf::kDataMsb && ps_ld16(d.data() + 18) == elf::kMachineSpu;
}

u64 align4(u64 v) { return (v + 3) & ~u64{3}; }

std::optional<std::string> printable_name(std::string_view s)
{
    if (s.empty() || s.size() > 255 || !is_printable_ascii(s)) return std::nullopt;
    return std::string(s);
}

// The program name in a PT_NOTE segment: the note named "SPUNAME" (RPCS3 reads the C string 0x14 bytes into
// the NOTE segment, which is where that note's descriptor starts).
std::optional<std::string> spuname_from_notes(const ElfFile& elf)
{
    for (const auto& p : elf.segments()) {
        if (p.type != elf::kPtNote || !p.data_in_file || p.filesz == 0) continue;
        const ByteReader r(elf.segment_data(p));
        u64 off = 0;
        for (int n = 0; n < 16 && r.in_bounds(off, 12); n++) {
            const u64 namesz = *r.be32(off);
            const u64 descsz = *r.be32(off + 4);
            const u64 name_off = off + 12;
            const u64 desc_off = name_off + align4(namesz);
            if (!r.in_bounds(name_off, namesz) || !r.in_bounds(desc_off, descsz)) break;
            if (r.cstring_lenient(name_off, static_cast<usize>(namesz)) == "SPUNAME") {
                if (auto name = printable_name(r.cstring_lenient(desc_off, static_cast<usize>(descsz)))) return name;
            }
            off = desc_off + align4(descsz);
        }
        if (auto name = printable_name(r.cstring_lenient(0x14, 256))) return name;
    }
    return std::nullopt;
}

std::optional<Sha1Digest> rpcs3_hash(const ElfFile& elf)
{
    Sha1 sha;
    for (const auto& p : elf.segments()) {
        u8 fields[12];
        ps_st32(fields + 0, static_cast<u32>(p.vaddr));
        ps_st32(fields + 4, static_cast<u32>(p.memsz));
        ps_st32(fields + 8, static_cast<u32>(p.filesz));
        sha.update(std::span<const u8>(fields));
        if ((p.type == elf::kPtLoad || p.type == elf::kPtNote) && p.filesz != 0) {
            if (!p.data_in_file) return std::nullopt;
            sha.update(elf.segment_data(p));
        }
    }
    return sha.finish();
}

// The printable string that ends right before `pos` (allowing NUL padding in between).
std::optional<std::string> nearby_string(std::span<const u8> data, u64 pos)
{
    u64 end = pos;
    u64 padding = 0;
    while (end > 0 && data[end - 1] == 0 && padding < 64) {
        end--;
        padding++;
    }
    u64 start = end;
    while (start > 0 && end - start < 128 && data[start - 1] >= 0x20 && data[start - 1] <= 0x7E) start--;
    if (end - start < 4 || (start > 0 && data[start - 1] != 0)) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(data.data() + start), static_cast<usize>(end - start));
}

bool generic_section_name(std::string_view name)
{
    return name.empty() || name == ".text" || name == ".data" || name == ".rodata" || name == ".bss" ||
           name == ".data.rel.ro";
}

} // namespace

std::string rpcs3_spu_hash_string(const Sha1Digest& digest) { return "SPU-" + to_hex(digest); }

std::optional<SpuImage> measure_spu_image(std::span<const u8> data, u64 offset)
{
    if (!looks_like_spu_elf(data)) return std::nullopt;
    ElfParseOptions parse_options;
    parse_options.symbols = false;
    auto elf = ElfFile::parse(data, parse_options);
    if (!elf) return std::nullopt;

    SpuImage img;
    img.offset = offset;
    img.size = elf->extent();
    img.truncated = elf->extent_truncated();
    img.sha1 = Sha1::hash(data.first(static_cast<usize>(img.size)));
    img.rpcs3_hash = rpcs3_hash(*elf);
    img.type = elf->header().type;
    img.entry = static_cast<u32>(elf->header().entry);
    img.phnum = elf->header().phnum;
    img.shnum = elf->header().shnum;
    img.segments = elf->segments();
    for (const auto& p : img.segments) {
        if (p.type == elf::kPtLoad) img.load_bytes += p.filesz;
    }
    img.warnings = elf->warnings();
    if (img.truncated) img.warnings.push_back("image extends past the available data");
    if (auto name = spuname_from_notes(*elf)) {
        img.name = std::move(*name);
        img.name_source = "SPUNAME note";
    }
    return img;
}

std::vector<SpuImage> scan_spu_images(std::span<const u8> data, u64 base_offset)
{
    std::vector<SpuImage> out;
    usize pos = 0;
    while (pos + 4 <= data.size()) {
        const void* hit = std::memchr(data.data() + pos, 0x7F, data.size() - pos);
        if (!hit) break;
        const usize at = static_cast<usize>(static_cast<const u8*>(hit) - data.data());
        if (at + 4 > data.size()) break;
        if (data[at + 1] == 'E' && data[at + 2] == 'L' && data[at + 3] == 'F') {
            if (auto img = measure_spu_image(data.subspan(at), base_offset + at)) {
                // A truncated image's tables can't be trusted to measure it, so only step over its magic.
                const u64 step = img->truncated ? 4 : std::max<u64>(img->size, 4);
                out.push_back(std::move(*img));
                pos = at + static_cast<usize>(std::min<u64>(step, data.size() - at));
                continue;
            }
        }
        pos = at + 1;
    }
    return out;
}

std::vector<SpuImage> scan_spu_images(const ElfFile& ppu)
{
    std::vector<SpuImage> out;
    std::set<u64> seen;
    const auto& phdrs = ppu.segments();
    for (usize i = 0; i < phdrs.size(); i++) {
        const ProgramHeader& p = phdrs[i];
        if (p.type != elf::kPtLoad || !p.data_in_file || p.filesz == 0) continue;
        const auto data = ppu.segment_data(p);
        for (auto& img : scan_spu_images(data, p.offset)) {
            if (!seen.insert(img.offset).second) continue;
            const u64 rel = img.offset - p.offset;
            img.container_segment = static_cast<u32>(i);
            if (p.vaddr + rel < kGuestSpace) img.guest_addr = static_cast<u32>(p.vaddr + rel);

            if (img.name.empty() && img.guest_addr) {
                // _binary_<name>_start symbols from the SDK's SPU embedding, or any symbol at the image address.
                for (const auto& sym : ppu.symbols()) {
                    if (sym.value != *img.guest_addr || sym.name.empty()) continue;
                    std::string_view n = sym.name;
                    if (n.starts_with("_binary_") && n.ends_with("_start")) n = n.substr(8, n.size() - 8 - 6);
                    if (auto name = printable_name(n)) {
                        img.name = std::move(*name);
                        img.name_source = "symbol";
                        break;
                    }
                }
            }
            if (img.name.empty() && img.guest_addr) {
                for (const auto& s : ppu.sections()) {
                    if (s.addr == *img.guest_addr && !generic_section_name(s.name)) {
                        if (auto name = printable_name(s.name)) {
                            img.name = std::move(*name);
                            img.name_source = "section";
                            break;
                        }
                    }
                }
            }
            if (img.name.empty()) {
                if (auto name = nearby_string(data, rel)) {
                    img.name = std::move(*name);
                    img.name_source = "nearby string";
                }
            }
            out.push_back(std::move(img));
        }
    }
    return out;
}

} // namespace pspocket::recomp
