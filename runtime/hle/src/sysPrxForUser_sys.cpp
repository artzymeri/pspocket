// sysPrxForUser's functions that are lv2 syscalls with little or no work of liblv2's own around them (D-501): the
// arcade board (sys_game_*), the RSX audio service (sys_rsxaudio_*), the Wi-Fi chip's command channel, process spawning,
// the console's input, the timebase and the end of an interrupt thread. Each does what liblv2.sprx's code does (read
// from the firmware's module, 4.92; RPCS3 has none of them): the same syscall with the same arguments, the same checks
// and copies around it, so the answer is whatever pspocket's lv2 gives, its own implementation or the logged gap of a
// syscall it lacks (CLAUDE.md rule 4).

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/ppu/runtime.h"

namespace pspocket::hle {

// sysPrxForUser_libc.cpp: liblv2's _sys_malloc and _sys_free (the spawn block), _sys_panic's end.
u32 lv2_malloc(PPUContext& ctx, u32 size);
void lv2_free(PPUContext& ctx, u32 addr);
[[noreturn]] void lv2_panic(PPUContext& ctx, std::string_view text);

namespace {

using lv2::err;

// liblv2's `li r11, number; sc` with the caller's r3..r10 as they are: r3 back.
u64 pass_syscall(PPUContext& ctx, u32 number)
{
    ctx.gpr[11] = number;
    thread_of(ctx).kernel.syscall(ctx);
    return ctx.gpr[3];
}

// The same with the arguments liblv2 puts in r3.. first.
u64 make_syscall(PPUContext& ctx, u32 number, std::initializer_list<u64> args)
{
    u32 i = 0;
    for (const u64 a : args) ctx.gpr[3 + i++] = a;
    return pass_syscall(ctx, number);
}

// liblv2 returns most syscall results through `extsw r3, r3`.
s32 word(u64 r3) { return static_cast<s32>(static_cast<u32>(r3)); }

void refuse(PPUContext& ctx, std::string_view why) { log_call_site(ctx, guest_base(ctx), why); }

bool guest_bytes(lv2::PpuThread& t, u32 addr, u32 size)
{
    for (u32 i = 0; i < size; i += 0x1000)
        if (!guest_readable(t, addr + i, std::min<u32>(size - i, 0x1000))) return false;
    return true;
}

// ------------------------------------------------------------------------------------------------- the arcade board

s32 sys_game_watchdog_start(PPUContext& ctx) { return word(pass_syscall(ctx, 372)); }
s32 sys_game_watchdog_stop(PPUContext& ctx) { return word(pass_syscall(ctx, 373)); }
s32 sys_game_watchdog_clear(PPUContext& ctx) { return word(pass_syscall(ctx, 374)); }
u64 sys_game_get_system_sw_version(PPUContext& ctx) { return pass_syscall(ctx, 376); } // r3 as the syscall left it
s32 sys_game_get_rtc_status(PPUContext& ctx) { return word(pass_syscall(ctx, 412)); }

// The 16 bytes of board storage through a buffer of liblv2's own on its stack, with a status byte the syscall sets:
// CELL_ENOSYS when the board says the storage is not there.
s32 sys_game_board_storage_read(PPUContext& ctx, u32 buffer)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    StackVar local(ctx, base, 0x20); // the status byte, then the 16 bytes at +8
    const u32 status = local.addr(), data = local.addr() + 8;
    if (const s32 r = word(make_syscall(ctx, 410, {data, status}))) return r;
    if (ps_read8(base, status)) return err(lv2::CELL_ENOSYS);
    if (!guest_bytes(t, buffer, 16)) {
        refuse(ctx, std::format("sys_game_board_storage_read({:#x}): not guest memory (liblv2 faults); CELL_EFAULT",
            buffer));
        return err(lv2::CELL_EFAULT);
    }
    std::memcpy(base + buffer, base + data, 16);
    return lv2::CELL_OK;
}

s32 sys_game_board_storage_write(PPUContext& ctx, u32 buffer)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (!guest_bytes(t, buffer, 16)) {
        refuse(ctx, std::format("sys_game_board_storage_write({:#x}): not guest memory (liblv2 faults); CELL_EFAULT",
            buffer));
        return err(lv2::CELL_EFAULT);
    }
    StackVar local(ctx, base, 0x20);
    const u32 status = local.addr(), data = local.addr() + 8;
    std::memcpy(base + data, base + buffer, 16);
    const s32 r = word(make_syscall(ctx, 411, {data, status}));
    if (r == lv2::CELL_OK && ps_read8(base, status)) return err(lv2::CELL_ENOSYS);
    return r;
}

// The thermal zone word from sys_sm_get_tzpb must have its two top bits set, else liblv2 panics; then the two
// temperatures (zones 0 and 1) as u16s at temp and temp + 2.
s32 sys_game_get_temperature(PPUContext& ctx, u32 temp)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    StackVar tzpb(ctx, base, 8);
    if (const s32 r = word(make_syscall(ctx, 384, {tzpb.addr()}))) {
        if (static_cast<u32>(r) == 0x8001000D) lv2_panic(ctx, std::format("sys_sm_get_tzpb returned {:x}\n", r));
        return r;
    }
    const u64 v = ps_read64(base, tzpb.addr());
    if ((v >> 62) != 3) lv2_panic(ctx, std::format("unexpected TZPB {:x}", v));
    if (const s32 r = word(make_syscall(ctx, 383, {0, temp}))) return r;
    return word(make_syscall(ctx, 383, {1, u64{temp} + 2}));
}

// ------------------------------------------------------------------------------------------------- RSX audio, Wi-Fi

s32 sys_rsxaudio_initialize(PPUContext& ctx) { return word(pass_syscall(ctx, 650)); }
s32 sys_rsxaudio_finalize(PPUContext& ctx) { return word(pass_syscall(ctx, 651)); }
s32 sys_rsxaudio_import_shared_memory(PPUContext& ctx) { return word(pass_syscall(ctx, 652)); }
s32 sys_rsxaudio_unimport_shared_memory(PPUContext& ctx) { return word(pass_syscall(ctx, 653)); }
s32 sys_rsxaudio_create_connection(PPUContext& ctx) { return word(pass_syscall(ctx, 654)); }
s32 sys_rsxaudio_close_connection(PPUContext& ctx) { return word(pass_syscall(ctx, 655)); }
s32 sys_rsxaudio_prepare_process(PPUContext& ctx) { return word(pass_syscall(ctx, 656)); }
s32 sys_rsxaudio_start_process(PPUContext& ctx) { return word(pass_syscall(ctx, 657)); }
s32 sys_rsxaudio_stop_process(PPUContext& ctx) { return word(pass_syscall(ctx, 658)); }
s32 _sys_net_eurus_post_command(PPUContext& ctx) { return word(pass_syscall(ctx, 726)); }

// ------------------------------------------------------------------------------------------------- the console, time

// sys_tty_read of one byte from channel 0 into liblv2's stack: that byte, sign-extended, whether or not the read gave
// one. liblv2 reads it from below its stack pointer, so with nothing read it returns what was there; 0 here.
s32 console_getc(PPUContext& ctx)
{
    u8* base = guest_base(ctx);
    StackVar local(ctx, base, 16); // the byte, then the length read at +4
    make_syscall(ctx, 402, {0, local.addr(), 1, local.addr() + 4});
    return static_cast<s8>(ps_read8(base, local.addr()));
}

// The timebase's low word, read again while it is 0.
u32 __sys_time(PPUContext&)
{
    u64 tb = 0;
    do tb = ps_ppu_timebase();
    while (static_cast<u32>(tb) == 0);
    return static_cast<u32>(tb);
}

// ------------------------------------------------------------------------------------------------- processes

// What liblv2 answers without asking lv2.
s32 sys_process_spawn_with_memory_budget(PPUContext&) { return err(lv2::CELL_ENOSYS); }
s32 sys_process_wait_for_game_process(PPUContext&) { return err(lv2::CELL_ENOSYS); }
s32 _sys_process_wait_for_game_process(PPUContext&) { return err(lv2::CELL_ENOSYS); }

constexpr u32 round16(u64 n) { return static_cast<u32>((n + 15) & ~u64{15}); }

// A guest list of string pointers up to its NULL: the strings, or nullopt when the list or a string is not guest memory.
std::optional<std::vector<std::string>> read_list(lv2::PpuThread& t, u32 list)
{
    std::vector<std::string> out;
    for (u32 i = 0; list; i++) {
        if (!guest_readable(t, list + 4 * i, 4)) return std::nullopt;
        const u32 s = ps_read32(t.kernel.base(), list + 4 * i);
        if (!s) break;
        const GuestString text = read_cstr(t, s, 0x100000);
        if (!text.terminated) return std::nullopt;
        out.push_back(text.text);
    }
    return out;
}

// liblv2's _proc_spawn: the path and the argv and envp strings in one block from _sys_malloc for _sys_process_spawn
// (syscall 21), freed after it. The block: 64-bit pointers {path, argv..., NULL, envp..., NULL}, 8 bytes more when
// their number is odd, then the strings, each from a 16-byte boundary. ENOMEM when the block is not had.
s32 _proc_spawn(PPUContext& ctx, u32 a1, u32 path, u32 argv, u32 envp, s32 prio, u64 flags, u64 a7, u32 a8)
{
    lv2::PpuThread& t = thread_of(ctx);
    const GuestString p = read_cstr(t, path, 0x100000);
    const auto args = read_list(t, argv), env = read_list(t, envp);
    if (!p.terminated || !args || !env) {
        refuse(ctx, std::format("_proc_spawn({:#x}, {:#x}, {:#x}): a string is not guest memory (liblv2 faults); "
                                "CELL_EFAULT", path, argv, envp));
        return err(lv2::CELL_EFAULT);
    }
    const u64 count = 1 + args->size() + 1 + env->size() + 1; // the pointers and the two NULLs
    u64 strings = round16(p.text.size() + 1);
    for (const auto* list : {&*args, &*env})
        for (const std::string& s : *list) strings += round16(s.size() + 1);
    const u64 pointers = 8 * count + ((args->size() + env->size() + 1) % 2 ? 8 : 0);
    const u64 size = pointers + strings;
    const u32 block = size <= 0xFFFFFFFF ? lv2_malloc(ctx, static_cast<u32>(size)) : 0;
    if (!block) return err(lv2::CELL_ENOMEM);
    u8* base = t.kernel.base();
    u32 slot = block, text = static_cast<u32>(block + pointers);
    const auto put = [&](const std::string& s) {
        std::memcpy(base + text, s.c_str(), s.size() + 1);
        ps_write64(base, slot, text);
        slot += 8;
        text += round16(s.size() + 1);
    };
    put(p.text);
    for (const std::string& s : *args) put(s);
    ps_write64(base, slot, 0);
    slot += 8;
    for (const std::string& s : *env) put(s);
    ps_write64(base, slot, 0);
    const s32 r = word(make_syscall(ctx, 21,
        {a1, static_cast<u64>(static_cast<s64>(prio)), flags, block, static_cast<u32>(size), a7, a8}));
    lv2_free(ctx, block);
    return r;
}

// CELL_EINVAL for flags in 0x000FF000, else _proc_spawn with the last two arguments 0.
s32 sys_process_spawn(PPUContext& ctx, u32 a1, u32 path, u32 argv, u32 envp, s32 prio, u64 flags)
{
    if (flags & 0x000FF000) return err(lv2::CELL_EINVAL);
    return _proc_spawn(ctx, a1, path, argv, envp, prio, flags, 0, 0);
}

// ------------------------------------------------------------------------------------------------- interrupt threads

// _sys_interrupt_thread_disestablish (syscall 89) gives the thread's r13 back; liblv2 then frees its TLS block, one
// teardown late: the block of the previous disestablished thread is freed and this one's is kept until the next, as the
// thread may still be running on its way out.
struct InterruptTlsState {
    std::atomic<u32> pending{0};
};

s32 sys_interrupt_thread_disestablish(PPUContext& ctx, u32 ih)
{
    u8* base = guest_base(ctx);
    StackVar r13(ctx, base, 8);
    const s32 r = word(make_syscall(ctx, 89, {ih, r13.addr()}));
    if (r != lv2::CELL_OK) return r;
    HleProcess* p = HleProcess::current();
    const u32 tls = static_cast<u32>(ps_read64(base, r13.addr()) - kTlsR13Offset);
    if (const u32 old = p->state<InterruptTlsState>().pending.exchange(tls)) p->free_tls(old);
    return lv2::CELL_OK;
}

// liblv2's variable sys_prx_version: 0x00010001 in the 4.92 module (RPCS3 registers it with no value).
void init_sys_prx_version(u8* base, u32 addr) { ps_write32(base, addr, 0x00010001); }

} // namespace

void register_sysPrxForUser_sys(Registry& r)
{
    r.add_variables("sysPrxForUser", {PS_HLE_VAR(0x7DF066CF, sys_prx_version, 4, 4, &init_sys_prx_version)});
    r.add("sysPrxForUser", {
        PS_HLE_FUNC(0x9E0623B5, sys_game_watchdog_start),
        PS_HLE_FUNC(0x6E05231D, sys_game_watchdog_stop),
        PS_HLE_FUNC(0xACAD8FB6, sys_game_watchdog_clear),
        PS_HLE_FUNC(0x620E35A7, sys_game_get_system_sw_version),
        PS_HLE_FUNC(0x9F950780, sys_game_get_rtc_status),
        PS_HLE_FUNC(0xE76964F5, sys_game_board_storage_read),
        PS_HLE_FUNC(0x8BB03AB8, sys_game_board_storage_write),
        PS_HLE_FUNC(0x3172759D, sys_game_get_temperature),
        PS_HLE_FUNC(0xC839BB4C, sys_rsxaudio_initialize),
        PS_HLE_FUNC(0x87C87495, sys_rsxaudio_finalize),
        PS_HLE_FUNC(0x2F256B29, sys_rsxaudio_import_shared_memory),
        PS_HLE_FUNC(0x39E9B068, sys_rsxaudio_unimport_shared_memory),
        PS_HLE_FUNC(0xA9879A33, sys_rsxaudio_create_connection),
        PS_HLE_FUNC(0xB1BD7A61, sys_rsxaudio_close_connection),
        PS_HLE_FUNC(0x7B034C78, sys_rsxaudio_prepare_process),
        PS_HLE_FUNC(0x9C9605EE, sys_rsxaudio_start_process),
        PS_HLE_FUNC(0xFF34C76C, sys_rsxaudio_stop_process),
        PS_HLE_FUNC(0x4BBF59D0, _sys_net_eurus_post_command),
        PS_HLE_FUNC(0x8A2F159B, console_getc),
        PS_HLE_FUNC(0xFECC05B6, __sys_time),
        PS_HLE_FUNC(0xBF8EE5BB, sys_process_spawn_with_memory_budget),
        PS_HLE_FUNC(0xE95FFA0A, sys_process_wait_for_game_process),
        PS_HLE_FUNC(0xBAB62B99, _sys_process_wait_for_game_process),
        PS_HLE_FUNC(0x637BDAAE, _proc_spawn),
        PS_HLE_FUNC(0x9D2EC4FF, sys_process_spawn),
        PS_HLE_FUNC(0x4A071D98, sys_interrupt_thread_disestablish),
    });
}

} // namespace pspocket::hle
