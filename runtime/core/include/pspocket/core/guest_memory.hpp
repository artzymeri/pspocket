#pragma once

#include <bit>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"

namespace pspocket {

enum class Prot : u8 { None = 0, Read = 1, Write = 2, ReadWrite = 3 };

// The guest's 4 GB address space (plan section 4).
//
// reserve() maps 4 GB + a guard area with no access. Regions become usable through commit() as the guest
// maps them. Guest-to-host translation is a plain add (base() + addr): no fastmem, and faults are used only
// for write tracking. Commit and protection work at host-page granularity (16 KB on Apple silicon); guest
// pages are 4 KB / 64 KB / 1 MB, so callers round guest ranges outwards.
class GuestMemory {
public:
    static constexpr u64 kGuestSize = 0x1'0000'0000ull;
    // A stray 128-bit access at 0xFFFFFFF0+ or a lifted base+offset overrun lands here and faults.
    static constexpr u64 kGuardSize = 0x10'0000ull;
    // base() is aligned to this; lifted prologues tell the compiler so (D-013, PS_PPU_BASE_ALIGN).
    static constexpr u64 kBaseAlign = 0x1'0000ull;

    GuestMemory() = default;
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // Reserves the address space. Fails (returns false, logs why) when the OS refuses, e.g. on iOS
    // without the com.apple.developer.kernel.extended-virtual-addressing entitlement.
    bool reserve();
    void release();
    bool reserved() const { return base_ != nullptr; }

    u8* base() const { return base_; }
    u8* host(u32 addr) const { return base_ + addr; }

    // Makes [addr, addr + size) readable and writable. Newly committed pages read as zero.
    bool commit(u32 addr, u64 size);
    // Returns the pages to the OS and makes them inaccessible again.
    bool decommit(u32 addr, u64 size);
    // Changes protection of committed pages (used by renderer write tracking).
    bool protect(u32 addr, u64 size, Prot prot);
    bool is_committed(u32 addr) const;
    u64 committed_bytes() const;

    static usize host_page_size();

    // Big-endian typed access. T is an unsigned integer, float or double.
    template <class T>
    T read(u32 addr) const
    {
        static_assert(std::is_arithmetic_v<T>);
        if constexpr (sizeof(T) == 1) {
            return static_cast<T>(ps_ld8(host(addr)));
        } else if constexpr (std::is_same_v<T, float>) {
            return ps_ldf32(host(addr));
        } else if constexpr (std::is_same_v<T, double>) {
            return ps_ldf64(host(addr));
        } else if constexpr (sizeof(T) == 2) {
            return static_cast<T>(ps_ld16(host(addr)));
        } else if constexpr (sizeof(T) == 4) {
            return static_cast<T>(ps_ld32(host(addr)));
        } else {
            static_assert(sizeof(T) == 8);
            return static_cast<T>(ps_ld64(host(addr)));
        }
    }

    template <class T>
    void write(u32 addr, T value)
    {
        static_assert(std::is_arithmetic_v<T>);
        if constexpr (sizeof(T) == 1) {
            ps_st8(host(addr), static_cast<u8>(value));
        } else if constexpr (std::is_same_v<T, float>) {
            ps_stf32(host(addr), value);
        } else if constexpr (std::is_same_v<T, double>) {
            ps_stf64(host(addr), value);
        } else if constexpr (sizeof(T) == 2) {
            ps_st16(host(addr), static_cast<u16>(value));
        } else if constexpr (sizeof(T) == 4) {
            ps_st32(host(addr), static_cast<u32>(value));
        } else {
            static_assert(sizeof(T) == 8);
            ps_st64(host(addr), static_cast<u64>(value));
        }
    }

    void read_bytes(u32 addr, void* dst, usize size) const { std::memcpy(dst, host(addr), size); }
    void write_bytes(u32 addr, const void* src, usize size) { std::memcpy(host(addr), src, size); }

private:
    bool page_range(u32 addr, u64 size, u64& first_page, u64& page_count) const;
    void mark(u64 first_page, u64 page_count, bool committed);

    u8* base_ = nullptr;
    usize page_size_ = 0;
    mutable std::mutex mutex_;
    std::vector<u64> committed_; // one bit per host page
    u64 committed_pages_ = 0;
};

} // namespace pspocket
