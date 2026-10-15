// The EDGE work items between the SPU jobs and the RSX backend (pspocket/core/edge_registry.hpp, D-988).

#include "pspocket/core/edge_registry.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "pspocket/core/log.hpp"

namespace pspocket::edge {
namespace {

Mode read_mode()
{
    const char* v = std::getenv("PSPOCKET_EDGE_GPU");
    if (!v || !*v || std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0) return Mode::off;
    Mode m = Mode::gpu;
    if (std::strcmp(v, "cpu") == 0) m = Mode::cpu;
    else if (std::strcmp(v, "ab") == 0) m = Mode::ab;
    PS_LOG_INFO("edge", "EDGE geometry on the GPU (PSPOCKET_EDGE_GPU={}): {}", v,
        m == Mode::cpu ? "recorded, its kernels run on the SPU's thread" : m == Mode::ab ? "in the on half of PSPOCKET_GPU_AB" : "on");
    return m;
}

std::atomic<bool> g_half{false};
std::atomic<bool> g_consumer{false};
std::mutex g_mutex;
std::unordered_map<uint32_t, std::unique_ptr<WorkItem>> g_items;
std::atomic<uint32_t> g_waiting{0};
std::atomic<uint64_t> g_published{0}, g_taken{0}, g_replaced{0}, g_jobs_gpu{0}, g_jobs_cpu{0}, g_rejected{0};

} // namespace

Mode mode()
{
    static const Mode m = read_mode();
    return m;
}

bool deferred_now()
{
    switch (mode()) {
    case Mode::off: return false;
    case Mode::cpu: return true;
    case Mode::ab: return g_half.load(std::memory_order_relaxed) && g_consumer.load(std::memory_order_acquire);
    default: return g_consumer.load(std::memory_order_acquire);
    }
}

void set_ab_half(bool on) { g_half.store(on, std::memory_order_relaxed); }
void set_consumer(bool ready) { g_consumer.store(ready, std::memory_order_release); }
bool ab_half() { return g_half.load(std::memory_order_relaxed); }

void publish(std::unique_ptr<WorkItem> item)
{
    const uint32_t ea = item->alloc_ea;
    std::lock_guard lock(g_mutex);
    auto& slot = g_items[ea];
    if (slot) g_replaced.fetch_add(1, std::memory_order_relaxed);
    else g_waiting.fetch_add(1, std::memory_order_relaxed);
    slot = std::move(item);
    g_published.fetch_add(1, std::memory_order_relaxed);
}

std::unique_ptr<WorkItem> take(uint32_t ea)
{
    if (!g_waiting.load(std::memory_order_acquire)) return nullptr;
    std::lock_guard lock(g_mutex);
    const auto it = g_items.find(ea);
    if (it == g_items.end()) return nullptr;
    std::unique_ptr<WorkItem> item = std::move(it->second);
    g_items.erase(it);
    g_waiting.fetch_sub(1, std::memory_order_relaxed);
    g_taken.fetch_add(1, std::memory_order_relaxed);
    return item;
}

bool any_waiting() { return g_waiting.load(std::memory_order_acquire) != 0; }

Stats stats()
{
    return {g_published.load(), g_taken.load(), g_replaced.load(), g_jobs_gpu.load(), g_jobs_cpu.load(), g_rejected.load()};
}

void count_job(bool deferred) { (deferred ? g_jobs_gpu : g_jobs_cpu).fetch_add(1, std::memory_order_relaxed); }
void count_rejected() { g_rejected.fetch_add(1, std::memory_order_relaxed); }

} // namespace pspocket::edge
