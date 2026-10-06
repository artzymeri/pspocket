// sysPrxForUser's SPU helpers (D-167): SPU ELF information and segments, sys_spu_image_import, and the SPU printf
// server's callbacks. Reference: RPCS3 rpcs3/Emu/Cell/Modules/sys_spu_.cpp (spu_elf_info::init, the loaders' header
// and program header reads, sys_spu_image::get_nsegs and fill from rpcs3/Emu/Cell/lv2/sys_spu.h). The lv2 side
// (images, thread groups) is runtime/lv2/src/spu.cpp. sys_spu_image_close is in sysPrxForUser.cpp.
//
// Not here: sys_raw_spu_load and sys_raw_spu_image_load (raw SPUs are not implemented, D-167).

#include <algorithm>
#include <format>
#include <mutex>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "sysPrxForUser";
constexpr u32 SYS_SPU_IMAGE_PROTECT = 0, SYS_SPU_IMAGE_DIRECT = 1;
constexpr u32 SYS_SPU_IMAGE_TYPE_USER = 0;
constexpr s32 SYS_SPU_SEGMENT_TYPE_COPY = 1, SYS_SPU_SEGMENT_TYPE_FILL = 2, SYS_SPU_SEGMENT_TYPE_INFO = 4;

s32 error(lv2::CellError e) { return lv2::err(e); }

struct Phdr {
    u32 type = 0;
    u64 offset = 0, vaddr = 0, filesz = 0, memsz = 0;
};

struct SpuElf {
    bool sce = false; // the image starts with an SCE header (RPCS3 spu_elf_info::sce0)
    u64 entry = 0;
    u16 machine = 0;
    std::vector<Phdr> phdrs;
};

// RPCS3 spu_elf_info::init and its loader's get_ehdr / get_phdr: an SCE header (version 2, type 1, metadata, an SELF
// header of type 3 naming the ELF and program headers) is followed to the ELF inside; the ELF must be big-endian,
// 32- or 64-bit. CELL_EINVAL for no image, CELL_ENOEXEC for anything else wrong.
s32 read_spu_elf(lv2::PpuThread& t, u32 src, SpuElf& out)
{
    u8* base = t.kernel.base();
    if (!src) return error(lv2::CELL_EINVAL);
    if (!guest_readable(t, src, 0x40)) return error(lv2::CELL_EFAULT);
    u32 ehdr_off = 0, phdr_off = 0;
    if (ps_read32(base, src) == 0x53434500u) {
        out.sce = true;
        if (!guest_readable(t, src + 0x20, 0x50)) return error(lv2::CELL_EFAULT);
        if (ps_read32(base, src + 4) != 2 || ps_read16(base, src + 0xA) != 1 || ps_read32(base, src + 0xC) == 0) {
            return error(lv2::CELL_ENOEXEC);
        }
        ehdr_off = static_cast<u32>(ps_read64(base, src + 0x20 + 0x10)); // se_elfoff
        phdr_off = static_cast<u32>(ps_read64(base, src + 0x20 + 0x18)); // se_phdroff
        if (ps_read64(base, src + 0x20) != 3 || !ehdr_off || !phdr_off) return error(lv2::CELL_ENOEXEC);
    }
    const u32 e = src + ehdr_off;
    if (!guest_readable(t, e, 0x40) || ps_read32(base, e) != 0x7F454C46u || ps_read8(base, e + 5) != 2) {
        return error(lv2::CELL_ENOEXEC);
    }
    const u8 cls = ps_read8(base, e + 4);
    if (cls != 1 && cls != 2) return error(lv2::CELL_ENOEXEC);
    out.machine = ps_read16(base, e + 0x12);
    u16 phnum = 0;
    u32 ph = 0;
    if (cls == 1) {
        out.entry = ps_read32(base, e + 0x18);
        ph = src + (phdr_off ? phdr_off : ps_read32(base, e + 0x1C));
        phnum = ps_read16(base, e + 0x2C);
    } else {
        out.entry = ps_read64(base, e + 0x18);
        ph = src + (phdr_off ? phdr_off : static_cast<u32>(ps_read64(base, e + 0x20)));
        phnum = ps_read16(base, e + 0x38);
    }
    const u32 entsize = cls == 1 ? 0x20 : 0x38;
    if (phnum && !guest_readable(t, ph, entsize * phnum)) return error(lv2::CELL_ENOEXEC);
    out.phdrs.resize(phnum);
    for (u32 i = 0; i < phnum; i++) {
        const u32 p = ph + entsize * i;
        Phdr& d = out.phdrs[i];
        d.type = ps_read32(base, p);
        if (cls == 1) {
            d.offset = ps_read32(base, p + 4);
            d.vaddr = ps_read32(base, p + 8);
            d.filesz = ps_read32(base, p + 16);
            d.memsz = ps_read32(base, p + 20);
        } else {
            d.offset = ps_read64(base, p + 8);
            d.vaddr = ps_read64(base, p + 16);
            d.filesz = ps_read64(base, p + 32);
            d.memsz = ps_read64(base, p + 40);
        }
    }
    return lv2::CELL_OK;
}

// RPCS3 sys_spu_image::get_nsegs<count_info>.
s32 count_segments(const std::vector<Phdr>& ph, bool count_info)
{
    s32 n = 0;
    for (const Phdr& p : ph) {
        if (p.type != 1 && p.type != 4) return -1;
        if (p.type == 1 && p.filesz != p.memsz && p.filesz) n += 2;
        else if (p.type == 1 || count_info) n += 1;
    }
    return n;
}

void write_segment(u8* base, u32 at, s32 type, u32 ls, u32 size, u32 addr)
{
    ps_write32(base, at, static_cast<u32>(type));
    ps_write32(base, at + 4, ls);
    ps_write32(base, at + 8, size);
    ps_write32(base, at + 12, addr);
    ps_write64(base, at + 16, 0);
}

// RPCS3 sys_spu_image::fill<write_info>: -2 when the table is too small, -1 for a program type it can't take.
s32 fill_segments(u8* base, u32 segs, s32 nsegs, const std::vector<Phdr>& ph, u32 src, bool write_info)
{
    s32 n = 0;
    for (const Phdr& p : ph) {
        if (p.type == 1) {
            if (p.filesz) {
                if (n >= nsegs) return -2;
                write_segment(base, segs + 0x18 * static_cast<u32>(n++), SYS_SPU_SEGMENT_TYPE_COPY, static_cast<u32>(p.vaddr),
                    static_cast<u32>(p.filesz), static_cast<u32>(p.offset + src));
            }
            if (p.memsz > p.filesz) {
                if (n >= nsegs) return -2;
                write_segment(base, segs + 0x18 * static_cast<u32>(n++), SYS_SPU_SEGMENT_TYPE_FILL,
                    static_cast<u32>(p.vaddr + p.filesz), static_cast<u32>(p.memsz - p.filesz), 0);
            }
        } else if (write_info && p.type == 4) {
            if (n >= nsegs) return -2;
            write_segment(base, segs + 0x18 * static_cast<u32>(n++), SYS_SPU_SEGMENT_TYPE_INFO, 0, 0x20,
                static_cast<u32>(p.offset + 0x14 + src));
        } else if (p.type != 4) {
            return -1;
        }
    }
    return n;
}

// The ELF header and program headers the three functions below read (RPCS3: init, then get_ehdr / get_phdr, an SPU
// machine and at least one program header).
s32 spu_elf(lv2::PpuThread& t, u32 src, SpuElf& elf, bool reject_sce)
{
    if (const s32 r = read_spu_elf(t, src, elf); r != lv2::CELL_OK) return r;
    if (reject_sce && elf.sce) return error(lv2::CELL_ENOEXEC);
    if (elf.machine != 23 || elf.phdrs.empty()) return error(lv2::CELL_ENOEXEC);
    return lv2::CELL_OK;
}

s32 sys_spu_elf_get_information(PPUContext& ctx, u32 elf_img, u32 entry, u32 nseg)
{
    lv2::PpuThread& t = thread_of(ctx);
    SpuElf elf;
    if (const s32 r = spu_elf(t, elf_img, elf, true); r != lv2::CELL_OK) return r;
    const s32 n = count_segments(elf.phdrs, false);
    if (n < 0) return error(lv2::CELL_ENOEXEC);
    if (!guest_readable(t, entry, 4) || !guest_readable(t, nseg, 4)) return error(lv2::CELL_EFAULT);
    ps_write32(t.kernel.base(), entry, static_cast<u32>(elf.entry));
    ps_write32(t.kernel.base(), nseg, static_cast<u32>(n));
    return lv2::CELL_OK;
}

s32 sys_spu_elf_get_segments(PPUContext& ctx, u32 elf_img, u32 segments, s32 nseg)
{
    lv2::PpuThread& t = thread_of(ctx);
    SpuElf elf;
    if (const s32 r = spu_elf(t, elf_img, elf, false); r != lv2::CELL_OK) return r;
    if (nseg > 0 && !guest_readable(t, segments, 0x18 * static_cast<u32>(nseg))) return error(lv2::CELL_EFAULT);
    const s32 n = fill_segments(t.kernel.base(), segments, nseg, elf.phdrs, elf_img, false);
    if (n == -2) return error(lv2::CELL_ENOMEM);
    if (n < 0) return error(lv2::CELL_ENOEXEC);
    return lv2::CELL_OK;
}

// sys_spu_image_import(img, src, type): PROTECT copies the image into a kernel image (_sys_spu_image_import, the
// file's size from its program headers); DIRECT makes a user image whose segment table (main memory) points into
// the ELF where it is.
s32 sys_spu_image_import(PPUContext& ctx, u32 img, u32 src, u32 type)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (type != SYS_SPU_IMAGE_PROTECT && type != SYS_SPU_IMAGE_DIRECT) return error(lv2::CELL_EINVAL);
    SpuElf elf;
    if (const s32 r = spu_elf(t, src, elf, true); r != lv2::CELL_OK) return r;
    if (!guest_readable(t, img, 16)) return error(lv2::CELL_EFAULT);
    if (type == SYS_SPU_IMAGE_PROTECT) {
        u32 size = 0;
        for (const Phdr& p : elf.phdrs) {
            if (p.type != 1 && p.type != 4) return error(lv2::CELL_ENOEXEC);
            size = std::max<u32>(size, static_cast<u32>(p.offset + p.filesz));
        }
        return lv2::_sys_spu_image_import(t, img, src, size, 0);
    }
    const s32 n = count_segments(elf.phdrs, true);
    if (n < 0) return error(lv2::CELL_ENOEXEC);
    ps_write32(base, img + 12, static_cast<u32>(n));
    ps_write32(base, img + 4, static_cast<u32>(elf.entry));
    const u32 segs = HleProcess::current()->main_memory().alloc(0x18 * static_cast<u32>(n));
    if (!segs) return error(lv2::CELL_ENOMEM);
    if (fill_segments(base, segs, n, elf.phdrs, src, true) != n) {
        HleProcess::current()->main_memory().free(segs);
        return error(lv2::CELL_ENOEXEC);
    }
    ps_write32(base, img, SYS_SPU_IMAGE_TYPE_USER);
    ps_write32(base, img + 8, segs);
    PS_LOG_INFO(kChan, "sys_spu_image_import({:#x}, DIRECT): user image, entry {:#x}, {} segments at {:#x}", src, elf.entry,
        n, segs);
    return lv2::CELL_OK;
}

// The SPU printf server's callbacks (RPCS3 _sys_spu_printf_*): kept, and called on the calling thread with the group
// or thread ID; CELL_ESTAT when none is set.
struct SpuPrintf {
    std::mutex mutex;
    u32 agcb = 0, dgcb = 0, atcb = 0, dtcb = 0;
};
SpuPrintf& spu_printf() { return HleProcess::current()->state<SpuPrintf>(); }

s32 _sys_spu_printf_initialize(PPUContext&, u32 agcb, u32 dgcb, u32 atcb, u32 dtcb)
{
    SpuPrintf& p = spu_printf();
    std::lock_guard lock(p.mutex);
    p.agcb = agcb;
    p.dgcb = dgcb;
    p.atcb = atcb;
    p.dtcb = dtcb;
    PS_LOG_INFO(kChan, "_sys_spu_printf_initialize({:#x}, {:#x}, {:#x}, {:#x})", agcb, dgcb, atcb, dtcb);
    return lv2::CELL_OK;
}

s32 _sys_spu_printf_finalize(PPUContext&)
{
    SpuPrintf& p = spu_printf();
    std::lock_guard lock(p.mutex);
    p.agcb = p.dgcb = p.atcb = p.dtcb = 0;
    return lv2::CELL_OK;
}

s32 call_printf_callback(PPUContext& ctx, u32 SpuPrintf::*which, u32 id)
{
    u32 cb = 0;
    {
        SpuPrintf& p = spu_printf();
        std::lock_guard lock(p.mutex);
        cb = p.*which;
    }
    if (!cb) return error(lv2::CELL_ESTAT);
    return static_cast<s32>(call_guest(ctx, cb, {id}));
}

s32 _sys_spu_printf_attach_group(PPUContext& ctx, u32 group) { return call_printf_callback(ctx, &SpuPrintf::agcb, group); }
s32 _sys_spu_printf_detach_group(PPUContext& ctx, u32 group) { return call_printf_callback(ctx, &SpuPrintf::dgcb, group); }
s32 _sys_spu_printf_attach_thread(PPUContext& ctx, u32 thread) { return call_printf_callback(ctx, &SpuPrintf::atcb, thread); }
s32 _sys_spu_printf_detach_thread(PPUContext& ctx, u32 thread) { return call_printf_callback(ctx, &SpuPrintf::dtcb, thread); }

} // namespace

void register_sys_spu(Registry& r)
{
    r.add("sysPrxForUser", {
        PS_HLE_FUNC(0x1ED454CE, sys_spu_elf_get_information),
        PS_HLE_FUNC(0xDB6B3250, sys_spu_elf_get_segments),
        PS_HLE_FUNC(0xEBE5F72F, sys_spu_image_import),
        PS_HLE_FUNC(0x45FE2FCE, _sys_spu_printf_initialize),
        PS_HLE_FUNC(0xDD3B27AC, _sys_spu_printf_finalize),
        PS_HLE_FUNC(0xDD0C1E09, _sys_spu_printf_attach_group),
        PS_HLE_FUNC(0x5FDFB2FE, _sys_spu_printf_detach_group),
        PS_HLE_FUNC(0x1AE10B92, _sys_spu_printf_attach_thread),
        PS_HLE_FUNC(0xB3BBCF2A, _sys_spu_printf_detach_thread),
    });
}

} // namespace pspocket::hle
