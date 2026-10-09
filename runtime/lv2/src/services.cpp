// lv2's system services: the HID manager, sys_config's service registry and raw storage (D-527). References: RPCS3
// rpcs3/Emu/Cell/lv2/sys_hid.cpp/.h, sys_config.cpp/.h, sys_storage.cpp/.h, and its syscall table's permission column
// (lv2.cpp). The calls it marks ROOT answer CELL_ENOSYS to a game, as in devices.cpp.
//
// HID manager. Handles count up from 0x100 (RPCS3's observation of real hardware). pspocket's pads, keyboards and mice
// are served by the native libio (cellPad, cellKb, cellMouse): lv2 has no input state of its own, so a root process
// reading the manager gets nothing (RPCS3 forwards cellPadGetData). The ioctl answers RPCS3 recorded on real hardware:
// package 2 a DualShock 3's descriptor (vendor 054C, product 0268), package 5 its IDs (little-endian there).
//
// sys_config. A process opens a handle on one of its event queues; it registers services (an ID, a user ID, a
// verbosity and data) and listens for services by ID and minimum verbosity. Each match makes a service event, sent to
// the listener's queue as (1, handle, registered << 32 | event ID, the event's size) and read with
// sys_config_get_service_event. lv2 registers the pad manager (services 0x11 and 0x12) with one controller's HID data
// at the first open, as RPCS3 does; a listener of service 0x11 hears of it only when its data starts with 1, as RPCS3
// saw on real hardware. A listener of type ONCE gets one event. I/O error events: none on this console.
//
// Raw storage. Opening a device is for root processes; pspocket gives them RPCS3's devices (the hard disk's three
// regions, the Blu-ray drive, the flash's regions) without their contents: reads give zeros, writes are taken. The
// calls that name an open device find none for a game.

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "internal.hpp"

namespace pspocket::lv2 {

namespace {

constexpr std::string_view kSvcChan = "lv2";

Result root_only(PpuThread& t, std::string_view name)
{
    if (t.kernel.config().root) return CELL_OK;
    static std::mutex m;
    static std::map<std::string_view, bool> logged;
    std::lock_guard lock(m);
    if (!logged[name]) {
        logged[name] = true;
        PS_LOG_WARN(kSvcChan, "{}: a root process's call, not a game's: CELL_ENOSYS", name);
    }
    return err(CELL_ENOSYS);
}

// ------------------------------------------------------------------------------------------------- state

constexpr u64 SYS_CONFIG_SERVICE_PADMANAGER = 0x11, SYS_CONFIG_SERVICE_PADMANAGER2 = 0x12;
constexpr u32 SYS_CONFIG_SERVICE_LISTENER_ONCE = 0;
constexpr u64 SYS_CONFIG_EVENT_SOURCE_SERVICE = 1;
constexpr u32 kConfigHandleBase = 0x41000000, kConfigListenerBase = 0x42000000, kConfigServiceBase = 0x43000000;
constexpr u32 kStorageBase = 0x45000000;
// sizeof(sys_config_service_event_t) is 48 (its u64 fields align it to 8): an event is 47 bytes plus its data.
constexpr u64 kServiceEventSize = 47;

struct ConfigService {
    u32 id = 0;
    u64 service_id = 0, user_id = 0, verbosity = 0;
    u32 padding = 0;
    std::vector<u8> data;
    u64 timestamp = 0;
    bool registered = true;
    u64 size() const { return kServiceEventSize + data.size(); }
};

struct ConfigListener {
    u32 id = 0, handle = 0;
    u64 service_id = 0, min_verbosity = 0;
    u32 type = 0;
    std::vector<u8> data;
    u32 events = 0; // service events made for it
};

struct ConfigEvent {
    u32 id = 0, handle = 0, listener = 0;
    std::shared_ptr<ConfigService> service;
};

struct Storage {
    u64 device = 0, mode = 0, flags = 0;
};

struct Services {
    std::mutex lock;
    u32 next_hid_handle = 0x100;
    // sys_config
    bool config_initialized = false;
    std::map<u32, std::shared_ptr<EventQueue>> config_handles;
    std::map<u32, std::shared_ptr<ConfigService>> config_services;
    std::map<u32, ConfigListener> config_listeners;
    std::map<u32, ConfigEvent> config_events;
    u32 next_config_handle = 0, next_config_listener = 0, next_config_service = 0, next_config_event = 0;
    u64 config_clock = 0; // orders services as they were registered (RPCS3 sorts them by time)
    // sys_storage
    std::map<u32, Storage> storages;
    u32 next_storage = 0;
    std::weak_ptr<EventQueue> storage_async_queue;
};

Services& services(Kernel& k)
{
    Kernel::Impl& s = k.impl();
    std::lock_guard lock(s.system_mutex);
    if (!s.services_state) s.services_state = std::make_shared<Services>();
    return *static_cast<Services*>(s.services_state.get());
}

// An ID of a type numbered from `base`, `step` apart, 2048 of them (RPCS3's id_count); 0 when all are taken.
template <class Map>
u32 new_id(const Map& m, u32 base, u32 step, u32& next)
{
    for (u32 tries = 0; tries < 2048; tries++) {
        const u32 id = base + next * step;
        next = (next + 1) % 2048;
        if (!m.contains(id)) return id;
    }
    return 0;
}

// ------------------------------------------------------------------------------------------------- sys_config

bool config_matches(const ConfigListener& l, const ConfigService& s)
{
    if (l.type == SYS_CONFIG_SERVICE_LISTENER_ONCE && l.events) return false;
    if (l.service_id != s.service_id || l.min_verbosity > s.verbosity) return false;
    if (l.service_id == SYS_CONFIG_SERVICE_PADMANAGER && (l.data.empty() || l.data[0] != 1)) return false;
    return true;
}

// A service event for listener `l`, sent to its handle's queue (svc.lock held). A queue that refuses it (full or gone)
// leaves no event behind.
void config_notify(Services& svc, ConfigListener& l, const std::shared_ptr<ConfigService>& s)
{
    if (!config_matches(l, *s)) return;
    const auto h = svc.config_handles.find(l.handle);
    if (h == svc.config_handles.end()) return;
    const u32 id = svc.next_config_event++;
    svc.config_events[id] = ConfigEvent{id, l.handle, l.id, s};
    l.events++;
    const u64 d2 = (u64{s->registered} << 32) | id;
    if (h->second->send(Event{SYS_CONFIG_EVENT_SOURCE_SERVICE, l.handle, d2, s->size()}) != CELL_OK) {
        svc.config_events.erase(id);
        l.events--;
    }
}

void config_notify_listeners(Services& svc, const std::shared_ptr<ConfigService>& s)
{
    for (auto& [id, l] : svc.config_listeners) config_notify(svc, l, s);
}

std::shared_ptr<ConfigService> config_service(Services& svc, u64 service_id, u64 user_id, u64 verbosity, u32 padding,
    std::vector<u8> data)
{
    const u32 id = new_id(svc.config_services, kConfigServiceBase, 0x100, svc.next_config_service);
    if (!id) return nullptr;
    auto s = std::make_shared<ConfigService>();
    s->id = id;
    s->service_id = service_id;
    s->user_id = user_id;
    s->verbosity = verbosity;
    s->padding = padding;
    s->data = std::move(data);
    s->timestamp = svc.config_clock++;
    svc.config_services[id] = s;
    return s;
}

// The pad manager lv2 registers at the first open (RPCS3 lv2_config::initialize): one controller's HID data.
void config_initialize(Services& svc)
{
    if (svc.config_initialized) return;
    svc.config_initialized = true;
    static constexpr u8 kHidInfo[0x1A] = {0x01, 0x01, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x4C, 0x02,
        0x68, 0x00, 0x10, 0x91, 0x88, 0x04, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00};
    for (const u64 sid : {SYS_CONFIG_SERVICE_PADMANAGER, SYS_CONFIG_SERVICE_PADMANAGER2}) {
        if (auto s = config_service(svc, sid, 0, 1, 0, {std::begin(kHidInfo), std::end(kHidInfo)})) {
            config_notify_listeners(svc, s);
        }
    }
}

void config_drop_listener_events(Services& svc, u32 listener)
{
    std::erase_if(svc.config_events, [&](const auto& e) { return e.second.listener == listener; });
}

bool read_bytes(const Guest& g, u32 addr, u64 size, std::vector<u8>& out)
{
    out.clear();
    if (!size) return true;
    if (size > 0x100000 || !g.valid(addr, size)) return false;
    out.assign(g.ptr(addr), g.ptr(addr) + size);
    return true;
}

} // namespace

// ------------------------------------------------------------------------------------------------- sys_hid

// sys_hid_manager_open(device_type, port_no, u32* handle): 1 a pad, 2 a keyboard, 3 a mouse.
Result sys_hid_manager_open(PpuThread& t, u64 device_type, u64, u32 handle)
{
    if (device_type > 3) return err(CELL_EINVAL);
    if (!handle || !t.kernel.impl().guest.valid(handle, 4)) return err(CELL_EFAULT);
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    t.kernel.impl().guest.w32(handle, svc.next_hid_handle++);
    return CELL_OK;
}

// sys_hid_manager_close(handle): lv2 keeps nothing per handle.
Result sys_hid_manager_close(PpuThread&, u32) { return CELL_OK; }

// sys_hid_manager_read(handle, pkg_id, buf, size): root; no input state in lv2 (see the top): nothing read.
Result sys_hid_manager_read(PpuThread& t, u32, u32, u32 buf, u64)
{
    if (const Result r = root_only(t, "sys_hid_manager_read"); r != CELL_OK) return r;
    if (!buf) return err(CELL_EFAULT);
    return CELL_OK;
}

// sys_hid_manager_ioctl(handle, pkg_id, buf, size).
Result sys_hid_manager_ioctl(PpuThread& t, u32, u32 pkg_id, u32 buf, u64)
{
    const Guest& g = t.kernel.impl().guest;
    if (pkg_id == 2) {
        static constexpr u8 kRealHw[17] = {0x01, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x03,
            0x50, 0x00, 0x00, 0x1C, 0x1F};
        if (!g.valid(buf, 21)) return err(CELL_EFAULT);
        g.w16(buf, 0x054C);
        g.w16(buf + 2, 0x0268);
        std::memcpy(g.ptr(buf + 4), kRealHw, sizeof(kRealHw));
    } else if (pkg_id == 5) {
        if (!g.valid(buf, 4)) return err(CELL_EFAULT);
        g.w8(buf, 0x4C); // little-endian there
        g.w8(buf + 1, 0x05);
        g.w8(buf + 2, 0x68);
        g.w8(buf + 3, 0x02);
    }
    return CELL_OK;
}

Result sys_hid_manager_map_logical_id_to_port_id(PpuThread& t)
{
    return root_only(t, "sys_hid_manager_map_logical_id_to_port_id");
}
Result sys_hid_manager_unmap_logical_id_to_port_id(PpuThread& t)
{
    return root_only(t, "sys_hid_manager_unmap_logical_id_to_port_id");
}
Result sys_hid_manager_add_hot_key_observer(PpuThread& t, u32, u32)
{
    return root_only(t, "sys_hid_manager_add_hot_key_observer");
}
Result sys_hid_manager_remove_hot_key_observer(PpuThread& t)
{
    return root_only(t, "sys_hid_manager_remove_hot_key_observer");
}
Result sys_hid_manager_grab_focus(PpuThread& t) { return root_only(t, "sys_hid_manager_grab_focus"); }
Result sys_hid_manager_release_focus(PpuThread& t) { return root_only(t, "sys_hid_manager_release_focus"); }
Result sys_hid_manager_set_master_process(PpuThread& t) { return root_only(t, "sys_hid_manager_set_master_process"); }

// sys_hid_manager_check_focus(): the game has the input focus (1).
Result sys_hid_manager_check_focus(PpuThread&) { return 1; }

// sys_hid_manager_is_process_permission_root(pid): root; 1.
Result sys_hid_manager_is_process_permission_root(PpuThread& t, u32)
{
    if (const Result r = root_only(t, "sys_hid_manager_is_process_permission_root"); r != CELL_OK) return r;
    return 1;
}

Result sys_hid_manager_513(PpuThread&, u64, u64, u32, u64) { return CELL_OK; }
Result sys_hid_manager_514(PpuThread&, u32, u32, u64) { return CELL_OK; }

// ------------------------------------------------------------------------------------------------- sys_config

// sys_config_open(equeue, u32* handle).
Result sys_config_open(PpuThread& t, u32 equeue, u32 out)
{
    auto q = t.kernel.event_queue(equeue);
    if (!q) return err(CELL_ESRCH);
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    config_initialize(svc);
    const u32 id = new_id(svc.config_handles, kConfigHandleBase, 0x100, svc.next_config_handle);
    if (!id) return err(CELL_EAGAIN);
    if (!t.kernel.impl().guest.put32(out, id)) return err(CELL_EFAULT);
    svc.config_handles[id] = std::move(q);
    return CELL_OK;
}

// sys_config_close(handle): its listeners and their events go with it.
Result sys_config_close(PpuThread& t, u32 handle)
{
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    if (!svc.config_handles.erase(handle)) return err(CELL_ESRCH);
    for (auto it = svc.config_listeners.begin(); it != svc.config_listeners.end();) {
        if (it->second.handle != handle) {
            ++it;
            continue;
        }
        config_drop_listener_events(svc, it->first);
        it = svc.config_listeners.erase(it);
    }
    return CELL_OK;
}

// sys_config_get_service_event(handle, event_id, sys_config_service_event_t* dst, size).
Result sys_config_get_service_event(PpuThread& t, u32 handle, u32 event_id, u32 dst, u64 size)
{
    const Guest& g = t.kernel.impl().guest;
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    if (!svc.config_handles.contains(handle)) return err(CELL_ESRCH);
    const auto it = svc.config_events.find(event_id);
    if (it == svc.config_events.end() || it->second.handle != handle) return err(CELL_ESRCH);
    const ConfigEvent& e = it->second;
    const ConfigService& s = *e.service;
    if (s.size() > size) return err(CELL_EAGAIN);
    if (!g.valid(dst, s.registered ? 40 + s.data.size() : 24)) return err(CELL_EFAULT);
    g.w32(dst, e.listener);
    g.w32(dst + 4, s.registered ? 1 : 0);
    g.w64(dst + 8, s.service_id);
    g.w64(dst + 16, s.user_id);
    if (s.registered) {
        g.w64(dst + 24, s.verbosity);
        g.w32(dst + 32, static_cast<u32>(s.data.size()));
        g.w32(dst + 36, s.padding);
        if (!s.data.empty()) std::memcpy(g.ptr(dst + 40), s.data.data(), s.data.size());
    }
    return CELL_OK;
}

// sys_config_add_service_listener(handle, service_id, min_verbosity, in, size, type, u32* listener): it hears of the
// services registered so far, oldest first.
Result sys_config_add_service_listener(PpuThread& t, u32 handle, u64 service_id, u64 min_verbosity, u32 in, u64 size,
    u32 type, u32 out)
{
    const Guest& g = t.kernel.impl().guest;
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    if (!svc.config_handles.contains(handle)) return err(CELL_ESRCH);
    std::vector<u8> data;
    if (!read_bytes(g, in, size, data)) return err(CELL_EFAULT);
    if (!g.valid(out, 4)) return err(CELL_EFAULT);
    const u32 id = new_id(svc.config_listeners, kConfigListenerBase, 0x100, svc.next_config_listener);
    if (!id) return err(CELL_EAGAIN);
    ConfigListener& l = svc.config_listeners[id];
    l = ConfigListener{id, handle, service_id, min_verbosity, type, std::move(data), 0};
    std::vector<std::shared_ptr<ConfigService>> past;
    for (const auto& [sid, s] : svc.config_services) past.push_back(s);
    std::sort(past.begin(), past.end(), [](const auto& a, const auto& b) { return a->timestamp < b->timestamp; });
    for (const auto& s : past) config_notify(svc, l, s);
    g.w32(out, id);
    return CELL_OK;
}

// sys_config_remove_service_listener(handle, listener).
Result sys_config_remove_service_listener(PpuThread& t, u32, u32 listener)
{
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    if (!svc.config_listeners.erase(listener)) return err(CELL_ESRCH);
    config_drop_listener_events(svc, listener);
    return CELL_OK;
}

// sys_config_register_service(handle, service_id, user_id, verbosity, data, size, u32* service).
Result sys_config_register_service(PpuThread& t, u32 handle, u64 service_id, u64 user_id, u64 verbosity, u32 data,
    u64 size, u32 out)
{
    const Guest& g = t.kernel.impl().guest;
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    if (!svc.config_handles.contains(handle)) return err(CELL_ESRCH);
    std::vector<u8> bytes;
    if (!read_bytes(g, data, size, bytes)) return err(CELL_EFAULT);
    if (!g.valid(out, 4)) return err(CELL_EFAULT);
    auto s = config_service(svc, service_id, user_id, verbosity, 0, std::move(bytes));
    if (!s) return err(CELL_EAGAIN);
    config_notify_listeners(svc, s);
    g.w32(out, s->id);
    return CELL_OK;
}

// sys_config_unregister_service(handle, service): the listeners hear of it (registered 0).
Result sys_config_unregister_service(PpuThread& t, u32, u32 service)
{
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    const auto it = svc.config_services.find(service);
    if (it == svc.config_services.end()) return err(CELL_ESRCH);
    std::shared_ptr<ConfigService> s = it->second;
    svc.config_services.erase(it);
    s->registered = false;
    config_notify_listeners(svc, s);
    return CELL_OK;
}

// I/O error events: none on this console.
Result sys_config_get_io_event(PpuThread&, u32, u32, u32, u64) { return CELL_OK; }
Result sys_config_register_io_error_listener(PpuThread&, u32) { return CELL_OK; }
Result sys_config_unregister_io_error_listener(PpuThread&, u32) { return CELL_OK; }

// ------------------------------------------------------------------------------------------------- sys_storage

constexpr u64 ATA_HDD = 0x101000000000007, BDVD_DRIVE = 0x101000000000006, NAND_FLASH = 0x100000000000001,
              NAND_UNK = 0x100000000000003, NOR_FLASH = 0x100000000000004, USB_MASS_STORAGE_1 = 0x10300000000000A;

// sys_storage_open(device, mode, u32* fd, flags): root.
Result sys_storage_open(PpuThread& t, u64 device, u64 mode, u32 fd, u64 flags)
{
    if (const Result r = root_only(t, "sys_storage_open"); r != CELL_OK) return r;
    if (device == 0) return err(CELL_ENOENT);
    if (!fd || !t.kernel.impl().guest.valid(fd, 4)) return err(CELL_EFAULT);
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    const u32 id = new_id(svc.storages, kStorageBase, 1, svc.next_storage);
    if (!id) return err(CELL_EAGAIN);
    svc.storages[id] = Storage{device, mode, flags};
    t.kernel.impl().guest.w32(fd, id);
    return CELL_OK;
}

// sys_storage_close(fd): CELL_OK, open or not (RPCS3).
Result sys_storage_close(PpuThread& t, u32 fd)
{
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    svc.storages.erase(fd);
    return CELL_OK;
}

// sys_storage_read(fd, mode, start_sector, num_sectors, buf, u32* sectors_read, flags): zeros (no device contents);
// the buffer is cleared before the device is looked for, as RPCS3 does.
Result sys_storage_read(PpuThread& t, u32 fd, u32, u32, u32 num_sectors, u32 buf, u32 sectors_read, u64)
{
    const Guest& g = t.kernel.impl().guest;
    if (!buf || !sectors_read) return err(CELL_EFAULT);
    const u64 bytes = u64{num_sectors} * 0x200;
    if (!g.valid(buf, bytes) || !g.valid(sectors_read, 4)) return err(CELL_EFAULT);
    std::memset(g.ptr(buf), 0, bytes);
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    if (!svc.storages.contains(fd)) return err(CELL_ESRCH);
    g.w32(sectors_read, num_sectors);
    return CELL_OK;
}

// sys_storage_write(fd, mode, start_sector, num_sectors, data, u32* sectors_wrote, flags): taken whole.
Result sys_storage_write(PpuThread& t, u32 fd, u32, u32, u32 num_sectors, u32, u32 sectors_wrote, u64)
{
    const Guest& g = t.kernel.impl().guest;
    if (!sectors_wrote || !g.valid(sectors_wrote, 4)) return err(CELL_EFAULT);
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    if (!svc.storages.contains(fd)) return err(CELL_ESRCH);
    g.w32(sectors_wrote, num_sectors);
    return CELL_OK;
}

// Device commands: no device to run them.
Result sys_storage_send_device_command(PpuThread&, u32, u64, u32, u64, u32, u64) { return CELL_OK; }

// sys_storage_async_configure(fd, io_buf, equeue, unk): the queue the asynchronous commands answer on.
Result sys_storage_async_configure(PpuThread& t, u32, u32, u32 equeue, u32)
{
    auto q = t.kernel.event_queue(equeue);
    if (!q) return err(CELL_ESRCH);
    Services& svc = services(t.kernel);
    std::lock_guard lock(svc.lock);
    svc.storage_async_queue = q;
    return CELL_OK;
}

Result sys_storage_async_read(PpuThread&) { return CELL_OK; }
Result sys_storage_async_write(PpuThread&) { return CELL_OK; }
Result sys_storage_async_cancel(PpuThread&) { return CELL_OK; }

// sys_storage_get_device_info(device, StorageDeviceInfo* buffer): root; RPCS3's devices (sizes from a console's
// kernel); a region past a device's last answers -5.
Result sys_storage_get_device_info(PpuThread& t, u64 device, u32 buffer)
{
    if (const Result r = root_only(t, "sys_storage_get_device_info"); r != CELL_OK) return r;
    const Guest& g = t.kernel.impl().guest;
    if (!buffer || !g.valid(buffer, 0x40)) return err(CELL_EFAULT);
    std::memset(g.ptr(buffer), 0, 0x40);
    const u64 storage = device & 0xFFFFF00FFFFFFFF;
    const u32 dev_num = static_cast<u32>(device >> 32) & 0xFF;
    const auto fill = [&](u64 sectors, u32 sector_size, u8 flag1) {
        std::memcpy(g.ptr(buffer), "unnamed", 7);
        g.w64(buffer + 0x28, sectors);
        g.w32(buffer + 0x30, sector_size);
        g.w32(buffer + 0x34, 1);
        g.w8(buffer + 0x38 + 1, flag1);
        g.w8(buffer + 0x38 + 2, 1);
        g.w8(buffer + 0x38 + 7, 1);
    };
    if (storage == ATA_HDD) {
        if (dev_num > 2) return -5;
        static constexpr u64 kSectors[] = {0x2542EAB0, 0x24FAEA98, 0x3FFFF8};
        fill(kSectors[dev_num], 0x200, 1);
    } else if (storage == BDVD_DRIVE) {
        if (dev_num > 0) return -5;
        fill(0x4D955, 0x800, 0);
    } else if (storage == USB_MASS_STORAGE_1) {
        if (dev_num > 0) return -5;
        fill(0, 0x200, 0);
    } else if (storage == NAND_FLASH) {
        if (dev_num > 6) return -5;
        static constexpr u64 kSectors[] = {0x80000, 0x75F8, 0x63E00, 0x8000, 0x400, 0x2000, 0x200};
        fill(kSectors[dev_num], 0x200, 1);
    } else if (storage == NOR_FLASH) {
        if (dev_num > 3) return -5;
        static constexpr u64 kSectors[] = {0x8000, 0x77F8, 0x100, 0x400};
        fill(kSectors[dev_num], 0x200, 0);
    } else if (storage == NAND_UNK) {
        if (dev_num > 1) return -5;
        fill(dev_num == 0 ? 0x7FFFFFFF : 0, 0x800, 0);
    } else {
        PS_LOG_ERROR(kSvcChan, "sys_storage_get_device_info({:#x}): unknown device, nothing reported", device);
    }
    return CELL_OK;
}

// sys_storage_get_device_config(u32* storages, u32* devices): root; 6 storages, 17 devices.
Result sys_storage_get_device_config(PpuThread& t, u32 storages, u32 devs)
{
    if (const Result r = root_only(t, "sys_storage_get_device_config"); r != CELL_OK) return r;
    const Guest& g = t.kernel.impl().guest;
    if (!g.put32(storages, 6)) return err(CELL_EFAULT);
    if (!g.put32(devs, 17)) return err(CELL_EFAULT);
    return CELL_OK;
}

// sys_storage_report_devices(storages, start, devices, u64* device_ids): root; the 17 device IDs.
Result sys_storage_report_devices(PpuThread& t, u32, u32 start, u32 devs, u32 device_ids)
{
    if (const Result r = root_only(t, "sys_storage_report_devices"); r != CELL_OK) return r;
    const Guest& g = t.kernel.impl().guest;
    if (!device_ids) return err(CELL_EFAULT);
    std::array<u64, 17> all{};
    all[0] = 0x10300000000000A;
    for (u64 i = 0; i < 7; i++) all[1 + i] = 0x100000000000001 | (i << 32);
    for (u64 i = 0; i < 3; i++) all[8 + i] = 0x101000000000007 | (i << 32);
    all[11] = 0x101000000000006;
    for (u64 i = 0; i < 4; i++) all[12 + i] = 0x100000000000004 | (i << 32);
    all[16] = 0x100000000000003;
    if (!devs || start >= all.size() || devs > all.size() - start) return err(CELL_EINVAL);
    if (!g.valid(device_ids, u64{devs} * 8)) return err(CELL_EFAULT);
    for (u32 i = 0; i < devs; i++) g.w64(device_ids + 8 * i, all[start + i]);
    return CELL_OK;
}

Result sys_storage_configure_medium_event(PpuThread& t, u32, u32, u32)
{
    return root_only(t, "sys_storage_configure_medium_event");
}
Result sys_storage_set_medium_polling_interval(PpuThread&) { return CELL_OK; }
Result sys_storage_create_region(PpuThread&) { return CELL_OK; }
Result sys_storage_delete_region(PpuThread&) { return CELL_OK; }
Result sys_storage_execute_device_command(PpuThread&, u32, u64, u32, u64, u32, u64, u32) { return CELL_OK; }
Result sys_storage_check_region_acl(PpuThread&) { return CELL_OK; }
Result sys_storage_set_region_acl(PpuThread&) { return CELL_OK; }
Result sys_storage_get_region_offset(PpuThread&) { return CELL_OK; }

// sys_storage_set_emulated_speed: a debug kernel's (RPCS3: CELL_ENOSYS).
Result sys_storage_set_emulated_speed(PpuThread&) { return err(CELL_ENOSYS); }

// sys_storage_async_send_device_command(dev, cmd, in, inlen, out, outlen, unk): the configured queue hears (0, unk,
// unk, unk) at once.
Result sys_storage_async_send_device_command(PpuThread& t, u32, u64, u32, u64, u32, u64, u64 unk)
{
    Services& svc = services(t.kernel);
    std::shared_ptr<EventQueue> q;
    {
        std::lock_guard lock(svc.lock);
        q = svc.storage_async_queue.lock();
    }
    if (q) q->send(Event{0, unk, unk, unk});
    return CELL_OK;
}

void register_service_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 500, sys_hid_manager_open);
    PS_LV2_BIND(table, 501, sys_hid_manager_close);
    PS_LV2_BIND(table, 502, sys_hid_manager_read);
    PS_LV2_BIND(table, 503, sys_hid_manager_ioctl);
    PS_LV2_BIND(table, 504, sys_hid_manager_map_logical_id_to_port_id);
    PS_LV2_BIND(table, 505, sys_hid_manager_unmap_logical_id_to_port_id);
    PS_LV2_BIND(table, 506, sys_hid_manager_add_hot_key_observer);
    PS_LV2_BIND(table, 507, sys_hid_manager_remove_hot_key_observer);
    PS_LV2_BIND(table, 508, sys_hid_manager_grab_focus);
    PS_LV2_BIND(table, 509, sys_hid_manager_release_focus);
    PS_LV2_BIND(table, 510, sys_hid_manager_check_focus);
    PS_LV2_BIND(table, 511, sys_hid_manager_set_master_process);
    PS_LV2_BIND(table, 512, sys_hid_manager_is_process_permission_root);
    PS_LV2_BIND(table, 513, sys_hid_manager_513);
    PS_LV2_BIND(table, 514, sys_hid_manager_514);
    PS_LV2_BIND(table, 516, sys_config_open);
    PS_LV2_BIND(table, 517, sys_config_close);
    PS_LV2_BIND(table, 518, sys_config_get_service_event);
    PS_LV2_BIND(table, 519, sys_config_add_service_listener);
    PS_LV2_BIND(table, 520, sys_config_remove_service_listener);
    PS_LV2_BIND(table, 521, sys_config_register_service);
    PS_LV2_BIND(table, 522, sys_config_unregister_service);
    PS_LV2_BIND(table, 523, sys_config_get_io_event);
    PS_LV2_BIND(table, 524, sys_config_register_io_error_listener);
    PS_LV2_BIND(table, 525, sys_config_unregister_io_error_listener);
    PS_LV2_BIND(table, 600, sys_storage_open);
    PS_LV2_BIND(table, 601, sys_storage_close);
    PS_LV2_BIND(table, 602, sys_storage_read);
    PS_LV2_BIND(table, 603, sys_storage_write);
    PS_LV2_BIND(table, 604, sys_storage_send_device_command);
    PS_LV2_BIND(table, 605, sys_storage_async_configure);
    PS_LV2_BIND(table, 606, sys_storage_async_read);
    PS_LV2_BIND(table, 607, sys_storage_async_write);
    PS_LV2_BIND(table, 608, sys_storage_async_cancel);
    PS_LV2_BIND(table, 609, sys_storage_get_device_info);
    PS_LV2_BIND(table, 610, sys_storage_get_device_config);
    PS_LV2_BIND(table, 611, sys_storage_report_devices);
    PS_LV2_BIND(table, 612, sys_storage_configure_medium_event);
    PS_LV2_BIND(table, 613, sys_storage_set_medium_polling_interval);
    PS_LV2_BIND(table, 614, sys_storage_create_region);
    PS_LV2_BIND(table, 615, sys_storage_delete_region);
    PS_LV2_BIND(table, 616, sys_storage_execute_device_command);
    PS_LV2_BIND(table, 617, sys_storage_check_region_acl);
    PS_LV2_BIND(table, 618, sys_storage_set_region_acl);
    PS_LV2_BIND(table, 619, sys_storage_async_send_device_command);
    PS_LV2_BIND(table, 622, sys_storage_get_region_offset);
    PS_LV2_BIND(table, 623, sys_storage_set_emulated_speed);
}

} // namespace pspocket::lv2
