#include "pspocket/recomp/guest_image.hpp"

#include <algorithm>
#include <cstring>
#include <format>

#include "pspocket/core/be.h"
#include "pspocket/recomp/bytes.hpp"

namespace pspocket::recomp {

namespace {

constexpr u64 kGuestSpace = 0x1'0000'0000ull;
// Relocation writes may materialize bss, but never more than this beyond a segment's file data.
constexpr u64 kMaxBssGrowth = 16ull << 20;

} // namespace

Result<GuestImage> GuestImage::from_elf(const ElfFile& elf)
{
    GuestImage img;
    u32 load_index = 0;
    const auto& phdrs = elf.segments();
    for (usize i = 0; i < phdrs.size(); i++) {
        const ProgramHeader& p = phdrs[i];
        if (p.type != elf::kPtLoad) continue;
        const u32 this_load = load_index++;
        const u64 memsz = std::max(p.memsz, p.filesz);
        if (memsz == 0) continue;
        if (p.vaddr >= kGuestSpace || memsz > kGuestSpace - p.vaddr) {
            return make_error("LOAD segment {} [{:#x}, +{:#x}) does not fit the 32-bit guest address space", i, p.vaddr, memsz);
        }
        if (p.filesz != 0 && !p.data_in_file) {
            return make_error("LOAD segment {} file data ({:#x} bytes at {:#x}) is outside the file", i, p.filesz, p.offset);
        }
        Segment s;
        s.vaddr = static_cast<u32>(p.vaddr);
        s.memsz = memsz;
        s.flags = p.flags;
        s.phdr_index = static_cast<u32>(i);
        s.load_index = this_load;
        s.file_offset = p.offset;
        s.file_size = p.filesz;
        const auto bytes = elf.segment_data(p);
        s.data.assign(bytes.begin(), bytes.end());
        img.any_executable_ |= (p.flags & elf::kPfX) != 0;
        img.segments_.push_back(std::move(s));
    }

    for (usize a = 0; a < img.segments_.size(); a++) {
        for (usize b = a + 1; b < img.segments_.size(); b++) {
            const auto& x = img.segments_[a];
            const auto& y = img.segments_[b];
            if (x.vaddr < y.vaddr + y.memsz && y.vaddr < x.vaddr + x.memsz) {
                img.warnings_.push_back(std::format("LOAD segments {} and {} overlap", x.phdr_index, y.phdr_index));
            }
        }
    }

    img.apply_prx_relocations(elf);
    return img;
}

const GuestImage::Segment* GuestImage::find(u64 addr, u64 size) const
{
    for (const auto& s : segments_) {
        if (addr < s.vaddr) continue;
        const u64 delta = addr - s.vaddr;
        if (delta >= s.memsz || size > s.memsz - delta) continue;
        return &s;
    }
    return nullptr;
}

GuestImage::Segment* GuestImage::find_mut(u64 addr, u64 size)
{
    return const_cast<Segment*>(static_cast<const GuestImage*>(this)->find(addr, size));
}

bool GuestImage::executable(u64 addr) const
{
    const Segment* s = find(addr, 1);
    if (!s) return false;
    if (any_executable_) return (s->flags & elf::kPfX) != 0;
    return s->load_index == 0;
}

bool GuestImage::read_bytes(u64 addr, std::span<u8> out) const
{
    const Segment* s = find(addr, out.size());
    if (!s) return false;
    const u64 delta = addr - s->vaddr;
    const u64 in_data = delta < s->data.size() ? std::min<u64>(out.size(), s->data.size() - delta) : 0;
    if (in_data != 0) std::memcpy(out.data(), s->data.data() + delta, static_cast<usize>(in_data));
    if (in_data < out.size()) std::memset(out.data() + in_data, 0, static_cast<usize>(out.size() - in_data));
    return true;
}

std::optional<u8> GuestImage::read8(u64 addr) const
{
    u8 b[1];
    if (!read_bytes(addr, b)) return std::nullopt;
    return b[0];
}

std::optional<u16> GuestImage::read16(u64 addr) const
{
    u8 b[2];
    if (!read_bytes(addr, b)) return std::nullopt;
    return ps_ld16(b);
}

std::optional<u32> GuestImage::read32(u64 addr) const
{
    u8 b[4];
    if (!read_bytes(addr, b)) return std::nullopt;
    return ps_ld32(b);
}

std::optional<u64> GuestImage::read64(u64 addr) const
{
    u8 b[8];
    if (!read_bytes(addr, b)) return std::nullopt;
    return ps_ld64(b);
}

std::optional<std::string> GuestImage::read_cstring(u64 addr, usize max_length) const
{
    const Segment* s = find(addr, 1);
    if (!s) return std::nullopt;
    const u64 delta = addr - s->vaddr;
    const u64 limit = std::min<u64>(s->memsz - delta, u64{max_length} + 1);
    std::string out;
    for (u64 i = 0; i < limit; i++) {
        const u64 at = delta + i;
        const char c = at < s->data.size() ? static_cast<char>(s->data[at]) : '\0';
        if (c == '\0') return out;
        out.push_back(c);
    }
    return std::nullopt;
}

bool GuestImage::write_bytes(u64 addr, std::span<const u8> in)
{
    Segment* s = find_mut(addr, in.size());
    if (!s) return false;
    const u64 end = addr - s->vaddr + in.size();
    if (end > s->data.size()) {
        if (end > s->file_size + kMaxBssGrowth) return false; // cumulative cap: hostile relocations can't balloon memory
        s->data.resize(static_cast<usize>(end), 0);
    }
    std::memcpy(s->data.data() + (addr - s->vaddr), in.data(), in.size());
    return true;
}

bool GuestImage::write16(u64 addr, u16 value)
{
    u8 b[2];
    ps_st16(b, value);
    return write_bytes(addr, b);
}

bool GuestImage::write32(u64 addr, u32 value)
{
    u8 b[4];
    ps_st32(b, value);
    return write_bytes(addr, b);
}

bool GuestImage::write64(u64 addr, u64 value)
{
    u8 b[8];
    ps_st64(b, value);
    return write_bytes(addr, b);
}

// PRX relocations, PT_SCE_PPURELA (0x700000A4). Entry layout and relocation semantics follow RPCS3
// rpcs3/Emu/Cell/PPUModule.cpp, ppu_load_prx (ppu_prx_relocation_info), with each LOAD segment based at its
// own p_vaddr instead of a vm::alloc address.
void GuestImage::apply_prx_relocations(const ElfFile& elf)
{
    struct Base {
        bool valid = false;
        u64 addr = 0;
        u64 size = 0;
    };
    std::vector<Base> bases; // by LOAD index, including empty LOAD segments (as RPCS3's prx->segs)
    for (const auto& p : elf.segments()) {
        if (p.type != elf::kPtLoad) continue;
        const u64 memsz = std::max(p.memsz, p.filesz);
        bases.push_back({p.vaddr < kGuestSpace && memsz <= kGuestSpace - p.vaddr && memsz != 0, p.vaddr, memsz});
    }

    for (usize i = 0; i < elf.segments().size(); i++) {
        const ProgramHeader& p = elf.segments()[i];
        if (p.type != elf::kPtScePpuRela) continue;
        if (!p.data_in_file) {
            warnings_.push_back(std::format("relocation segment {} is outside the file", i));
            continue;
        }
        const ByteReader r(elf.segment_data(p));
        if (r.size() % 24 != 0) warnings_.push_back(std::format("relocation segment {} has {} trailing bytes", i, r.size() % 24));
        for (u64 off = 0; off + 24 <= r.size(); off += 24) {
            relocations_.total++;
            const u64 offset = *r.be64(off);
            const u8 index_value = *r.u8_at(off + 10);
            const u8 index_addr = *r.u8_at(off + 11);
            const u32 type = *r.be32(off + 12);
            const u64 ptr = *r.be64(off + 16);
            relocations_.by_type[type]++;

            if (index_addr >= bases.size() || !bases[index_addr].valid ||
                offset >= ((bases[index_addr].size + 0xFF) & ~u64{0xFF})) {
                relocations_.out_of_range++;
                continue;
            }
            u64 data_base = 0;
            if (index_value != 0xFF) {
                if (index_value >= bases.size() || !bases[index_value].valid) {
                    relocations_.out_of_range++;
                    continue;
                }
                data_base = bases[index_value].addr;
            }
            const u64 raddr = bases[index_addr].addr + offset;
            const u64 rdata = data_base + ptr;

            bool ok = false;
            switch (type) {
            case 1: ok = write32(raddr, static_cast<u32>(rdata)); break;  // R_PPC64_ADDR32
            case 4: ok = write16(raddr, static_cast<u16>(rdata)); break;  // R_PPC64_ADDR16_LO
            case 5: ok = write16(raddr, static_cast<u16>(rdata >> 16)); break; // R_PPC64_ADDR16_HI
            case 6:                                                          // R_PPC64_ADDR16_HA
                ok = write16(raddr, static_cast<u16>((rdata >> 16) + ((rdata & 0x8000) ? 1 : 0)));
                break;
            case 10: { // R_PPC64_REL24: 24-bit field at bit 6 (MSB 0) of the instruction
                const auto insn = read32(raddr);
                const u32 field = (static_cast<u32>(rdata - raddr) >> 2) & 0xFFFFFF;
                ok = insn && write32(raddr, (*insn & ~0x03FFFFFCu) | (field << 2));
                break;
            }
            case 11: { // R_PPC64_REL14: 14-bit field at bit 16
                const auto insn = read32(raddr);
                const u32 field = (static_cast<u32>(rdata - raddr) >> 2) & 0x3FFF;
                ok = insn && write32(raddr, (*insn & ~0x0000FFFCu) | (field << 2));
                break;
            }
            case 38: ok = write64(raddr, rdata); break;         // R_PPC64_ADDR64
            case 44: ok = write64(raddr, rdata - raddr); break; // R_PPC64_REL64
            case 57: { // R_PPC64_ADDR16_LO_DS: 14-bit field at bit 0 of a halfword
                const auto half = read16(raddr);
                const u16 field = static_cast<u16>((static_cast<u16>(rdata) >> 2) & 0x3FFF);
                ok = half && write16(raddr, static_cast<u16>((*half & 0x3u) | (field << 2)));
                break;
            }
            default: relocations_.unsupported++; continue;
            }
            if (ok) {
                relocations_.applied++;
            } else {
                relocations_.out_of_range++;
            }
        }
    }
}

} // namespace pspocket::recomp
