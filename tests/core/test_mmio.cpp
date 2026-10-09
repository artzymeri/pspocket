// Guest MMIO through faults (D-528): each load and store form the emulator decodes, run against a recording device in
// an inaccessible guest range with a real SIGSEGV/SIGBUS handler.

#include <catch_amalgamated.hpp>

#include <csignal>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/mmio.hpp"

using namespace pspocket;

namespace {

constexpr u32 kRange = 0xE0040000, kRangeSize = 0x20000;

struct Recorder final : MmioDevice {
    struct Write {
        u32 offset, size;
        u64 value;
    };
    std::vector<Write> writes;
    std::vector<std::pair<u32, u32>> reads;
    // Register at offset o holds 0x1122334455667788 + o, truncated to the access size.
    u64 read(u32 offset, u32 size) override
    {
        reads.emplace_back(offset, size);
        const u64 v = 0x1122334455667788ull + offset;
        return size == 8 ? v : v & ((1ull << (size * 8)) - 1);
    }
    void write(u32 offset, u32 size, u64 value) override { writes.push_back({offset, size, value}); }
};

struct sigaction g_old_segv, g_old_bus;

void on_fault(int sig, siginfo_t* info, void* uctx)
{
    if (mmio_handle_fault(info->si_addr, uctx)) return;
    sigaction(sig, sig == SIGBUS ? &g_old_bus : &g_old_segv, nullptr);
    raise(sig);
}

struct Handler {
    Handler()
    {
        struct sigaction sa{};
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &g_old_segv);
        sigaction(SIGBUS, &sa, &g_old_bus);
    }
    ~Handler()
    {
        sigaction(SIGSEGV, &g_old_segv, nullptr);
        sigaction(SIGBUS, &g_old_bus, nullptr);
    }
};

} // namespace

TEST_CASE("MMIO: guest loads and stores of a device register through faults", "[core][mmio]")
{
    if (!mmio_supported()) SKIP("fault emulation needs Apple AArch64");
    GuestMemory mem;
    REQUIRE(mem.reserve());
    Recorder dev;
    REQUIRE(mmio_register(mem.base(), kRange, kRangeSize, &dev));
    CHECK(!mmio_register(mem.base(), kRange + 0x1000, 0x1000, &dev)); // overlaps
    Handler handler;
    u8* base = mem.base();

    // The guest's view through the big-endian accessors (compiled loads and stores, then byte swaps).
    CHECK(ps_read32(base, kRange + 0x401C) == static_cast<u32>(0x55667788 + 0x401C));
    CHECK(ps_read16(base, kRange + 0x10) == static_cast<u16>(0x7788 + 0x10));
    CHECK(ps_read8(base, kRange + 0x3) == static_cast<u8>(0x88 + 0x3));
    CHECK(ps_read64(base, kRange + 0x3000) == 0x1122334455667788ull + 0x3000);
    ps_write32(base, kRange + 0x400C, 0xCAFEF00D);
    ps_write16(base, kRange + 0x20, 0xBEEF);
    ps_write64(base, kRange + 0x3008, 0x0123456789ABCDEFull);
    REQUIRE(dev.writes.size() == 3);
    CHECK(dev.writes[0].offset == 0x400C);
    CHECK(dev.writes[0].size == 4);
    CHECK(dev.writes[0].value == 0xCAFEF00D);
    CHECK(dev.writes[1].value == 0xBEEF);
    CHECK(dev.writes[2].size == 8);
    CHECK(dev.writes[2].value == 0x0123456789ABCDEFull);

#if defined(__aarch64__)
    u8* p = base + kRange;
    dev.writes.clear();
    // Sign-extending, pair, indexed and register-offset forms (raw host loads: the bytes as memory holds them).
    {
        s64 v = 0;
        asm volatile("ldrsw %0, [%1, #8]" : "=r"(v) : "r"(p) : "memory");
        CHECK(static_cast<u32>(v) == __builtin_bswap32(static_cast<u32>(0x55667788 + 8)));
        CHECK(v == static_cast<s32>(__builtin_bswap32(static_cast<u32>(0x55667788 + 8))));
        s64 b = 0;
        asm volatile("ldrsb %0, [%1, #0x80]" : "=r"(b) : "r"(p) : "memory");
        CHECK(b == static_cast<s8>(0x88 + 0x80));
    }
    {
        u32 lo = 0, hi = 0;
        asm volatile("ldp %w0, %w1, [%2, #0x40]" : "=r"(lo), "=r"(hi) : "r"(p) : "memory");
        CHECK(lo == __builtin_bswap32(static_cast<u32>(0x55667788 + 0x40)));
        CHECK(hi == __builtin_bswap32(static_cast<u32>(0x55667788 + 0x44)));
        const u64 a = 0x1111111122222222ull, b = 0x3333333344444444ull;
        asm volatile("stp %0, %1, [%2, #0x50]" : : "r"(a), "r"(b), "r"(p) : "memory");
        REQUIRE(dev.writes.size() == 2);
        CHECK(dev.writes[0].offset == 0x50);
        CHECK(dev.writes[0].value == __builtin_bswap64(a));
        CHECK(dev.writes[1].offset == 0x58);
        CHECK(dev.writes[1].value == __builtin_bswap64(b));
    }
    {
        u8* q = p + 0x100;
        u32 v = 0;
        asm volatile("ldr %w0, [%1, #4]!" : "=r"(v), "+r"(q) : : "memory");
        CHECK(q == p + 0x104);
        CHECK(v == __builtin_bswap32(static_cast<u32>(0x55667788 + 0x104)));
        asm volatile("ldr %w0, [%1], #-4" : "=r"(v), "+r"(q) : : "memory");
        CHECK(q == p + 0x100);
        CHECK(v == __builtin_bswap32(static_cast<u32>(0x55667788 + 0x104)));
        const u64 idx = 0x30;
        asm volatile("ldr %w0, [%1, %2, lsl #2]" : "=r"(v) : "r"(p), "r"(idx) : "memory");
        CHECK(v == __builtin_bswap32(static_cast<u32>(0x55667788 + 0xC0)));
        asm volatile("ldur %w0, [%1, #-4]" : "=r"(v) : "r"(p + 0x204) : "memory");
        CHECK(v == __builtin_bswap32(static_cast<u32>(0x55667788 + 0x200)));
    }
    {
        // SIMD: a 128-bit load is two 8-byte reads; a store two writes.
        dev.writes.clear();
        __uint128_t v = 0;
        asm volatile("ldr %q0, [%1, #0x300]" : "=w"(v) : "r"(p) : "memory");
        CHECK(static_cast<u64>(v) == __builtin_bswap64(0x1122334455667788ull + 0x300));
        CHECK(static_cast<u64>(v >> 64) == __builtin_bswap64(0x1122334455667788ull + 0x308));
        asm volatile("str %q0, [%1, #0x310]" : : "w"(v), "r"(p) : "memory");
        REQUIRE(dev.writes.size() == 2);
        CHECK(dev.writes[0].offset == 0x310);
        CHECK(dev.writes[0].value == 0x1122334455667788ull + 0x300);
        CHECK(dev.writes[1].value == 0x1122334455667788ull + 0x308);
        double d = 0;
        asm volatile("ldr %d0, [%1, #0x320]" : "=w"(d) : "r"(p) : "memory");
        u64 bits = 0;
        std::memcpy(&bits, &d, 8);
        CHECK(bits == __builtin_bswap64(0x1122334455667788ull + 0x320));
    }
#endif
    mmio_unregister(kRange);
}
