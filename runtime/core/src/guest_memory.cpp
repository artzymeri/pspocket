#include "pspocket/core/guest_memory.hpp"

#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "pspocket/core/log.hpp"

namespace pspocket {
namespace {

constexpr std::string_view kChan = "mem";

int to_native(Prot prot)
{
    int native = PROT_NONE;
    if (static_cast<u8>(prot) & static_cast<u8>(Prot::Read)) native |= PROT_READ;
    if (static_cast<u8>(prot) & static_cast<u8>(Prot::Write)) native |= PROT_WRITE;
    return native;
}

} // namespace

GuestMemory::~GuestMemory() { release(); }

usize GuestMemory::host_page_size() { return static_cast<usize>(sysconf(_SC_PAGESIZE)); }

bool GuestMemory::reserve()
{
    std::lock_guard lock(mutex_);
    if (base_) return true;

    page_size_ = host_page_size();
    const u64 total = kGuestSize + kGuardSize;
    // Over-reserve by the alignment, then hand back the slack on both sides so base() is kBaseAlign-aligned.
    const u64 padded = total + kBaseAlign;
    void* p = mmap(nullptr, padded, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        PS_LOG_ERROR(kChan, "reserving {} GB of guest address space failed: {} (on iOS this needs the "
                            "extended-virtual-addressing entitlement)", total >> 30, std::strerror(errno));
        return false;
    }
    const auto raw = reinterpret_cast<uintptr_t>(p);
    const uintptr_t aligned = (raw + kBaseAlign - 1) & ~static_cast<uintptr_t>(kBaseAlign - 1);
    if (aligned > raw) munmap(p, aligned - raw);
    if (const uintptr_t tail = raw + padded - (aligned + total); tail > 0)
        munmap(reinterpret_cast<void*>(aligned + total), tail);
    base_ = reinterpret_cast<u8*>(aligned);
    committed_.assign((kGuestSize / page_size_ + 63) / 64, 0);
    committed_pages_ = 0;
    PS_LOG_DEBUG(kChan, "guest memory reserved at {} ({} KB host pages)", static_cast<void*>(base_), page_size_ >> 10);
    return true;
}

void GuestMemory::release()
{
    std::lock_guard lock(mutex_);
    if (!base_) return;
    munmap(base_, kGuestSize + kGuardSize);
    base_ = nullptr;
    committed_.clear();
    committed_pages_ = 0;
}

bool GuestMemory::page_range(u32 addr, u64 size, u64& first_page, u64& page_count) const
{
    if (!base_ || size == 0 || u64{addr} + size > kGuestSize) return false;
    const u64 begin = u64{addr} / page_size_;
    const u64 end = (u64{addr} + size + page_size_ - 1) / page_size_;
    first_page = begin;
    page_count = end - begin;
    return true;
}

void GuestMemory::mark(u64 first_page, u64 page_count, bool committed)
{
    for (u64 page = first_page; page < first_page + page_count; page++) {
        u64& word = committed_[page / 64];
        const u64 bit = u64{1} << (page % 64);
        const bool was = (word & bit) != 0;
        if (committed && !was) {
            word |= bit;
            committed_pages_++;
        } else if (!committed && was) {
            word &= ~bit;
            committed_pages_--;
        }
    }
}

bool GuestMemory::commit(u32 addr, u64 size)
{
    std::lock_guard lock(mutex_);
    u64 first = 0, count = 0;
    if (!page_range(addr, size, first, count)) return false;
    if (mprotect(base_ + first * page_size_, count * page_size_, PROT_READ | PROT_WRITE) != 0) {
        PS_LOG_ERROR(kChan, "commit {:#010x}+{:#x} failed: {}", addr, size, std::strerror(errno));
        return false;
    }
    mark(first, count, true);
    return true;
}

bool GuestMemory::decommit(u32 addr, u64 size)
{
    std::lock_guard lock(mutex_);
    u64 first = 0, count = 0;
    if (!page_range(addr, size, first, count)) return false;
    // Mapping fresh anonymous memory over the range frees the old pages and leaves them inaccessible.
    void* p = mmap(base_ + first * page_size_, count * page_size_, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        PS_LOG_ERROR(kChan, "decommit {:#010x}+{:#x} failed: {}", addr, size, std::strerror(errno));
        return false;
    }
    mark(first, count, false);
    return true;
}

bool GuestMemory::protect(u32 addr, u64 size, Prot prot)
{
    std::lock_guard lock(mutex_);
    u64 first = 0, count = 0;
    if (!page_range(addr, size, first, count)) return false;
    if (mprotect(base_ + first * page_size_, count * page_size_, to_native(prot)) != 0) {
        PS_LOG_ERROR(kChan, "protect {:#010x}+{:#x} failed: {}", addr, size, std::strerror(errno));
        return false;
    }
    return true;
}

bool GuestMemory::is_committed(u32 addr) const
{
    std::lock_guard lock(mutex_);
    if (!base_) return false;
    const u64 page = u64{addr} / page_size_;
    return (committed_[page / 64] >> (page % 64)) & 1;
}

u64 GuestMemory::committed_bytes() const
{
    std::lock_guard lock(mutex_);
    return committed_pages_ * page_size_;
}

} // namespace pspocket
