// sys_usbd: lv2's USB host interface, for a console with no USB device attached (D-522). Reference: RPCS3
// rpcs3/Emu/Cell/lv2/sys_usbd.cpp (handle, logical device driver registration, the event wait and its termination
// value, the error codes), less its emulated devices: no device ever attaches, so the device list is empty and
// everything that names a device or a pipe is CELL_EINVAL, as RPCS3 answers for an unknown one.
//
// libusbd's callback thread waits in sys_usbd_receive_event; it gets an event only from sys_usbd_event_port_send (the
// library's own) or, as RPCS3's termination signal (arg1 4), from sys_usbd_finalize. Registered drivers (by product
// name, with a vendor and product range; the names RPCS3 knows are registered with their IDs) are kept for the
// handle's life: a second registration of a name is CELL_EEXIST, unregistering an unknown one CELL_ESRCH.
//
// Memory for transfers (sys_usbd_allocate_memory) is user memory the kernel keeps, in 64 KB pages, as
// sys_fs_mapped_allocate's: freed by sys_usbd_free_memory or sys_usbd_finalize. RPCS3 answers both with CELL_OK and no
// memory (todo); libusbd turns a zero address into CELL_USBD_ERROR_FATAL, so its cellUsbdAllocateMemory never succeeds
// there. The unnamed calls libusbd makes for memory from a container (565) and shared memory (567..569) stay RPCS3's
// CELL_OK with nothing written (the name table has no entry to bind them to).

#include <deque>
#include <map>
#include <string>

#include "internal.hpp"

namespace pspocket::lv2 {

namespace {

constexpr std::string_view kUsbdChan = "sys_usbd";
constexpr u32 kHandle = 0x115B; // RPCS3's handle
constexpr u64 SYS_USBD_TERMINATE = 4;

struct Ldd {
    u16 vendor = 0, product_min = 0, product_max = 0;
};

struct UsbdEvent {
    u64 arg1 = 0, arg2 = 0, arg3 = 0;
};

struct Usbd {
    std::mutex lock;
    bool init = false;
    std::map<std::string, Ldd> ldds;
    std::deque<UsbdEvent> events;
    WaitQueue waiters;
    std::map<u32, u64> memory; // allocations' sizes by address
};

Usbd& usbd(Kernel& k)
{
    Kernel::Impl& s = k.impl();
    std::lock_guard lock(s.system_mutex);
    if (!s.usbd_state) s.usbd_state = std::make_shared<Usbd>();
    return *static_cast<Usbd*>(s.usbd_state.get());
}

// A product name argument: `len` bytes (RPCS3 reads them as they are).
bool read_name(const Guest& g, u32 addr, u16 len, std::string& out)
{
    if (len && !g.valid(addr, len)) return false;
    out.assign(reinterpret_cast<const char*>(g.ptr(addr)), len);
    return true;
}

// RPCS3's drivers known by name, registered with their device IDs (sys_usbd_register_ldd).
const std::map<std::string, Ldd, std::less<>> kKnownLdds = {
    {"cellUsbPspcm", {0x054C, 0x01CB, 0x01CB}},
    {"guncon3", {0x0B9A, 0x0800, 0x0800}},
    {"PS3A-USJ", {0x0B9A, 0x0900, 0x0910}},
};

Result register_extra(Kernel& k, u32 name, u16 len, u16 vendor, u16 pmin, u16 pmax)
{
    Usbd& u = usbd(k);
    std::string product;
    if (!read_name(k.impl().guest, name, len, product)) return err(CELL_EFAULT);
    std::lock_guard lock(u.lock);
    if (!u.init) return err(CELL_EINVAL);
    if (!u.ldds.emplace(product, Ldd{vendor, pmin, pmax}).second) return err(CELL_EEXIST);
    PS_LOG_INFO(kUsbdChan, "driver \"{}\" registered for {:04x}:{:04x}..{:04x} (no USB device is attached)", product,
        vendor, pmin, pmax);
    return CELL_OK;
}

Result unregister_extra(Kernel& k, u32 name, u16 len)
{
    Usbd& u = usbd(k);
    std::string product;
    if (!read_name(k.impl().guest, name, len, product)) return err(CELL_EFAULT);
    std::lock_guard lock(u.lock);
    if (!u.init) return err(CELL_EINVAL);
    return u.ldds.erase(product) ? CELL_OK : err(CELL_ESRCH);
}

// Gives back what sys_usbd_allocate_memory took (u.lock held).
void free_memory(Kernel& k, Usbd& u, std::map<u32, u64>::iterator it)
{
    Kernel::Impl& s = k.impl();
    std::lock_guard ml(s.memory.mutex);
    s.memory.free_kernel(it->first);
    u.memory.erase(it);
}

// The calls that name a device or a pipe: none exists. CELL_EINVAL (also before sys_usbd_initialize, as RPCS3).
Result no_device(PpuThread& t, std::string_view what, u32 id)
{
    PS_LOG_DEBUG(kUsbdChan, "{}({:#x}): no USB device is attached", what, id);
    (void)t;
    return err(CELL_EINVAL);
}

} // namespace

// sys_usbd_initialize(u32* handle): the handle (RPCS3's value). A second one while initialised is RPCS3's assertion;
// here CELL_OK with the same handle (lv2 allows several, libusbd makes one).
Result sys_usbd_initialize(PpuThread& t, u32 handle)
{
    Usbd& u = usbd(t.kernel);
    if (!t.kernel.impl().guest.valid(handle, 4)) return err(CELL_EFAULT);
    {
        std::lock_guard lock(u.lock);
        u.init = true;
    }
    t.kernel.impl().guest.w32(handle, kHandle);
    PS_LOG_INFO(kUsbdChan, "sys_usbd_initialize: handle {:#x}, no USB device attached", kHandle);
    return CELL_OK;
}

// sys_usbd_finalize(handle): every thread waiting for an event wakes with the termination signal (4, 0, 0).
Result sys_usbd_finalize(PpuThread& t, u32)
{
    Usbd& u = usbd(t.kernel);
    std::lock_guard lock(u.lock);
    u.init = false;
    u.ldds.clear();
    u.events.clear();
    while (!u.memory.empty()) free_memory(t.kernel, u, u.memory.begin());
    for (Waiter* w : u.waiters.take_all(SYS_SYNC_FIFO)) {
        w->data[0] = SYS_USBD_TERMINATE;
        w->data[1] = 0;
        w->data[2] = 0;
        wake(*w, CELL_OK);
    }
    return CELL_OK;
}

// sys_usbd_get_device_list(handle, list, max): the number of devices copied: none.
Result sys_usbd_get_device_list(PpuThread& t, u32, u32, u32)
{
    Usbd& u = usbd(t.kernel);
    std::lock_guard lock(u.lock);
    return u.init ? CELL_OK : err(CELL_EINVAL);
}

Result sys_usbd_register_extra_ldd(PpuThread& t, u32, u32 name, u32 len, u32 vendor, u32 pmin, u32 pmax)
{
    return register_extra(t.kernel, name, static_cast<u16>(len), static_cast<u16>(vendor), static_cast<u16>(pmin),
        static_cast<u16>(pmax));
}

Result sys_usbd_unregister_extra_ldd(PpuThread& t, u32, u32 name, u32 len)
{
    return unregister_extra(t.kernel, name, static_cast<u16>(len));
}

// sys_usbd_register_ldd(handle, name, len): the drivers RPCS3 knows by name are registered with their devices; any
// other name is accepted and kept with no device range (RPCS3 answers CELL_OK and keeps nothing).
Result sys_usbd_register_ldd(PpuThread& t, u32, u32 name, u32 len)
{
    std::string product;
    if (!read_name(t.kernel.impl().guest, name, static_cast<u16>(len), product)) return err(CELL_EFAULT);
    if (const auto it = kKnownLdds.find(product); it != kKnownLdds.end()) {
        return register_extra(t.kernel, name, static_cast<u16>(len), it->second.vendor, it->second.product_min,
            it->second.product_max);
    }
    Usbd& u = usbd(t.kernel);
    std::lock_guard lock(u.lock);
    if (!u.init) return CELL_OK; // RPCS3 does not check
    u.ldds.emplace(product, Ldd{});
    return CELL_OK;
}

Result sys_usbd_unregister_ldd(PpuThread& t, u32, u32 name, u32 len)
{
    return unregister_extra(t.kernel, name, static_cast<u16>(len));
}

Result sys_usbd_get_descriptor_size(PpuThread& t, u32, u32 device)
{
    return no_device(t, "sys_usbd_get_descriptor_size", device);
}

Result sys_usbd_get_descriptor(PpuThread& t, u32, u32 device, u32 descriptor, u32)
{
    if (!descriptor) return err(CELL_EINVAL);
    return no_device(t, "sys_usbd_get_descriptor", device);
}

Result sys_usbd_open_pipe(PpuThread& t, u32, u32 device, u32, u64, u64, u32, u64)
{
    return no_device(t, "sys_usbd_open_pipe", device);
}

Result sys_usbd_open_default_pipe(PpuThread& t, u32, u32 device)
{
    return no_device(t, "sys_usbd_open_default_pipe", device);
}

Result sys_usbd_close_pipe(PpuThread& t, u32, u32 pipe) { return no_device(t, "sys_usbd_close_pipe", pipe); }

// sys_usbd_receive_event(handle, u64* arg1, u64* arg2, u64* arg3): the next event the library sent itself, else a wait
// until one comes or sys_usbd_finalize ends it (4, 0, 0). CELL_EINVAL before sys_usbd_initialize.
Result sys_usbd_receive_event(PpuThread& t, u32 handle, u32 arg1, u32 arg2, u32 arg3)
{
    Kernel::Impl& s = t.kernel.impl();
    if (!s.guest.valid(arg1, 8) || !s.guest.valid(arg2, 8) || !s.guest.valid(arg3, 8)) return err(CELL_EFAULT);
    Usbd& u = usbd(t.kernel);
    UsbdEvent ev;
    {
        std::unique_lock lk(u.lock);
        if (!u.init) return err(CELL_EINVAL);
        if (!u.events.empty()) {
            ev = u.events.front();
            u.events.pop_front();
        } else {
            Waiter& w = t.waiter;
            prepare_wait(w);
            u.waiters.push(&w);
            const WaitStatus ws = park(w, lk, 0, {WaitKind::Usbd, handle},
                [&](bool) { return u.waiters.remove(&w) ? CancelResult::Removed : CancelResult::Woken; });
            if (ws != WaitStatus::Woken) return finish(t, err(CELL_EABORT));
            ev = {w.data[0], w.data[1], w.data[2]};
        }
    }
    s.guest.w64(arg1, ev.arg1);
    s.guest.w64(arg2, ev.arg2);
    s.guest.w64(arg3, ev.arg3);
    return CELL_OK;
}

// sys_usbd_event_port_send(handle, arg1, arg2, arg3): an event for a waiting thread (or the next to wait).
Result sys_usbd_event_port_send(PpuThread& t, u32, u64 arg1, u64 arg2, u64 arg3)
{
    Usbd& u = usbd(t.kernel);
    std::lock_guard lock(u.lock);
    if (!u.init) return err(CELL_EINVAL);
    if (Waiter* w = u.waiters.pop(SYS_SYNC_FIFO)) {
        w->data[0] = arg1;
        w->data[1] = arg2;
        w->data[2] = arg3;
        wake(*w, CELL_OK);
    } else {
        u.events.push_back({arg1, arg2, arg3});
    }
    return CELL_OK;
}

Result sys_usbd_transfer_data(PpuThread& t, u32, u32 pipe, u32, u32, u32, u32)
{
    return no_device(t, "sys_usbd_transfer_data", pipe);
}

Result sys_usbd_isochronous_transfer_data(PpuThread& t, u32, u32 pipe, u32)
{
    return no_device(t, "sys_usbd_isochronous_transfer_data", pipe);
}

Result sys_usbd_get_transfer_status(PpuThread& t, u32, u32 transfer, u32, u32, u32)
{
    return no_device(t, "sys_usbd_get_transfer_status", transfer);
}

Result sys_usbd_get_isochronous_transfer_status(PpuThread& t, u32, u32 transfer, u32, u32, u32)
{
    return no_device(t, "sys_usbd_get_isochronous_transfer_status", transfer);
}

Result sys_usbd_get_device_location(PpuThread& t, u32, u32 device, u32)
{
    return no_device(t, "sys_usbd_get_device_location", device);
}

Result sys_usbd_get_device_speed(PpuThread& t, u32, u32 device, u32)
{
    return no_device(t, "sys_usbd_get_device_speed", device);
}

// sys_usbd_detect_event(): a scan for devices that changed: none can (RPCS3 answers CELL_OK).
Result sys_usbd_detect_event(PpuThread& t)
{
    Usbd& u = usbd(t.kernel);
    std::lock_guard lock(u.lock);
    return u.init ? CELL_OK : err(CELL_EINVAL);
}

// sys_usbd_attach(handle, ldd, device id high, device id low): libusbd hands a device its driver probed; none exists.
Result sys_usbd_attach(PpuThread& t, u32, u32, u32 device_high, u32 device_low)
{
    return no_device(t, "sys_usbd_attach", device_high << 16 | (device_low & 0xFFFF));
}

// sys_usbd_send_event(handle, device): what libusbd's cellUsbdResetDevice calls (RPCS3's name for it); no device.
Result sys_usbd_send_event(PpuThread& t, u32, u32 device) { return no_device(t, "sys_usbd_send_event", device); }

// sys_usbd_allocate_memory(handle, u32* addr, size): memory for transfers (see the top of the file).
Result sys_usbd_allocate_memory(PpuThread& t, u32, u32 out, u64 size)
{
    Kernel::Impl& s = t.kernel.impl();
    Usbd& u = usbd(t.kernel);
    if (!size || size > 0x10000000) return err(CELL_EINVAL);
    if (!s.guest.valid(out, 4)) return err(CELL_EFAULT);
    std::lock_guard lock(u.lock);
    if (!u.init) return err(CELL_EINVAL);
    const u64 bytes = (size + 0xFFFF) & ~u64{0xFFFF};
    u32 addr = 0;
    {
        std::lock_guard ml(s.memory.mutex);
        addr = s.memory.allocate_kernel(bytes);
    }
    if (!addr) return err(CELL_ENOMEM);
    u.memory[addr] = bytes;
    s.guest.w32(out, addr);
    return CELL_OK;
}

// sys_usbd_free_memory(handle, addr): what sys_usbd_allocate_memory gave; anything else is CELL_EINVAL.
Result sys_usbd_free_memory(PpuThread& t, u32, u32 addr)
{
    Usbd& u = usbd(t.kernel);
    std::lock_guard lock(u.lock);
    const auto it = u.memory.find(addr);
    if (it == u.memory.end()) return err(CELL_EINVAL);
    free_memory(t.kernel, u, it);
    return CELL_OK;
}

void register_usbd_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 530, sys_usbd_initialize);
    PS_LV2_BIND(table, 531, sys_usbd_finalize);
    PS_LV2_BIND(table, 532, sys_usbd_get_device_list);
    PS_LV2_BIND(table, 533, sys_usbd_get_descriptor_size);
    PS_LV2_BIND(table, 534, sys_usbd_get_descriptor);
    PS_LV2_BIND(table, 535, sys_usbd_register_ldd);
    PS_LV2_BIND(table, 536, sys_usbd_unregister_ldd);
    PS_LV2_BIND(table, 537, sys_usbd_open_pipe);
    PS_LV2_BIND(table, 538, sys_usbd_open_default_pipe);
    PS_LV2_BIND(table, 539, sys_usbd_close_pipe);
    PS_LV2_BIND(table, 540, sys_usbd_receive_event);
    PS_LV2_BIND(table, 541, sys_usbd_detect_event);
    PS_LV2_BIND(table, 542, sys_usbd_attach);
    PS_LV2_BIND(table, 543, sys_usbd_transfer_data);
    PS_LV2_BIND(table, 544, sys_usbd_isochronous_transfer_data);
    PS_LV2_BIND(table, 545, sys_usbd_get_transfer_status);
    PS_LV2_BIND(table, 546, sys_usbd_get_isochronous_transfer_status);
    PS_LV2_BIND(table, 547, sys_usbd_get_device_location);
    PS_LV2_BIND(table, 548, sys_usbd_send_event);
    PS_LV2_BIND(table, 549, sys_usbd_event_port_send);
    PS_LV2_BIND(table, 550, sys_usbd_allocate_memory);
    PS_LV2_BIND(table, 551, sys_usbd_free_memory);
    PS_LV2_BIND(table, 556, sys_usbd_get_device_speed);
    PS_LV2_BIND(table, 558, sys_usbd_unregister_extra_ldd);
    PS_LV2_BIND(table, 559, sys_usbd_register_extra_ldd);
}

} // namespace pspocket::lv2
