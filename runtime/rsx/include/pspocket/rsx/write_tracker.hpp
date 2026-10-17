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
#include <utility>
#include <vector>

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
    // watch(), check(), is_dirty() and unwatch() are called from one thread (the RSX thread); any thread may fault.
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

    // Read watching (D-1059, a measurement; protection mode only): the pages of `ranges` are made inaccessible until
    // their next access. An access from any thread but the owner (the RSX thread, whose own reads and write-backs are
    // the backend's business) is a hit: kept (the first kMaxReadHits, address and thread) and counted; the page goes
    // back to what the write tracking wants of it and the access is retried. Each call replaces the last one's pages.
    // Called from the owner thread.
    void watch_reads(const std::vector<std::pair<u32, u32>>& ranges);
    void set_owner_thread();
    struct ReadHit {
        u32 addr = 0;   // the guest address the access faulted at
        u64 thread = 0; // pthread_t of the reader
    };
    static constexpr u32 kMaxReadHits = 256;
    // Hits since the last call (at most kMaxReadHits kept; `total` counts every one).
    std::vector<ReadHit> take_read_hits(u64* total);
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
    // Per-page protection state (D-144): the low bits are one of
    //   kUnprotected  never protected by this tracker: a fault there is not ours;
    //   kProtecting   protect_pages() is making it read-only;
    //   kProtected    read-only, watched;
    //   kResolving    a fault is making it writable;
    //   kResolved     writable after a fault, still watched (the next watch() or check() protects it again);
    //   kReleasing    unwatch() is making it writable;
    //   kReleased     writable, no watch left.
    // Every mprotect happens in one of the transitional states (protecting, resolving, releasing), entered by
    // compare-and-swap and left by the thread that entered it; whoever meets one waits for it to end. So the state
    // always says what the page's protection is. Before, a fault's mprotect to writable could land after check() had
    // made the page read-only again: the page stayed writable while the tracker took it for read-only, and every later
    // store went unseen (GTA V drew stale textures, flickering, on the iPhone and the Mac); and a page could be left
    // read-only while the tracker took it for writable (a store there crashed).
    // A fault is handled after its store faulted: by then the page may be protected again (resolved again, harmless)
    // or released. Released, the store is retried (the page is writable); the bits above the state count those
    // retries, and past kMaxStale a fault that is not ours (an unaligned atomic) goes on to the crash handler.
    static constexpr u16 kUnprotected = 0, kProtecting = 1, kProtected = 2, kResolving = 3, kResolved = 4,
                         kReleasing = 5, kReleased = 6;
    static constexpr u16 kStateMask = 7, kStaleOne = 8, kMaxStale = 1024;
    // Waits while another thread changes page `p`'s protection; returns the state then (with its count).
    u16 settled_state(u64 p) const;
    std::unique_ptr<std::atomic<u16>[]> state_;
    std::atomic<bool> protection_{false};
    std::atomic<u64> faults_{0};
    // Read watching (D-1059): pages armed (1) until their first access; the armed list (owner thread); the hits.
    std::unique_ptr<std::atomic<u8>[]> read_armed_;
    std::unique_ptr<std::atomic<u8>[]> read_watched_; // in the current set: a fault there retries until restored
    std::vector<u64> read_pages_, read_leaving_;
    std::atomic<u64> owner_thread_{0};
    std::unique_ptr<ReadHit[]> read_hits_;
    std::atomic<u32> read_hit_count_{0};
    std::atomic<u64> read_hits_total_{0};
    bool handle_read_fault(u64 page, u32 addr);
    bool handle_write_fault(const void* host_addr);
};

} // namespace pspocket::rsx

extern "C" {
// Entry point for the platform's SIGSEGV/SIGBUS handler: returns true if the fault was a write-tracking fault that
// has been resolved (the handler then returns and the store retries), false if it belongs to someone else.
bool ps_rsx_handle_write_fault(void* host_addr);
}
