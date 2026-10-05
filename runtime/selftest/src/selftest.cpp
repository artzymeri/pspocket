#include <array>
#include <chrono>
#include <string>

#include "probe.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#ifndef PSPOCKET_VERSION
#define PSPOCKET_VERSION "0.1.0"
#endif
#ifndef PSPOCKET_GIT_HASH
#define PSPOCKET_GIT_HASH "unknown"
#endif

namespace pspocket::selftest {

std::span<const Probe> all_probes()
{
    // Order matters: cheap platform facts first, then memory, then compute.
    static constexpr std::array probes{
        Probe{"platform", "Host facts: OS, CPU cores, memory, page size", probe_platform},
        Probe{"mem.reserve", "Reserve the 4 GB guest space and commit main, RSX and stack regions", probe_mem_reserve},
        Probe{"mem.bandwidth", "Big-endian load/store throughput through guest memory", probe_mem_bandwidth},
        Probe{"mem.fault_tracking", "Write tracking by page protection: fault, mark dirty, resume", probe_mem_fault_tracking},
        Probe{"timebase", "Guest timebase runs at 79.8 MHz", probe_timebase},
        Probe{"threads.qos", "Single-thread speed per QoS class (performance vs efficiency cores)", probe_threads_qos},
        Probe{"ppu.interp", "PPU interpreter: checksum + recursive fib benchmark, MIPS", probe_ppu_interp},
        Probe{"ppu.lifted", "Lifted PPU code: same benchmark, speedup over the interpreter", probe_ppu_lifted},
        Probe{"codegen.dlopen", "Libraries made after the app was signed: signed, loaded and run (route B)", probe_codegen_dlopen},
#if PSPOCKET_HAVE_CODEGEN
        Probe{"codegen.compile", "C compiled, linked and signed on this device; translated code compiled (route B)", probe_codegen_compile},
#endif
    };
    return probes;
}

} // namespace pspocket::selftest

using namespace pspocket::selftest;

namespace {

ps_selftest_summary run_probe(const Probe& probe, ps_selftest_callback callback, void* user)
{
    ps_selftest_summary summary{};
    ProbeContext ctx(probe.name, callback, user);
    ctx.emit(PS_SELFTEST_BEGIN, probe.description);
    const auto start = std::chrono::steady_clock::now();
    probe.run(ctx);
    summary.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (ctx.skipped()) {
        summary.skipped = 1;
        ctx.emit(PS_SELFTEST_SKIP, ctx.skip_reason());
    } else if (ctx.failed()) {
        summary.failed = 1;
        ctx.emit(PS_SELFTEST_FAIL, ctx.summary().empty() ? "failed" : ctx.summary());
    } else {
        summary.passed = 1;
        ctx.emit(PS_SELFTEST_PASS, ctx.summary().empty() ? "ok" : ctx.summary());
    }
    return summary;
}

} // namespace

extern "C" int ps_selftest_probe_count(void) { return static_cast<int>(all_probes().size()); }

extern "C" const char* ps_selftest_probe_name(int index)
{
    const auto probes = all_probes();
    return index >= 0 && static_cast<size_t>(index) < probes.size() ? probes[static_cast<size_t>(index)].name : nullptr;
}

extern "C" const char* ps_selftest_probe_description(int index)
{
    const auto probes = all_probes();
    return index >= 0 && static_cast<size_t>(index) < probes.size() ? probes[static_cast<size_t>(index)].description
                                                                     : nullptr;
}

extern "C" ps_selftest_summary ps_selftest_run_one(int index, ps_selftest_callback callback, void* user)
{
    const auto probes = all_probes();
    if (index < 0 || static_cast<size_t>(index) >= probes.size()) return ps_selftest_summary{0, 1, 0, 0.0};
    return run_probe(probes[static_cast<size_t>(index)], callback, user);
}

extern "C" ps_selftest_summary ps_selftest_run_all(ps_selftest_callback callback, void* user)
{
    ps_selftest_summary total{};
    for (const Probe& probe : all_probes()) {
        const ps_selftest_summary s = run_probe(probe, callback, user);
        total.passed += s.passed;
        total.failed += s.failed;
        total.skipped += s.skipped;
        total.seconds += s.seconds;
    }
    return total;
}

extern "C" const char* ps_selftest_build_info(void)
{
    static const std::string info = [] {
        std::string s = "pspocket " PSPOCKET_VERSION " (" PSPOCKET_GIT_HASH ")";
#if defined(NDEBUG)
        s += ", release";
#else
        s += ", debug";
#endif
#if defined(__clang__)
        s += ", clang " __clang_version__;
#endif
#if defined(__APPLE__)
#if TARGET_OS_IPHONE
        s += ", iOS";
#else
        s += ", macOS";
#endif
#elif defined(__linux__)
        s += ", Linux";
#endif
#if defined(__aarch64__)
        s += " arm64";
#elif defined(__x86_64__)
        s += " x86_64";
#endif
        return s;
    }();
    return info.c_str();
}
