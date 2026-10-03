// sys_process and sys_tty.
// References: RPCS3 rpcs3/Emu/Cell/lv2/sys_process.h/.cpp, sys_tty.cpp.
//
// Deviations:
// - sys_tty_read returns CELL_EIO (what RPCS3 returns outside debug-console mode; there is no TTY input here).
// - sys_tty_write uses debug-console error checks (EINVAL for a bad channel, EFAULT for a bad buffer); RPCS3 in
//   retail mode silently drops such writes.
// - _sys_process_exit2 (exitspawn) ignores the program to launch next and just exits.

#include <cstring>

#include "internal.hpp"

namespace pspocket::lv2 {

namespace {

constexpr std::string_view kTty = "tty";

// sys_process.h process-local object types (the top byte of their IDs).
enum : u32 {
    SYS_MEM_OBJECT = 0x08,
    SYS_MUTEX_OBJECT = 0x85,
    SYS_COND_OBJECT = 0x86,
    SYS_RWLOCK_OBJECT = 0x88,
    SYS_INTR_TAG_OBJECT = 0x0A,
    SYS_INTR_SERVICE_HANDLE_OBJECT = 0x0B,
    SYS_EVENT_QUEUE_OBJECT = 0x8D,
    SYS_EVENT_PORT_OBJECT = 0x0E,
    SYS_TRACE_OBJECT = 0x21,
    SYS_SPUIMAGE_OBJECT = 0x22,
    SYS_PRX_OBJECT = 0x23,
    SYS_SPUPORT_OBJECT = 0x24,
    SYS_OVERLAY_OBJECT = 0x25,
    SYS_LWMUTEX_OBJECT = 0x95,
    SYS_TIMER_OBJECT = 0x11,
    SYS_SEMAPHORE_OBJECT = 0x96,
    SYS_FS_FD_OBJECT = 0x73,
    SYS_LWCOND_OBJECT = 0x97,
    SYS_EVENT_FLAG_OBJECT = 0x98,
    SYS_RSXAUDIO_OBJECT = 0x60,
};

[[noreturn]] void exit_process(PpuThread& t, s32 status)
{
    t.kernel.impl().process_exit(status);
    thread_finish(t, 0);
    throw ThreadExit{};
}

} // namespace

s32 sys_process_getpid(PpuThread&) { return 1; } // RPCS3 process_getpid

s32 sys_process_getppid(PpuThread&)
{
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) PS_LOG_WARN(kChan, "sys_process_getppid: stub, returns 0 (as RPCS3)");
    return 0;
}

Result sys_process_get_number_of_object(PpuThread& t, u32 object, u32 nump)
{
    Kernel::Impl& s = t.kernel.impl();
    u32 count = 0;
    switch (object) {
    case SYS_MEM_OBJECT:
    case SYS_MUTEX_OBJECT:
    case SYS_COND_OBJECT:
    case SYS_RWLOCK_OBJECT:
    case SYS_EVENT_QUEUE_OBJECT:
    case SYS_EVENT_PORT_OBJECT:
    case SYS_LWMUTEX_OBJECT:
    case SYS_TIMER_OBJECT:
    case SYS_SEMAPHORE_OBJECT:
    case SYS_LWCOND_OBJECT:
    case SYS_EVENT_FLAG_OBJECT: count = s.objects.count(object << 24); break;
    case SYS_FS_FD_OBJECT: count = static_cast<u32>(s.fds.all().size()); break;
    case SYS_INTR_TAG_OBJECT:
    case SYS_INTR_SERVICE_HANDLE_OBJECT:
    case SYS_TRACE_OBJECT:
    case SYS_SPUIMAGE_OBJECT:
    case SYS_PRX_OBJECT:
    case SYS_SPUPORT_OBJECT:
    case SYS_OVERLAY_OBJECT:
    case SYS_RSXAUDIO_OBJECT: count = 0; break; // object types this kernel doesn't create (yet)
    default: return err(CELL_EINVAL);
    }
    if (!s.guest.put32(nump, count)) return err(CELL_EFAULT);
    return CELL_OK;
}

Result sys_process_get_sdk_version(PpuThread& t, u32, u32 version)
{
    if (!t.kernel.impl().guest.put32(version, t.kernel.config().sdk_version)) return err(CELL_EFAULT);
    return CELL_OK;
}

Result _sys_process_get_paramsfo(PpuThread& t, u32 buffer)
{
    Guest& g = t.kernel.impl().guest;
    const std::string& title = t.kernel.config().title_id;
    if (title.empty()) return err(CELL_ENOENT);
    if (!g.valid(buffer, 0x40)) return err(CELL_EFAULT);
    std::memset(g.ptr(buffer), 0, 0x40);
    std::memcpy(g.ptr(buffer + 1), title.data(), std::min<usize>(title.size(), 9)); // RPCS3: title ID at +1
    return CELL_OK;
}

void sys_process_exit3(PpuThread& t, s32 status) { exit_process(t, status); }

void _sys_process_exit(PpuThread& t, s32 status, u32, u32) { exit_process(t, status); }

void _sys_process_exit2(PpuThread& t, s32 status, u32 arg, u32 arg_size, u32)
{
    PS_LOG_WARN(kChan, "_sys_process_exit2(status {}, arg {:#x}, size {:#x}): the next program is not launched", status,
        arg, arg_size);
    exit_process(t, status);
}

Result sys_tty_read(PpuThread&, s32, u32, u32, u32) { return err(CELL_EIO); }

Result sys_tty_write(PpuThread& t, s32 ch, u32 buf, u32 len, u32 pwritelen)
{
    Kernel::Impl& s = t.kernel.impl();
    if (ch < 0 || ch > 15) return err(CELL_EINVAL);
    if (static_cast<s32>(len) > 0) {
        if (!s.guest.valid(buf, len)) return err(CELL_EFAULT);
        std::string msg(reinterpret_cast<const char*>(s.guest.ptr(buf)), len);
        while (!msg.empty() && msg.back() == '\n') msg.pop_back();
        std::lock_guard lock(s.tty_mutex); // keep one write's lines together
        usize start = 0;
        for (;;) {
            const usize nl = msg.find('\n', start);
            const std::string_view line = std::string_view(msg).substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (ch == 0) {
                PS_LOG_INFO(kTty, "{}", line);
            } else {
                PS_LOG_INFO(kTty, "[{}] {}", ch, line);
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    if (!s.guest.put32(pwritelen, static_cast<s32>(len) > 0 ? len : 0)) return err(CELL_EFAULT);
    return CELL_OK;
}

void register_process_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 1, sys_process_getpid);
    PS_LV2_BIND(table, 3, sys_process_exit3);
    PS_LV2_BIND(table, 12, sys_process_get_number_of_object);
    PS_LV2_BIND(table, 18, sys_process_getppid);
    PS_LV2_BIND(table, 22, _sys_process_exit);
    PS_LV2_BIND(table, 25, sys_process_get_sdk_version);
    PS_LV2_BIND(table, 26, _sys_process_exit2);
    PS_LV2_BIND(table, 30, _sys_process_get_paramsfo);
    PS_LV2_BIND(table, 402, sys_tty_read);
    PS_LV2_BIND(table, 403, sys_tty_write);
}

} // namespace pspocket::lv2
