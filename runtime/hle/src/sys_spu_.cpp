// sysPrxForUser's SPU helpers (D-167): SPU ELF information and segments, sys_spu_image_import, and the SPU printf
// server's callbacks. Reference: RPCS3 rpcs3/Emu/Cell/Modules/sys_spu_.cpp (spu_elf_info::init, the loaders' header
// and program header reads, sys_spu_image::get_nsegs and fill from rpcs3/Emu/Cell/lv2/sys_spu.h). The lv2 side
// (images, thread groups) is runtime/lv2/src/spu.cpp. sys_spu_image_close is in sysPrxForUser.cpp.
//
// sys_raw_spu_load and sys_raw_spu_image_load are liblv2's (D-529): a raw SPU's local store filled through its MFC
// proxy registers, as liblv2 does, with lv2's raw SPUs behind them (D-528).

#include <algorithm>
#include <atomic>
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

// liblv2's own heap (sysPrxForUser_libc.cpp).
u32 lv2_malloc(PPUContext& ctx, u32 size);
u32 lv2_memalign(PPUContext& ctx, u32 align, u32 size);
void lv2_free(PPUContext& ctx, u32 addr);

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

// sys_spu_segment: the address or fill value at +0x10, the first word of a doubleword-aligned union (RPCS3 sys_spu.h,
// liblv2); +0xC is padding.
void write_segment(u8* base, u32 at, s32 type, u32 ls, u32 size, u32 addr)
{
    ps_write32(base, at, static_cast<u32>(type));
    ps_write32(base, at + 4, ls);
    ps_write32(base, at + 8, size);
    ps_write32(base, at + 12, 0);
    ps_write32(base, at + 16, addr);
    ps_write32(base, at + 20, 0);
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

// liblv2's raw SPU deploy (D-529): each COPY segment through the raw SPU's MFC proxy, GETs of up to 16 KB on tag 0 (the
// LSA, EAH 0, EAL and size registers, a barrier, the command; written again until the command status takes it), each
// FILL segment a word at a time straight into the local store, other types skipped; then the proxy's tag query for
// tag 0, polled until it completes. The registers are guest MMIO (D-528): these stores and loads fault into lv2's raw
// SPU, as liblv2's do.
constexpr u32 kRawLs = 0xE0000000, kRawProb = 0x40000;
constexpr u32 MFC_LSA = 0x3004, MFC_EAH = 0x3008, MFC_EAL = 0x300C, MFC_Size_Tag = 0x3010, MFC_Class_CMD = 0x3014,
              Prxy_QueryMask = 0x321C, Prxy_TagStatus = 0x322C, SPU_NPC = 0x4034, kMfcGet = 0x40;

void raw_deploy(u8* base, u32 id, u32 segs, s32 nsegs)
{
    const u32 ls = kRawLs + (id << 20), prob = ls + kRawProb;
    for (s32 i = 0; i < nsegs; i++) {
        const u32 seg = segs + 24 * static_cast<u32>(i);
        const s32 type = static_cast<s32>(ps_read32(base, seg));
        if (type == SYS_SPU_SEGMENT_TYPE_COPY) {
            s32 left = static_cast<s32>(ps_read32(base, seg + 8));
            for (u32 chunk = 0; left > 0; chunk++, left -= 0x4000) {
                const u32 n = left > 0x3FFF ? 0x4000u : static_cast<u32>(left);
                do {
                    ps_write32(base, prob + MFC_LSA, ps_read32(base, seg + 4) + (chunk << 14));
                    ps_write32(base, prob + MFC_EAH, 0);
                    ps_write32(base, prob + MFC_EAL, ps_read32(base, seg + 16) + (chunk << 14));
                    ps_write32(base, prob + MFC_Size_Tag, n << 16);
                    std::atomic_thread_fence(std::memory_order_seq_cst); // eieio
                    ps_write32(base, prob + MFC_Class_CMD, kMfcGet);
                } while (ps_read32(base, prob + MFC_Class_CMD) != 0);
            }
        } else if (type == SYS_SPU_SEGMENT_TYPE_FILL) {
            const u32 at = ps_read32(base, seg + 4), value = ps_read32(base, seg + 16);
            for (u32 a = at; ps_read32(base, seg + 8) + at > a; a += 4) ps_write32(base, kRawLs + (id << 20) + a, value);
        }
    }
    ps_write32(base, prob + Prxy_QueryMask, 1);
    do std::atomic_thread_fence(std::memory_order_seq_cst);
    while (!(ps_read32(base, prob + Prxy_TagStatus) & 1));
}

// Whether raw SPU `id`'s local store and registers are there (liblv2 has no check: its first store would fault).
bool raw_spu_present(lv2::PpuThread& t, s32 id, const char* fn)
{
    if (id >= 0 && id < 5 && guest_readable(t, kRawLs + (static_cast<u32>(id) << 20), 0x40000)) return true;
    PS_LOG_ERROR(kChan, "{}({}): no such raw SPU (liblv2 would fault on its local store)", fn, id);
    return false;
}

// sys_raw_spu_image_load(id, img): a user image's segment table as it is, a kernel image's through lv2 (155 and 159,
// the table in liblv2's heap for the deploy); then SPU_NPC = the entry. CELL_EINVAL for another image type.
s32 sys_raw_spu_image_load(PPUContext& ctx, s32 id, u32 img)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_readable(t, img, 16)) return error(lv2::CELL_EFAULT);
    if (!raw_spu_present(t, id, "sys_raw_spu_image_load")) return error(lv2::CELL_ESRCH);
    const u32 type = ps_read32(base, img);
    u32 entry = 0;
    if (type == SYS_SPU_IMAGE_TYPE_USER) {
        raw_deploy(base, static_cast<u32>(id), ps_read32(base, img + 8), static_cast<s32>(ps_read32(base, img + 12)));
        entry = ps_read32(base, img + 4);
    } else if (type == 1) {
        StackVar v(ctx, base, 8);
        if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 155, {img, v.addr(), v.addr() + 4})); r != lv2::CELL_OK) {
            return r;
        }
        entry = ps_read32(base, v.addr());
        const s32 nsegs = static_cast<s32>(ps_read32(base, v.addr() + 4));
        const u32 segs = lv2_malloc(ctx, (static_cast<u32>(nsegs) * 24) & ~7u);
        if (!segs) return error(lv2::CELL_ENOMEM);
        if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 159, {img, segs, static_cast<u64>(static_cast<s64>(nsegs))}));
            r != lv2::CELL_OK) {
            lv2_free(ctx, segs);
            return r;
        }
        raw_deploy(base, static_cast<u32>(id), segs, nsegs);
        lv2_free(ctx, segs);
    } else {
        return error(lv2::CELL_EINVAL);
    }
    ps_write32(base, kRawLs + (static_cast<u32>(id) << 20) + kRawProb + SPU_NPC, entry);
    return lv2::CELL_OK;
}

// sys_raw_spu_load(id, path, entry): the file read whole into liblv2's heap (sys_fs open, fstat, read, close; a short
// read or a read error is CELL_ENOENT), its entry into *entry and its segments into the raw SPU's local store.
s32 sys_raw_spu_load(PPUContext& ctx, s32 id, u32 path, u32 entry)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    StackVar v(ctx, base, 0x50); // +0 nsegs, +4 fd, +8 nread, +0x10 stat
    const u32 nseg = v.addr(), fd = v.addr() + 4, nread = v.addr() + 8, st = v.addr() + 0x10;
    if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 801, {path, 0, fd, 0, 0, 0})); r != lv2::CELL_OK) return r;
    const auto close = [&] { lv2_syscall(ctx, 804, {ps_read32(base, fd)}); };
    if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 809, {ps_read32(base, fd), st})); r != lv2::CELL_OK) {
        close();
        return r;
    }
    const u32 size = ps_read32(base, st + 40); // CellFsStat's size, its low word
    const u32 buf = lv2_memalign(ctx, 16, size);
    if (!buf) {
        close();
        return error(lv2::CELL_ENOMEM);
    }
    const s32 rr = static_cast<s32>(lv2_syscall(ctx, 802, {ps_read32(base, fd), buf, size, nread}));
    if (rr != lv2::CELL_OK || ps_read64(base, nread) != size) {
        close();
        lv2_free(ctx, buf);
        return error(lv2::CELL_ENOENT);
    }
    close();
    if (const s32 r = sys_spu_elf_get_information(ctx, buf, entry, nseg); r != lv2::CELL_OK) {
        lv2_free(ctx, buf);
        return r;
    }
    const s32 n = static_cast<s32>(ps_read32(base, nseg));
    const u32 segs = lv2_malloc(ctx, (static_cast<u32>(n) * 24) & ~7u);
    if (!segs) {
        lv2_free(ctx, buf);
        return error(lv2::CELL_ENOMEM);
    }
    const s32 r = sys_spu_elf_get_segments(ctx, buf, segs, n);
    if (r == lv2::CELL_OK) {
        if (!raw_spu_present(t, id, "sys_raw_spu_load")) {
            lv2_free(ctx, buf);
            lv2_free(ctx, segs);
            return error(lv2::CELL_ESRCH);
        }
        raw_deploy(base, static_cast<u32>(id), segs, n);
    }
    lv2_free(ctx, buf);
    lv2_free(ctx, segs);
    return r;
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
        PS_HLE_FUNC(0xB995662E, sys_raw_spu_image_load),
        PS_HLE_FUNC(0x893305FA, sys_raw_spu_load),
        PS_HLE_FUNC(0x45FE2FCE, _sys_spu_printf_initialize),
        PS_HLE_FUNC(0xDD3B27AC, _sys_spu_printf_finalize),
        PS_HLE_FUNC(0xDD0C1E09, _sys_spu_printf_attach_group),
        PS_HLE_FUNC(0x5FDFB2FE, _sys_spu_printf_detach_group),
        PS_HLE_FUNC(0x1AE10B92, _sys_spu_printf_attach_thread),
        PS_HLE_FUNC(0xB3BBCF2A, _sys_spu_printf_detach_thread),
    });
}

} // namespace pspocket::hle
