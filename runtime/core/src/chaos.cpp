// Shake mode (pspocket/core/chaos.hpp, D-1045).

#include "pspocket/core/chaos.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <pthread.h>

#include "pspocket/core/log.hpp"

extern "C" {
uint32_t ps_chaos_mask = 0;
}

namespace pspocket::chaos {
namespace {

constexpr std::string_view kChan = "shake";

constexpr std::array<PointInfo, kPoints> kInfo{{
    {"S0", "none", Family::Ps3Legal, 0, 0},
    {"S1", "syscall entry", Family::Ps3Legal, 20000, 200},
    {"S2", "library function entry", Family::Ps3Legal, 20000, 200},
    {"S3", "after a wake (the waker)", Family::Ps3Legal, 100000, 500},
    {"S4", "after a wait (the woken)", Family::Ps3Legal, 100000, 500},
    {"S5", "thread start", Family::Ps3Legal, 500000, 2000},
    {"S6", "timer late", Family::Ps3Legal, 100000, 1000},
    {"S7", "timebase rate", Family::HostAdversarial, 1000000, 0},
    {"S8", "conditional store fails", Family::Ps3Legal, 10000, 0},
    {"S9", "SPU channel poll", Family::Ps3Legal, 20000, 100},
    {"S10", "SPURS workload choice", Family::Ps3Legal, 50000, 0},
    {"S11", "RSX between batches", Family::Ps3Legal, 20000, 1000},
    {"S12", "file read", Family::Ps3Legal, 200000, 5000},
    {"S13", "dialog and save data answer", Family::Ps3Legal, 1000000, 200000},
    {"S14", "audio period", Family::Ps3Legal, 20000, 2000},
    {"S15", "host load", Family::HostAdversarial, 1000000, 20000},
    {"S16", "woken thread overtakes its waker", Family::HostAdversarial, 200000, 0},
}};

struct Config {
    u64 seed = 0;
    std::array<u32, kPoints> ppm{}, max_us{};
};
Config g_config;
std::mutex g_config_mutex;
const auto g_start = std::chrono::steady_clock::now();

std::array<std::atomic<u64>, kPoints> g_hits{}, g_delay_us{};

struct LogEntry {
    u64 us;
    u64 detail;
    u32 thread;
    u8 point;
};
constexpr usize kLogSize = 1 << 16;
std::array<LogEntry, kLogSize> g_log{};
std::atomic<u64> g_log_next{0};

u64 splitmix(u64& x)
{
    u64 z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::atomic<u32> g_thread_order{0};

// Each thread's stream (xorshift128+), seeded from the seed and the thread's name, or its order of first use.
struct Stream {
    u64 s0 = 0, s1 = 0;
    u32 id = 0;
    bool ready = false;
    void init()
    {
        char name[64] = {};
        pthread_getname_np(pthread_self(), name, sizeof name);
        u64 h = 1469598103934665603ull;
        for (const char* p = name; *p; p++) h = (h ^ static_cast<u8>(*p)) * 1099511628211ull;
        if (!name[0]) h ^= u64{g_thread_order.fetch_add(1, std::memory_order_relaxed)} * 0x9E3779B97F4A7C15ull;
        u64 x = g_config.seed ^ h;
        s0 = splitmix(x);
        s1 = splitmix(x);
        id = static_cast<u32>(h);
        ready = true;
    }
    u64 next()
    {
        if (!ready) init();
        u64 a = s0;
        const u64 b = s1;
        s0 = b;
        a ^= a << 23;
        s1 = a ^ b ^ (a >> 17) ^ (b >> 26);
        return s1 + b;
    }
};
thread_local Stream t_stream;

void note(Point p, u64 detail)
{
    g_hits[p].fetch_add(1, std::memory_order_relaxed);
    const u64 at = g_log_next.fetch_add(1, std::memory_order_relaxed);
    if (at >= kLogSize) return;
    LogEntry& e = g_log[at];
    e.us = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_start).count());
    e.detail = detail;
    e.thread = t_stream.id;
    e.point = p;
}

// ---- S15: busy host threads

std::atomic<bool> g_load_stop{false};
std::vector<std::thread> g_load_threads;

void start_host_load(u32 max_burst_us)
{
    const u32 n = std::max(1u, std::thread::hardware_concurrency() / 2);
    g_load_stop = false;
    for (u32 i = 0; i < n; i++) {
        g_load_threads.emplace_back([i, max_burst_us] {
#if defined(__APPLE__)
            pthread_setname_np(std::format("shake load {}", i).c_str());
#endif
            while (!g_load_stop.load(std::memory_order_relaxed)) {
                const u64 r = t_stream.next();
                const auto busy = std::chrono::microseconds(r % std::max(1u, max_burst_us));
                const auto idle = std::chrono::microseconds((r >> 32) % std::max(1u, max_burst_us * 3 / 2));
                const auto until = std::chrono::steady_clock::now() + busy;
                volatile u64 spin = 0;
                while (std::chrono::steady_clock::now() < until) spin = spin + 1;
                std::this_thread::sleep_for(idle);
            }
        });
    }
    PS_LOG_INFO(kChan, "S15: {} busy host threads, bursts up to {} us", n, max_burst_us);
}

void stop_host_load()
{
    g_load_stop = true;
    for (std::thread& t : g_load_threads)
        if (t.joinable()) t.join();
    g_load_threads.clear();
}

// ---- S7: the timebase's rate, a while at a time

std::mutex g_tb_mutex;
double g_tb_rate = 1.0;
u64 g_tb_seg_raw = 0, g_tb_seg_tb = 0;
bool g_tb_started = false;
std::atomic<u64> g_tb_last{0};
u64 g_tb_stream = 0x5EED;

} // namespace

const PointInfo& info(Point p) { return kInfo[p < kPoints ? p : 0]; }

const char* family_name(Family f) { return f == Family::Ps3Legal ? "PS3-legal" : "host-adversarial"; }

bool hit(Point p, u64 detail)
{
    // Off is off: disable() clears the mask, not the configuration (D-1048: a test that configured S1 before another
    // that disabled it saw S1 still hit, depending on Catch2's random order).
    if (!(__atomic_load_n(&ps_chaos_mask, __ATOMIC_RELAXED) & (1u << p))) return false;
    const u32 ppm = g_config.ppm[p];
    if (!ppm) return false;
    if (ppm < 1000000 && t_stream.next() % 1000000 >= ppm) return false;
    note(p, detail);
    return true;
}

u32 delay(Point p, u64 detail)
{
    if (!hit(p, detail)) return 0;
    const u32 max = g_config.max_us[p];
    if (!max) return 0;
    const u32 us = static_cast<u32>(t_stream.next() % (u64{max} + 1));
    g_delay_us[p].fetch_add(us, std::memory_order_relaxed);
    if (us) std::this_thread::sleep_for(std::chrono::microseconds(us));
    return us;
}

u64 next() { return t_stream.next(); }

u32 max_us(Point p) { return g_config.max_us[p]; }

bool configure(std::string_view spec, std::string* error)
{
    Config c;
    c.seed = 1;
    u32 mask = 0;
    const auto fail = [&](std::string why) {
        if (error) *error = std::format("PSPOCKET_SHAKE: {}", why);
        return false;
    };
    const auto enable = [&](usize p, u32 ppm, u32 max) {
        mask |= 1u << p;
        c.ppm[p] = ppm;
        c.max_us[p] = max;
    };
    std::string_view rest = spec;
    while (!rest.empty()) {
        const usize end = rest.find_first_of(";,");
        std::string_view tok = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
        while (!tok.empty() && tok.back() == ' ') tok.remove_suffix(1);
        if (tok.empty()) continue;
        if (tok.starts_with("seed=")) {
            c.seed = std::strtoull(std::string(tok.substr(5)).c_str(), nullptr, 0);
            continue;
        }
        if (tok == "legal" || tok == "all" || tok == "adversarial") {
            for (usize p = 1; p < kPoints; p++) {
                const bool legal = kInfo[p].family == Family::Ps3Legal;
                if (tok == "all" || (tok == "legal") == legal) enable(p, kInfo[p].ppm, kInfo[p].max_us);
            }
            continue;
        }
        const bool off = tok.front() == '-';
        if (off) tok.remove_prefix(1);
        if (tok.empty() || tok.front() != 'S') return fail(std::format("'{}' is not seed=, a family or a point", tok));
        const usize eq = tok.find('=');
        const std::string id(tok.substr(0, eq));
        usize p = 0;
        for (usize i = 1; i < kPoints; i++)
            if (id == kInfo[i].id) p = i;
        if (!p) return fail(std::format("no point {}", id));
        if (off) {
            mask &= ~(1u << p);
            c.ppm[p] = 0;
            continue;
        }
        u32 ppm = kInfo[p].ppm, max = kInfo[p].max_us;
        if (eq != std::string_view::npos) {
            const std::string v(tok.substr(eq + 1));
            char* e = nullptr;
            ppm = static_cast<u32>(std::min<unsigned long>(std::strtoul(v.c_str(), &e, 10), 1000000));
            if (e && *e == ':') max = static_cast<u32>(std::strtoul(e + 1, &e, 10));
            if (!e || *e) return fail(std::format("'{}': PPM[:MAXUS]", tok));
        }
        enable(p, ppm, max);
    }
    stop_host_load();
    {
        std::lock_guard lock(g_config_mutex);
        g_config = c;
        t_stream.ready = false; // this thread's stream from the new seed
    }
    __atomic_store_n(&ps_chaos_mask, mask, __ATOMIC_RELEASE);
    if (mask & (1u << S15_HostLoad)) start_host_load(c.max_us[S15_HostLoad]);
    return true;
}

bool configure_from_env()
{
    static const bool on = [] {
        const char* v = std::getenv("PSPOCKET_SHAKE");
        if (!v || !*v) return false;
        std::string error;
        if (!configure(v, &error)) {
            PS_LOG_ERROR(kChan, "{}: shake mode stays off", error);
            return false;
        }
        PS_LOG_WARN(kChan, "shake mode on: {}", description());
        return any();
    }();
    return on;
}

void disable()
{
    __atomic_store_n(&ps_chaos_mask, 0u, __ATOMIC_RELEASE);
    stop_host_load();
}

u64 seed() { return g_config.seed; }

std::string description()
{
    std::string out = std::format("seed {}", g_config.seed);
    for (usize p = 1; p < kPoints; p++) {
        if (!on(static_cast<Point>(p))) continue;
        out += std::format("; {} {} ({}) {:.3g}%", kInfo[p].id, kInfo[p].name,
            kInfo[p].family == Family::Ps3Legal ? "legal" : "adversarial",
            static_cast<double>(g_config.ppm[p]) / 10000.0);
        if (g_config.max_us[p]) out += std::format(" <= {} us", g_config.max_us[p]);
    }
    return out;
}

Counts counts()
{
    Counts c;
    for (usize p = 0; p < kPoints; p++) {
        c.hits[p] = g_hits[p].load(std::memory_order_relaxed);
        c.delay_us[p] = g_delay_us[p].load(std::memory_order_relaxed);
    }
    return c;
}

std::string log_text(usize max_lines)
{
    const usize n = static_cast<usize>(std::min<u64>(g_log_next.load(std::memory_order_acquire), kLogSize));
    std::string out = std::format("# {}\n# us thread point detail\n", description());
    for (usize i = 0; i < n && i < max_lines; i++) {
        const LogEntry& e = g_log[i];
        out += std::format("{} {:08x} {} {:#x}\n", e.us, e.thread, info(static_cast<Point>(e.point)).id, e.detail);
    }
    if (g_log_next.load() > kLogSize) out += std::format("# {} more not kept\n", g_log_next.load() - kLogSize);
    return out;
}

std::string write_log()
{
    const char* path = std::getenv("PSPOCKET_SHAKE_LOG");
    if (!path || !*path || !any()) return {};
    std::ofstream f(path, std::ios::trunc);
    f << log_text();
    return f ? std::string(path) : std::string();
}

u64 random_seed()
{
    if (any()) return t_stream.next();
    std::random_device rd;
    return (u64{rd()} << 32) | rd();
}

u32 random32()
{
    if (any()) return static_cast<u32>(t_stream.next() >> 16);
    thread_local std::random_device rd;
    return rd();
}

double timebase_rate()
{
    std::lock_guard lock(g_tb_mutex);
    return g_tb_rate;
}

// The timebase with S7 (timebase.cpp): continuous and monotonic, its rate 0.99 to 1.01 of the true one, chosen anew
// every quarter second of host time.
u64 shaken_timebase(u64 raw)
{
    u64 tb;
    {
        std::lock_guard lock(g_tb_mutex);
        constexpr u64 kSegment = 79800000ull / 4;
        if (!g_tb_started) {
            g_tb_started = true;
            g_tb_seg_raw = raw;
            g_tb_seg_tb = raw;
            g_tb_stream ^= g_config.seed;
        }
        raw = std::max(raw, g_tb_seg_raw); // read before the lock, by a thread that came late to it
        if (raw - g_tb_seg_raw >= kSegment) {
            g_tb_seg_tb += static_cast<u64>(static_cast<double>(raw - g_tb_seg_raw) * g_tb_rate);
            g_tb_seg_raw = raw;
            g_tb_rate = 0.99 + static_cast<double>(splitmix(g_tb_stream) % 20001) / 1e6;
            note(S7_TimebaseRate, static_cast<u64>(g_tb_rate * 1e6));
        }
        tb = g_tb_seg_tb + static_cast<u64>(static_cast<double>(raw - g_tb_seg_raw) * g_tb_rate);
    }
    u64 last = g_tb_last.load(std::memory_order_relaxed);
    while (tb > last && !g_tb_last.compare_exchange_weak(last, tb, std::memory_order_relaxed)) {
    }
    return std::max(tb, last);
}

} // namespace pspocket::chaos
