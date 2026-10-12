// cellSailRec's video converters (D-918, part 5), as libsail_rec (fw 4.92) makes and runs them, and what the recorder
// takes from outside its PPU code: its SPU programs and tables (libsail_rec's own, read from the installed system
// software at run time, never kept here: CLAUDE.md rule 1, as cellSpursJq's policy module, D-537), the firmware's
// libraries it calls (cellAvchatJpgDec now; the encoders, cellVpost2, cellApostSrcMini and cellMp4 with the streams:
// imports the loader links for a process using cellSailRec, D-197), the common taskset and its module loads.
//
// A converter (CreateVideoConverter, the control thread's message 3) is one block from the game's allocator (boundary
// 0x80): the conversion's work area first, then the converter (cellSailRec.hpp cv::). By the codings (cellSail.h): a
// YUV 4:2:2 coding (2 UYVY, 4 YUYV, 9 VYUY, 10 Y1VY0U) to ARGB or RGBA (0, 1) is kind 0, raw Bayer (12) to ARGB or RGBA
// kind 1, raw Bayer to a 4:2:2 coding kind 2: each request one task of libsail_rec's SPU program for that kind in the
// common taskset, its work area holding what the program reads (the sizes, pitches, a message for the recorder's SPURS
// queue, the coefficients from libsail_rec's tables); JPEG (11) to RGBA, YUYV or VYUY (1, 4, 9) is kind 3, through
// cellAvchatJpgDec (its module loaded, a decoder opened on the work
// area with the common taskset's SPURS settings, told where to send its message). The conversion's end comes back as
// that message: the task handler takes it from the SPURS queue (the result, the JPEG decoder's size for kind 3), the
// next request starts, and the control thread calls the converter's callback (messages 0x32 to 0x35). Failures all
// answer MEMORY, as libsail_rec answers them (a converter refused after its sync objects were made keeps them, and the
// block is freed from the converter's address, not the block's: libsail_rec's own slips, kept).

#include <cstring>
#include <format>
#include <mutex>
#include <vector>

#include "cellSailRec.hpp"
#include "firmware_data.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle::sailrec {

namespace {

// libsail_rec.sprx of firmware 4.92 (recomp::prx_identity): the module whose layout this file knows.
constexpr std::string_view kModuleFile = "libsail_rec.sprx";
constexpr std::string_view kModuleIdentity = "952c85225d275347c8cbe563509bfbcb6950409544a5d99d1565b4582327618e";

// libsail_rec's SPU programs for the converters (its ELF images) and their tables.
constexpr u32 kProgramBayerToRgb = 0x1DC00, kProgramBayerTo422 = 0x1F680, kProgram422ToRgb = 0x2B900;
constexpr u32 kTableBayerToRgb = 0x1CD50, kTableBayerTo422 = 0x1CD90, kTable422ToRgb = 0x1D790;

// The JPEG decoder's functions (cellAvchatJpgDec, NID only): the work area's size, the result's size, where its
// message goes, open, decode, close.
constexpr u32 kJpgQuery = 0x0E520B15, kJpgResult = 0x4644B39A, kJpgQueue = 0x4D99E6DF, kJpgOpen = 0x943A6675,
              kJpgDecode = 0xEF9E83D2, kJpgClose = 0xFD51609E;
constexpr u32 kJpgModule = 0xF009;

constexpr HostImport kImports[] = {
    {{}, "cellAvchatJpgDec", kJpgQuery, false},
    {{}, "cellAvchatJpgDec", kJpgResult, false},
    {{}, "cellAvchatJpgDec", kJpgQueue, false},
    {{}, "cellAvchatJpgDec", kJpgOpen, false},
    {{}, "cellAvchatJpgDec", kJpgDecode, false},
    {{}, "cellAvchatJpgDec", kJpgClose, false},
};

s32 lw(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args)
{
    return static_cast<s32>(sail::lv2_lib(ctx, name, args));
}

u64 sx(s32 v) { return static_cast<u64>(static_cast<s64>(v)); }

// ------------------------------------------------------------------------------------------------- the module's data

struct ModuleData {
    std::mutex m;
    std::map<u32, u32> programs; // vaddr -> guest copy (0: could not be had)
};

std::optional<std::vector<u8>> read_module(u32 vaddr, u32 size)
{
    return firmware_module_data(*HleProcess::current(), kModuleFile, kModuleIdentity, vaddr, size);
}

} // namespace

bool module_bytes(u32 vaddr, u32 size, u8* out)
{
    const auto bytes = read_module(vaddr, size);
    if (!bytes || bytes->size() < size) return false;
    std::memcpy(out, bytes->data(), size);
    return true;
}

u32 spu_program(PPUContext& ctx, u32 vaddr)
{
    ModuleData& d = HleProcess::current()->state<ModuleData>();
    std::lock_guard lock(d.m);
    if (const auto it = d.programs.find(vaddr); it != d.programs.end()) return it->second;
    u32& at = d.programs[vaddr];
    // The ELF's extent: its program and section headers and every segment's file bytes.
    const auto head = read_module(vaddr, 0x34);
    if (!head || ps_ld32(head->data()) != 0x7F454C46u) {
        PS_LOG_ERROR(kChan, "libsail_rec's SPU program at {:#x}: {} (the installed system software's {} is needed)",
            vaddr, head ? "not an ELF" : "the module cannot be read", kModuleFile);
        return 0;
    }
    const u8* h = head->data();
    const u32 phoff = ps_ld32(h + 0x1C), shoff = ps_ld32(h + 0x20);
    const u32 phentsize = ps_ld16(h + 0x2A), phnum = ps_ld16(h + 0x2C), shentsize = ps_ld16(h + 0x2E),
              shnum = ps_ld16(h + 0x30);
    u32 end = std::max(phoff + phnum * phentsize, shoff + shnum * shentsize);
    const auto phdrs = read_module(vaddr + phoff, phnum * phentsize);
    if (!phdrs) return 0;
    for (u32 i = 0; i < phnum; i++) {
        const u8* p = phdrs->data() + i * phentsize;
        end = std::max(end, ps_ld32(p + 4) + ps_ld32(p + 16));
    }
    const auto image = read_module(vaddr, end);
    if (!image) return 0;
    const u32 span = (end + 127) & ~127u;
    at = HleProcess::current()->main_memory().alloc(span, 0x80);
    if (!at) {
        PS_LOG_ERROR(kChan, "no guest memory for libsail_rec's SPU program at {:#x} ({:#x} bytes)", vaddr, span);
        return 0;
    }
    std::memcpy(gb(ctx) + at, image->data(), end);
    PS_LOG_INFO(kChan, "libsail_rec's SPU program at {:#x}: {:#x} bytes of {} at {:#x}", vaddr, end, kModuleFile, at);
    return at;
}

u64 firmware_call(PPUContext& ctx, std::string_view library, u32 nid, std::initializer_list<u64> args)
{
    const u32 fn = HleProcess::current()->imported(library, nid);
    if (!fn) {
        log_call_site(ctx, gb(ctx),
            std::format("cellSailRec: {}::{:#010x} is the firmware's: no installed module gave it", library, nid));
        PS_LOG_ERROR(kChan, "{} ({:#010x}): no firmware module gave it (cellSailRec needs the installed system "
            "software for it)", library, nid);
        return sx(static_cast<s32>(sail::CELL_SAIL_ERROR_FATAL));
    }
    return call_guest(ctx, fn, args);
}

s32 load_module(PPUContext& ctx, u32 id)
{
    if (modules_flag(ctx)) return 0;
    if (id >= 0xF000)
        return static_cast<s32>(sail::module_call(ctx, "cellSysmodule", "cellSysmoduleLoadModuleInternal", {id & 0xFFFF}));
    return static_cast<s32>(sail::module_call(ctx, "cellSysmodule", "cellSysmoduleLoadModule", {id & 0xFFFF}));
}

s32 unload_module(PPUContext& ctx, u32 id)
{
    if (modules_flag(ctx)) return 0;
    if (id >= 0xF000)
        return static_cast<s32>(
            sail::module_call(ctx, "cellSysmodule", "cellSysmoduleUnloadModuleInternal", {id & 0xFFFF}));
    return static_cast<s32>(sail::module_call(ctx, "cellSysmodule", "cellSysmoduleUnloadModule", {id & 0xFFFF}));
}

u32 common_taskset(PPUContext& ctx, u32 rec)
{
    u32 ts = r32(ctx, rec + rc::kTaskset);
    if (!ts) {
        ts = alloc(ctx, rec, 0x80, 0x2900);
        if (!ts) return 0;
        StackVar attr(ctx, gb(ctx), 512);
        StackVar name(ctx, gb(ctx), 32);
        std::memcpy(gb(ctx) + name.addr(), "_libsail-common-taskset", 24);
        const u32 spurs = r32(ctx, rec + rc::kSpurs);
        sail::module_call(ctx, "cellSpurs", "_cellSpursTasksetAttributeInitialize",
            {attr.addr(), 1, 0x330000, 0, rec + rc::kTasksetPriorities, r32(ctx, rec + rc::kTasksetParam)});
        s32 r = static_cast<s32>(
            sail::module_call(ctx, "cellSpurs", "cellSpursTasksetAttributeSetName", {attr.addr(), name.addr()}));
        if (r >= 0)
            r = static_cast<s32>(
                sail::module_call(ctx, "cellSpurs", "cellSpursTasksetAttributeSetTasksetSize", {attr.addr(), 0x2900}));
        if (r >= 0)
            r = static_cast<s32>(sail::module_call(ctx, "cellSpurs", "cellSpursCreateTasksetWithAttribute",
                {spurs, ts, attr.addr()}));
        if (r < 0) {
            dealloc(ctx, rec, 0x80, ts);
            return 0;
        }
        w32(ctx, rec + rc::kTaskset, ts);
    }
    w32(ctx, rec + rc::kTasksetUsers, r32(ctx, rec + rc::kTasksetUsers) + 1);
    return ts;
}

void release_taskset(PPUContext& ctx, u32 rec)
{
    const s32 users = static_cast<s32>(r32(ctx, rec + rc::kTasksetUsers));
    const u32 ts = r32(ctx, rec + rc::kTaskset);
    if (users <= 0) return;
    w32(ctx, rec + rc::kTasksetUsers, static_cast<u32>(users - 1));
    if (users - 1 != 0 || !ts) return;
    if (static_cast<s32>(sail::module_call(ctx, "cellSpurs", "cellSpursShutdownTaskset", {ts})) >= 0)
        sail::module_call(ctx, "cellSpurs", "cellSpursJoinTaskset", {ts});
    dealloc(ctx, rec, 0x80, ts);
    w32(ctx, rec + rc::kTaskset, 0);
}

namespace {

// ------------------------------------------------------------------------------------------------- set-up

s8 coding(PPUContext& ctx, u32 format) { return static_cast<s8>(r8(ctx, format)); }

// libsail_rec's work area size for the conversion (0x200 for an SPU kind; the JPEG decoder's, rounded to 8, its module
// loaded for the question and unloaded again), *spu 1 for the SPU kinds; UNSUPPORTED_STREAM for another pair.
s32 work_size(PPUContext& ctx, u32 in, u32 out, bool* spu)
{
    const u8 cin = r8(ctx, in), cout = r8(ctx, out);
    const s32 unsupported = error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    if (cin > 12) return unsupported;
    const u64 bit = u64{1} << cin;
    const auto has = [](u32 mask, u8 c) { return (mask >> c) & 1; };
    if (bit & 0x614) {
        if (static_cast<u32>(static_cast<s32>(static_cast<s8>(cout))) > 1) return unsupported;
    } else if (bit & 0x800) {
        if (cout > 9 || !has(0x212, cout)) return unsupported;
        s32 r = load_module(ctx, kJpgModule);
        if (r < 0) return r;
        StackVar size(ctx, gb(ctx), 16);
        r = static_cast<s32>(firmware_call(ctx, "cellAvchatJpgDec", kJpgQuery, {size.addr()}));
        unload_module(ctx, kJpgModule);
        if (r < 0) return r;
        *spu = false;
        return static_cast<s32>((r32(ctx, size.addr()) + 7) & ~7u);
    } else if (bit & 0x1000) {
        if (cout > 10 || !has(0x617, cout)) return unsupported;
    } else {
        return unsupported;
    }
    *spu = true;
    return 0x200;
}

// Each kind's coefficients (libsail_rec's tables) copied into the work area as its SPU program reads them; `index`
// rows of 16 bytes. Without the module the area keeps zeros (the program's set-up is then refused at the task's
// creation: no program).
void table_row(PPUContext& ctx, u32 dst, u32 table, s32 offset)
{
    u8 row[16]{};
    if (!module_bytes(static_cast<u32>(static_cast<s32>(table) + offset), 16, row))
        PS_LOG_ERROR(kChan, "libsail_rec's converter table at {:#x}: the installed {} is needed", table, kModuleFile);
    std::memcpy(gb(ctx) + dst, row, 16);
}

// The message the SPU program sends the recorder when it is done: the SPURS queue, the code, the converter, 0.
void message(PPUContext& ctx, u32 at, u32 queue, u32 code, u32 conv)
{
    w32(ctx, at, queue);
    w32(ctx, at + 4, code);
    w32(ctx, at + 8, conv);
    w32(ctx, at + 12, 0);
}

// Kind 1 (raw Bayer to ARGB or RGBA).
void setup_bayer_to_rgb(PPUContext& ctx, u32 work, u32 msg, s32 w, s32 h, s32 pitch_in, s32 pitch_out, s32 sel, u8 fill)
{
    w32(ctx, work + 0x58, static_cast<u32>(w));
    w32(ctx, work + 0x5C, static_cast<u32>(h));
    w32(ctx, work + 0x60, static_cast<u32>(pitch_in));
    w32(ctx, work + 0x64, static_cast<u32>(pitch_out));
    std::memmove(gb(ctx) + work + 0x40, gb(ctx) + msg, 16);
    table_row(ctx, work, kTableBayerToRgb, sel * 0x10);
    table_row(ctx, work + 0x10, kTableBayerToRgb, sel * 0x10 + 0x20);
    std::memset(gb(ctx) + work + 0x20, fill, 0x20);
}

// Kind 2 (raw Bayer to a 4:2:2 coding).
void setup_bayer_to_422(PPUContext& ctx, u32 work, u32 msg, s32 w, s32 h, s32 pitch_in, s32 pitch_out, s32 sel)
{
    w32(ctx, work + 0x60, static_cast<u32>(pitch_in));
    w32(ctx, work + 0x74, static_cast<u32>(h) >> 1);
    w32(ctx, work + 0x5C, static_cast<u32>(h));
    w32(ctx, work + 0x6C, static_cast<u32>(h));
    w32(ctx, work + 0x58, static_cast<u32>(w));
    w32(ctx, work + 0x68, static_cast<u32>(w));
    w32(ctx, work + 0x64, static_cast<u32>(pitch_out));
    w32(ctx, work + 0x70, static_cast<u32>(w) >> 1);
    std::memmove(gb(ctx) + work + 0x40, gb(ctx) + msg, 16);
    table_row(ctx, work, kTableBayerTo422, sel * 0x10);
    table_row(ctx, work + 0x10, kTableBayerTo422, sel * 0x10 + 0x40);
    table_row(ctx, work + 0x20, kTableBayerTo422, sel * 0x10 + 0x80);
}

// Kind 0 (a 4:2:2 coding to ARGB or RGBA): the input's rows, the output's, the scan's.
void setup_422_to_rgb(PPUContext& ctx, u32 work, u32 msg, s32 w, s32 h, s32 pitch_in, s32 sel_in, s32 scan,
    s32 pitch_out, s32 sel_out, u8 fill)
{
    w32(ctx, work + 0x14C, static_cast<u32>(h));
    w32(ctx, work + 0x150, static_cast<u32>(pitch_in));
    w32(ctx, work + 0x148, static_cast<u32>(w));
    w32(ctx, work + 0x154, static_cast<u32>(pitch_out));
    std::memmove(gb(ctx) + work + 0x130, gb(ctx) + msg, 16);
    const u32 t = kTable422ToRgb;
    table_row(ctx, work, t, sel_in * 0x10);
    table_row(ctx, work + 0x10, t, sel_in * 0x10 + 0x40);
    table_row(ctx, work + 0x20, t, sel_in * 0x10 + 0x80);
    table_row(ctx, work + 0x30, t, ((sel_in + 2) % 4) * 0x10 + 0x80);
    table_row(ctx, work + 0x100, t, sel_out * 0x10 + 0xC0);
    std::memset(gb(ctx) + work + 0x110, fill, 0x20);
    const s32 s = scan * 0x10;
    table_row(ctx, work + 0x40, t, s + 0xE0);
    table_row(ctx, work + 0x50, t, s + 0x120);
    table_row(ctx, work + 0x60, t, s + 0x160);
    table_row(ctx, work + 0x70, t, s + 0x1A0);
    table_row(ctx, work + 0x80, t, s + 0x1E0);
    table_row(ctx, work + 0x90, t, s + 0x160);
    table_row(ctx, work + 0xA0, t, s + 0x220);
    table_row(ctx, work + 0xB0, t, s + 0x260);
    table_row(ctx, work + 0xC0, t, s + 0x160);
    table_row(ctx, work + 0xD0, t, s + 0x2A0);
    table_row(ctx, work + 0xE0, t, s + 0x2E0);
    table_row(ctx, work + 0xF0, t, scan * 0x20 + 0x320);
}

// libsail_rec's converter set-up (its fn at 0x14010): the size checked, the converter cleared, its lwmutex and lwconds,
// the conversion's kind from the codings, the kind's set-up, the fields. The callback, or libsail_rec's own (which
// does nothing) with the converter as its argument.
s32 converter_init(PPUContext& ctx, u32 conv, u32 rec, u32 work, u32 size, u32 capacity, u32 in, u32 out,
    u32 callback, u32 arg)
{
    bool spu = false;
    s32 r = work_size(ctx, in, out, &spu);
    if (r < 0) return r;
    if (static_cast<u32>(r) < size) return error(CELL_SAIL_ERROR_MEMORY);
    std::memset(gb(ctx) + conv, 0, 0x100);
    {
        StackVar attr(ctx, gb(ctx), 32);
        w32(ctx, attr.addr(), 2);
        w32(ctx, attr.addr() + 4, 0x20);
        std::memcpy(gb(ctx) + attr.addr() + 8, "_sailf0", 8);
        r = lw(ctx, "sys_lwmutex_create", {conv + cv::kLock, attr.addr()});
        if (r < 0) return r;
        StackVar cattr(ctx, gb(ctx), 16);
        std::memcpy(gb(ctx) + cattr.addr(), "_sailf0", 8);
        if (lw(ctx, "sys_lwcond_create", {conv + cv::kRoom, conv + cv::kLock, cattr.addr()}) < 0)
            lw(ctx, "sys_lwmutex_destroy", {conv + cv::kLock});
        std::memcpy(gb(ctx) + cattr.addr(), "_sailf1", 8);
        r = lw(ctx, "sys_lwcond_create", {conv + cv::kReady, conv + cv::kLock, cattr.addr()});
        if (r < 0) {
            lw(ctx, "sys_lwcond_destroy", {conv + cv::kRoom});
            lw(ctx, "sys_lwmutex_destroy", {conv + cv::kLock});
            return r;
        }
    }
    const u32 spurs = r32(ctx, rec + rc::kSpurs);
    w32(ctx, conv + cv::kCapacity, capacity);
    w32(ctx, conv + cv::kIn, 0xFFFFFFFF);
    const s8 cin = coding(ctx, in), cout = coding(ctx, out);
    const s32 w = static_cast<s16>(r16(ctx, in + 4));
    const u16 h = r16(ctx, in + 6);
    const s32 unsupported = error(sail::CELL_SAIL_ERROR_UNSUPPORTED_STREAM);
    if (static_cast<u8>(cin) > 12) return unsupported;
    const u64 bit = u64{1} << static_cast<u8>(cin);
    s32 pitch_in_default = 0, pitch_out_default = 0;
    u32 kind = 0, sub = 0;
    if (bit & 0x614) {
        pitch_in_default = w * 2;
        pitch_out_default = w * 4;
        if (static_cast<u32>(static_cast<s32>(cout)) > 1) return unsupported;
    } else if (bit & 0x800) {
        pitch_in_default = -1;
        kind = 3;
        if (cout == 4) {
            pitch_out_default = w * 2;
            sub = 1;
        } else if (cout == 9) {
            pitch_out_default = w * 2;
            sub = 2;
        } else if (cout == 1) {
            pitch_out_default = w * 4;
        } else {
            return unsupported;
        }
    } else if (bit & 0x1000) {
        if (static_cast<u8>(cout) > 10) return unsupported;
        pitch_in_default = w;
        pitch_out_default = w * 2;
        kind = 2;
        if (!((u64{1} << static_cast<u8>(cout)) & 0x614)) {
            pitch_out_default = w * 4;
            kind = 1;
            if (!((u64{1} << static_cast<u8>(cout)) & 3)) return unsupported;
        }
    } else {
        return unsupported;
    }
    u32 fill = r32(ctx, out + 0xC);
    if (fill > 0xFF) fill = 0xFF;
    s32 pitch_in = static_cast<s32>(r32(ctx, in + 8)), pitch_out = static_cast<s32>(r32(ctx, out + 8));
    if (pitch_in < 0) pitch_in = pitch_in_default;
    if (pitch_out < 0) pitch_out = pitch_out_default;
    const u32 queue = rec + rc::kSpursQueue;
    StackVar msg(ctx, gb(ctx), 16);
    if (kind == 1) {
        message(ctx, msg.addr(), queue, 0x35, conv);
        const s32 sel = cout == 1 ? 0 : cout == 0 ? 1 : -1;
        setup_bayer_to_rgb(ctx, work, msg.addr(), w, static_cast<s16>(h), pitch_in, pitch_out, sel,
            static_cast<u8>(fill));
        w32(ctx, conv + cv::kHandle, work);
    } else if (kind == 2) {
        message(ctx, msg.addr(), queue, 0x35, conv);
        const s32 sel = cout == 4 ? 0 : cout == 2 ? 1 : cout == 10 ? 2 : cout == 9 ? 3 : -1;
        setup_bayer_to_422(ctx, work, msg.addr(), w, static_cast<s16>(h), pitch_in, pitch_out, sel);
        w32(ctx, conv + cv::kHandle, work);
    } else if (kind == 3) {
        const u64 priorities = r64(ctx, rec + rc::kTasksetPriorities);
        const u32 contention = r32(ctx, rec + rc::kTasksetParam);
        r = load_module(ctx, kJpgModule);
        if (r >= 0) {
            w32(ctx, conv + cv::kModule, kJpgModule);
            StackVar cfg(ctx, gb(ctx), 32);
            std::memset(gb(ctx) + cfg.addr(), 0, 0x18);
            w32(ctx, cfg.addr() + 4, size);
            w32(ctx, cfg.addr() + 8, spurs);
            w32(ctx, cfg.addr(), work);
            w64(ctx, cfg.addr() + 12, priorities);
            w32(ctx, cfg.addr() + 20, contention);
            r = static_cast<s32>(firmware_call(ctx, "cellAvchatJpgDec", kJpgOpen, {cfg.addr(), conv + cv::kHandle}));
            if (r >= 0) {
                message(ctx, msg.addr(), queue, 0x33, conv);
                r = static_cast<s32>(
                    firmware_call(ctx, "cellAvchatJpgDec", kJpgQueue, {r32(ctx, conv + cv::kHandle), msg.addr()}));
                if (r < 0) firmware_call(ctx, "cellAvchatJpgDec", kJpgClose, {r32(ctx, conv + cv::kHandle)});
            }
        }
        if (r < 0) {
            lw(ctx, "sys_lwcond_destroy", {conv + cv::kReady});
            lw(ctx, "sys_lwcond_destroy", {conv + cv::kRoom});
            lw(ctx, "sys_lwmutex_destroy", {conv + cv::kLock});
            return r;
        }
    } else {
        message(ctx, msg.addr(), queue, 0x34, conv);
        const s32 sel_in = cin == 2 ? 1 : cin == 4 ? 0 : cin == 9 ? 3 : cin == 10 ? 2 : -1;
        const s32 sel_out = cout == 1 ? 0 : cout == 0 ? 1 : -1;
        // The scan: progressive (byte 0x10 of the input format 1, or past 1 a height above 719) or not, with its
        // field order (byte 0x12).
        s32 progressive = static_cast<s8>(r8(ctx, in + 0x10));
        if (progressive > 1) progressive = h > 719 ? 1 : 0;
        s32 order = static_cast<s8>(r8(ctx, in + 0x12));
        if (order > 1) order = 0;
        const s32 scan = progressive == 1 ? 3 - (order != 0 ? 1 : 0) : (order == 0 ? 1 : 0);
        setup_422_to_rgb(ctx, work, msg.addr(), w, static_cast<s16>(h), pitch_in, sel_in, scan, pitch_out, sel_out,
            static_cast<u8>(fill));
        w32(ctx, conv + cv::kHandle, work);
    }
    w32(ctx, conv + cv::kKind, kind);
    w32(ctx, conv + cv::kIn, static_cast<u32>(static_cast<s32>(cin)));
    w32(ctx, conv + cv::kOut, static_cast<u32>(static_cast<s32>(cout)));
    w32(ctx, conv + cv::kFill, fill);
    w32(ctx, conv + cv::kSub, sub);
    w32(ctx, conv + cv::kRecorder, rec);
    w32(ctx, conv + cv::kWork, work);
    if (!callback) {
        w32(ctx, conv + cv::kArg, conv);
        w32(ctx, conv + cv::kCallback,
            HleProcess::current()->link_function(kChan, recomp::nid_of("_pspocket_cellSailRec_ConverterNone"),
                "_pspocket_cellSailRec_ConverterNone"));
    } else {
        w32(ctx, conv + cv::kCallback, callback);
        w32(ctx, conv + cv::kArg, arg);
    }
    return 0;
}

// The converter's lwconds and lwmutex destroyed (libsail_rec's order).
void converter_sync_fini(PPUContext& ctx, u32 conv)
{
    lw(ctx, "sys_lwcond_destroy", {conv + cv::kReady});
    lw(ctx, "sys_lwcond_destroy", {conv + cv::kRoom});
    lw(ctx, "sys_lwmutex_destroy", {conv + cv::kLock});
}

// A converter's end: the JPEG decoder closed and its module unloaded (kind 3), the sync objects; its work area, and
// whether it held the common taskset.
void converter_fini(PPUContext& ctx, u32 conv, u32* work, bool* taskset)
{
    const u32 kind = r32(ctx, conv + cv::kKind), handle = r32(ctx, conv + cv::kHandle);
    bool held = true;
    if (kind == 3) {
        held = false;
        if ((!handle || static_cast<s32>(firmware_call(ctx, "cellAvchatJpgDec", kJpgClose, {handle})) >= 0) &&
            r32(ctx, conv + cv::kModule))
            unload_module(ctx, kJpgModule);
    }
    converter_sync_fini(ctx, conv);
    *work = r32(ctx, conv + cv::kWork);
    *taskset = held;
}

// An SPU kind's task: the source and destination kept in the work area, one task of the kind's program in the common
// taskset with the work area as its argument's first doubleword and `second` as its second (libsail_rec gives kind 2's
// program the work area twice, the others 0; the program sends the second doubleword's low word back in its message).
s32 start_task(PPUContext& ctx, u32 work, u32 taskset, u32 program_vaddr, u32 src_at, u32 dst_at, u32 id_at, u32 src,
    u32 dst, u32 second)
{
    w64(ctx, work + src_at, src);
    w64(ctx, work + dst_at, dst);
    const u32 program = spu_program(ctx, program_vaddr);
    if (!program) {
        log_call_site(ctx, gb(ctx), "cellSailRec: a conversion needs libsail_rec's SPU program");
        PS_FATAL(kChan, "libsail_rec's SPU program at {:#x} is needed for this conversion: install the system "
            "software (pspocket reads it from /dev_flash/sys/external/{})", program_vaddr, kModuleFile);
    }
    StackVar attr(ctx, gb(ctx), 0x110);
    StackVar argument(ctx, gb(ctx), 16);
    StackVar id(ctx, gb(ctx), 16);
    w64(ctx, argument.addr(), work);
    w64(ctx, argument.addr() + 8, second);
    s32 r = static_cast<s32>(sail::module_call(ctx, "cellSpurs", "_cellSpursTaskAttributeInitialize",
        {attr.addr(), 1, 0x330000, program, 0, argument.addr()}));
    if (r != 0) return r;
    r = static_cast<s32>(
        sail::module_call(ctx, "cellSpurs", "cellSpursCreateTaskWithAttribute", {taskset, id.addr(), attr.addr()}));
    if (r != 0) return r;
    w64(ctx, work + id_at, r32(ctx, id.addr()));
    return 0;
}

s32 conv_lock(PPUContext& ctx, u32 conv) { return lw(ctx, "sys_lwmutex_lock", {conv + cv::kLock, 0}); }
void conv_unlock(PPUContext& ctx, u32 conv) { lw(ctx, "sys_lwmutex_unlock", {conv + cv::kLock}); }

} // namespace

s32 converter_start_next(PPUContext& ctx, u32 conv)
{
    if (!r32(ctx, conv + cv::kQueued) || r32(ctx, conv + cv::kBusy)) return 0;
    const u32 i = r32(ctx, conv + cv::kStart), cap = r32(ctx, conv + cv::kCapacity);
    const u32 e = conv + cv::kRequests + 0x18 * i;
    const u32 a = r32(ctx, e), b = r32(ctx, e + 4), c = r32(ctx, e + 8);
    w32(ctx, conv + cv::kStart, cap ? (i + 1) % cap : 0);
    w32(ctx, conv + cv::kBusy, 1);
    const u32 kind = r32(ctx, conv + cv::kKind), work = r32(ctx, conv + cv::kHandle);
    const u32 taskset = r32(ctx, r32(ctx, conv + cv::kRecorder) + rc::kTaskset);
    switch (kind) {
    case 1: return start_task(ctx, work, taskset, kProgramBayerToRgb, 0x30, 0x38, 0x50, a, c, 0);
    case 0: return start_task(ctx, work, taskset, kProgram422ToRgb, 0x120, 0x128, 0x140, a, c, 0);
    case 2: return start_task(ctx, work, taskset, kProgramBayerTo422, 0x30, 0x38, 0x50, a, c, work);
    case 3: {
        StackVar req(ctx, gb(ctx), 32);
        const u32 q = req.addr();
        w32(ctx, q, a);
        w32(ctx, q + 4, b);
        w32(ctx, q + 8, 0);
        w32(ctx, q + 12, 0);
        w32(ctx, q + 16, r32(ctx, conv + cv::kSub));
        w32(ctx, q + 20, c);
        w32(ctx, q + 24, 1);
        w32(ctx, q + 28, 5);
        w8(ctx, q + 32, static_cast<u8>(r32(ctx, conv + cv::kFill)));
        w8(ctx, q + 33, 0);
        w8(ctx, q + 34, 0);
        w8(ctx, q + 35, 0);
        const s32 r = static_cast<s32>(firmware_call(ctx, "cellAvchatJpgDec", kJpgDecode, {work, q}));
        return r > 0 ? 0 : r;
    }
    default: return -1;
    }
}

void converter_done(PPUContext& ctx, u32 conv, u32 result)
{
    if (conv_lock(ctx, conv) < 0) return;
    const u32 i = r32(ctx, conv + cv::kDone), cap = r32(ctx, conv + cv::kCapacity);
    if (static_cast<s32>(r32(ctx, conv + cv::kIn)) == 11)
        result = static_cast<u32>(firmware_call(ctx, "cellAvchatJpgDec", kJpgResult, {r32(ctx, conv + cv::kHandle)}));
    const u32 at = conv + cv::kResultTable + 8 * i;
    w32(ctx, at + 4, result);
    w32(ctx, at, r32(ctx, conv + cv::kRequests + 0x18 * i + 0x10));
    w32(ctx, conv + cv::kBusy, 0);
    w32(ctx, conv + cv::kQueued, r32(ctx, conv + cv::kQueued) - 1);
    w32(ctx, conv + cv::kDone, cap ? (i + 1) % cap : 0);
    lw(ctx, "sys_lwcond_signal", {conv + cv::kRoom});
    w32(ctx, conv + cv::kResults, r32(ctx, conv + cv::kResults) + 1);
    lw(ctx, "sys_lwcond_signal", {conv + cv::kReady});
    converter_start_next(ctx, conv);
    conv_unlock(ctx, conv);
}

void converter_notified(PPUContext& ctx, u32 conv)
{
    call_guest(ctx, r32(ctx, conv + cv::kCallback), {r32(ctx, conv + cv::kArg)});
}

// CreateVideoConverter (message 3, {capacity, in, out, 0, callback, argument, converter*}): INVALID_STATE in state 0;
// the work area's size, the common taskset for an SPU kind, one block (work area + 0x100, boundary 0x80), the set-up;
// the converter first on the recorder's list and given to the game. Every failure answers MEMORY (a refused set-up
// frees the block from the converter's address and walks the list for nothing, as libsail_rec does).
void create_converter(PPUContext& ctx, u32 rec, u32 block, u32 record)
{
    lock(ctx, rec);
    if (!r32(ctx, rec + rc::kState)) return reply(ctx, rec, record, error(CELL_SAIL_ERROR_INVALID_STATE));
    const u32 capacity = r32(ctx, block), in = r32(ctx, block + 4), out = r32(ctx, block + 8);
    const u32 callback = r32(ctx, block + 16), arg = r32(ctx, block + 20);
    const s32 memory = error(CELL_SAIL_ERROR_MEMORY);
    bool spu = false;
    const s32 size = work_size(ctx, in, out, &spu);
    if (size < 0) return reply(ctx, rec, record, memory);
    if (spu && !common_taskset(ctx, rec)) return reply(ctx, rec, record, memory);
    // libsail_rec then walks the converters' list looking for none (its DestroyVideoConverter's walk with 0).
    const auto refuse = [&] {
        for (u32 p = r32(ctx, rec + rc::kConverters); p; p = r32(ctx, p + cv::kNext)) {
        }
        reply(ctx, rec, record, memory);
    };
    const u32 work = alloc(ctx, rec, 0x80, static_cast<u32>(size) + 0x100);
    if (!work) return refuse();
    const u32 conv = work + static_cast<u32>(size);
    if (converter_init(ctx, conv, rec, work, static_cast<u32>(size), capacity, in, out, callback, arg) < 0) {
        dealloc(ctx, rec, 0x80, conv);
        return refuse();
    }
    w32(ctx, conv + cv::kNext, r32(ctx, rec + rc::kConverters));
    w32(ctx, rec + rc::kConverters, conv);
    w32(ctx, r32(ctx, block + 24), conv);
    reply(ctx, rec, record, 0);
}

// DestroyVideoConverter (message 4): taken off the list (0 also for one not there), its end, its block freed (from the
// work area), the common taskset released when it held it.
void destroy_converter(PPUContext& ctx, u32 rec, u32 conv, u32 record)
{
    lock(ctx, rec);
    for (u32 link = rec + rc::kConverters, cur; (cur = r32(ctx, link)) != 0; link = cur) {
        if (cur != conv) continue;
        w32(ctx, link, r32(ctx, conv + cv::kNext));
        u32 work = 0;
        bool held = false;
        converter_fini(ctx, conv, &work, &held);
        dealloc(ctx, rec, 0x80, work);
        if (held) release_taskset(ctx, rec);
        break;
    }
    reply(ctx, rec, record, 0);
}

void destroy_converters(PPUContext& ctx, u32 rec)
{
    for (u32 conv = r32(ctx, rec + rc::kConverters); conv;) {
        const u32 next = r32(ctx, conv + cv::kNext);
        u32 work = 0;
        bool held = false;
        converter_fini(ctx, conv, &work, &held);
        dealloc(ctx, rec, 0x80, work);
        if (held) release_taskset(ctx, rec);
        conv = next;
    }
}

namespace {

// libsail_rec's own converter callback: nothing (r3 left as it came).
u64 _pspocket_cellSailRec_ConverterNone(PPUContext& ctx) { return ctx.gpr[3]; }

} // namespace

void register_convert(Registry& r)
{
    r.add(kChan, {
        PS_HLE_INTERNAL(recomp::nid_of("_pspocket_cellSailRec_ConverterNone"), _pspocket_cellSailRec_ConverterNone),
    });
    r.add_imports(kChan, kImports);
}

} // namespace pspocket::hle::sailrec
