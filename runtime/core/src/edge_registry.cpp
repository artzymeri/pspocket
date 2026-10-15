// The EDGE work items between the SPU jobs and the RSX backend (pspocket/core/edge_registry.hpp, D-988).

#include "pspocket/core/edge_registry.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>
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
    else if (std::strcmp(v, "verify") == 0) m = Mode::verify;
    PS_LOG_INFO("edge", "EDGE geometry on the GPU (PSPOCKET_EDGE_GPU={}): {}", v,
        m == Mode::cpu      ? "recorded, its kernels run on the SPU's thread"
        : m == Mode::ab     ? "in every other PSPOCKET_EDGE_AB frames"
        : m == Mode::verify ? "the SPUs' path, the GPU's beside it and compared"
                            : "on");
    return m;
}

std::atomic<bool> g_half{false};
std::atomic<bool> g_consumer{false};
std::atomic<uint32_t> g_waiting{0};

// Publishing (D-1008): each publishing thread (an SPU worker) has a ring of its own, one producer and one consumer (the
// RSX thread, taking): a slot's pointer stored, then the ring's tail released; the consumer moves what the rings hold
// into a map only it touches. No lock, no shared map; a full ring (the RSX more than its size behind) waits for room.
struct Ring {
    static constexpr uint32_t kSize = 8192;
    std::atomic<uint32_t> head{0}, tail{0}; // head: the consumer's next; tail: the producer's next
    std::atomic<WorkItem*> slots[kSize];
};
constexpr uint32_t kRings = 32;
Ring g_rings[kRings];
std::atomic<uint32_t> g_ring_count{0};
std::mutex g_overflow_mutex; // more publishing threads than rings: theirs under this lock
std::vector<std::unique_ptr<WorkItem>> g_overflow;
std::atomic<uint32_t> g_overflow_count{0};

Ring* my_ring()
{
    thread_local Ring* ring = [] {
        const uint32_t n = g_ring_count.fetch_add(1, std::memory_order_relaxed);
        return n < kRings ? &g_rings[n] : nullptr;
    }();
    return ring;
}

// The consumer's own: the items by allocation, moved out of the rings as it takes.
std::unordered_map<uint32_t, std::unique_ptr<WorkItem>>& consumer_items()
{
    static std::unordered_map<uint32_t, std::unique_ptr<WorkItem>> items;
    return items;
}

// The pool of used items: each thread keeps a few of its own, the rest shared in batches (a lock every 32 items).
std::mutex g_pool_mutex;
std::vector<std::unique_ptr<WorkItem>> g_pool;
constexpr size_t kBatch = 32;
thread_local std::vector<std::unique_ptr<WorkItem>> t_pool;
std::atomic<uint64_t> g_published{0}, g_taken{0}, g_replaced{0};
std::atomic<uint64_t> g_routes[static_cast<int>(Route::count)][static_cast<int>(Reason::count)];
std::atomic<uint64_t> g_jobs[2], g_ns[2], g_label_gets[2]; // [0] run whole by the SPU, [1] deferred

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

std::unique_ptr<WorkItem> acquire()
{
    if (t_pool.empty()) {
        std::lock_guard lock(g_pool_mutex);
        const size_t n = std::min(kBatch, g_pool.size());
        for (size_t i = 0; i < n; i++) {
            t_pool.push_back(std::move(g_pool.back()));
            g_pool.pop_back();
        }
    }
    if (t_pool.empty()) return std::make_unique<WorkItem>();
    std::unique_ptr<WorkItem> item = std::move(t_pool.back());
    t_pool.pop_back();
    return item;
}

void recycle(std::unique_ptr<WorkItem> item)
{
    if (!item) return;
    item->blob.clear();
    t_pool.push_back(std::move(item));
    if (t_pool.size() < 2 * kBatch) return;
    std::lock_guard lock(g_pool_mutex);
    while (t_pool.size() > kBatch) {
        if (g_pool.size() < 4096) g_pool.push_back(std::move(t_pool.back()));
        t_pool.pop_back();
    }
}

void publish(std::unique_ptr<WorkItem> item)
{
    g_published.fetch_add(1, std::memory_order_relaxed);
    Ring* ring = my_ring();
    if (!ring) {
        std::lock_guard lock(g_overflow_mutex);
        g_overflow.push_back(std::move(item));
        g_overflow_count.store(static_cast<uint32_t>(g_overflow.size()), std::memory_order_release);
        g_waiting.fetch_add(1, std::memory_order_release);
        return;
    }
    const uint32_t tail = ring->tail.load(std::memory_order_relaxed);
    while (tail - ring->head.load(std::memory_order_acquire) >= Ring::kSize) std::this_thread::yield();
    ring->slots[tail % Ring::kSize].store(item.release(), std::memory_order_relaxed);
    ring->tail.store(tail + 1, std::memory_order_release);
    g_waiting.fetch_add(1, std::memory_order_release);
}

namespace {

// The consumer: what the rings hold into its map (an item published again for the same allocation replaces the
// earlier one, as a job's later output does).
void drain()
{
    auto& items = consumer_items();
    const auto put = [&](std::unique_ptr<WorkItem> item) {
        auto& slot = items[item->alloc_ea];
        if (slot) {
            g_replaced.fetch_add(1, std::memory_order_relaxed);
            g_waiting.fetch_sub(1, std::memory_order_relaxed);
            recycle(std::move(slot));
        }
        slot = std::move(item);
    };
    const uint32_t rings = std::min(g_ring_count.load(std::memory_order_acquire), kRings);
    for (uint32_t r = 0; r < rings; r++) {
        Ring& ring = g_rings[r];
        const uint32_t tail = ring.tail.load(std::memory_order_acquire);
        uint32_t head = ring.head.load(std::memory_order_relaxed);
        for (; head != tail; head++) put(std::unique_ptr<WorkItem>(ring.slots[head % Ring::kSize].load(std::memory_order_relaxed)));
        ring.head.store(head, std::memory_order_release);
    }
    if (g_overflow_count.load(std::memory_order_acquire)) {
        std::vector<std::unique_ptr<WorkItem>> more;
        {
            std::lock_guard lock(g_overflow_mutex);
            more.swap(g_overflow);
            g_overflow_count.store(0, std::memory_order_relaxed);
        }
        for (auto& item : more) put(std::move(item));
    }
}

} // namespace

std::unique_ptr<WorkItem> take(uint32_t ea)
{
    if (!g_waiting.load(std::memory_order_acquire)) return nullptr;
    drain();
    auto& items = consumer_items();
    const auto it = items.find(ea);
    if (it == items.end()) return nullptr;
    std::unique_ptr<WorkItem> item = std::move(it->second);
    items.erase(it);
    g_waiting.fetch_sub(1, std::memory_order_relaxed);
    g_taken.fetch_add(1, std::memory_order_relaxed);
    return item;
}

bool any_waiting() { return g_waiting.load(std::memory_order_acquire) != 0; }

Stats stats()
{
    Stats s{g_published.load(), g_taken.load(), g_replaced.load(), g_jobs[1].load(), g_ns[1].load(), g_jobs[0].load(),
        g_ns[0].load(), {g_label_gets[0].load(), g_label_gets[1].load()}, {}};
    for (int r = 0; r < static_cast<int>(Route::count); r++)
        for (int w = 0; w < static_cast<int>(Reason::count); w++) s.routes[r][w] = g_routes[r][w].load(std::memory_order_relaxed);
    return s;
}

bool verify_this(Route route, Reason why)
{
    static std::atomic<uint64_t> seen[static_cast<int>(Route::count)][static_cast<int>(Reason::count)];
    static const uint64_t every = [] {
        const char* v = std::getenv("PSPOCKET_EDGE_VERIFY_EVERY");
        const uint64_t n = v ? std::strtoull(v, nullptr, 0) : 1;
        if (n > 1) PS_LOG_INFO("edge", "EDGE verify: the first 64 jobs of each route and reason, then every {}th", n);
        return n ? n : 1;
    }();
    const uint64_t n = seen[static_cast<int>(route)][static_cast<int>(why)].fetch_add(1, std::memory_order_relaxed);
    return n < 64 || n % every == 0;
}

Route earliest_route()
{
    static const Route r = [] {
        const char* v = std::getenv("PSPOCKET_EDGE_GPU_ROUTE");
        const Route e = !v ? Route::decompress : std::strcmp(v, "cull") == 0 ? Route::cull : std::strcmp(v, "skin") == 0 ? Route::skin : Route::decompress;
        if (e != Route::decompress) PS_LOG_INFO("edge", "EDGE on the GPU: every job {} on (PSPOCKET_EDGE_GPU_ROUTE={})", route_name(e), v);
        return e;
    }();
    return r;
}

void count_route(Route route, Reason why)
{
    g_routes[static_cast<int>(route)][static_cast<int>(why)].fetch_add(1, std::memory_order_relaxed);
}

const char* reason_name(Reason r)
{
    switch (r) {
    case Reason::none: return "none";
    case Reason::input_format: return "predefined input format";
    case Reason::blend_shapes: return "blend shapes";
    case Reason::skin_flavour: return "skinning flavour";
    case Reason::matrix_format: return "matrix format";
    case Reason::output_format: return "output format";
    case Reason::custom_decompress: return "custom decompression";
    case Reason::context_flag: return "context byte 0";
    case Reason::callback: return "vertex-output callback";
    case Reason::projecting: return "projecting callback";
    case Reason::counts: return "counts";
    case Reason::primary_stream: return "primary stream";
    case Reason::fixed_point_wide: return "fixed point over 24 bits";
    case Reason::secondary_attribute: return "secondary attribute";
    case Reason::stream_description: return "stream description";
    case Reason::output_attribute: return "output attribute";
    case Reason::output_stride: return "output stride";
    case Reason::position_offset: return "position offset";
    case Reason::colour_pass: return "colour pass";
    case Reason::blend_path: return "0x1a4 bit 2 path";
    case Reason::matrix_path: return "0x1a4 bits 3-4 path";
    case Reason::output_mode: return "0x1a7 output mode";
    case Reason::triangle_filter: return "triangle filter";
    case Reason::normal_pass: return "normal pass";
    case Reason::tables: return "tables";
    case Reason::count: break;
    }
    return "?";
}

const char* route_name(Route r)
{
    switch (r) {
    case Route::spu: return "left to the SPU";
    case Route::decompress: return "from the decompression";
    case Route::skin: return "from the skinning";
    case Route::cull: return "from the culling";
    case Route::count: break;
    }
    return "?";
}

std::string describe_routes(const Stats& now, const Stats& before, double frames)
{
    std::string out;
    const auto per = [&](uint64_t v) { return frames > 0 ? static_cast<double>(v) / frames : static_cast<double>(v); };
    for (int r = 1; r <= static_cast<int>(Route::count); r++) {
        const int route = r % static_cast<int>(Route::count); // the GPU's routes first, the SPU's last
        uint64_t total = 0;
        std::string why;
        for (int w = 0; w < static_cast<int>(Reason::count); w++) {
            const uint64_t n = now.routes[route][w] - before.routes[route][w];
            total += n;
            if (n && w != static_cast<int>(Reason::none))
                why += std::format("{}{} {:.1f}", why.empty() ? "" : ", ", reason_name(static_cast<Reason>(w)), per(n));
        }
        out += std::format("{}{} {:.1f}{}", out.empty() ? "" : "; ", route_name(static_cast<Route>(route)), per(total),
            why.empty() ? "" : " (" + why + ")");
    }
    return out;
}

void count_job(bool deferred, uint64_t ns, uint64_t label_gets)
{
    g_label_gets[deferred].fetch_add(label_gets, std::memory_order_relaxed);
    g_jobs[deferred].fetch_add(1, std::memory_order_relaxed);
    g_ns[deferred].fetch_add(ns, std::memory_order_relaxed);
}

} // namespace pspocket::edge
