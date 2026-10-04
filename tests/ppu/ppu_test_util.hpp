#pragma once

// Shared helpers for the PPU tests: guest memory, random contexts, whole-context comparison.

#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <string>

#include <stdexcept>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/ppu/context.h"
#include "pspocket/ppu/interpreter.hpp"
#include "pspocket/ppu/lifted.h"

namespace pspocket::ppu::test {

// xoshiro256** (deterministic, fast).
class Rng {
public:
    explicit Rng(u64 seed)
    {
        u64 z = seed + 0x9E3779B97F4A7C15ull;
        for (u64& s : s_) {
            z += 0x9E3779B97F4A7C15ull;
            u64 x = z;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            s = x ^ (x >> 31);
        }
    }
    u64 next()
    {
        const u64 result = std::rotl(s_[1] * 5, 7) * 9;
        const u64 t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = std::rotl(s_[3], 45);
        return result;
    }
    u32 below(u32 n) { return n ? static_cast<u32>(next() % n) : 0; }
    bool chance(u32 percent) { return below(100) < percent; }

private:
    u64 s_[4];
};

// Interesting 64-bit integers for register values.
inline u64 random_gpr(Rng& rng)
{
    static constexpr u64 kSpecial[] = {0, 1, 2, 3, 4, 7, 8, 31, 32, 33, 63, 64, 127, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000,
        0xFFFF, 0x7FFFFFFF, 0x80000000ull, 0xFFFFFFFFull, 0x100000000ull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
        0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFF80000000ull, 0xFFFFFFFF7FFFFFFFull, 0xFFFFFFFFFFFF8000ull};
    switch (rng.below(6)) {
    case 0: return kSpecial[rng.below(std::size(kSpecial))];
    case 1: return rng.next() & 0xFF;
    case 2: return static_cast<u64>(static_cast<s64>(static_cast<s32>(rng.next()))); // sign-extended word
    case 3: return rng.next() & 0xFFFFFFFFull;
    case 4: return kSpecial[rng.below(std::size(kSpecial))] + (rng.below(5)) - 2;
    default: return rng.next();
    }
}

// Interesting doubles, as bit patterns.
inline u64 random_fpr_bits(Rng& rng)
{
    static constexpr double kValues[] = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, 2.5, -2.5, 3.5, 0.1, 1e10, -1e10,
        2147483647.0, 2147483647.5, 2147483648.0, -2147483648.0, -2147483648.5, -2147483649.0, 4294967296.0,
        9223372036854775807.0, -9223372036854775808.0, 1.8446744073709552e19, 1e300, -1e300, 1e-300, 3.4028234663852886e38,
        1.1754943508222875e-38, 1.401298464324817e-45, HUGE_VAL, -HUGE_VAL};
    static constexpr u64 kBits[] = {0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF0000000000001ull,
        0x7FF4000000000000ull, 0x7FFC000000000123ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull, 0x000FFFFFFFFFFFFFull,
        0x3FF0000000000001ull, 0x3FEFFFFFFFFFFFFFull, 0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull};
    switch (rng.below(8)) {
    case 0: return std::bit_cast<u64>(kValues[rng.below(std::size(kValues))]);
    case 1: return kBits[rng.below(std::size(kBits))];
    case 2: return std::bit_cast<u64>(static_cast<double>(static_cast<s32>(rng.next())) / (1 + rng.below(1000)));
    case 3: return std::bit_cast<u64>(static_cast<double>(std::bit_cast<float>(static_cast<u32>(rng.next()))));
    case 4: return std::bit_cast<u64>(static_cast<double>(static_cast<s64>(rng.next())));
    case 5: return std::bit_cast<u64>((static_cast<double>(rng.below(2000000)) - 1000000.0) / 8.0);
    default: return rng.next();
    }
}

// A fully random context: every register, CR bit, XER flag, SPR and the reservation.
inline void random_context(PPUContext& ctx, Rng& rng)
{
    ps_ppu_context_init(&ctx);
    for (u64& r : ctx.gpr) r = random_gpr(rng);
    for (double& f : ctx.fpr) f = std::bit_cast<double>(random_fpr_bits(rng));
    for (ps_v128& v : ctx.vr) {
        v.u64[0] = rng.next();
        v.u64[1] = rng.next();
    }
    for (u8& c : ctx.cr) c = static_cast<u8>(rng.next() & 1);
    ctx.lr = rng.chance(50) ? (rng.next() & 0xFFFFFFFFull) : rng.next();
    switch (rng.below(4)) {
    case 0: ctx.ctr = rng.below(3); break;
    case 1: ctx.ctr = rng.next() & 0xFFFF; break;
    default: ctx.ctr = rng.next(); break;
    }
    ctx.xer_so = static_cast<u8>(rng.next() & 1);
    ctx.xer_ov = static_cast<u8>(rng.next() & 1);
    ctx.xer_ca = static_cast<u8>(rng.next() & 1);
    ctx.xer_bc = static_cast<u8>(rng.below(128));
    ctx.fpscr = rng.chance(50) ? static_cast<u32>(rng.next()) : static_cast<u32>(rng.below(4)); // RN mostly
    ctx.vrsave = static_cast<u32>(rng.next());
    ctx.vscr_sat = static_cast<u8>(rng.next() & 1);
    ctx.vscr_nj = static_cast<u8>(rng.next() & 1);
    ctx.reserve_valid = static_cast<u8>(rng.next() & 1);
    ctx.reserve_size = rng.chance(50) ? 4 : 8;
    ctx.reserve_addr = static_cast<u32>(rng.below(0x800)) & ~3u;
    ctx.reserve_value = rng.next();
    ctx.reserve_version = rng.next() | 1; // odd: a version no line has between stores, so a stray stwcx. fails
    for (u8& b : ctx.reserve_line) b = static_cast<u8>(rng.next());
}

inline bool is_nan_bits(u64 b) { return (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (b & 0x000FFFFFFFFFFFFFull); }

// Differences between two contexts ("" when equal). FPRs compare bitwise; with nan_relaxed two NaNs with different
// payload or sign also match (host NaN propagation of FP arithmetic is not specified, as in RPCS3). cia compares
// unless ignore_cia (lifted functions do not maintain it).
inline std::string diff_contexts(const PPUContext& a, const PPUContext& b, bool nan_relaxed, bool ignore_cia = false)
{
    std::string d;
    for (int i = 0; i < 32; i++)
        if (a.gpr[i] != b.gpr[i]) d += std::format(" r{}: {:#x} vs {:#x};", i, a.gpr[i], b.gpr[i]);
    for (int i = 0; i < 32; i++) {
        const u64 x = std::bit_cast<u64>(a.fpr[i]), y = std::bit_cast<u64>(b.fpr[i]);
        if (x != y && !(nan_relaxed && is_nan_bits(x) && is_nan_bits(y)))
            d += std::format(" f{}: {:#018x} vs {:#018x};", i, x, y);
    }
    for (int i = 0; i < 32; i++)
        if (std::memcmp(&a.vr[i], &b.vr[i], 16) != 0) d += std::format(" v{} differs;", i);
    for (int i = 0; i < 32; i++)
        if (a.cr[i] != b.cr[i]) d += std::format(" cr bit {}: {} vs {};", i, a.cr[i], b.cr[i]);
    if (a.lr != b.lr) d += std::format(" lr: {:#x} vs {:#x};", a.lr, b.lr);
    if (a.ctr != b.ctr) d += std::format(" ctr: {:#x} vs {:#x};", a.ctr, b.ctr);
    if (a.xer_so != b.xer_so) d += std::format(" so: {} vs {};", a.xer_so, b.xer_so);
    if (a.xer_ov != b.xer_ov) d += std::format(" ov: {} vs {};", a.xer_ov, b.xer_ov);
    if (a.xer_ca != b.xer_ca) d += std::format(" ca: {} vs {};", a.xer_ca, b.xer_ca);
    if (a.xer_bc != b.xer_bc) d += std::format(" bc: {} vs {};", a.xer_bc, b.xer_bc);
    if (a.fpscr != b.fpscr) d += std::format(" fpscr: {:#010x} vs {:#010x};", a.fpscr, b.fpscr);
    if (a.vrsave != b.vrsave) d += std::format(" vrsave: {:#x} vs {:#x};", a.vrsave, b.vrsave);
    if (a.vscr_sat != b.vscr_sat || a.vscr_nj != b.vscr_nj) d += " vscr;";
    if (!ignore_cia && a.cia != b.cia) d += std::format(" cia: {:#010x} vs {:#010x};", a.cia, b.cia);
    if (a.reserve_valid != b.reserve_valid || a.reserve_addr != b.reserve_addr || a.reserve_size != b.reserve_size ||
        a.reserve_value != b.reserve_value)
        d += std::format(" reservation: {}/{:#x}/{}/{:#x} vs {}/{:#x}/{}/{:#x};", a.reserve_valid, a.reserve_addr,
            a.reserve_size, a.reserve_value, b.reserve_valid, b.reserve_addr, b.reserve_size, b.reserve_value);
    // A live reservation also holds the line's version and bytes (a consumed one keeps what its side last saw).
    if (a.reserve_valid && b.reserve_valid &&
        (a.reserve_version != b.reserve_version || std::memcmp(a.reserve_line, b.reserve_line, 128) != 0))
        d += std::format(" reservation line: version {:#x} vs {:#x};", a.reserve_version, b.reserve_version);
    return d;
}

// Guest memory for tests; the base is checked against the lifted-code alignment assumption (D-013).
inline bool reserve_for_lifted(GuestMemory& mem)
{
    if (!mem.reserve()) return false;
    return (reinterpret_cast<uintptr_t>(mem.base()) % PS_PPU_BASE_ALIGN) == 0;
}

// One guest with code at kCode and data at kData, for single-instruction interpreter tests.
struct Machine {
    static constexpr u32 kCode = 0x00010000;
    static constexpr u32 kData = 0x00020000;

    GuestMemory mem;
    PPUContext ctx{};

    Machine()
    {
        if (!mem.reserve() || !mem.commit(kCode, 0x40000)) throw std::runtime_error("guest memory");
        ps_ppu_context_init(&ctx);
    }

    u8* base() { return mem.base(); }

    // Executes one instruction at pc (default kCode) on the interpreter.
    void exec(u32 insn, u32 pc = kCode)
    {
        ps_write32(base(), pc, insn);
        ctx.cia = pc;
        step(ctx, base());
    }

    u32 r32(u32 addr) { return ps_read32(base(), addr); }
    u64 r64(u32 addr) { return ps_read64(base(), addr); }
    u8 r8(u32 addr) { return ps_read8(base(), addr); }
    void w32(u32 addr, u32 v) { ps_write32(base(), addr, v); }
    void w64(u32 addr, u64 v) { ps_write64(base(), addr, v); }
    void w8(u32 addr, u8 v) { ps_write8(base(), addr, v); }

    // CR field n as a 4-bit value LT GT EQ SO.
    u32 crf(u32 n) const
    {
        return (u32{ctx.cr[n * 4]} << 3) | (u32{ctx.cr[n * 4 + 1]} << 2) | (u32{ctx.cr[n * 4 + 2]} << 1) | ctx.cr[n * 4 + 3];
    }
    void set_crf(u32 n, u32 v)
    {
        for (u32 i = 0; i < 4; i++) ctx.cr[n * 4 + i] = static_cast<u8>((v >> (3 - i)) & 1);
    }
    double f(u32 n) const { return ctx.fpr[n]; }
    u64 fbits(u32 n) const { return std::bit_cast<u64>(ctx.fpr[n]); }
    void set_f(u32 n, double v) { ctx.fpr[n] = v; }
    void set_fbits(u32 n, u64 v) { ctx.fpr[n] = std::bit_cast<double>(v); }
};

inline constexpr u32 LT = 8, GT = 4, EQ = 2, SO = 1; // CR field values

} // namespace pspocket::ppu::test
