// Guest MMIO through faults (D-528): the range table and the AArch64 load/store emulator. Encodings: Arm Architecture
// Reference Manual for A-profile, C4.1.66 "Loads and Stores" (load/store register: unsigned immediate, unscaled
// immediate, immediate pre/post-indexed, register offset; load/store register pair).

#include "pspocket/core/mmio.hpp"

#include <array>
#include <atomic>

#if defined(__APPLE__) && defined(__aarch64__)
#include <mach/arm/thread_status.h>
#include <sys/ucontext.h>
#endif

namespace pspocket {

namespace {

struct Range {
    std::atomic<bool> active{false};
    uintptr_t host_start = 0, host_end = 0;
    u32 guest_start = 0;
    MmioDevice* device = nullptr;
};

std::array<Range, 16> g_ranges;

[[maybe_unused]] Range* find(uintptr_t host)
{
    for (Range& r : g_ranges) {
        if (r.active.load(std::memory_order_acquire) && host >= r.host_start && host < r.host_end) return &r;
    }
    return nullptr;
}

[[maybe_unused]] u64 bswap(u64 v, u32 size)
{
    switch (size) {
    case 1: return v & 0xFF;
    case 2: return __builtin_bswap16(static_cast<u16>(v));
    case 4: return __builtin_bswap32(static_cast<u32>(v));
    default: return __builtin_bswap64(v);
    }
}

[[maybe_unused]] u64 sign_extend(u64 v, u32 bits) { return bits >= 64 ? v : static_cast<u64>(static_cast<s64>(v << (64 - bits)) >> (64 - bits)); }

// One decoded load or store.
struct Access {
    bool load = false;
    bool simd = false;  // the register file is V (SIMD and FP)
    bool pair = false;  // LDP/STP: rt and rt2
    u32 size = 0;       // bytes per register: 1, 2, 4, 8 or 16
    bool sign = false;  // sign-extending load
    bool to64 = true;   // the integer destination is an X register (else W: upper 32 bits cleared)
    u32 rt = 0, rt2 = 0, rn = 0;
    s64 offset = 0;     // added to the base for the access (not for post-index)
    enum class Wb { None, Pre, Post } wb = Wb::None;
    s64 wb_imm = 0;     // pre/post-index immediate
    bool reg_offset = false;
    u32 rm = 0, option = 0, shift = 0;
    bool prefetch = false;
};

[[maybe_unused]] bool decode(u32 insn, Access& a)
{
    const u32 size_bits = insn >> 30;
    const bool v = (insn >> 26) & 1;
    if ((insn & 0x3A000000) == 0x28000000) { // load/store pair
        const u32 opc = insn >> 30;
        const u32 type = (insn >> 23) & 3;
        a.load = (insn >> 22) & 1;
        a.pair = true;
        a.simd = v;
        if (v) {
            if (opc == 3) return false;
            a.size = 4u << opc;
        } else {
            if (opc == 0) a.size = 4, a.to64 = false;
            else if (opc == 2) a.size = 8;
            else if (opc == 1 && a.load) a.size = 4, a.sign = true; // LDPSW
            else return false;                                        // STGP (memory tagging)
        }
        a.rt = insn & 31;
        a.rt2 = (insn >> 10) & 31;
        a.rn = (insn >> 5) & 31;
        const s64 imm = static_cast<s64>(sign_extend((insn >> 15) & 0x7F, 7)) * a.size;
        switch (type) {
        case 0:
        case 2: a.offset = imm; break;
        case 1: a.wb = Access::Wb::Post, a.wb_imm = imm; break;
        default: a.wb = Access::Wb::Pre, a.wb_imm = imm, a.offset = imm; break;
        }
        return true;
    }
    const bool unsigned_imm = (insn & 0x3B000000) == 0x39000000;
    const bool imm9 = (insn & 0x3B200000) == 0x38000000;
    const bool regoff = (insn & 0x3B200C00) == 0x38200800;
    if (!unsigned_imm && !imm9 && !regoff) return false;
    const u32 opc = (insn >> 22) & 3;
    a.simd = v;
    if (v) {
        if (size_bits == 0 && (opc & 2)) a.size = 16, a.load = opc & 1;
        else if (opc & 2) return false;
        else a.size = 1u << size_bits, a.load = opc & 1;
    } else {
        a.size = 1u << size_bits;
        switch (opc) {
        case 0: a.load = false, a.to64 = a.size == 8; break;
        case 1: a.load = true, a.to64 = a.size == 8; break;
        case 2:
            if (a.size == 8) a.prefetch = true; // PRFM
            a.load = true, a.sign = true, a.to64 = true;
            break;
        default:
            if (a.size >= 4) return false;
            a.load = true, a.sign = true, a.to64 = false;
            break;
        }
    }
    a.rt = insn & 31;
    a.rn = (insn >> 5) & 31;
    const u32 scale = a.size == 16 ? 4 : size_bits;
    if (unsigned_imm) {
        a.offset = static_cast<s64>((insn >> 10) & 0xFFF) << scale;
    } else if (imm9) {
        const s64 imm = static_cast<s64>(sign_extend((insn >> 12) & 0x1FF, 9));
        switch ((insn >> 10) & 3) {
        case 0: a.offset = imm; break;
        case 1: a.wb = Access::Wb::Post, a.wb_imm = imm; break;
        case 2: a.offset = imm; break; // unprivileged: as unscaled here
        default: a.wb = Access::Wb::Pre, a.wb_imm = imm, a.offset = imm; break;
        }
    } else {
        a.reg_offset = true;
        a.rm = (insn >> 16) & 31;
        a.option = (insn >> 13) & 7;
        a.shift = ((insn >> 12) & 1) ? scale : 0;
        if ((a.option & 2) == 0) return false;
    }
    return true;
}

} // namespace

bool mmio_register(u8* guest_base, u32 addr, u32 size, MmioDevice* device)
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(guest_base) + addr;
    for (Range& r : g_ranges) {
        if (r.active.load(std::memory_order_acquire) && start < r.host_end && start + size > r.host_start) return false;
    }
    for (Range& r : g_ranges) {
        if (r.active.load(std::memory_order_acquire)) continue;
        r.host_start = start;
        r.host_end = start + size;
        r.guest_start = addr;
        r.device = device;
        r.active.store(true, std::memory_order_release);
        return true;
    }
    return false;
}

void mmio_unregister(u32 addr)
{
    for (Range& r : g_ranges) {
        if (r.active.load(std::memory_order_acquire) && r.guest_start == addr) r.active.store(false, std::memory_order_release);
    }
}

#if defined(__APPLE__) && defined(__aarch64__) && !__has_feature(ptrauth_calls)

bool mmio_supported() { return true; }

namespace {

using ThreadState = _STRUCT_ARM_THREAD_STATE64;
using NeonState = _STRUCT_ARM_NEON_STATE64;

// X register n (31: SP when `sp`, else XZR).
u64 xget(const ThreadState& ss, u32 n, bool sp)
{
    if (n < 29) return ss.__x[n];
    if (n == 29) return ss.__fp;
    if (n == 30) return ss.__lr;
    return sp ? ss.__sp : 0;
}

void xset(ThreadState& ss, u32 n, u64 v, bool sp)
{
    if (n < 29) ss.__x[n] = v;
    else if (n == 29) ss.__fp = v;
    else if (n == 30) ss.__lr = v;
    else if (sp) ss.__sp = v;
}

u64 extend_reg(u64 v, u32 option)
{
    switch (option) {
    case 2: return static_cast<u32>(v);                           // UXTW
    case 6: return static_cast<u64>(static_cast<s64>(static_cast<s32>(v))); // SXTW
    default: return v;                                            // LSL (UXTX), SXTX
    }
}

// One register's load or store at `guest` (an MMIO address of the range).
void transfer(Range& r, const Access& a, ThreadState& ss, NeonState& ns, u32 reg, u32 guest)
{
    const u32 off = guest - r.guest_start;
    if (a.simd) {
        if (a.load) {
            __uint128_t value = 0;
            if (a.size == 16) {
                const u64 lo = bswap(r.device->read(off, 8), 8), hi = bswap(r.device->read(off + 8, 8), 8);
                value = (static_cast<__uint128_t>(hi) << 64) | lo;
            } else {
                value = bswap(r.device->read(off, a.size), a.size);
            }
            ns.__v[reg] = value;
        } else {
            const __uint128_t value = ns.__v[reg];
            if (a.size == 16) {
                r.device->write(off, 8, bswap(static_cast<u64>(value), 8));
                r.device->write(off + 8, 8, bswap(static_cast<u64>(value >> 64), 8));
            } else {
                r.device->write(off, a.size, bswap(static_cast<u64>(value), a.size));
            }
        }
        return;
    }
    if (a.load) {
        u64 value = bswap(r.device->read(off, a.size), a.size);
        if (a.sign) value = sign_extend(value, a.size * 8);
        if (!a.to64) value = static_cast<u32>(value);
        xset(ss, reg, value, false);
    } else {
        r.device->write(off, a.size, bswap(xget(ss, reg, false), a.size));
    }
}

} // namespace

bool mmio_handle_fault(const void* fault_addr, void* ucontext)
{
    Range* r = find(reinterpret_cast<uintptr_t>(fault_addr));
    if (!r || !ucontext) return false;
    auto* uc = static_cast<ucontext_t*>(ucontext);
    ThreadState& ss = uc->uc_mcontext->__ss;
    NeonState& ns = uc->uc_mcontext->__ns;
    const u32 insn = *reinterpret_cast<const u32*>(ss.__pc);
    Access a;
    if (!decode(insn, a)) return false;
    if (a.prefetch) {
        ss.__pc += 4;
        return true;
    }
    const u64 base = xget(ss, a.rn, true);
    u64 ea = base + static_cast<u64>(a.offset);
    if (a.reg_offset) ea = base + (extend_reg(xget(ss, a.rm, false), a.option) << a.shift);
    const u64 total = a.pair ? 2ull * a.size : a.size;
    if (ea < r->host_start || ea + total > r->host_end) return false; // straddles the range: not emulated
    const u32 guest = r->guest_start + static_cast<u32>(ea - r->host_start);
    transfer(*r, a, ss, ns, a.rt, guest);
    if (a.pair) transfer(*r, a, ss, ns, a.rt2, guest + a.size);
    if (a.wb == Access::Wb::Pre) xset(ss, a.rn, base + static_cast<u64>(a.wb_imm), true);
    if (a.wb == Access::Wb::Post) xset(ss, a.rn, base + static_cast<u64>(a.wb_imm), true);
    ss.__pc += 4;
    return true;
}

#else

bool mmio_supported() { return false; }
bool mmio_handle_fault(const void*, void*) { return false; }

#endif

} // namespace pspocket
