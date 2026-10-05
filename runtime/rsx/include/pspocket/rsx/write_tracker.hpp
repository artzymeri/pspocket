#pragma once

// Write tracking for GPU copies of guest memory (plan section 4, "Write tracking for the renderer").
//
// The texture cache (and later the vertex/index caches) keep decoded copies of guest memory on the GPU. When the
// guest writes that memory, the copy must be refreshed. Two modes:
//
//  - Protection (the plan's design): watched host pages (16 KB, D-010) are made read-only with GuestMemory::protect.
//    The first guest store faults; the platform's SIGSEGV/SIGBUS handler calls ps_rsx_handle_write_fault() with the
//    faulting address; the tracker bumps the page's generation, makes the page writable again and returns true so
//    the handler can resume the store. The fault round trip was measured at 3.7 us on the A19 Pro (docs/runs/001).
//  - Hash (fallback, the default until the platform handler exists): nothing is protected; is_dirty() compares a
//    content hash of the range. Correct but costs a pass over the memory per check.
//
// Every page has a generation counter that only grows. A Watch records the sum of its pages' generations; the range
// is dirty when the sum changed. Writes the tracker cannot see as faults (the RSX's own CPU copies, HLE functions,
// SPU DMA) go through notify_write().
//
// The fault path is async-signal-safe: atomics and mprotect(2) only, no locks, no allocation.

#include <atomic>
#include <memory>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/types.hpp"

namespace pspocket::rsx {

class WriteTracker {
public:
    explicit WriteTracker(GuestMemory& mem);
    ~WriteTracker();
    WriteTracker(const WriteTracker&) = delete;
    WriteTracker& operator=(const WriteTracker&) = delete;

    // Protection needs a fault handler that calls ps_rsx_handle_write_fault(); enable it only once that handler is
    // installed. Switching modes does not touch existing watches (they stay valid in their own mode).
    void set_protection(bool enabled) { protection_.store(enabled, std::memory_order_release); }
    bool protection() const { return protection_.load(std::memory_order_acquire); }

    struct Watch {
        u32 addr = 0;
        u32 size = 0;
        u64 generation = 0;   // sum of page generations when taken
        u64 content_hash = 0; // hash mode
        bool protected_mode = false;
        bool active = false;
    };

    // Starts watching [addr, addr + size). In protection mode the pages are made read-only (only committed pages;
    // a page is protected once however many watches cover it). The snapshot is taken before protecting, so a store
    // racing with watch() is either visible to the caller's subsequent read or makes the watch dirty.
    Watch watch(u32 addr, u32 size);
    // True if the range may have changed since the watch was taken.
    bool is_dirty(const Watch& w) const;
    // is_dirty for a watch the caller keeps (D-134). In protection mode nothing is read unless a page of the range
    // was written; then the bytes decide: unchanged (a store elsewhere in the page), the watch is taken again (the
    // pages protected again) and the range is clean. *hashed says whether the range was read.
    bool check(Watch& w, bool* hashed = nullptr);
    // Drops a watch; pages no watch needs any more become writable again.
    void unwatch(Watch& w);
    // A write the tracker cannot observe through faults. Bumps generations (and wakes nothing: callers check).
    void notify_write(u32 addr, u32 size);

    // Fault hook. host_addr is the faulting host address. Returns true if it is a page this tracker protected: the
    // page is now writable and the faulting store can be retried. Async-signal-safe.
    bool handle_fault(const void* host_addr);

    // Makes this the tracker ps_rsx_handle_write_fault() dispatches to (one per process: the guest address space).
    void install();
    void uninstall();

    u64 faults_handled() const { return faults_.load(std::memory_order_relaxed); }
    u64 page_generation(u32 addr) const;
    static u64 content_hash(const u8* data, usize size);

private:
    u64 generation_sum(u64 first_page, u64 last_page) const;
    // Makes the committed pages of [first, last] read-only that are not, in runs of one mprotect each.
    void protect_pages(u64 first_page, u64 last_page);

    GuestMemory& mem_;
    usize page_size_;
    u32 page_shift_;
    u64 page_count_;
    std::unique_ptr<std::atomic<u32>[]> generation_;
    std::unique_ptr<std::atomic<u16>[]> watchers_;
    // Per-page protection state: kUnprotected (never, or no watch left), kProtected (read-only, watched),
    // kResolved (a fault made it writable again; the next watch() protects it again).
    static constexpr u8 kUnprotected = 0, kProtected = 1, kResolved = 2;
    std::unique_ptr<std::atomic<u8>[]> protected_;
    std::atomic<bool> protection_{false};
    std::atomic<u64> faults_{0};
};

} // namespace pspocket::rsx

extern "C" {
// Entry point for the platform's SIGSEGV/SIGBUS handler: returns true if the fault was a write-tracking fault that
// has been resolved (the handler then returns and the store retries), false if it belongs to someone else.
bool ps_rsx_handle_write_fault(void* host_addr);
}
