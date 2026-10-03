// SPU program images: SPU ELF (through recomp/common's parser) or raw LS bytes.

#include "pspocket/recomp/spu/image.hpp"

#include <algorithm>
#include <cstring>

#include "pspocket/core/be.h"
#include "pspocket/recomp/elf.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/runtime.h"

namespace pspocket::recomp::spu {

u32 LsImage::word(u32 addr) const { return ps_ld32(ls.data() + (addr & PS_SPU_LS_MASK & ~3u)); }

u64 LsImage::hash() const { return ps_spu_hash64(ls.data() + code_start, code_size()); }

Result<LsImage> load_spu_elf(std::span<const u8> data, std::string name)
{
    auto parsed = ElfFile::parse(data);
    if (!parsed) return parsed.error();
    const ElfFile& elf = *parsed;
    if (!elf.is_spu()) return make_error("not an SPU ELF (machine {}, class {})", elf.header().machine, elf.header().elf_class);

    LsImage image;
    image.name = std::move(name);
    u64 code_lo = PS_SPU_LS_SIZE, code_hi = 0, load_lo = PS_SPU_LS_SIZE, load_hi = 0;
    for (const ProgramHeader& ph : elf.segments()) {
        if (ph.type != elf::kPtLoad || ph.memsz == 0) continue;
        if (ph.vaddr + ph.memsz > PS_SPU_LS_SIZE || ph.filesz > ph.memsz)
            return make_error("PT_LOAD at 0x{:x} (+0x{:x}) does not fit in the local store", ph.vaddr, ph.memsz);
        if (ph.filesz) {
            if (!ph.data_in_file) return make_error("PT_LOAD at 0x{:x} has no data in the file", ph.vaddr);
            const std::span<const u8> bytes = elf.segment_data(ph);
            std::memcpy(image.ls.data() + ph.vaddr, bytes.data(), bytes.size());
        }
        load_lo = std::min(load_lo, ph.vaddr);
        load_hi = std::max(load_hi, ph.vaddr + ph.filesz);
        if (ph.flags & (elf::kPfX | elf::kPfSpuX)) {
            code_lo = std::min(code_lo, ph.vaddr);
            code_hi = std::max(code_hi, ph.vaddr + ph.filesz);
        }
    }
    if (code_hi <= code_lo) {
        code_lo = load_lo;
        code_hi = load_hi;
    }
    if (code_hi <= code_lo) return make_error("SPU ELF has no loadable code");
    image.code_start = static_cast<u32>(code_lo) & ~3u;
    image.code_end = (static_cast<u32>(code_hi) + 3) & ~3u;
    const u32 entry = static_cast<u32>(elf.header().entry);
    if (!image.in_code(entry)) return make_error("entry point 0x{:x} is outside the code segments", entry);
    image.entries.push_back(entry);
    return image;
}

Result<LsImage> load_raw_image(std::span<const u8> bytes, u32 base, std::vector<u32> entries, std::string name)
{
    if ((base & 3) || base >= PS_SPU_LS_SIZE || bytes.size() > PS_SPU_LS_SIZE - base)
        return make_error("raw image at 0x{:x} (+0x{:x}) does not fit in the local store", base, bytes.size());
    LsImage image;
    image.name = std::move(name);
    std::memcpy(image.ls.data() + base, bytes.data(), bytes.size());
    image.code_start = base;
    image.code_end = base + ((static_cast<u32>(bytes.size()) + 3) & ~3u);
    for (u32 e : entries)
        if (!image.in_code(e)) return make_error("entry 0x{:x} is outside the image", e);
    image.entries = std::move(entries);
    return image;
}

} // namespace pspocket::recomp::spu
