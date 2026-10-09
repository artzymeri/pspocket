// cellUsbd (libusbd), the USB host library over lv2's sys_usbd, and cellUsbPspcm (libusbpspcm), the PSP link over it:
// pspocket's own (D-522), compared with Sony's libusbd.sprx and libusbpspcm.sprx running on pspocket's lv2 in
// tests/hle/test_usb_compare.cpp. RPCS3's cellUsbd.cpp and cellUsbpspcm.cpp have only todo stubs (CELL_OK); the
// behaviour here is the firmware modules', read from their code. cellUsbPspcm is at the end of the file.
//
// cellUsbdInit makes the library's lv2 objects (an event queue "_ubdorg" with a local port connected to it) and its two
// threads, priority 500 with 4 KB stacks: libusbd_callback_thread waits in sys_usbd_receive_event, passes device
// arrivals and departures (events 1 and 2) to the queue and transfer completions (3) to their callbacks;
// libusbd_usbd_thread takes the arrivals and departures from the queue. Event 4 ends either (cellUsbdEnd sends it to
// both and joins them). Every call but cellUsbdInit answers CELL_USBD_ERROR_NOT_INITIALIZED before it
// (cellUsbdGetPrivateData and cellUsbdScanStaticDescriptor a null pointer).
//
// Logical device drivers (CellUsbdLddOps {name, probe, attach, detach}) are kept in registration order: a name already
// registered is CELL_USBD_ERROR_LDD_ALREADY_REGISTERED, compared as the names read now (libusbd keeps the game's
// pointers); unregistering looks for a driver whose four pointers are the ones in `ops` now. A failure from lv2 is
// CELL_USBD_ERROR_FATAL.
//
// pspocket's lv2 has no USB device (runtime/lv2/src/usbd.cpp): the device list is empty, so device ids and pipe ids are
// never found (CELL_USBD_ERROR_DEVICE_NOT_FOUND, _PIPE_NOT_FOUND) and no driver is probed. lv2 never sends arrivals,
// departures or transfer completions; one would stop a dev build here (rule 5) until a device model lands, compared
// with libusbd on it. Left out: libusbd's seven exports with no known name (a Bluetooth helper among them), and its
// answer for ten titles (lv2 refusing a product registration taken as CELL_OK), which pspocket's lv2 never does for a
// name libusbd lets through.

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellUsbd";
constexpr std::string_view kModule = "cellUsbd";

constexpr s32 CELL_USBD_ERROR_NOT_INITIALIZED = static_cast<s32>(0x80110001);
constexpr s32 CELL_USBD_ERROR_ALREADY_INITIALIZED = static_cast<s32>(0x80110002);
constexpr s32 CELL_USBD_ERROR_INVALID_PARAM = static_cast<s32>(0x80110004);
constexpr s32 CELL_USBD_ERROR_LDD_ALREADY_REGISTERED = static_cast<s32>(0x80110006);
constexpr s32 CELL_USBD_ERROR_LDD_NOT_FOUND = static_cast<s32>(0x80110009);
constexpr s32 CELL_USBD_ERROR_DEVICE_NOT_FOUND = static_cast<s32>(0x8011000A);
constexpr s32 CELL_USBD_ERROR_PIPE_NOT_FOUND = static_cast<s32>(0x8011000D);
constexpr s32 CELL_USBD_ERROR_FATAL = static_cast<s32>(0x801100FF);

constexpr u32 NID_usbd_thread = 0x80C87966;     // nid_of("_pspocket_usbd_thread"): pspocket's own
constexpr u32 NID_callback_thread = 0x127CCC85; // nid_of("_pspocket_usbd_callback_thread"): pspocket's own

constexpr u64 kEventAttach = 1, kEventDetach = 2, kEventTransfer = 3, kEventTerminate = 4;
constexpr s32 kDefaultPriority = 500;
constexpr u32 kThreadStack = 0x1000;

enum class LddKind { Plain, Extra, Composite };

// A registered driver: the game's CellUsbdLddOps as they were then (libusbd copies the four pointers), lv2's id.
struct Ldd {
    std::array<u32, 4> ops{}; // name, probe, attach, detach
    s32 id = 0;
    LddKind kind = LddKind::Plain;
};

struct UsbdState {
    std::recursive_mutex lock; // libusbd's lightweight mutexes (_ubdorg, _ubdres, _ubdprb)
    bool initialized = false;
    u32 handle = 0;
    u32 queue = 0, port = 0;
    u64 usbd_thread = 0, callback_thread = 0;
    u32 usbd_tls = 0, callback_tls = 0;
    s32 ldd_priority = kDefaultPriority; // cellUsbdGetThreadPriority(0): kept, no thread runs at it
    std::vector<Ldd> ldds;
};

UsbdState& usbd() { return HleProcess::current()->state<UsbdState>(); }

s32 fatal_unless_ok(u64 r) { return static_cast<s32>(r) == lv2::CELL_OK ? lv2::CELL_OK : CELL_USBD_ERROR_FATAL; }

// The four pointers of a CellUsbdLddOps, all set (libusbd's check before every registration and unregistration).
bool read_ops(u8* base, u32 ops, std::array<u32, 4>& out)
{
    if (!ops) return false;
    for (u32 i = 0; i < 4; i++) {
        out[i] = ps_read32(base, ops + 4 * i);
        if (!out[i]) return false;
    }
    return true;
}

std::string guest_cstr(u8* base, u32 addr) { return reinterpret_cast<const char*>(base + addr); }

// A driver of that name is registered (the names compared as they read now).
bool name_registered(u8* base, const UsbdState& u, u32 name)
{
    const std::string wanted = guest_cstr(base, name);
    return std::any_of(u.ldds.begin(), u.ldds.end(), [&](const Ldd& l) { return guest_cstr(base, l.ops[0]) == wanted; });
}

// ------------------------------------------------------------------------------------------------- threads

// libusbd_callback_thread (argument: lv2's handle << 32 | the queue's port): lv2's USB events. Arrivals and departures
// go to the queue for libusbd_usbd_thread; 4 ends.
void _pspocket_usbd_callback_thread(PPUContext& ctx, u64 arg)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 handle = static_cast<u32>(arg >> 32), port = static_cast<u32>(arg);
    for (;;) {
        StackVar ev(ctx, base, 24);
        if (lv2_syscall(ctx, 540, {handle, ev.addr(), ev.addr() + 8, ev.addr() + 16}) != lv2::CELL_OK) continue;
        const u64 a1 = ps_read64(base, ev.addr()), a2 = ps_read64(base, ev.addr() + 8);
        const u64 a3 = ps_read64(base, ev.addr() + 16);
        if (a1 == kEventTerminate) break;
        if (a1 == kEventAttach || a1 == kEventDetach) {
            lv2_syscall(ctx, 138, {port, a1, a2, a3});
        } else if (a1 == kEventTransfer) {
            PS_FATAL(kChan, "a transfer completion for {:#x} from lv2, which has no USB device (D-522)", a2);
        }
    }
    lv2::_sys_ppu_thread_exit(t, 0);
}

// libusbd_usbd_thread (argument: the queue): device arrivals and departures from the library's queue; 4 ends.
void _pspocket_usbd_thread(PPUContext& ctx, u64 queue)
{
    lv2::PpuThread& t = thread_of(ctx);
    for (;;) {
        if (lv2::sys_event_queue_receive(t, static_cast<u32>(queue), 0, 0) != lv2::CELL_OK) continue;
        const u64 kind = ctx.gpr[5], device = ctx.gpr[6];
        if (kind == kEventTerminate) break;
        if (kind == kEventAttach || kind == kEventDetach) {
            PS_FATAL(kChan, "USB device {:#x} {} from lv2, which has no USB device (D-522)", device,
                kind == kEventAttach ? "arrived" : "left");
        }
    }
    lv2::_sys_ppu_thread_exit(t, 0);
}

// sysPrxForUser's sys_ppu_thread_create as libusbd calls it: joinable, priority 500, a 4 KB stack, started.
s32 start_thread(PPUContext& ctx, u32 nid, std::string_view name, u64 arg, u64& id, u32& tls)
{
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    HleProcess& p = *HleProcess::current();
    const u32 entry = p.descriptor_of(kModule, nid);
    if (!entry) PS_FATAL(kChan, "libusbd's thread function {} was not linked at load", name);
    tls = p.alloc_tls();
    StackVar v(ctx, base, 0x40); // thread ID (u64), ppu_thread_param_t {entry, tls}, the name
    ps_write32(base, v.addr() + 8, entry);
    ps_write32(base, v.addr() + 12, tls ? tls + kTlsR13Offset : 0);
    std::memcpy(base + v.addr() + 0x10, name.data(), name.size());
    s32 r = lv2::_sys_ppu_thread_create(t, v.addr(), v.addr() + 8, arg, 0, kDefaultPriority, kThreadStack, 1,
        v.addr() + 0x10);
    if (r == lv2::CELL_OK) {
        id = ps_read64(base, v.addr());
        r = lv2::sys_ppu_thread_start(t, static_cast<u32>(id));
    }
    if (r != lv2::CELL_OK && tls) {
        p.free_tls(tls);
        tls = 0;
    }
    return r;
}

// Joins a thread that was sent its end.
s32 join_thread(PPUContext& ctx, u64& id, u32& tls)
{
    StackVar status(ctx, guest_base(ctx), 8);
    const s32 r = static_cast<s32>(lv2_syscall(ctx, 44, {id, status.addr()}));
    if (r == lv2::CELL_OK && tls) {
        HleProcess::current()->free_tls(tls);
        tls = 0;
    }
    id = 0;
    return r;
}

// ------------------------------------------------------------------------------------------------- init and end

// cellUsbdInit(): lv2's handle, the queue and its port, the threads, then the devices attached (none). A failure
// undoes what was made and is the answer (lv2's error as it is; FATAL for lv2's handle and the device list).
s32 cellUsbdInit(PPUContext& ctx)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    if (u.initialized) return CELL_USBD_ERROR_ALREADY_INITIALIZED;
    u8* base = guest_base(ctx);
    u.ldd_priority = kDefaultPriority;
    u.ldds.clear();
    StackVar out(ctx, base, 0x20);
    if (lv2_syscall(ctx, 530, {out.addr()}) != lv2::CELL_OK) return CELL_USBD_ERROR_FATAL;
    u.handle = ps_read32(base, out.addr());
    const auto finalize = [&](s32 r) {
        lv2_syscall(ctx, 531, {u.handle});
        return r;
    };
    // sys_event_queue_attribute_t {SYS_SYNC_PRIORITY, SYS_PPU_QUEUE, "_ubdorg"}, a local queue of 127 events.
    StackVar attr(ctx, base, 0x10);
    ps_write32(base, attr.addr(), 2);
    ps_write32(base, attr.addr() + 4, 1);
    std::memcpy(base + attr.addr() + 8, "_ubdorg", 8);
    if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 128, {out.addr(), attr.addr(), 0, 127})); r != lv2::CELL_OK) {
        return finalize(r);
    }
    u.queue = ps_read32(base, out.addr());
    const auto destroy_queue = [&](s32 r) {
        lv2_syscall(ctx, 129, {u.queue, 0});
        return finalize(r);
    };
    if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 134, {out.addr(), 1, 0})); r != lv2::CELL_OK) {
        return destroy_queue(r);
    }
    u.port = ps_read32(base, out.addr());
    const auto destroy_port = [&](s32 r) {
        lv2_syscall(ctx, 135, {u.port});
        return destroy_queue(r);
    };
    if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 136, {u.port, u.queue})); r != lv2::CELL_OK) {
        return destroy_port(r);
    }
    const auto disconnect = [&](s32 r) {
        lv2_syscall(ctx, 137, {u.port});
        return destroy_port(r);
    };
    if (const s32 r = start_thread(ctx, NID_callback_thread, "libusbd_callback_thread", u64{u.handle} << 32 | u.port,
            u.callback_thread, u.callback_tls);
        r != lv2::CELL_OK) {
        return disconnect(r);
    }
    const auto stop_callback_thread = [&](s32 r) {
        lv2_syscall(ctx, 549, {u.handle, kEventTerminate, 0, 0});
        join_thread(ctx, u.callback_thread, u.callback_tls);
        return disconnect(r);
    };
    if (const s32 r = start_thread(ctx, NID_usbd_thread, "libusbd_usbd_thread", u.queue, u.usbd_thread, u.usbd_tls);
        r != lv2::CELL_OK) {
        return stop_callback_thread(r);
    }
    StackVar list(ctx, base, 4 * 127); // {device id high, device id low, ...} each
    const s32 devices = static_cast<s32>(lv2_syscall(ctx, 532, {u.handle, list.addr(), 127}));
    if (devices != 0) {
        if (devices > 0) PS_FATAL(kChan, "cellUsbdInit: lv2 lists {} USB devices; it has none (D-522)", devices);
        lv2_syscall(ctx, 138, {u.port, kEventTerminate, 0, 0});
        join_thread(ctx, u.usbd_thread, u.usbd_tls);
        return stop_callback_thread(CELL_USBD_ERROR_FATAL);
    }
    u.initialized = true;
    return lv2::CELL_OK;
}

// cellUsbdEnd(): both threads sent their end and joined, the queue and port destroyed, the drivers dropped, lv2's
// handle finalised. The first failure is the answer (FATAL for the finalisation); the rest still runs.
s32 cellUsbdEnd(PPUContext& ctx)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    if (!u.initialized) return CELL_USBD_ERROR_NOT_INITIALIZED;
    u.initialized = false;
    s32 result = static_cast<s32>(lv2_syscall(ctx, 138, {u.port, kEventTerminate, 0, 0}));
    const auto keep = [&](s32 r) {
        if (result == lv2::CELL_OK) result = r;
    };
    keep(join_thread(ctx, u.usbd_thread, u.usbd_tls));
    keep(static_cast<s32>(lv2_syscall(ctx, 549, {u.handle, kEventTerminate, 0, 0})));
    keep(join_thread(ctx, u.callback_thread, u.callback_tls));
    keep(static_cast<s32>(lv2_syscall(ctx, 137, {u.port})));
    keep(static_cast<s32>(lv2_syscall(ctx, 135, {u.port})));
    keep(static_cast<s32>(lv2_syscall(ctx, 129, {u.queue, 0})));
    u.ldds.clear();
    if (lv2_syscall(ctx, 531, {u.handle}) != lv2::CELL_OK) keep(CELL_USBD_ERROR_FATAL);
    return result;
}

// ------------------------------------------------------------------------------------------------- thread priority

// The thread a cellUsbd*ThreadPriority type names: 1 libusbd_usbd_thread, 2 libusbd_callback_thread.
u64 thread_of_type(const UsbdState& u, s32 type) { return type == 1 ? u.usbd_thread : u.callback_thread; }

// cellUsbdGetThreadPriority(type): 0 the drivers' priority kept, 1 and 2 the threads' (lv2's error as it is).
s32 cellUsbdGetThreadPriority(PPUContext& ctx, s32 type)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    if (!u.initialized) return CELL_USBD_ERROR_NOT_INITIALIZED;
    if (type == 0) return u.ldd_priority;
    if (type != 1 && type != 2) return CELL_USBD_ERROR_INVALID_PARAM;
    StackVar prio(ctx, guest_base(ctx), 8);
    if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 48, {thread_of_type(u, type), prio.addr()}));
        r != lv2::CELL_OK) {
        return r;
    }
    return static_cast<s32>(ps_read32(guest_base(ctx), prio.addr()));
}

// cellUsbdSetThreadPriority(type, priority): as cellUsbdGetThreadPriority's types.
s32 cellUsbdSetThreadPriority(PPUContext& ctx, s32 type, s32 priority)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    if (!u.initialized) return CELL_USBD_ERROR_NOT_INITIALIZED;
    if (type == 0) {
        u.ldd_priority = priority;
        return lv2::CELL_OK;
    }
    if (type != 1 && type != 2) return CELL_USBD_ERROR_INVALID_PARAM;
    return static_cast<s32>(lv2_syscall(ctx, 47, {thread_of_type(u, type), static_cast<u64>(s64{priority})}));
}

// cellUsbdSetThreadPriority2(ldd, usbd, callback): all three, ldd < usbd < callback; the usbd thread's priority is put
// back when the callback thread's cannot be set.
s32 cellUsbdSetThreadPriority2(PPUContext& ctx, s32 ldd, s32 usbd_priority, s32 callback)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    if (!u.initialized) return CELL_USBD_ERROR_NOT_INITIALIZED;
    if (!(ldd < usbd_priority && usbd_priority < callback)) return CELL_USBD_ERROR_INVALID_PARAM;
    StackVar old(ctx, guest_base(ctx), 8);
    if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 48, {u.usbd_thread, old.addr()})); r != lv2::CELL_OK) return r;
    const auto set = [&](u64 thread, s32 prio) {
        return static_cast<s32>(lv2_syscall(ctx, 47, {thread, static_cast<u64>(s64{prio})}));
    };
    if (const s32 r = set(u.usbd_thread, usbd_priority); r != lv2::CELL_OK) return r;
    if (const s32 r = set(u.callback_thread, callback); r != lv2::CELL_OK) {
        set(u.usbd_thread, static_cast<s32>(ps_read32(guest_base(ctx), old.addr())));
        return r;
    }
    u.ldd_priority = ldd;
    return lv2::CELL_OK;
}

// ------------------------------------------------------------------------------------------------- drivers

// Registers `ops` with lv2 (`call(handle, name, length)`: lv2's driver id, negative for a failure) and keeps it.
template <typename Register>
s32 register_ldd(PPUContext& ctx, u32 ops, LddKind kind, Register&& call)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    if (!u.initialized) return CELL_USBD_ERROR_NOT_INITIALIZED;
    u8* base = guest_base(ctx);
    Ldd l;
    l.kind = kind;
    if (!read_ops(base, ops, l.ops)) return CELL_USBD_ERROR_INVALID_PARAM;
    if (name_registered(base, u, l.ops[0])) return CELL_USBD_ERROR_LDD_ALREADY_REGISTERED;
    const s32 id = call(u.handle, l.ops[0], static_cast<u32>(guest_cstr(base, l.ops[0]).size()));
    if (id < 0) return CELL_USBD_ERROR_FATAL;
    l.id = id;
    u.ldds.push_back(l);
    // libusbd now offers the plain and composite drivers the devices no driver has: there is none.
    return lv2::CELL_OK;
}

// cellUsbdRegisterLdd(CellUsbdLddOps* ops): sys_usbd_register_ldd.
s32 cellUsbdRegisterLdd(PPUContext& ctx, u32 ops)
{
    return register_ldd(ctx, ops, LddKind::Plain, [&](u32 handle, u32 name, u32 len) {
        return static_cast<s32>(lv2_syscall(ctx, 535, {handle, name, len}));
    });
}

// cellUsbdRegisterCompositeLdd(CellUsbdLddOps* ops): lv2's call 563 (one RPCS3 answers with CELL_OK and nothing).
s32 cellUsbdRegisterCompositeLdd(PPUContext& ctx, u32 ops)
{
    return register_ldd(ctx, ops, LddKind::Composite, [&](u32 handle, u32 name, u32 len) {
        return static_cast<s32>(lv2_syscall(ctx, 563, {handle, name, len}));
    });
}

// cellUsbdRegisterExtraLdd2(ops, vendor, product_min, product_max): sys_usbd_register_extra_ldd.
s32 cellUsbdRegisterExtraLdd2(PPUContext& ctx, u32 ops, u32 vendor, u32 product_min, u32 product_max)
{
    return register_ldd(ctx, ops, LddKind::Extra, [&](u32 handle, u32 name, u32 len) {
        return static_cast<s32>(lv2_syscall(ctx, 559,
            {handle, name, len, vendor & 0xFFFF, product_min & 0xFFFF, product_max & 0xFFFF}));
    });
}

// cellUsbdRegisterExtraLdd(ops, vendor, product): one product.
s32 cellUsbdRegisterExtraLdd(PPUContext& ctx, u32 ops, u32 vendor, u32 product)
{
    return cellUsbdRegisterExtraLdd2(ctx, ops, vendor, product, product);
}

// Unregisters the driver whose kept pointers are `ops`'s now: composite ones by cellUsbdUnregisterCompositeLdd, the
// others by cellUsbdUnregisterLdd and cellUsbdUnregisterExtraLdd (one function in libusbd). lv2's answer when it
// succeeds, CELL_USBD_ERROR_FATAL when not; the driver is dropped either way.
s32 unregister_ldd(PPUContext& ctx, u32 ops, bool composite)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    if (!u.initialized) return CELL_USBD_ERROR_NOT_INITIALIZED;
    u8* base = guest_base(ctx);
    std::array<u32, 4> now{};
    if (!read_ops(base, ops, now)) return CELL_USBD_ERROR_INVALID_PARAM;
    const auto it = std::find_if(u.ldds.begin(), u.ldds.end(),
        [&](const Ldd& l) { return (l.kind == LddKind::Composite) == composite && l.ops == now; });
    if (it == u.ldds.end()) return CELL_USBD_ERROR_LDD_NOT_FOUND;
    // libusbd first detaches the devices the driver has: none.
    const u32 len = static_cast<u32>(guest_cstr(base, now[0]).size());
    const u32 call = it->kind == LddKind::Extra ? 558 : 536;
    const s32 r = static_cast<s32>(lv2_syscall(ctx, call, {u.handle, now[0], len}));
    u.ldds.erase(it);
    return r < 0 ? CELL_USBD_ERROR_FATAL : r;
}

s32 cellUsbdUnregisterLdd(PPUContext& ctx, u32 ops) { return unregister_ldd(ctx, ops, false); }
s32 cellUsbdUnregisterCompositeLdd(PPUContext& ctx, u32 ops) { return unregister_ldd(ctx, ops, true); }

// ------------------------------------------------------------------------------------------------- devices and pipes

// The calls that look a device or a pipe up in libusbd's lists, which stay empty.
s32 initialized_or(s32 answer)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    return u.initialized ? answer : CELL_USBD_ERROR_NOT_INITIALIZED;
}

// cellUsbdOpenPipe(device, UsbEndpointDescriptor* ed): the device is looked up first.
s32 cellUsbdOpenPipe(PPUContext&) { return initialized_or(CELL_USBD_ERROR_DEVICE_NOT_FOUND); }

// cellUsbdClosePipe(pipe); cellUsbdBulkTransfer, cellUsbdInterruptTransfer (pipe, buf, len, done, arg),
// cellUsbdIsochronousTransfer, cellUsbdHSIsochronousTransfer (pipe, request, done, arg): the pipe first.
s32 cellUsbdPipeCall(PPUContext&) { return initialized_or(CELL_USBD_ERROR_PIPE_NOT_FOUND); }

// cellUsbdControlTransfer(pipe, UsbDeviceRequest* request, buf, done, arg): a request is required before the pipe.
s32 cellUsbdControlTransfer(PPUContext&, s32, u32 request)
{
    return initialized_or(request ? CELL_USBD_ERROR_PIPE_NOT_FOUND : CELL_USBD_ERROR_INVALID_PARAM);
}

// cellUsbdSetPrivateData(device, priv): a pointer is required before the device.
s32 cellUsbdSetPrivateData(PPUContext&, s32, u32 priv)
{
    return initialized_or(priv ? CELL_USBD_ERROR_DEVICE_NOT_FOUND : CELL_USBD_ERROR_INVALID_PARAM);
}

// cellUsbdGetPrivateData(device), cellUsbdScanStaticDescriptor(device, ptr, type): a null pointer for any device.
u32 cellUsbdNoPointer(PPUContext&) { return 0; }

// cellUsbdGetDeviceLocation(device, u8* location), cellUsbdGetDeviceSpeed(device, u8* speed), cellUsbdResetDevice
// (device): lv2's sys_usbd_get_device_location, _get_device_speed and _send_event; a failure FATAL.
s32 device_call(PPUContext& ctx, u32 number, s32 device, u32 out, bool with_out)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    if (!u.initialized) return CELL_USBD_ERROR_NOT_INITIALIZED;
    const u64 dev = static_cast<u64>(s64{device});
    return fatal_unless_ok(with_out ? lv2_syscall(ctx, number, {u.handle, dev, out})
                                    : lv2_syscall(ctx, number, {u.handle, dev}));
}

s32 cellUsbdGetDeviceLocation(PPUContext& ctx, s32 device, u32 location)
{
    return device_call(ctx, 547, device, location, true);
}

s32 cellUsbdGetDeviceSpeed(PPUContext& ctx, s32 device, u32 speed) { return device_call(ctx, 556, device, speed, true); }

s32 cellUsbdResetDevice(PPUContext& ctx, s32 device) { return device_call(ctx, 548, device, 0, false); }

// ------------------------------------------------------------------------------------------------- memory

// cellUsbdAllocateMemory(void** ptr, size): sys_usbd_allocate_memory; lv2's error as it is, a null address FATAL.
s32 cellUsbdAllocateMemory(PPUContext& ctx, u32 ptr, u32 size)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    if (!u.initialized) return CELL_USBD_ERROR_NOT_INITIALIZED;
    if (!ptr || !size) return CELL_USBD_ERROR_INVALID_PARAM;
    u8* base = guest_base(ctx);
    StackVar out(ctx, base, 8);
    if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 550, {u.handle, out.addr(), size})); r != lv2::CELL_OK) {
        return r;
    }
    const u32 addr = ps_read32(base, out.addr());
    if (!addr) return CELL_USBD_ERROR_FATAL;
    ps_write32(base, ptr, addr);
    return lv2::CELL_OK;
}

// cellUsbdAllocateMemoryFromContainer(void** ptr, size, container): lv2's call 565 with the caller's r6 after the
// container; any failure or a null address FATAL.
s32 cellUsbdAllocateMemoryFromContainer(PPUContext& ctx, u32 ptr, u32 size, u32 container, u64 unk)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    if (!u.initialized) return CELL_USBD_ERROR_NOT_INITIALIZED;
    if (!ptr || !size) return CELL_USBD_ERROR_INVALID_PARAM;
    u8* base = guest_base(ctx);
    StackVar out(ctx, base, 8);
    if (lv2_syscall(ctx, 565, {u.handle, out.addr(), size, container, unk}) != lv2::CELL_OK) {
        return CELL_USBD_ERROR_FATAL;
    }
    const u32 addr = ps_read32(base, out.addr());
    if (!addr) return CELL_USBD_ERROR_FATAL;
    ps_write32(base, ptr, addr);
    return lv2::CELL_OK;
}

// cellUsbdAllocateSharedMemory(u32* id, void** ptr, size): lv2's call 569 (one RPCS3 answers with CELL_OK and nothing,
// so a null address: FATAL); lv2's error as it is.
s32 cellUsbdAllocateSharedMemory(PPUContext& ctx, u32 id, u32 ptr, u32 size)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    if (!u.initialized) return CELL_USBD_ERROR_NOT_INITIALIZED;
    if (!id || !ptr || !size) return CELL_USBD_ERROR_INVALID_PARAM;
    u8* base = guest_base(ctx);
    StackVar out(ctx, base, 8);
    if (const s32 r = static_cast<s32>(lv2_syscall(ctx, 569, {u.handle, out.addr(), out.addr() + 4, size}));
        r != lv2::CELL_OK) {
        return r;
    }
    const u32 addr = ps_read32(base, out.addr() + 4);
    if (!addr) return CELL_USBD_ERROR_FATAL;
    ps_write32(base, ptr, addr);
    ps_write32(base, id, ps_read32(base, out.addr()));
    return lv2::CELL_OK;
}

// cellUsbdFreeMemory(ptr): sys_usbd_free_memory; a failure FATAL.
s32 cellUsbdFreeMemory(PPUContext& ctx, u32 ptr)
{
    UsbdState& u = usbd();
    std::lock_guard lock(u.lock);
    if (!u.initialized) return CELL_USBD_ERROR_NOT_INITIALIZED;
    return fatal_unless_ok(lv2_syscall(ctx, 551, {u.handle, ptr}));
}


// ================================================================================================= cellUsbPspcm
//
// libusbpspcm: a PSP connected over USB (054C:01CB, its driver "cellUsbPspcm" registered with cellUsbd) bound to by
// device slot, then a connection sending and receiving over bulk transfers. cellUsbPspcmInit(CellUsbPspcmParam*):
// {u32 size (32), s32 devices (1..16), s32 connections (> 0), pool, pool size, thread priority, thread stack size,
// const char* name (optional, at most 63 characters)}. The pool, cellUsbPspcmCalcPoolSize bytes of the game's memory,
// is the library's own (its tables and transfer buffers) and pspocket keeps its state on the host instead; a pool at
// address 0 leaves the library uninitialised, as libusbpspcm's flag is the pool's address. Init also starts the
// library's thread "cellUsbPspcm_Library" (the parameters' priority, their stack size plus 4 KB), which handles devices
// arriving and the registered handler's changes, and ends at cellUsbPspcmEnd.
//
// With no USB device (see the top of the file) no device slot ever holds a PSP: a bind answers
// CELL_USBPSPCM_ERROR_CLOSED, a connection is never open (CLOSED for transfers, resets and data waits, ALREADY for a
// close), and nothing is ever pending (INVALID for the polls, waits and cancels of asynchronous operations). The checks
// before those answers are libusbpspcm's, in its order.

constexpr std::string_view kPspcmModule = "cellUsbPspcm";
constexpr s32 CELL_USBPSPCM_ERROR_NOT_INITIALIZED = static_cast<s32>(0x80110401);
constexpr s32 CELL_USBPSPCM_ERROR_ALREADY = static_cast<s32>(0x80110402);
constexpr s32 CELL_USBPSPCM_ERROR_INVALID = static_cast<s32>(0x80110403);
constexpr s32 CELL_USBPSPCM_ERROR_NO_MEMORY = static_cast<s32>(0x80110404);
constexpr s32 CELL_USBPSPCM_ERROR_CLOSED = static_cast<s32>(0x8011040B);

constexpr u32 NID_pspcm_thread = 0x41323BCE; // nid_of("_pspocket_usbpspcm_thread"): pspocket's own
constexpr u32 NID_pspcm_probe = 0xF3403859;  // nid_of("_pspocket_usbpspcm_probe")
constexpr u32 NID_pspcm_attach = 0x933C26FB; // nid_of("_pspocket_usbpspcm_attach")
constexpr u32 NID_pspcm_detach = 0xF8D2EA3C; // nid_of("_pspocket_usbpspcm_detach")
constexpr u64 kPspcmEnd = 0x80;              // the thread's end, as libusbpspcm's event bit
constexpr s32 kAnyDevice = 255;

struct PspcmState {
    std::mutex lock;
    u32 pool = 0; // the library is initialised while this is not 0
    s32 devices = 0, connections = 0;
    u32 handler = 0; // cellUsbPspcmRegister's
    u32 ops = 0;     // the driver's CellUsbdLddOps in guest memory (made once)
    u64 thread = 0;
    u32 tls = 0, flag = 0;
};

PspcmState& pspcm() { return HleProcess::current()->state<PspcmState>(); }

bool pspcm_initialized()
{
    PspcmState& s = pspcm();
    std::lock_guard lock(s.lock);
    return s.pool != 0;
}

// cellUsbPspcmCalcPoolSize(const CellUsbPspcmParam* param): 8268 * devices * connections + 128 * devices + 144 *
// connections bytes (32-bit arithmetic, as libusbpspcm's).
s32 cellUsbPspcmCalcPoolSize(PPUContext& ctx, u32 param)
{
    if (!param) return CELL_USBPSPCM_ERROR_INVALID;
    u8* base = guest_base(ctx);
    const u32 n = ps_read32(base, param + 4), m = ps_read32(base, param + 8);
    if (ps_read32(base, param) != 32 || static_cast<s32>(n) <= 0 || static_cast<s32>(n) > 16 || static_cast<s32>(m) <= 0) {
        return CELL_USBPSPCM_ERROR_INVALID;
    }
    return static_cast<s32>(8268 * n * m + 128 * n + 144 * m);
}

// The library's thread (argument: its flag): waits for its end (no device ever arrives to handle).
void _pspocket_usbpspcm_thread(PPUContext& ctx, u64 flag)
{
    lv2::PpuThread& t = thread_of(ctx);
    StackVar result(ctx, t.kernel.base(), 8);
    while (lv2::sys_event_flag_wait(t, static_cast<u32>(flag), kPspcmEnd, lv2::SYS_EVENT_FLAG_WAIT_AND | lv2::SYS_EVENT_FLAG_WAIT_CLEAR,
               result.addr(), 0) != lv2::CELL_OK) {
    }
    lv2::_sys_ppu_thread_exit(t, 0);
}

// The driver's probe, attach and detach: lv2 has no device to offer them.
void _pspocket_usbpspcm_probe(PPUContext&) { PS_FATAL(kChan, "cellUsbPspcm's driver probed a USB device (D-522)"); }
void _pspocket_usbpspcm_attach(PPUContext&) { PS_FATAL(kChan, "cellUsbPspcm's driver attached a USB device (D-522)"); }
void _pspocket_usbpspcm_detach(PPUContext&) { PS_FATAL(kChan, "cellUsbPspcm's driver detached a USB device (D-522)"); }

// The driver's CellUsbdLddOps {"cellUsbPspcm", probe, attach, detach} in guest memory.
u32 pspcm_ops(PspcmState& s)
{
    if (s.ops) return s.ops;
    HleProcess& p = *HleProcess::current();
    const u32 at = p.main_memory().alloc(0x20, 0x10);
    if (!at) PS_FATAL(kChan, "cellUsbPspcmInit: no main memory for the driver's operations");
    u8* base = p.kernel().base();
    ps_write32(base, at, at + 0x10);
    const u32 nids[3] = {NID_pspcm_probe, NID_pspcm_attach, NID_pspcm_detach};
    for (u32 i = 0; i < 3; i++) {
        const u32 opd = p.descriptor_of(kPspcmModule, nids[i]);
        if (!opd) PS_FATAL(kChan, "cellUsbPspcm's driver functions were not linked at load");
        ps_write32(base, at + 4 + 4 * i, opd);
    }
    std::memcpy(base + at + 0x10, "cellUsbPspcm", 13);
    s.ops = at;
    return at;
}

// Ends the library's thread and destroys its flag.
void pspcm_stop_thread(PPUContext& ctx, PspcmState& s)
{
    lv2::PpuThread& t = thread_of(ctx);
    lv2::sys_event_flag_set(t, s.flag, kPspcmEnd);
    join_thread(ctx, s.thread, s.tls);
    lv2::sys_event_flag_destroy(t, s.flag);
    s.flag = 0;
}

// cellUsbPspcmInit(const CellUsbPspcmParam* param): the checks, the thread, then the driver registered with cellUsbd
// (its error as it is: CELL_USBD_ERROR_NOT_INITIALIZED before cellUsbdInit).
s32 cellUsbPspcmInit(PPUContext& ctx, u32 param)
{
    PspcmState& s = pspcm();
    std::lock_guard lock(s.lock);
    if (s.pool) return CELL_USBPSPCM_ERROR_ALREADY;
    const s32 size = cellUsbPspcmCalcPoolSize(ctx, param);
    if (size < 0) return size;
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    if (const u32 name = ps_read32(base, param + 28); name && guest_cstr(base, name).size() > 63) {
        return CELL_USBPSPCM_ERROR_INVALID;
    }
    if (static_cast<u32>(size) > ps_read32(base, param + 16)) return CELL_USBPSPCM_ERROR_NO_MEMORY;
    // The thread's flag (sys_event_flag_attribute_t {SYS_SYNC_PRIORITY, not shared, 0, 0, waiter single, "_ubpcev"}).
    StackVar v(ctx, base, 0x40);
    ps_write32(base, v.addr() + 8, lv2::SYS_SYNC_PRIORITY);
    ps_write32(base, v.addr() + 12, lv2::SYS_SYNC_NOT_PROCESS_SHARED);
    ps_write32(base, v.addr() + 28, lv2::SYS_SYNC_WAITER_SINGLE);
    std::memcpy(base + v.addr() + 32, "_ubpcev", 8);
    if (const s32 r = lv2::sys_event_flag_create(t, v.addr(), v.addr() + 8, 0); r != lv2::CELL_OK) return r;
    s.flag = ps_read32(base, v.addr());
    HleProcess& p = *HleProcess::current();
    const u32 entry = p.descriptor_of(kPspcmModule, NID_pspcm_thread);
    if (!entry) PS_FATAL(kChan, "cellUsbPspcm's thread function was not linked at load");
    s.tls = p.alloc_tls();
    if (!s.tls) {
        lv2::sys_event_flag_destroy(t, s.flag);
        return lv2::err(lv2::CELL_ENOMEM);
    }
    ps_write32(base, v.addr() + 8, entry); // ppu_thread_param_t {entry, tls}
    ps_write32(base, v.addr() + 12, s.tls + kTlsR13Offset);
    std::memcpy(base + v.addr() + 0x10, "cellUsbPspcm_Library", 21);
    s32 r = lv2::_sys_ppu_thread_create(t, v.addr(), v.addr() + 8, s.flag, 0, static_cast<s32>(ps_read32(base, param + 20)),
        ps_read32(base, param + 24) + 0x1000, 1, v.addr() + 0x10);
    if (r == lv2::CELL_OK) {
        s.thread = ps_read64(base, v.addr());
        r = lv2::sys_ppu_thread_start(t, static_cast<u32>(s.thread));
    }
    if (r != lv2::CELL_OK) {
        p.free_tls(s.tls);
        s.tls = 0;
        lv2::sys_event_flag_destroy(t, s.flag);
        return r;
    }
    if (const s32 rr = cellUsbdRegisterLdd(ctx, pspcm_ops(s)); rr < 0) {
        pspcm_stop_thread(ctx, s);
        return rr;
    }
    s.devices = static_cast<s32>(ps_read32(base, param + 4));
    s.connections = static_cast<s32>(ps_read32(base, param + 8));
    s.handler = 0;
    s.pool = ps_read32(base, param + 12);
    return lv2::CELL_OK;
}

// cellUsbPspcmEnd(): the thread ended, the driver unregistered (cellUsbd's answer ignored).
s32 cellUsbPspcmEnd(PPUContext& ctx)
{
    PspcmState& s = pspcm();
    std::lock_guard lock(s.lock);
    if (!s.pool) return CELL_USBPSPCM_ERROR_NOT_INITIALIZED;
    pspcm_stop_thread(ctx, s);
    cellUsbdUnregisterLdd(ctx, s.ops);
    s.pool = 0;
    return lv2::CELL_OK;
}

// cellUsbPspcmRegister(handler): the function a PSP's requests go to, one at a time.
s32 cellUsbPspcmRegister(PPUContext&, u32 handler)
{
    PspcmState& s = pspcm();
    std::lock_guard lock(s.lock);
    if (!s.pool) return CELL_USBPSPCM_ERROR_NOT_INITIALIZED;
    if (!handler) return CELL_USBPSPCM_ERROR_INVALID;
    if (s.handler) return CELL_USBPSPCM_ERROR_ALREADY;
    s.handler = handler;
    return lv2::CELL_OK;
}

// cellUsbPspcmUnregister(handler): the registered one (CELL_USBPSPCM_ERROR_ALREADY when there is none).
s32 cellUsbPspcmUnregister(PPUContext&, u32 handler)
{
    PspcmState& s = pspcm();
    std::lock_guard lock(s.lock);
    if (!s.pool) return CELL_USBPSPCM_ERROR_NOT_INITIALIZED;
    if (!s.handler) return CELL_USBPSPCM_ERROR_ALREADY;
    if (!handler || handler != s.handler) return CELL_USBPSPCM_ERROR_INVALID;
    s.handler = 0;
    return lv2::CELL_OK;
}

// cellUsbPspcmGetAddr(): the mask of the device slots a PSP has bound: none.
s32 cellUsbPspcmGetAddr(PPUContext&) { return pspcm_initialized() ? 0 : CELL_USBPSPCM_ERROR_NOT_INITIALIZED; }

// A device slot argument (Bind and the bind calls): 255 ("any") and anything outside 0..devices-1 refused.
s32 check_device(s32 device)
{
    PspcmState& s = pspcm();
    std::lock_guard lock(s.lock);
    if (!s.pool) return CELL_USBPSPCM_ERROR_NOT_INITIALIZED;
    if (device == kAnyDevice || device < 0 || device >= s.devices) return CELL_USBPSPCM_ERROR_INVALID;
    return lv2::CELL_OK;
}

// A connection argument: 0..connections-1.
s32 check_connection(s32 id)
{
    PspcmState& s = pspcm();
    std::lock_guard lock(s.lock);
    if (!s.pool) return CELL_USBPSPCM_ERROR_NOT_INITIALIZED;
    if (id < 0 || id >= s.connections) return CELL_USBPSPCM_ERROR_INVALID;
    return lv2::CELL_OK;
}

// cellUsbPspcmBind(device), cellUsbPspcmBindAsync(device): no PSP is in the slot.
s32 cellUsbPspcmBind(PPUContext&, s32 device)
{
    const s32 r = check_device(device);
    return r != lv2::CELL_OK ? r : CELL_USBPSPCM_ERROR_CLOSED;
}

// cellUsbPspcmPollBindAsync, cellUsbPspcmWaitBindAsync, cellUsbPspcmCancelBind (device): no bind is in progress.
s32 cellUsbPspcmBindPending(PPUContext&, s32 device)
{
    const s32 r = check_device(device);
    return r != lv2::CELL_OK ? r : CELL_USBPSPCM_ERROR_INVALID;
}

// cellUsbPspcmClose(id): the connection is not open.
s32 cellUsbPspcmClose(PPUContext&, s32 id)
{
    const s32 r = check_connection(id);
    return r != lv2::CELL_OK ? r : CELL_USBPSPCM_ERROR_ALREADY;
}

// cellUsbPspcmSend, cellUsbPspcmSendAsync, cellUsbPspcmRecv, cellUsbPspcmRecvAsync (id, buf, size, mode): a buffer
// unless the size is 0, a mode of 0 or 1, then the connection is not open.
s32 cellUsbPspcmTransfer(PPUContext&, s32 id, u32 buf, u32 size, u32 mode)
{
    if (const s32 r = check_connection(id); r != lv2::CELL_OK) return r;
    if ((!buf && size) || mode > 1) return CELL_USBPSPCM_ERROR_INVALID;
    return CELL_USBPSPCM_ERROR_CLOSED;
}

// cellUsbPspcmReset, cellUsbPspcmResetAsync, cellUsbPspcmWaitData, cellUsbPspcmPollData (id): not open.
s32 cellUsbPspcmClosedCall(PPUContext&, s32 id)
{
    const s32 r = check_connection(id);
    return r != lv2::CELL_OK ? r : CELL_USBPSPCM_ERROR_CLOSED;
}

// cellUsbPspcmPoll/WaitSendAsync, _RecvAsync, _ResetAsync, cellUsbPspcmCancelWaitData (id): nothing is pending.
s32 cellUsbPspcmPendingCall(PPUContext&, s32 id)
{
    const s32 r = check_connection(id);
    return r != lv2::CELL_OK ? r : CELL_USBPSPCM_ERROR_INVALID;
}

} // namespace

void register_usbd(Registry& r)
{
    using detail::thunk;
    const auto f = [](std::string_view name, HleFn fn) { return Function{{}, name, recomp::nid_of(name), fn, false}; };
    r.add(kModule, {
        f("cellUsbdInit", &thunk<&cellUsbdInit>),
        f("cellUsbdEnd", &thunk<&cellUsbdEnd>),
        f("cellUsbdGetThreadPriority", &thunk<&cellUsbdGetThreadPriority>),
        f("cellUsbdSetThreadPriority", &thunk<&cellUsbdSetThreadPriority>),
        f("cellUsbdSetThreadPriority2", &thunk<&cellUsbdSetThreadPriority2>),
        f("cellUsbdRegisterLdd", &thunk<&cellUsbdRegisterLdd>),
        f("cellUsbdRegisterCompositeLdd", &thunk<&cellUsbdRegisterCompositeLdd>),
        f("cellUsbdRegisterExtraLdd", &thunk<&cellUsbdRegisterExtraLdd>),
        f("cellUsbdRegisterExtraLdd2", &thunk<&cellUsbdRegisterExtraLdd2>),
        f("cellUsbdUnregisterLdd", &thunk<&cellUsbdUnregisterLdd>),
        f("cellUsbdUnregisterCompositeLdd", &thunk<&cellUsbdUnregisterCompositeLdd>),
        f("cellUsbdUnregisterExtraLdd", &thunk<&cellUsbdUnregisterLdd>),
        f("cellUsbdOpenPipe", &thunk<&cellUsbdOpenPipe>),
        f("cellUsbdClosePipe", &thunk<&cellUsbdPipeCall>),
        f("cellUsbdControlTransfer", &thunk<&cellUsbdControlTransfer>),
        f("cellUsbdBulkTransfer", &thunk<&cellUsbdPipeCall>),
        f("cellUsbdInterruptTransfer", &thunk<&cellUsbdPipeCall>),
        f("cellUsbdIsochronousTransfer", &thunk<&cellUsbdPipeCall>),
        f("cellUsbdHSIsochronousTransfer", &thunk<&cellUsbdPipeCall>),
        f("cellUsbdScanStaticDescriptor", &thunk<&cellUsbdNoPointer>),
        f("cellUsbdGetDeviceSpeed", &thunk<&cellUsbdGetDeviceSpeed>),
        f("cellUsbdGetDeviceLocation", &thunk<&cellUsbdGetDeviceLocation>),
        f("cellUsbdSetPrivateData", &thunk<&cellUsbdSetPrivateData>),
        f("cellUsbdGetPrivateData", &thunk<&cellUsbdNoPointer>),
        f("cellUsbdAllocateMemory", &thunk<&cellUsbdAllocateMemory>),
        f("cellUsbdAllocateMemoryFromContainer", &thunk<&cellUsbdAllocateMemoryFromContainer>),
        f("cellUsbdAllocateSharedMemory", &thunk<&cellUsbdAllocateSharedMemory>),
        f("cellUsbdFreeMemory", &thunk<&cellUsbdFreeMemory>),
        f("cellUsbdResetDevice", &thunk<&cellUsbdResetDevice>),
        PS_HLE_INTERNAL(NID_usbd_thread, _pspocket_usbd_thread),
        PS_HLE_INTERNAL(NID_callback_thread, _pspocket_usbd_callback_thread),
    });
    r.add(kPspcmModule, {
        f("cellUsbPspcmCalcPoolSize", &thunk<&cellUsbPspcmCalcPoolSize>),
        f("cellUsbPspcmInit", &thunk<&cellUsbPspcmInit>),
        f("cellUsbPspcmEnd", &thunk<&cellUsbPspcmEnd>),
        f("cellUsbPspcmRegister", &thunk<&cellUsbPspcmRegister>),
        f("cellUsbPspcmUnregister", &thunk<&cellUsbPspcmUnregister>),
        f("cellUsbPspcmGetAddr", &thunk<&cellUsbPspcmGetAddr>),
        f("cellUsbPspcmBind", &thunk<&cellUsbPspcmBind>),
        f("cellUsbPspcmBindAsync", &thunk<&cellUsbPspcmBind>),
        f("cellUsbPspcmPollBindAsync", &thunk<&cellUsbPspcmBindPending>),
        f("cellUsbPspcmWaitBindAsync", &thunk<&cellUsbPspcmBindPending>),
        f("cellUsbPspcmCancelBind", &thunk<&cellUsbPspcmBindPending>),
        f("cellUsbPspcmClose", &thunk<&cellUsbPspcmClose>),
        f("cellUsbPspcmSend", &thunk<&cellUsbPspcmTransfer>),
        f("cellUsbPspcmSendAsync", &thunk<&cellUsbPspcmTransfer>),
        f("cellUsbPspcmRecv", &thunk<&cellUsbPspcmTransfer>),
        f("cellUsbPspcmRecvAsync", &thunk<&cellUsbPspcmTransfer>),
        f("cellUsbPspcmReset", &thunk<&cellUsbPspcmClosedCall>),
        f("cellUsbPspcmResetAsync", &thunk<&cellUsbPspcmClosedCall>),
        f("cellUsbPspcmWaitData", &thunk<&cellUsbPspcmClosedCall>),
        f("cellUsbPspcmPollData", &thunk<&cellUsbPspcmClosedCall>),
        f("cellUsbPspcmPollSendAsync", &thunk<&cellUsbPspcmPendingCall>),
        f("cellUsbPspcmWaitSendAsync", &thunk<&cellUsbPspcmPendingCall>),
        f("cellUsbPspcmPollRecvAsync", &thunk<&cellUsbPspcmPendingCall>),
        f("cellUsbPspcmWaitRecvAsync", &thunk<&cellUsbPspcmPendingCall>),
        f("cellUsbPspcmPollResetAsync", &thunk<&cellUsbPspcmPendingCall>),
        f("cellUsbPspcmWaitResetAsync", &thunk<&cellUsbPspcmPendingCall>),
        f("cellUsbPspcmCancelWaitData", &thunk<&cellUsbPspcmPendingCall>),
        PS_HLE_INTERNAL(NID_pspcm_thread, _pspocket_usbpspcm_thread),
        PS_HLE_INTERNAL(NID_pspcm_probe, _pspocket_usbpspcm_probe),
        PS_HLE_INTERNAL(NID_pspcm_attach, _pspocket_usbpspcm_attach),
        PS_HLE_INTERNAL(NID_pspcm_detach, _pspocket_usbpspcm_detach),
    });
}

} // namespace pspocket::hle
