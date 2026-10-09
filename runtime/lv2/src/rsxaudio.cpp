// sys_rsxaudio: the RSX's audio DMA as lv2 gives it to the system's audio service (D-527 part 4). Reference: RPCS3
// rpcs3/Emu/Cell/lv2/sys_rsxaudio.cpp and .h (the shared page's layout, the syscalls, the ring buffer reader and the
// data thread's schedule).
//
// One context at a time: sys_rsxaudio_initialize makes the object (IDs from 0x60000000) and its 128 KB shared page
// (DMA regions for the serial and two S/PDIF ports, a silence block, the control area with three ring buffers of 16
// entries and the client's three event queue IDs). The client fills the ring buffers; create_connection takes its
// queues and points the entries at the DMA regions (lv1's IO address 0x30000000 for the page); prepare makes the
// object the hardware's; start_process clears the active ring buffers, turns their DMA on and tells each queue
// (port name, port, 0, 0); stop_process turns DMA off.
//
// The hardware: a thread reads each port's ring buffer once per block period (128 frames: about 2.67 ms for the serial
// port at 48 kHz, 2.90 ms for S/PDIF at 44.1 kHz), stamps the entry it plays, and with DMA on retires it, sending the
// port's queue (port name, port, block index, timestamp) at each notify step, as RPCS3's data thread. pspocket's
// console has no AV manager to enable an output (sys_uart, misc.cpp), so the samples go nowhere, as RPCS3 plays a port
// lv1 left disabled: muted.

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>

#include "internal.hpp"
#include "pspocket/core/timebase.h"

namespace pspocket::lv2 {

namespace {

constexpr std::string_view kAudioChan = "sys_rsxaudio";

constexpr u32 kPorts = 3, kRingEntries = 16;
constexpr u32 kShmemSize = 0x20000;
constexpr u32 kSerialRegion = 0, kSpdif0Region = 0x10000, kSpdif1Region = 0x14000, kSilenceRegion = 0x18000;
constexpr u32 kCtrl = 0x1C000;
constexpr u32 kRingSize = 560, kEntrySize = 32, kEntries = 48;
// The ring buffer's fields.
constexpr u32 rb_active = 0, rb_unk2 = 4, rb_read_idx = 8, rb_write_idx = 12, rb_rw_max_idx = 16,
              rb_queue_notify_idx = 20, rb_queue_notify_step = 24, rb_dma_silence_addr = 32, rb_next_blk_idx = 40;
// An entry's fields.
constexpr u32 e_valid = 0, e_audio_blk_idx = 8, e_timestamp = 16, e_dma_addr = 28;
// The control area after the ring buffers.
constexpr u32 c_event_queue_1 = 1684, c_event_queue_2 = 1704, c_event_queue_3 = 1724, c_unk4 = 1740,
              c_intr_thread_prio = 1744, c_unk5 = 1748, c_channel_uf = 2000, kUfSize = 256;
constexpr u32 kDmaIoBase = 0x30000000;
constexpr u64 kDmaIoId = 1;
constexpr std::array<u64, kPorts> kEventPortName = {0x8000000000400100, 0x8000000000400200, 0x8000000000400300};
constexpr std::array<u32, kPorts> kBlockSize = {0x1000, 0x400, 0x400}; // serial: 4 streams of 1 KB; S/PDIF: one
constexpr std::array<u32, kPorts> kRegion = {kSerialRegion, kSpdif0Region, kSpdif1Region};
// Block periods: 128 frames at 48 kHz (serial, 384 kHz / 8) and 44.1 kHz (S/PDIF, 352.8 kHz / 8).
constexpr std::array<u64, kPorts> kPeriodNs = {128'000'000'000ull / 48000, 128'000'000'000ull / 44100,
    128'000'000'000ull / 44100};

class RsxAudioObject final : public Object {
public:
    static constexpr u32 kIdBase = 0x60000000; // RPCS3 lv2_rsxaudio
    u32 id_base() const override { return kIdBase; }
    bool init = false;
    u32 shmem = 0;
    std::array<std::shared_ptr<EventQueue>, kPorts> queues{};
};

struct RsxAudio {
    std::mutex lock; // guards everything below and the shared page's control area
    bool allocated = false;
    std::shared_ptr<RsxAudioObject> active; // sys_rsxaudio_prepare_process's
    std::array<bool, kPorts> dma_en{};
    std::array<u64, kPorts> next_tick{};
    std::thread hardware;
    std::condition_variable wake;
    bool quit = false;

    ~RsxAudio()
    {
        {
            std::lock_guard l(lock);
            quit = true;
        }
        wake.notify_all();
        if (hardware.joinable()) hardware.join();
    }
};

RsxAudio& rsxaudio(Kernel& k)
{
    Kernel::Impl& s = k.impl();
    std::lock_guard lock(s.system_mutex);
    if (!s.rsxaudio_state) s.rsxaudio_state = std::make_shared<RsxAudio>();
    return *static_cast<RsxAudio*>(s.rsxaudio_state.get());
}

u64 now_ns()
{
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

u32 ring(u32 shmem, u32 port) { return shmem + kCtrl + kRingSize * port; }
u32 entry(u32 rb, u32 i) { return rb + kEntries + kEntrySize * i; }

// RPCS3's rsxaudio_ringbuf_reader::clean_buf.
void clean_buf(const Guest& g, u32 rb)
{
    g.w32(rb + rb_unk2, 100);
    g.w32(rb + rb_read_idx, 0);
    g.w32(rb + rb_write_idx, 0);
    g.w32(rb + rb_queue_notify_idx, 0);
    g.w64(rb + rb_next_blk_idx, 0);
    for (u32 i = 0; i < kRingEntries; i++) {
        g.w32(entry(rb, i) + e_valid, 0);
        g.w64(entry(rb, i) + e_audio_blk_idx, 0);
        g.w64(entry(rb, i) + e_timestamp, 0);
    }
}

s32 clamp_entry(s32 v) { return std::clamp<s32>(v, 0, static_cast<s32>(kRingEntries) - 1); }

// One block period of a port (RPCS3 rsxaudio_data_thread::extract_audio_data's process_rb): the entry playing gets
// the time; with DMA on, a valid entry is retired and every notify step the port's queue hears of it.
void play_block(const Guest& g, RsxAudio& a, RsxAudioObject& o, u32 port)
{
    const u32 rb = ring(o.shmem, port);
    const s32 rw_max = static_cast<s32>(g.r32(rb + rb_rw_max_idx));
    if (rw_max <= 0) return;
    const s32 read = static_cast<s32>(g.r32(rb + rb_read_idx));
    const s32 stamp_idx = clamp_entry((read + rw_max - (rw_max > 2) - 1) % rw_max);
    g.w64(entry(rb, static_cast<u32>(stamp_idx)) + e_timestamp, ps_timebase_now());
    if (!a.dma_en[port]) return;
    const s32 r = clamp_entry(read);
    if ((g.r32(entry(rb, static_cast<u32>(r)) + e_valid) & 1) == 0) return;
    const s32 report = clamp_entry((read + rw_max - (rw_max > 2)) % rw_max);
    const s32 step = static_cast<s32>(g.r32(rb + rb_queue_notify_step));
    g.w32(entry(rb, static_cast<u32>(r)) + e_valid, 0);
    const s32 notify = step > 0 ? (static_cast<s32>(g.r32(rb + rb_queue_notify_idx)) + 1) % step : 0;
    g.w32(rb + rb_queue_notify_idx, static_cast<u32>(notify));
    g.w32(rb + rb_read_idx, static_cast<u32>((read + 1) % rw_max));
    if (((rw_max > 2) ^ notify) == 0 && o.queues[port]) {
        o.queues[port]->send(Event{kEventPortName[port], port,
            g.r64(entry(rb, static_cast<u32>(report)) + e_audio_blk_idx),
            g.r64(entry(rb, static_cast<u32>(report)) + e_timestamp)});
    }
}

void hardware_thread(Kernel* k)
{
    RsxAudio& a = rsxaudio(*k);
    const Guest& g = k->impl().guest;
    std::unique_lock lock(a.lock);
    for (;;) {
        if (a.quit) return;
        if (!a.active) {
            a.wake.wait(lock);
            continue;
        }
        const u64 t = now_ns();
        u64 next = ~u64{0};
        for (u32 p = 0; p < kPorts; p++) {
            if (!a.next_tick[p]) a.next_tick[p] = t + kPeriodNs[p];
            while (a.next_tick[p] <= t) {
                if (a.active && a.active->init) play_block(g, a, *a.active, p);
                a.next_tick[p] += kPeriodNs[p];
                if (a.next_tick[p] + 100 * kPeriodNs[p] < t) a.next_tick[p] = t + kPeriodNs[p]; // far behind: skip
            }
            next = std::min(next, a.next_tick[p]);
        }
        a.wake.wait_for(lock, std::chrono::nanoseconds(next > t ? next - t : 0));
    }
}

std::shared_ptr<RsxAudioObject> object(PpuThread& t, u32 handle)
{
    auto o = t.kernel.impl().objects.get<RsxAudioObject>(handle);
    return o && o->init ? o : nullptr;
}

} // namespace

// sys_rsxaudio_initialize(u32* handle): one context at a time (CELL_EINVAL for a second).
Result sys_rsxaudio_initialize(PpuThread& t, u32 handle)
{
    Kernel::Impl& s = t.kernel.impl();
    RsxAudio& a = rsxaudio(t.kernel);
    std::lock_guard lock(a.lock);
    if (a.allocated) return err(CELL_EINVAL);
    if (!s.guest.valid(handle, 4)) return err(CELL_EFAULT);
    u32 page = 0;
    {
        std::lock_guard ml(s.memory.mutex);
        page = s.memory.allocate_kernel(kShmemSize);
    }
    if (!page) return err(CELL_ENOMEM);
    auto o = std::make_shared<RsxAudioObject>();
    const u32 id = s.objects.insert(RsxAudioObject::kIdBase, o);
    if (!id) {
        std::lock_guard ml(s.memory.mutex);
        s.memory.free_kernel(page);
        return err(CELL_ENOMEM);
    }
    o->id = id;
    o->shmem = page;
    o->init = true;
    const Guest& g = s.guest;
    std::memset(g.ptr(page + kCtrl), 0, kShmemSize - kCtrl);
    g.w32(page + kCtrl + c_unk4, 0x8000);
    g.w32(page + kCtrl + c_intr_thread_prio, 0xDEADBEEF);
    g.w32(page + kCtrl + c_unk5, 0);
    a.allocated = true;
    g.w32(handle, id);
    return CELL_OK;
}

// sys_rsxaudio_finalize(handle).
Result sys_rsxaudio_finalize(PpuThread& t, u32 handle)
{
    Kernel::Impl& s = t.kernel.impl();
    RsxAudio& a = rsxaudio(t.kernel);
    std::lock_guard lock(a.lock);
    auto o = object(t, handle);
    if (!o) return err(CELL_ESRCH);
    if (a.active == o) a.active = nullptr;
    o->init = false;
    {
        std::lock_guard ml(s.memory.mutex);
        s.memory.free_kernel(o->shmem);
    }
    s.objects.remove(RsxAudioObject::kIdBase, handle);
    a.allocated = false;
    a.dma_en = {};
    return CELL_OK;
}

// sys_rsxaudio_import_shared_memory(handle, u64* addr): the page's address (RPCS3 unprotects it; pspocket's guest
// memory has no page protection).
Result sys_rsxaudio_import_shared_memory(PpuThread& t, u32 handle, u32 addr)
{
    RsxAudio& a = rsxaudio(t.kernel);
    std::lock_guard lock(a.lock);
    auto o = object(t, handle);
    if (!o) return err(CELL_ESRCH);
    if (!t.kernel.impl().guest.put64(addr, o->shmem)) return err(CELL_EFAULT);
    return CELL_OK;
}

Result sys_rsxaudio_unimport_shared_memory(PpuThread& t, u32 handle, u32)
{
    RsxAudio& a = rsxaudio(t.kernel);
    std::lock_guard lock(a.lock);
    return object(t, handle) ? CELL_OK : err(CELL_ESRCH);
}

// sys_rsxaudio_create_connection(handle): the client's three queues (all must exist) and the entries' DMA addresses.
Result sys_rsxaudio_create_connection(PpuThread& t, u32 handle)
{
    const Guest& g = t.kernel.impl().guest;
    RsxAudio& a = rsxaudio(t.kernel);
    std::lock_guard lock(a.lock);
    auto o = object(t, handle);
    if (!o) return err(CELL_ESRCH);
    const u32 ctrl = o->shmem + kCtrl;
    std::array<std::shared_ptr<EventQueue>, kPorts> qs{};
    const std::array<u32, kPorts> ids = {g.r32(ctrl + c_event_queue_1), g.r32(ctrl + c_event_queue_2),
        g.r32(ctrl + c_event_queue_3)};
    for (u32 p = 0; p < kPorts; p++) {
        qs[p] = t.kernel.event_queue(ids[p]);
        if (!qs[p]) return err(CELL_ESRCH);
    }
    o->queues = qs;
    for (u32 p = 0; p < kPorts; p++) {
        const u32 rb = ring(o->shmem, p);
        g.w32(rb + rb_dma_silence_addr, kDmaIoBase + kSilenceRegion);
        g.w32(rb + rb_unk2, 100);
        for (u32 i = 0; i < kRingEntries; i++) g.w32(entry(rb, i) + e_dma_addr, kDmaIoBase + kRegion[p] + kBlockSize[p] * i);
    }
    return CELL_OK;
}

Result sys_rsxaudio_close_connection(PpuThread& t, u32 handle)
{
    RsxAudio& a = rsxaudio(t.kernel);
    std::lock_guard lock(a.lock);
    auto o = object(t, handle);
    if (!o) return err(CELL_ESRCH);
    if (a.active == o) a.active = nullptr;
    o->queues = {};
    return CELL_OK;
}

// sys_rsxaudio_prepare_process(handle): the object becomes the hardware's (-1 when another already is).
Result sys_rsxaudio_prepare_process(PpuThread& t, u32 handle)
{
    RsxAudio& a = rsxaudio(t.kernel);
    std::lock_guard lock(a.lock);
    auto o = object(t, handle);
    if (!o) return err(CELL_ESRCH);
    if (a.active) return -1;
    a.active = o;
    a.next_tick = {};
    if (!a.hardware.joinable()) {
        PS_LOG_INFO(kAudioChan, "RSX audio: the hardware thread starts (outputs muted: no AV manager enables one)");
        a.hardware = std::thread(hardware_thread, &t.kernel);
    }
    a.wake.notify_all();
    return CELL_OK;
}

// sys_rsxaudio_start_process(handle): see the top.
Result sys_rsxaudio_start_process(PpuThread& t, u32 handle)
{
    const Guest& g = t.kernel.impl().guest;
    RsxAudio& a = rsxaudio(t.kernel);
    std::lock_guard lock(a.lock);
    auto o = object(t, handle);
    if (!o) return err(CELL_ESRCH);
    for (u32 p = 0; p < kPorts; p++) {
        const u32 rb = ring(o->shmem, p);
        if (g.r32(rb + rb_active)) clean_buf(g, rb);
    }
    for (u32 p = 0; p < kPorts; p++) {
        const u32 uf = o->shmem + kCtrl + c_channel_uf + kUfSize * p;
        g.w32(uf + 8, 0);
        g.w64(uf, 0);
    }
    for (u32 p = 0; p < kPorts; p++) {
        if (g.r32(ring(o->shmem, p) + rb_active)) a.dma_en[p] = true;
    }
    for (u32 p = 0; p < kPorts; p++) {
        if (o->queues[p] && g.r32(ring(o->shmem, p) + rb_active)) o->queues[p]->send(Event{kEventPortName[p], p, 0, 0});
    }
    a.wake.notify_all();
    return CELL_OK;
}

// sys_rsxaudio_stop_process(handle): DMA off, the active ring buffers cleared.
Result sys_rsxaudio_stop_process(PpuThread& t, u32 handle)
{
    const Guest& g = t.kernel.impl().guest;
    RsxAudio& a = rsxaudio(t.kernel);
    std::lock_guard lock(a.lock);
    auto o = object(t, handle);
    if (!o) return err(CELL_ESRCH);
    a.dma_en = {};
    for (u32 p = 0; p < kPorts; p++) {
        const u32 rb = ring(o->shmem, p);
        if (g.r32(rb + rb_active)) clean_buf(g, rb);
    }
    return CELL_OK;
}

// sys_rsxaudio_get_dma_param(handle, flag, u64* out): flag 0 the page's IO address, 1 the IO ID.
Result sys_rsxaudio_get_dma_param(PpuThread& t, u32 handle, u32 flag, u32 out)
{
    RsxAudio& a = rsxaudio(t.kernel);
    std::lock_guard lock(a.lock);
    if (!object(t, handle)) return err(CELL_ESRCH);
    const Guest& g = t.kernel.impl().guest;
    if (!g.valid(out, 8)) return err(CELL_EFAULT);
    if (flag == 1) g.w64(out, kDmaIoId);
    else if (flag == 0) g.w64(out, kDmaIoBase);
    return CELL_OK;
}

void register_rsxaudio_syscalls(SyscallTable& table)
{
    PS_LV2_BIND(table, 650, sys_rsxaudio_initialize);
    PS_LV2_BIND(table, 651, sys_rsxaudio_finalize);
    PS_LV2_BIND(table, 652, sys_rsxaudio_import_shared_memory);
    PS_LV2_BIND(table, 653, sys_rsxaudio_unimport_shared_memory);
    PS_LV2_BIND(table, 654, sys_rsxaudio_create_connection);
    PS_LV2_BIND(table, 655, sys_rsxaudio_close_connection);
    PS_LV2_BIND(table, 656, sys_rsxaudio_prepare_process);
    PS_LV2_BIND(table, 657, sys_rsxaudio_start_process);
    PS_LV2_BIND(table, 658, sys_rsxaudio_stop_process);
    PS_LV2_BIND(table, 659, sys_rsxaudio_get_dma_param);
}

} // namespace pspocket::lv2
