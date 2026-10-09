#pragma once

// Guest memory-mapped I/O through faults (D-528).
//
// Guest code reaches guest memory directly (base + address), so a device register cannot answer a load by itself.
// An MMIO range is guest address space left without host pages: every load and store there faults, and the platform's
// SIGSEGV/SIGBUS handler calls mmio_handle_fault(), which decodes the faulting AArch64 load or store, performs it on the
// range's device and resumes the thread after the instruction. The decoder covers the forms compiled code uses for
// memory: LDR/STR and their sign-extending, byte and halfword forms with an unsigned, unscaled, pre- or post-indexed
// immediate or a register offset, LDP/STP (and LDPSW), and the SIMD and FP registers' loads and stores (8 to 128
// bits). Exclusive and atomic accesses are not emulated: the fault stays a fault.
//
// Values are the guest's: memory is big-endian, so a 4-byte device register holding V is the bytes of V in big-endian
// order, and the host load that reads them gets them as the CPU would from memory (compiled code byte-swaps after).
// A device sees each access as (offset from the range's start, size 1, 2, 4 or 8, value); a 16-byte access is two
// 8-byte ones, a pair two of its element's size, lower address first.
//
// Only Apple's AArch64 targets (iOS, macOS on Apple silicon) decode faults; elsewhere mmio_handle_fault() is false.

#include "pspocket/core/types.hpp"

namespace pspocket {

class MmioDevice {
public:
    virtual ~MmioDevice() = default;
    virtual u64 read(u32 offset, u32 size) = 0;
    virtual void write(u32 offset, u32 size, u64 value) = 0;
};

// Registers [addr, addr + size) of the guest space at `guest_base` (its host pages must stay inaccessible). False when
// the table (16 ranges) is full or the range overlaps one. Not async-signal-safe: call outside the handler.
bool mmio_register(u8* guest_base, u32 addr, u32 size, MmioDevice* device);
// Removes the range starting at `addr`. The caller makes sure no access to it is in flight.
void mmio_unregister(u32 addr);
// From a SIGSEGV/SIGBUS handler on the faulting thread: true when `fault_addr` is in a registered range and the
// instruction was emulated (the context's PC moved past it).
bool mmio_handle_fault(const void* fault_addr, void* ucontext);
// Whether this build can emulate faults (Apple AArch64).
bool mmio_supported();

} // namespace pspocket
