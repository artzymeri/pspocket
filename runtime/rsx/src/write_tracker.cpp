#include "pspocket/rsx/write_tracker.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <sys/mman.h>

#include "pspocket/core/log.hpp"

namespace pspocket::rsx {
namespace {

std::atomic<WriteTracker*> g_tracker{nullptr};

} // namespace

WriteTracker::WriteTracker(GuestMemory& mem)
    : mem_(mem),
      page_size_(GuestMemory::host_page_size()),
      page_shift_(static_cast<u32>(std::countr_zero(GuestMemory::host_page_size()))),
      page_count_(GuestMemory::kGuestSize / GuestMemory::host_page_size()),
      generation_(new std::atomic<u32>[page_count_]),
      watchers_(new std::atomic<u16>[page_count_]),
      state_(new std::atomic<u16>[page_count_]),
      read_armed_(new std::atomic<u8>[page_count_]),
      read_watched_(new std::atomic<u8>[page_count_]),
      read_hits_(new ReadHit[kMaxReadHits])
{
    for (u64 i = 0; i < page_count_; i++) {
        generation_[i].store(0, std::memory_order_relaxed);
        watchers_[i].store(0, std::memory_order_relaxed);
        state_[i].store(kUnprotected, std::memory_order_relaxed);
        read_armed_[i].store(0, std::memory_order_relaxed);
        read_watched_[i].store(0, std::memory_order_relaxed);
    }
}

WriteTracker::~WriteTracker()
{
    uninstall();
    watch_reads({}); // no page left inaccessible
    // Leave no page read-only behind us.
    for (u64 p = 0; p < page_count_; p++) {
        if ((state_[p].load(std::memory_order_relaxed) & kStateMask) == kProtected) {
            mem_.protect(static_cast<u32>(p << page_shift_), page_size_, Prot::ReadWrite);
        }
    }
}

void WriteTracker::install() { g_tracker.store(this, std::memory_order_release); }

void WriteTracker::uninstall()
{
    WriteTracker* self = this;
    g_tracker.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
}

u64 WriteTracker::content_hash(const u8* data, usize size)
{
    // FNV-1a's step over 8-byte words in four independent lanes of 32-byte blocks (one lane waits on each multiply:
    // the hash was half the RSX thread's time in run 075, D-104), then the tail.
    constexpr u64 kPrime = 0x100000001b3ull;
    u64 h0 = 0xcbf29ce484222325ull, h1 = 0x9e3779b97f4a7c15ull, h2 = 0xc2b2ae3d27d4eb4full, h3 = 0x165667b19e3779f9ull;
    usize i = 0;
    for (; i + 32 <= size; i += 32) {
        u64 w[4];
        std::memcpy(w, data + i, 32);
        h0 = (h0 ^ w[0]) * kPrime;
        h1 = (h1 ^ w[1]) * kPrime;
        h2 = (h2 ^ w[2]) * kPrime;
        h3 = (h3 ^ w[3]) * kPrime;
    }
    u64 h = h0 ^ std::rotl(h1, 17) ^ std::rotl(h2, 31) ^ std::rotl(h3, 47);
    for (; i + 8 <= size; i += 8) {
        u64 w;
        std::memcpy(&w, data + i, 8);
        h = (h ^ w) * kPrime;
    }
    for (; i < size; i++) h = (h ^ data[i]) * kPrime;
    return h ^ size;
}

u64 WriteTracker::generation_sum(u64 first_page, u64 last_page) const
{
    u64 sum = 0;
    for (u64 p = first_page; p <= last_page; p++) sum += generation_[p].load(std::memory_order_acquire);
    return sum;
}

u64 WriteTracker::page_generation(u32 addr) const
{
    return generation_[addr >> page_shift_].load(std::memory_order_acquire);
}

WriteTracker::Watch WriteTracker::watch(u32 addr, u32 size)
{
    Watch w;
    w.addr = addr;
    w.size = size;
    w.active = size != 0;
    if (!w.active) return w;
    const u64 first = addr >> page_shift_;
    const u64 last = (u64{addr} + size - 1) >> page_shift_;
    w.generation = generation_sum(first, last);
    w.protected_mode = protection();
    // The bytes as of the generation (protection mode keeps them too: check() tells a store to the range from one
    // elsewhere in its pages).
    w.content_hash = content_hash(mem_.host(addr), size);
    if (!w.protected_mode) return w;
    for (u64 p = first; p <= last; p++) watchers_[p].fetch_add(1, std::memory_order_acq_rel);
    protect_pages(first, last);
    return w;
}

void WriteTracker::protect_pages(u64 first, u64 last)
{
    u64 run = 0, run_pages = 0; // the pending run of pages to make read-only (all protecting)
    const auto flush = [&] {
        if (!run_pages) return;
        mem_.protect(static_cast<u32>(run << page_shift_), run_pages * page_size_, Prot::Read);
        for (u64 p = run; p < run + run_pages; p++) state_[p].store(kProtected, std::memory_order_release);
        run_pages = 0;
    };
    for (u64 p = first; p <= last; p++) {
        // Any state but protected is ours to protect, once a fault making it writable has finished (its mprotect
        // must not land after this one).
        bool take = mem_.is_committed(static_cast<u32>(p << page_shift_));
        while (take) {
            u16 state = settled_state(p);
            if ((state & kStateMask) == kProtected) take = false;
            else if (state_[p].compare_exchange_weak(state, kProtecting, std::memory_order_acq_rel)) break;
        }
        if (!take) {
            flush();
            continue;
        }
        if (!run_pages) run = p;
        run_pages++;
    }
    flush();
}

bool WriteTracker::check(Watch& w, bool* hashed)
{
    if (hashed) *hashed = false;
    if (!w.active) return true;
    if (!w.protected_mode) {
        if (hashed) *hashed = true;
        return is_dirty(w);
    }
    const u64 first = w.addr >> page_shift_;
    const u64 last = (u64{w.addr} + w.size - 1) >> page_shift_;
    const u64 generation = generation_sum(first, last);
    if (generation == w.generation) return false;
    // A page was written since the watch (a fault, or notify_write). The generation is read before the pages are made
    // read-only again: a store after that faults and dirties the next check; one before it is in the bytes compared.
    protect_pages(first, last);
    if (hashed) *hashed = true;
    if (content_hash(mem_.host(w.addr), w.size) != w.content_hash) return true;
    w.generation = generation;
    return false;
}

bool WriteTracker::is_dirty(const Watch& w) const
{
    if (!w.active) return true;
    const u64 first = w.addr >> page_shift_;
    const u64 last = (u64{w.addr} + w.size - 1) >> page_shift_;
    if (generation_sum(first, last) != w.generation) return true;
    if (w.protected_mode) {
        // A page protected by this watch may have been unprotected by a fault (generation bumped, caught above) or
        // never protected because it was not committed; both are covered.
        return false;
    }
    return content_hash(mem_.host(w.addr), w.size) != w.content_hash;
}

void WriteTracker::unwatch(Watch& w)
{
    if (!w.active) return;
    w.active = false;
    if (!w.protected_mode) return;
    const u64 first = w.addr >> page_shift_;
    const u64 last = (u64{w.addr} + w.size - 1) >> page_shift_;
    for (u64 p = first; p <= last; p++) {
        if (watchers_[p].fetch_sub(1, std::memory_order_acq_rel) != 1) continue;
        // The last watch on the page: writable again (resolved: it is already).
        for (;;) {
            u16 state = settled_state(p);
            const u16 s = state & kStateMask;
            if (s != kProtected && s != kResolved) break; // never protected (not committed then), or released
            if (!state_[p].compare_exchange_weak(state, s == kProtected ? kReleasing : kReleased,
                    std::memory_order_acq_rel)) {
                continue;
            }
            if (s == kProtected) {
                mem_.protect(static_cast<u32>(p << page_shift_), page_size_, Prot::ReadWrite);
                state_[p].store(kReleased, std::memory_order_release);
            }
            break;
        }
    }
}

u16 WriteTracker::settled_state(u64 p) const
{
    u16 state = state_[p].load(std::memory_order_acquire);
    for (u32 spins = 0;; spins++) {
        const u16 s = state & kStateMask;
        if (s != kProtecting && s != kResolving && s != kReleasing) return state;
        if (spins > 64) std::this_thread::yield();
        state = state_[p].load(std::memory_order_acquire);
    }
}

void WriteTracker::notify_write(u32 addr, u32 size)
{
    if (size == 0) return;
    const u64 first = addr >> page_shift_;
    const u64 last = (u64{addr} + size - 1) >> page_shift_;
    for (u64 p = first; p <= last; p++) generation_[p].fetch_add(1, std::memory_order_acq_rel);
}

void WriteTracker::set_owner_thread()
{
    owner_thread_.store(reinterpret_cast<u64>(pthread_self()), std::memory_order_release);
}

void WriteTracker::watch_reads(const std::vector<std::pair<u32, u32>>& ranges)
{
    // What the write tracking wants of a page: read-only while it is protected, else writable.
    const auto restore = [&](u64 p) {
        const u16 s = settled_state(p) & kStateMask;
        mem_.protect(static_cast<u32>(p << page_shift_), page_size_, s == kProtected ? Prot::Read : Prot::ReadWrite);
    };
    std::vector<u64> next;
    for (const auto& [addr, size] : ranges) {
        if (!size) continue;
        const u64 first = addr >> page_shift_, last = (u64{addr} + size - 1) >> page_shift_;
        for (u64 p = first; p <= last && p < page_count_; p++)
            if (mem_.is_committed(static_cast<u32>(p << page_shift_))) next.push_back(p);
    }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    // Pages out of the set since the call before: out of the watch now (a fault taken while one was inaccessible
    // has been handled by now).
    for (u64 p : read_leaving_)
        if (!std::binary_search(next.begin(), next.end(), p)) read_watched_[p].store(0, std::memory_order_release);
    read_leaving_.clear();
    for (u64 p : read_pages_) { // the last call's pages not armed again: as the write tracking wants them
        if (std::binary_search(next.begin(), next.end(), p)) continue;
        u8 armed = 1;
        if (read_armed_[p].compare_exchange_strong(armed, 2, std::memory_order_acq_rel)) {
            restore(p);
            read_armed_[p].store(0, std::memory_order_release);
        }
        read_leaving_.push_back(p);
    }
    // Every page armed again (the write tracking's own protection changes may have opened one), in runs.
    for (usize i = 0; i < next.size();) {
        usize j = i;
        while (j + 1 < next.size() && next[j + 1] == next[j] + 1) j++;
        for (usize k = i; k <= j; k++) {
            read_watched_[next[k]].store(1, std::memory_order_release);
            read_armed_[next[k]].store(1, std::memory_order_release);
        }
        mem_.protect(static_cast<u32>(next[i] << page_shift_), (j - i + 1) * page_size_, Prot::None);
        i = j + 1;
    }
    read_pages_ = std::move(next);
}

std::vector<WriteTracker::ReadHit> WriteTracker::take_read_hits(u64* total)
{
    const u32 n = std::min(read_hit_count_.exchange(0, std::memory_order_acq_rel), kMaxReadHits);
    std::vector<ReadHit> out(read_hits_.get(), read_hits_.get() + n);
    if (total) *total = read_hits_total_.exchange(0, std::memory_order_acq_rel);
    return out;
}

// A fault on a page armed for reads (D-1059): disarmed, its protection as the write tracking wants it, and the
// access retried; a hit unless the owner thread made it. Async-signal-safe.
bool WriteTracker::handle_read_fault(u64 page, u32 addr)
{
    // 1: armed (inaccessible); 2: a thread is restoring it (the access retries until it has); 0: the write tracking's
    // own protection (a fault there is the write tracking's).
    u8 armed = 1;
    if (!read_armed_[page].compare_exchange_strong(armed, 2, std::memory_order_acq_rel)) return armed == 2;
    const u16 s = settled_state(page) & kStateMask;
    ::mprotect(mem_.base() + (page << page_shift_), page_size_, s == kProtected ? PROT_READ : PROT_READ | PROT_WRITE);
    read_armed_[page].store(0, std::memory_order_release);
    const u64 self = reinterpret_cast<u64>(pthread_self());
    if (self != owner_thread_.load(std::memory_order_acquire)) {
        read_hits_total_.fetch_add(1, std::memory_order_relaxed);
        const u32 i = read_hit_count_.fetch_add(1, std::memory_order_acq_rel);
        if (i < kMaxReadHits) read_hits_[i] = {addr, self};
    }
    return true;
}

bool WriteTracker::handle_fault(const void* host_addr)
{
    if (handle_write_fault(host_addr)) return true;
    // A page in the read watch's set whose fault came while it was inaccessible and found it restored by another
    // thread (and not the write tracking's): accessible now, the access is retried (D-1059).
    const auto a = reinterpret_cast<uintptr_t>(host_addr);
    const auto b = reinterpret_cast<uintptr_t>(mem_.base());
    if (a < b || a - b >= GuestMemory::kGuestSize) return false;
    const u64 page = (a - b) >> page_shift_;
    return read_watched_[page].load(std::memory_order_acquire) && mem_.is_committed(static_cast<u32>(page << page_shift_));
}

bool WriteTracker::handle_write_fault(const void* host_addr)
{
    const auto a = reinterpret_cast<uintptr_t>(host_addr);
    const auto b = reinterpret_cast<uintptr_t>(mem_.base());
    if (a < b || a - b >= GuestMemory::kGuestSize) return false;
    const u64 page = (a - b) >> page_shift_;
    const bool read_watched = read_watched_[page].load(std::memory_order_acquire) != 0; // D-1059
    if (read_watched && handle_read_fault(page, static_cast<u32>(a - b))) return true;
    // Writable already (resolved, released) means the store runs again: not for a page freed since (no access to it
    // can succeed; it goes on to the crash handler).
    const bool committed = mem_.is_committed(static_cast<u32>(page << page_shift_));
    for (;;) {
        u16 state = settled_state(page); // waits out a change in flight: its mprotect decides whether the store faults
        switch (state & kStateMask) {
        case kProtected:
            if (!state_[page].compare_exchange_weak(state, kResolving, std::memory_order_acq_rel)) continue;
            // Generation first, so a reader that sees the page writable also sees it dirty.
            generation_[page].fetch_add(1, std::memory_order_acq_rel);
            faults_.fetch_add(1, std::memory_order_relaxed);
            ::mprotect(mem_.base() + (page << page_shift_), page_size_, PROT_READ | PROT_WRITE);
            state_[page].store(kResolved, std::memory_order_release);
            return true;
        case kResolved:
            return committed; // another thread's fault made it writable
        case kReleased:
            // The store faulted while the page was protected; unwatch() has made it writable since. Counted: a fault
            // that is not ours would come back every time.
            if (!committed || state >= kMaxStale * kStaleOne) return false;
            if (!state_[page].compare_exchange_weak(state, static_cast<u16>(state + kStaleOne),
                    std::memory_order_acq_rel)) {
                continue;
            }
            return true;
        default:
            return false; // never protected: not ours
        }
    }
}

} // namespace pspocket::rsx

extern "C" bool ps_rsx_handle_write_fault(void* host_addr)
{
    pspocket::rsx::WriteTracker* t = pspocket::rsx::g_tracker.load(std::memory_order_acquire);
    return t != nullptr && t->handle_fault(host_addr);
}
