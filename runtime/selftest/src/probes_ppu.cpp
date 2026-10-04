// PPU probes: interpreter throughput and the speed of lifted code on the same synthetic benchmark.
//
// The benchmark (recomp/ppu synthetic::build_selftest_bench: a checksum loop over a 64 KB buffer plus recursive fib,
// with calls, loads and stores) is generated at build time into ${CMAKE_BINARY_DIR}/generated/selftest/:
//   ps_selftest_ppu_image.h / .c   the guest image, its parameters and the natively computed expected result
//   ps_selftest_ppu.h, ps_selftest_ppu_0.c, ps_selftest_ppu_reg.c   the image lifted by ppu-lift
// The iOS app compiles those .c files from the host build. Without them (a build that never ran the host
// generator) both probes report SKIP instead of failing to link.

#include "probe.hpp"

// CMake defines PSPOCKET_SELFTEST_PPU_REQUIRED when it generates the sources (a missing header is then an error)
// and PSPOCKET_SELFTEST_PPU_DISABLED when runtime/ppu or recomp/ppu is left out (PSPOCKET_ONLY). The iOS build
// defines neither and goes by what the host build generated.
#if defined(PSPOCKET_SELFTEST_PPU_DISABLED)
#define PS_SELFTEST_PPU_AVAILABLE 0
#elif __has_include("ps_selftest_ppu_image.h") && __has_include("ps_selftest_ppu.h")
#define PS_SELFTEST_PPU_AVAILABLE 1
#else
#define PS_SELFTEST_PPU_AVAILABLE 0
#if defined(PSPOCKET_SELFTEST_PPU_REQUIRED)
#error "generated PPU self-test sources are missing (runtime/selftest/CMakeLists.txt generates them)"
#endif
#endif

#if PS_SELFTEST_PPU_AVAILABLE
#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "ps_selftest_ppu.h"
#include "ps_selftest_ppu_image.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/ppu/interpreter.hpp"
#include "pspocket/ppu/lifted.h"
#include "pspocket/ppu/runtime.h"
#endif

namespace pspocket::selftest {

#if PS_SELFTEST_PPU_AVAILABLE
namespace {

using Clock = std::chrono::steady_clock;

constexpr u32 kBuffer = 0x00100000;
constexpr u32 kStackBase = 0x00E00000;
constexpr u32 kStackSize = 0x00100000;
constexpr u32 kStackTop = kStackBase + kStackSize - 0x1000;
constexpr u32 kSentinel = 0x00000100; // return address: the interpreter stops there
constexpr u32 kBufferBytes = PS_SELFTEST_PPU_WORDS * 4;

class Bench {
public:
    bool init(ProbeContext& ctx)
    {
        if (!ctx.check(mem_.reserve(), "reserve guest memory")) return false;
        if (!ctx.check(mem_.commit(PS_SELFTEST_PPU_IMAGE_BASE, PS_SELFTEST_PPU_IMAGE_SIZE), "commit code")) return false;
        if (!ctx.check(mem_.commit(kBuffer, kBufferBytes), "commit buffer")) return false;
        if (!ctx.check(mem_.commit(kStackBase, kStackSize), "commit stack")) return false;
        return true;
    }

    u8* base() const { return mem_.base(); }

    // Fresh image, buffer pattern and stack; the benchmark arguments in a clean context.
    PPUContext reset()
    {
        mem_.write_bytes(PS_SELFTEST_PPU_IMAGE_BASE, ps_selftest_ppu_image, PS_SELFTEST_PPU_IMAGE_SIZE);
        for (u32 i = 0; i < PS_SELFTEST_PPU_WORDS; i++) mem_.write<u32>(kBuffer + i * 4, PS_SELFTEST_PPU_PATTERN(i));
        std::memset(mem_.host(kStackBase), 0, kStackSize);
        PPUContext c;
        ps_ppu_context_init(&c);
        c.gpr[1] = kStackTop;
        c.gpr[3] = kBuffer;
        c.gpr[4] = PS_SELFTEST_PPU_WORDS;
        c.gpr[5] = PS_SELFTEST_PPU_ITERATIONS;
        c.gpr[6] = PS_SELFTEST_PPU_FIB_N;
        c.lr = kSentinel;
        return c;
    }

    std::vector<u8> buffer() const
    {
        std::vector<u8> out(kBufferBytes);
        mem_.read_bytes(kBuffer, out.data(), out.size());
        return out;
    }

private:
    GuestMemory mem_;
};

struct InterpResult {
    PPUContext ctx;
    std::vector<u8> buffer;
    u64 steps = 0;
    double seconds = 0;
    bool returned = false;
};

InterpResult run_interpreter(Bench& bench)
{
    InterpResult r;
    r.ctx = bench.reset();
    r.ctx.cia = PS_SELFTEST_PPU_ENTRY;
    ppu::RunOptions opt;
    opt.stop_at_return = true;
    opt.stop_lr = kSentinel;
    opt.stop_sp = r.ctx.gpr[1];
    opt.max_steps = 2'000'000'000ull;
    const auto start = Clock::now();
    const ppu::RunResult res = ppu::run(r.ctx, bench.base(), opt);
    r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    r.steps = res.steps;
    r.returned = res.reason == ppu::StopReason::Returned;
    r.buffer = bench.buffer();
    return r;
}

bool same_context(const PPUContext& a, const PPUContext& b)
{
    // Everything but cia (lifted code does not maintain it) and the reservation's line version (the second run's
    // lwarx sees the versions the first run's stwcx. moved on) must match bit for bit.
    PPUContext x = a, y = b;
    x.cia = y.cia = 0;
    x.reserve_version = y.reserve_version = 0;
    return std::memcmp(&x, &y, sizeof(PPUContext)) == 0;
}

} // namespace
#endif

void probe_ppu_interp(ProbeContext& ctx)
{
#if !PS_SELFTEST_PPU_AVAILABLE
    ctx.skip("generated PPU benchmark not present (build the host CMake tree, which generates it)");
#else
    Bench bench;
    if (!bench.init(ctx)) return;
    const InterpResult r = run_interpreter(bench);
    if (!ctx.check(r.returned, "benchmark returned to the caller")) return;
    ctx.check(r.ctx.gpr[3] == PS_SELFTEST_PPU_EXPECTED,
        std::format("result {:#018x} matches the native reference {:#018x}", r.ctx.gpr[3], PS_SELFTEST_PPU_EXPECTED));
    const double mips = static_cast<double>(r.steps) / r.seconds / 1e6;
    ctx.metric("instructions = {}", r.steps);
    ctx.metric("time = {:.1f} ms", r.seconds * 1e3);
    ctx.metric("interpreter = {:.1f} MIPS", mips);
    ctx.set_summary(std::format("{:.0f} MIPS ({} instructions in {:.0f} ms)", mips, r.steps, r.seconds * 1e3));
#endif
}

void probe_ppu_lifted(ProbeContext& ctx)
{
#if !PS_SELFTEST_PPU_AVAILABLE
    ctx.skip("generated lifted PPU code not present (build the host CMake tree, which generates it)");
#else
    Bench bench;
    if (!bench.init(ctx)) return;
    if (!ctx.check(reinterpret_cast<uintptr_t>(bench.base()) % PS_PPU_BASE_ALIGN == 0,
            "guest base is 64 KB-aligned, as lifted code assumes (D-013)"))
        return;

    const InterpResult ref = run_interpreter(bench);
    if (!ctx.check(ref.returned && ref.ctx.gpr[3] == PS_SELFTEST_PPU_EXPECTED, "interpreter reference run")) return;

    ps_selftest_ppu_register();
    const ps_ppu_fn entry = ps_ppu_dispatch_find(PS_SELFTEST_PPU_ENTRY);
    if (!ctx.check(entry != nullptr, "lifted entry registered in the dispatch table")) {
        ps_selftest_ppu_unregister();
        return;
    }

    double best = 1e30;
    bool identical = true;
    constexpr int kRuns = 5;
    for (int run = 0; run < kRuns; run++) {
        PPUContext c = bench.reset();
        const auto start = std::chrono::steady_clock::now();
        entry(&c, bench.base());
        best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        identical = identical && same_context(c, ref.ctx) && bench.buffer() == ref.buffer;
    }
    ps_selftest_ppu_unregister();

    ctx.check(identical, "lifted runs end in the interpreter's exact context and memory");
    const double speedup = ref.seconds / best;
    ctx.metric("lifted = {:.2f} ms (best of {})", best * 1e3, kRuns);
    ctx.metric("interpreter = {:.1f} ms", ref.seconds * 1e3);
    ctx.metric("speedup = {:.1f}x", speedup);
    ctx.metric("lifted equivalent = {:.0f} MIPS", static_cast<double>(ref.steps) / best / 1e6);
    ctx.set_summary(std::format("{:.1f}x faster than the interpreter ({:.2f} ms vs {:.1f} ms)", speedup, best * 1e3,
        ref.seconds * 1e3));
#endif
}

} // namespace pspocket::selftest
