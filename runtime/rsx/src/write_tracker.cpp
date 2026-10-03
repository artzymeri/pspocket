#include "pspocket/rsx/write_tracker.hpp"

#include <bit>
#include <cstring>
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
      protected_(new std::atomic<u8>[page_count_])
{
    for (u64 i = 0; i < page_count_; i++) {
        generation_[i].store(0, std::memory_order_relaxed);
        watchers_[i].store(0, std::memory_order_relaxed);
        protected_[i].store(0, std::memory_order_relaxed);
    }
}

WriteTracker::~WriteTracker()
{
    uninstall();
    // Leave no page read-only behind us.
    for (u64 p = 0; p < page_count_; p++) {
        if (protected_[p].load(std::memory_order_relaxed) == kProtected) {
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
    // FNV-1a over 8-byte words, then the tail: fast enough for a fallback, no dependency.
    u64 h = 0xcbf29ce484222325ull;
    usize i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 w;
        std::memcpy(&w, data + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
    }
    for (; i < size; i++) h = (h ^ data[i]) * 0x100000001b3ull;
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
    if (!w.protected_mode) {
        w.content_hash = content_hash(mem_.host(addr), size);
        return w;
    }
    for (u64 p = first; p <= last; p++) {
        watchers_[p].fetch_add(1, std::memory_order_acq_rel);
        const u32 page_addr = static_cast<u32>(p << page_shift_);
        if (protected_[p].load(std::memory_order_acquire) != kProtected && mem_.is_committed(page_addr)) {
            protected_[p].store(kProtected, std::memory_order_release);
            mem_.protect(page_addr, page_size_, Prot::Read);
        }
    }
    return w;
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
        if (watchers_[p].fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (protected_[p].exchange(kUnprotected, std::memory_order_acq_rel) == kProtected) {
                mem_.protect(static_cast<u32>(p << page_shift_), page_size_, Prot::ReadWrite);
            }
        }
    }
}

void WriteTracker::notify_write(u32 addr, u32 size)
{
    if (size == 0) return;
    const u64 first = addr >> page_shift_;
    const u64 last = (u64{addr} + size - 1) >> page_shift_;
    for (u64 p = first; p <= last; p++) generation_[p].fetch_add(1, std::memory_order_acq_rel);
}

bool WriteTracker::handle_fault(const void* host_addr)
{
    const auto a = reinterpret_cast<uintptr_t>(host_addr);
    const auto b = reinterpret_cast<uintptr_t>(mem_.base());
    if (a < b || a - b >= GuestMemory::kGuestSize) return false;
    const u64 page = (a - b) >> page_shift_;
    u8 expected = kProtected;
    if (!protected_[page].compare_exchange_strong(expected, kResolved, std::memory_order_acq_rel)) {
        // kResolved: another thread's fault on this page won the race; retrying the store is right (it faults again
        // until the winner's mprotect lands). kUnprotected: not a page we protected: not ours.
        return expected == kResolved;
    }
    // Generation first, so a reader that sees the page writable also sees it dirty.
    generation_[page].fetch_add(1, std::memory_order_acq_rel);
    faults_.fetch_add(1, std::memory_order_relaxed);
    ::mprotect(mem_.base() + (page << page_shift_), page_size_, PROT_READ | PROT_WRITE);
    return true;
}

} // namespace pspocket::rsx

extern "C" bool ps_rsx_handle_write_fault(void* host_addr)
{
    pspocket::rsx::WriteTracker* t = pspocket::rsx::g_tracker.load(std::memory_order_acquire);
    return t != nullptr && t->handle_fault(host_addr);
}
