#include "pspocket/rsx/io_map.hpp"

namespace pspocket::rsx {

IoMap::IoMap() { clear(); }

void IoMap::clear()
{
    for (auto& e : io_to_ea_) e.store(kUnmapped, std::memory_order_relaxed);
    for (auto& e : ea_to_io_) e.store(kUnmapped, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

bool IoMap::map(u32 io, u32 ea, u32 size)
{
    const u64 io_end = u64{io} + size;
    const u64 ea_end = u64{ea} + size;
    if (size == 0 || (io | ea | size) & (kPageSize - 1) || io_end > u64{kIoPages} << kPageShift ||
        ea_end > u64{kEaPages} << kPageShift) {
        return false;
    }
    const u32 count = size >> kPageShift;
    const u32 io_page = io >> kPageShift;
    const u32 ea_page = ea >> kPageShift;
    for (u32 i = 0; i < count; i++) {
        // RPCS3 sys_rsx_context_iomap: replacing an IO page drops the reverse entry of the EA it used to map.
        const u32 prev_ea = io_to_ea_[io_page + i].exchange((ea_page + i) << kPageShift, std::memory_order_acq_rel);
        if (prev_ea != kUnmapped) ea_to_io_[prev_ea >> kPageShift].store(kUnmapped, std::memory_order_release);
        // The reverse entry points at the latest IO page mapping this EA; an older IO mapping of the same EA keeps
        // working forwards (RPCS3 does not clear it either).
        ea_to_io_[ea_page + i].store((io_page + i) << kPageShift, std::memory_order_release);
    }
    return true;
}

bool IoMap::unmap(u32 io, u32 size)
{
    const u64 io_end = u64{io} + size;
    if (size == 0 || (io | size) & (kPageSize - 1) || io_end > u64{kIoPages} << kPageShift) return false;
    const u32 count = size >> kPageShift;
    const u32 io_page = io >> kPageShift;
    for (u32 i = 0; i < count; i++) {
        const u32 ea = io_to_ea_[io_page + i].exchange(kUnmapped, std::memory_order_acq_rel);
        if (ea != kUnmapped) ea_to_io_[ea >> kPageShift].store(kUnmapped, std::memory_order_release);
    }
    return true;
}

std::optional<u32> IoMap::ea_to_io(u32 ea) const
{
    const u32 io = ea_to_io_[ea >> kPageShift].load(std::memory_order_acquire);
    if (io == kUnmapped) return std::nullopt;
    return io | (ea & (kPageSize - 1));
}

bool IoMap::is_mapped(u32 io, u32 size) const
{
    if (size == 0) return io_to_ea(io).has_value();
    const u64 end = u64{io} + size;
    if (end > u64{kIoPages} << kPageShift) return false;
    for (u64 page = io >> kPageShift; page <= (end - 1) >> kPageShift; page++) {
        if (io_to_ea_[page].load(std::memory_order_acquire) == kUnmapped) return false;
    }
    return true;
}

} // namespace pspocket::rsx
