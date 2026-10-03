// Core platform probes: memory model, fault-based write tracking, timebase, thread QoS.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <pthread.h>
#include <string>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <pthread/qos.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#endif

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/core/timebase.h"
#include "probe.hpp"

namespace pspocket::selftest {
namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

#if defined(__APPLE__)
template <class T>
bool sysctl_value(const char* name, T& out)
{
    size_t size = sizeof(T);
    return sysctlbyname(name, &out, &size, nullptr, 0) == 0;
}

std::string sysctl_string(const char* name)
{
    char buf[256] = {};
    size_t size = sizeof(buf) - 1;
    if (sysctlbyname(name, buf, &size, nullptr, 0) != 0) return "?";
    return buf;
}
#endif

} // namespace

bool debugger_attached()
{
#if defined(__APPLE__)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info {};
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) return false;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
    return false;
#endif
}

void probe_platform(ProbeContext& ctx)
{
    utsname name{};
    uname(&name);
    // utsname fields are fixed-size arrays; format them as C strings, not as whole arrays.
    const std::string_view sysname(name.sysname), release(name.release), machine(name.machine);
    ctx.info("kernel: {} {} ({})", sysname, release, machine);
#if defined(__APPLE__)
    ctx.info("model: {}", sysctl_string("hw.machine"));
    ctx.info("cpu: {}", sysctl_string("machdep.cpu.brand_string"));
    int ncpu = 0, pcores = 0, ecores = 0;
    sysctl_value("hw.ncpu", ncpu);
    sysctl_value("hw.perflevel0.physicalcpu", pcores);
    sysctl_value("hw.perflevel1.physicalcpu", ecores);
    ctx.metric("cores = {} ({} performance, {} efficiency)", ncpu, pcores, ecores);
    uint64_t memsize = 0;
    sysctl_value("hw.memsize", memsize);
    ctx.metric("physical_memory = {:.1f} GB", static_cast<double>(memsize) / (1u << 30));
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // What jetsam lets this process use right now; the increased-memory-limit entitlement raises it.
    ctx.metric("process_available_memory = {:.2f} GB", static_cast<double>(os_proc_available_memory()) / (1u << 30));
#endif
    ctx.metric("host_page_size = {} KB", GuestMemory::host_page_size() >> 10);
    ctx.info("debugger attached: {}", debugger_attached() ? "yes" : "no");
    ctx.set_summary(std::format("{} {}", sysname, release));
}

void probe_mem_reserve(ProbeContext& ctx)
{
    GuestMemory mem;
    auto start = Clock::now();
    if (!mem.reserve()) {
        ctx.fail("could not reserve 4 GB + guard of address space");
        ctx.set_summary("reserve failed (is the extended-virtual-addressing entitlement signed in?)");
        return;
    }
    ctx.metric("reserve_time = {:.3f} ms", ms_since(start));

    struct Region {
        const char* name;
        u32 base;
        u64 size;
    };
    const Region regions[] = {
        {"main", PS_MEM_MAIN_BASE, PS_MEM_MAIN_END - PS_MEM_MAIN_BASE},
        {"rsx_local", PS_MEM_RSX_LOCAL_BASE, PS_MEM_RSX_LOCAL_SIZE},
        {"stack", PS_MEM_STACK_BASE, 16u << 20},
    };

    const usize page = GuestMemory::host_page_size();
    u64 total = 0;
    for (const Region& r : regions) {
        start = Clock::now();
        if (!ctx.check(mem.commit(r.base, r.size), std::format("commit {} {:#010x}+{:#x}", r.name, r.base, r.size)))
            return;
        const double commit_ms = ms_since(start);

        // First touch of every host page: measures the page-fault-in cost and makes the memory resident.
        start = Clock::now();
        const u64 pages = r.size / page;
        for (u64 i = 0; i < pages; i++) {
            const u32 addr = r.base + static_cast<u32>(i * page);
            mem.write<u32>(addr, addr ^ 0xA5A5A5A5u);
        }
        const double touch_ms = ms_since(start);
        bool ok = true;
        for (u64 i = 0; i < pages && ok; i++) {
            const u32 addr = r.base + static_cast<u32>(i * page);
            ok = mem.read<u32>(addr) == (addr ^ 0xA5A5A5A5u);
        }
        ctx.check(ok, std::format("{} pattern readback", r.name));
        ctx.metric("{}: {} MB committed in {:.2f} ms, first touch {:.2f} ms ({:.2f} us/page)", r.name, r.size >> 20,
            commit_ms, touch_ms, touch_ms * 1000.0 / static_cast<double>(pages));
        total += r.size;
    }

    // The very top of the guest space must work too (stray 32-bit addresses wrap there).
    const u32 last_page = static_cast<u32>(GuestMemory::kGuestSize - page);
    if (ctx.check(mem.commit(last_page, page), "commit the last guest page")) {
        mem.write<u32>(0xFFFFFFFCu, 0xCAFEF00Du);
        ctx.check(mem.read<u32>(0xFFFFFFFCu) == 0xCAFEF00Du, "read back at 0xFFFFFFFC");
    }

    ctx.metric("resident_guest_memory = {} MB", mem.committed_bytes() >> 20);
    for (const Region& r : regions) mem.decommit(r.base, r.size);
    ctx.check(mem.committed_bytes() == page, "decommit returns the regions");
    ctx.set_summary(std::format("4 GB reserved, {} MB committed and verified", total >> 20));
}

void probe_mem_bandwidth(ProbeContext& ctx)
{
    GuestMemory mem;
    if (!ctx.check(mem.reserve(), "reserve")) return;
    constexpr u32 kBase = PS_MEM_MAIN_BASE;
    constexpr u32 kSize = 64u << 20;
    if (!ctx.check(mem.commit(kBase, kSize), "commit 64 MB")) return;
    u8* base = mem.base();

    // Warm up (fault everything in) so the timings measure the accessors, not the kernel.
    std::memset(mem.host(kBase), 0, kSize);

    constexpr int kRounds = 4;
    auto start = Clock::now();
    for (int round = 0; round < kRounds; round++)
        for (u32 off = 0; off < kSize; off += 4) ps_write32(base, kBase + off, off + static_cast<u32>(round));
    const double write_ms = ms_since(start);

    start = Clock::now();
    u64 sum = 0;
    for (int round = 0; round < kRounds; round++)
        for (u32 off = 0; off < kSize; off += 4) sum += ps_read32(base, kBase + off);
    const double read_ms = ms_since(start);

    std::vector<u8> scratch(kSize);
    start = Clock::now();
    for (int round = 0; round < kRounds; round++) {
        std::memcpy(scratch.data(), mem.host(kBase), kSize);
        asm volatile("" : : "r"(scratch.data()) : "memory"); // keep the copy
    }
    const double copy_ms = ms_since(start);

    const double gb = static_cast<double>(kSize) * kRounds / 1e9;
    ctx.metric("be_write32 = {:.2f} GB/s", gb / (write_ms / 1000.0));
    ctx.metric("be_read32 = {:.2f} GB/s (checksum {:x})", gb / (read_ms / 1000.0), sum);
    ctx.metric("memcpy = {:.2f} GB/s", gb / (copy_ms / 1000.0));
    ctx.check(ps_read32(base, kBase + 4) == 4u + kRounds - 1, "last write visible");
    ctx.set_summary(std::format("read {:.1f} GB/s, write {:.1f} GB/s", gb / (read_ms / 1000.0),
        gb / (write_ms / 1000.0)));
}

// ---- fault-based write tracking (plan section 4) ----

namespace {

std::atomic<u8*> g_track_begin{nullptr};
std::atomic<u8*> g_track_end{nullptr};
std::atomic<int> g_track_faults{0};
size_t g_track_page = 0;
u8* g_track_dirty = nullptr;
struct sigaction g_prev_segv {};
struct sigaction g_prev_bus {};

void track_handler(int sig, siginfo_t* info, void* uctx)
{
    u8* addr = static_cast<u8*>(info->si_addr);
    u8* begin = g_track_begin.load(std::memory_order_relaxed);
    u8* end = g_track_end.load(std::memory_order_relaxed);
    if (begin && addr >= begin && addr < end) {
        const size_t index = static_cast<size_t>(addr - begin) / g_track_page;
        u8* page = begin + index * g_track_page;
        mprotect(page, g_track_page, PROT_READ | PROT_WRITE);
        g_track_dirty[index] = 1;
        g_track_faults.fetch_add(1, std::memory_order_relaxed);
        return; // resume: the faulting store re-executes and succeeds
    }
    // Not ours: hand it to whoever was installed before us.
    const struct sigaction& prev = sig == SIGBUS ? g_prev_bus : g_prev_segv;
    if (prev.sa_flags & SA_SIGINFO) {
        if (prev.sa_sigaction) {
            prev.sa_sigaction(sig, info, uctx);
            return;
        }
    } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler) {
        prev.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
}

} // namespace

void probe_mem_fault_tracking(ProbeContext& ctx)
{
    if (debugger_attached()) {
        ctx.skip("a debugger is attached and would stop on the intentional faults; run without it "
                 "(or in LLDB: process handle -p true -s false SIGSEGV SIGBUS)");
        return;
    }

    GuestMemory mem;
    if (!ctx.check(mem.reserve(), "reserve")) return;
    const usize page = GuestMemory::host_page_size();
    constexpr u64 kPages = 256;
    const u32 base = PS_MEM_RSX_LOCAL_BASE;
    if (!ctx.check(mem.commit(base, kPages * page), "commit tracked region")) return;

    std::vector<u8> dirty(kPages, 0);
    g_track_page = page;
    g_track_dirty = dirty.data();
    g_track_faults = 0;
    g_track_begin = mem.host(base);
    g_track_end = mem.host(base) + kPages * page;

    struct sigaction sa {};
    sa.sa_sigaction = track_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS, &sa, &g_prev_bus);

    // Pass 1: every page write-protected, each first write faults once.
    mem.protect(base, kPages * page, Prot::Read);
    auto start = Clock::now();
    for (u64 i = 0; i < kPages; i++) mem.write<u32>(base + static_cast<u32>(i * page + 64), static_cast<u32>(i));
    const double fault_ms = ms_since(start);
    const int faults = g_track_faults.load();

    // Pass 2: pages are writable again, no faults.
    start = Clock::now();
    for (u64 i = 0; i < kPages; i++) mem.write<u32>(base + static_cast<u32>(i * page + 128), static_cast<u32>(i));
    const double plain_ms = ms_since(start);
    const int faults_after = g_track_faults.load();

    sigaction(SIGSEGV, &g_prev_segv, nullptr);
    sigaction(SIGBUS, &g_prev_bus, nullptr);
    g_track_begin = nullptr;
    g_track_end = nullptr;

    u64 dirty_count = 0;
    for (u8 d : dirty) dirty_count += d;
    bool values_ok = true;
    for (u64 i = 0; i < kPages; i++)
        values_ok = values_ok && mem.read<u32>(base + static_cast<u32>(i * page + 64)) == i;

    ctx.check(faults == static_cast<int>(kPages), std::format("one fault per page (got {})", faults));
    ctx.check(faults_after == faults, "no faults once pages are unprotected");
    ctx.check(dirty_count == kPages, "every page marked dirty");
    ctx.check(values_ok, "faulting stores completed");
    const double us_per_fault = (fault_ms - plain_ms) * 1000.0 / static_cast<double>(kPages);
    ctx.metric("fault_round_trip = {:.2f} us", us_per_fault);
    ctx.set_summary(std::format("{} faults handled, {:.2f} us each", faults, us_per_fault));
}

void probe_timebase(ProbeContext& ctx)
{
    const uint64_t t0 = ps_timebase_now();
    const uint64_t h0 = ps_host_now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint64_t t1 = ps_timebase_now();
    const uint64_t h1 = ps_host_now_ns();
    const double hz = static_cast<double>(t1 - t0) / (static_cast<double>(h1 - h0) / 1e9);
    ctx.metric("timebase = {:.3f} MHz", hz / 1e6);
    ctx.check(hz > 79.8e6 * 0.98 && hz < 79.8e6 * 1.02, "within 2% of 79.8 MHz");
    ctx.set_summary(std::format("{:.2f} MHz", hz / 1e6));
}

namespace {

struct QosRun {
    const char* name;
#if defined(__APPLE__)
    qos_class_t qos;
#endif
    double ms = 0;
};

u64 integer_workload()
{
    // xorshift chain: dependent integer ops, no memory traffic.
    u64 x = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 50'000'000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
    }
    return x;
}

void* qos_thread(void* arg)
{
    auto* run = static_cast<QosRun*>(arg);
    const auto start = Clock::now();
    volatile u64 sink = integer_workload();
    (void)sink;
    run->ms = ms_since(start);
    return nullptr;
}

} // namespace

void probe_threads_qos(ProbeContext& ctx)
{
#if defined(__APPLE__)
    QosRun runs[] = {
        {"user_interactive", QOS_CLASS_USER_INTERACTIVE},
        {"user_initiated", QOS_CLASS_USER_INITIATED},
        {"utility", QOS_CLASS_UTILITY},
        {"background", QOS_CLASS_BACKGROUND},
    };
    for (QosRun& run : runs) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_set_qos_class_np(&attr, run.qos, 0);
        pthread_t thread;
        if (!ctx.check(pthread_create(&thread, &attr, qos_thread, &run) == 0, "pthread_create")) return;
        pthread_join(thread, nullptr);
        pthread_attr_destroy(&attr);
        ctx.metric("qos.{} = {:.1f} ms", run.name, run.ms);
    }
    ctx.set_summary(std::format("interactive {:.0f} ms, background {:.0f} ms", runs[0].ms, runs[3].ms));
#else
    ctx.skip("QoS classes are Apple-only");
#endif
}

} // namespace pspocket::selftest
