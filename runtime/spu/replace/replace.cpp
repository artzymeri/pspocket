// Native replacements for known builds of SPU middleware (pspocket/spu/replace.hpp, D-965): the table of builds, the
// programs derived from them, the dispatcher their listed entries go to, the switches and the capture.

#include "pspocket/spu/replace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "native.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/timebase.h"

namespace pspocket::spu::replace {
namespace {

constexpr const char* kChan = "spu";

using Function = KnownFunction;

constexpr Registers regs(std::initializer_list<uint32_t> list)
{
    Registers r{};
    for (const uint32_t n : list) r.bits[n >> 6] |= 1ull << (n & 63);
    return r;
}

struct Build {
    const char* name;
    uint64_t hash; // ps_spu_hash64 of the image as lifted
    uint32_t code_start, code_size;
    std::span<const Function> functions;
};

// Sony's EDGE geometry library (libedgegeom), the build GTA V links into its geometry job (the job binary's strings:
// "EDGE ASSERTION FAILURE", "attempt to use skinning / blend shapes / culling without allocating an extra uniform
// table"), lifted relocatable at 0x4C00. Its six hottest functions take 84% of the job's time on the Mac (run 206,
// docs/improve.md "A steady 30 fps"). 0x07A38, 0x0F008 and 0x0A098 are entered through the get-pc idiom
// (brsl $x, next) from 0x07A10, 0x0EFF8 and 0x0A088; 0x0ECF8 is called through a pointer (the vertex compression
// callback); 0x0AC10, 0x0AC20 and 0x0B778 through a table. What their callers still read after them (spu-live with the
// walk's indirect-call targets, the job's jump tables, its printf to the PPU as variadic and calls never made as ABI
// callbacks): nothing of $2-$79 after 0x07A38, 0x0A098 and 0x0ECF8; $3 (its result) after 0x0F008; after the
// decompression fragments, the state the next fragment of the table at 0x0C1B8 (or 0x0AC10 after 0x0AC20) goes on with.
constexpr Registers kFragmentState = regs({4, 35, 37, 39, 41, 42, 43, 44, 45, 46, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
    61, 65, 66, 67, 72, 76});
constexpr Function kEdgeDad4[] = {
    {0x07A38, nullptr, {}},
    // its skinning loop (78% of its instructions): head and exit; after the exit the job reads these
    {0x080F8, ps_edge_dad4_080f8,
        regs({1, 36, 39, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 81, 82, 84, 85, 86, 87, 88, 89, 90,
            91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114,
            115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 127}),
        nullptr, 0x082E8},
    {0x082E8, nullptr, {}},
    {0x0A098, nullptr, {}},
    {0x0AC10, nullptr, kFragmentState},
    {0x0AC20, nullptr, kFragmentState},
    {0x0B778, nullptr, kFragmentState},
    {0x0ECF8, ps_edge_dad4_0ecf8, {}},
    {0x0F008, ps_edge_dad4_0f008, regs({3})},
};

// Sony's MP3 decoder module, dev_flash/sys/external/flashMP3.pic from offset 0x100 (the build every MultiStream game
// loads into its SPU decode jobs: GTA V's mixer at LS 0x17400), lifted relocatable at 0x17400. Its entry, decodeFrame,
// is reached by the job's tail call through a register; its caller reads $3 (the PCM's address) after it (D-974).
//
// decodeFrame's arguments: $3 the frame, $4/$5 where it puts the bytes consumed and the PCM bytes a channel, $6 its
// 22528-byte heap, $7/$8 the channels' hybrid overlap (576 floats), $9/$10 their filterbank history (544 floats), $11
// the 512-byte reservoir, $12 the filterbank position. Where the native decoder may differ (flashmp3.c):
// - equivalent: the PCM (16-bit, the heap + 2304 and + 6400: one step at most where a sum falls on the other side of
//   an integer), the four float state buffers and the float scratch of the heap past the PCM (the granule's spectra,
//   the hybrid filter's output and its last new block): within 1e-4 of each region's largest magnitude;
// - dead: the padding of the module's variables (each has a quadword of its own; the bytes past its size, by the
//   module's symbol table, belong to no variable, and the compiler's whole-quadword stores fill them with whatever its
//   registers held); its rval (0x100A0), the last value a bit read assembled, which it only ever stores (101 stores,
//   no load, its address never taken: none of its 9,437 instructions reads it); and the stack below the caller's
//   stack pointer (the module's frames, free once it returns: the SPU ABI). The module's variables are reloaded with
//   its image before calls anyway (the job DMAs the module in: the captured calls find the image's bytes there).
void flashmp3_regions(const SPUContext& entry, const uint8_t*, std::vector<Region>& out)
{
    const auto pref = [&](int r) { return entry.gpr[r].u32[3] & PS_SPU_LS_MASK; };
    const uint32_t mod = (0x17400u + entry.reloc) & PS_SPU_LS_MASK, heap = pref(6), sp = pref(1);
    constexpr double kFloat = 1e-4, kPcm = 1;
    out.push_back({Region::Kind::s16, heap + 2304, 4096, kPcm});
    out.push_back({Region::Kind::s16, heap + 2304 + 4096, 4096, kPcm});
    out.push_back({Region::Kind::f32, heap + 10624, 22144 - 10624, kFloat});
    out.push_back({Region::Kind::f32, pref(7), 2304, kFloat});
    out.push_back({Region::Kind::f32, pref(8), 2304, kFloat});
    out.push_back({Region::Kind::f32, pref(9), 2176, kFloat});
    out.push_back({Region::Kind::f32, pref(10), 2176, kFloat});
    // The variables (image vaddr, size) of the module's .data and .bss, from 0x10000 to 0x103E0.
    static constexpr std::pair<uint32_t, uint32_t> kVars[] = {{0x10000, 4}, {0x10010, 4}, {0x10020, 4}, {0x10030, 8},
        {0x10040, 4}, {0x10050, 0x48}, {0x100A0, 4}, {0x100B0, 0x100}, {0x101B0, 0x48}, {0x10200, 4}, {0x10210, 1}, {0x10220, 0x10},
        {0x10230, 0x48}, {0x10280, 4}, {0x10290, 4}, {0x102A0, 8}, {0x102B0, 0x10}, {0x102C0, 4}, {0x102D0, 4},
        {0x102E0, 4}, {0x102F0, 4}, {0x10300, 0x10}, {0x10310, 4}, {0x10320, 4}, {0x10330, 4}, {0x10340, 0x44},
        {0x10390, 4}, {0x103A0, 4}, {0x103B0, 8}, {0x103C0, 0x10}, {0x103D0, 4}};
    for (const auto& [vaddr, size] : kVars) {
        const uint32_t end = vaddr + size, quad_end = (end + 15) & ~15u;
        if (end < quad_end) out.push_back({Region::Kind::dead, (mod + end - 0x80) & PS_SPU_LS_MASK, quad_end - end, 0});
    }
    out.push_back({Region::Kind::dead, (mod + 0x100A0 - 0x80) & PS_SPU_LS_MASK, 4, 0}); // rval
    constexpr uint32_t kStack = 8192;
    if (sp >= kStack) out.push_back({Region::Kind::dead, sp - kStack, kStack, 0});
}

constexpr Function kFlashMp3[] = {
    {0x1E820, ps_flashmp3_884a_1e820, regs({3}), flashmp3_regions},
};

// zlib 1.2.3's inflate as RAGE links it into GTA V's "inflateServer" SPURS task (spu_3ffe1d50f8a58e15: the strings
// "1.2.3" and zlib's messages; its main loop DMAs a request's input in, calls inflate and DMAs the output out),
// lifted at 0x3000. inflate(strm, flush) at 0x05190 returns its result in $3 (D-975).
// Not reproduced (dead): the stack below the caller's stack pointer (the frames of inflate, inflate_fast and the
// task's copy routine, free once it returns: the SPU ABI), and the copy routine's scratch quadword at 0x9720 (the
// routine 0x03110 that zmemcpy calls writes it before it reads it: 400 captured calls replayed with it scrambled at
// entry leave everything else as they did).
void inflate3ffe_regions(const SPUContext& entry, const uint8_t*, std::vector<Region>& out)
{
    const uint32_t sp = entry.gpr[1].u32[3] & PS_SPU_LS_MASK;
    out.push_back({Region::Kind::dead, 0x9720, 16, 0});
    constexpr uint32_t kStack = 8192;
    if (sp >= kStack) out.push_back({Region::Kind::dead, sp - kStack, kStack, 0});
}

constexpr Function kInflate3ffe[] = {
    {0x05190, ps_zlib_3ffe_05190, regs({3}), inflate3ffe_regions},
};

constexpr Build kBuilds[] = {
    {"EDGE geometry (spu_dad457863220b1cc)", 0xDAD457863220B1CCull, 0x04C00, 0xE900, kEdgeDad4},
    {"Sony's MP3 decoder module (flashMP3.pic)", 0x884AE88F68A9E778ull, 0x17400, 0x10380, kFlashMp3},
    {"zlib 1.2.3 inflate (spu_3ffe1d50f8a58e15)", 0x3FFE1D50F8A58E15ull, 0x03000, 0x6690, kInflate3ffe},
};

// Builds added at run time (add_build): never removed, so a Build found stays valid.
struct AddedBuild {
    std::string name;
    std::vector<Function> functions;
    Build build;
};
std::mutex g_builds_mutex;
std::vector<std::unique_ptr<AddedBuild>> g_added;

template <typename F>
const Build* each_build(F&& match)
{
    for (const Build& b : kBuilds)
        if (match(b)) return &b;
    std::lock_guard lock(g_builds_mutex);
    for (const auto& a : g_added)
        if (match(a->build)) return &a->build;
    return nullptr;
}

const Build* find_build(uint64_t hash, uint32_t code_start, uint32_t code_size)
{
    return each_build([&](const Build& b) { return b.hash == hash && b.code_start == code_start && b.code_size == code_size; });
}

// ---- switches ----

struct Config {
    Mode mode = Mode::on;
    uint64_t ab_period_ns = 0;
    std::vector<uint32_t> only; // empty: every function
    uint64_t capture_hash = 0;
    std::vector<uint32_t> capture_addrs;
    std::string capture_dir = "spu-capture";
    uint64_t capture_max = 500, capture_every = 1;
    bool scramble = false;   // PSPOCKET_SPU_NATIVE_SCRAMBLE: the unspecified registers garbage after each call
    bool timed = false;      // PSPOCKET_SPU_NATIVE_TIME: every call timed
    uint64_t trace_hash = 0; // PSPOCKET_SPU_TRACE_ENTRIES: every entry of this program traced
    std::string trace_file;
};

std::mutex g_config_mutex;
bool g_configured = false;
Config g_config;

std::vector<uint32_t> parse_addrs(const char* s)
{
    std::vector<uint32_t> out;
    while (s && *s) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(s, &end, 16);
        if (end == s) break;
        out.push_back(static_cast<uint32_t>(v) & PS_SPU_PC_MASK);
        s = *end == ',' ? end + 1 : end;
    }
    return out;
}

const Config& config()
{
    std::lock_guard lock(g_config_mutex);
    if (g_configured) return g_config;
    g_configured = true;
    Config& c = g_config;
    if (const char* v = std::getenv("PSPOCKET_SPU_NATIVE")) {
        if (std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0) c.mode = Mode::off;
        else if (std::strcmp(v, "ab") == 0) c.mode = Mode::ab;
    }
    if (c.mode == Mode::ab) {
        const char* ab = std::getenv("PSPOCKET_SPU_AB");
        c.ab_period_ns = ab && std::atoi(ab) > 0 ? static_cast<uint64_t>(std::atoi(ab)) * 1'000'000'000ull : 0;
        if (!c.ab_period_ns) {
            PS_LOG_WARN(kChan, "PSPOCKET_SPU_NATIVE=ab without PSPOCKET_SPU_AB (seconds): native code always on");
            c.mode = Mode::on;
        }
    }
    c.only = parse_addrs(std::getenv("PSPOCKET_SPU_NATIVE_ONLY"));
    if (const char* v = std::getenv("PSPOCKET_SPU_NATIVE_SCRAMBLE"); v && std::strcmp(v, "0") != 0) {
        c.scramble = true;
        PS_LOG_INFO(kChan, "native SPU replacements: the registers outside their contract garbage after each call");
    }
    if (const char* v = std::getenv("PSPOCKET_SPU_NATIVE_TIME"); v && std::strcmp(v, "0") != 0) c.timed = true;
    if (const char* v = std::getenv("PSPOCKET_SPU_CAPTURE")) {
        char* end = nullptr;
        c.capture_hash = std::strtoull(v, &end, 16);
        if (end && *end == ':') c.capture_addrs = parse_addrs(end + 1);
        if (const char* d = std::getenv("PSPOCKET_SPU_CAPTURE_DIR")) c.capture_dir = d;
        if (const char* m = std::getenv("PSPOCKET_SPU_CAPTURE_MAX")) c.capture_max = std::strtoull(m, nullptr, 10);
        if (const char* e = std::getenv("PSPOCKET_SPU_CAPTURE_EVERY"))
            c.capture_every = std::max<uint64_t>(1, std::strtoull(e, nullptr, 10));
        std::error_code ec;
        std::filesystem::create_directories(c.capture_dir, ec);
        PS_LOG_INFO(kChan, "capturing {} function(s) of program {:016x} into {} (at most {} each, every {})",
            c.capture_addrs.size(), c.capture_hash, c.capture_dir, c.capture_max, c.capture_every);
    }
    if (const char* v = std::getenv("PSPOCKET_SPU_TRACE_ENTRIES")) {
        c.trace_hash = std::strtoull(v, nullptr, 16);
        const char* f = std::getenv("PSPOCKET_SPU_TRACE_FILE");
        c.trace_file = f ? f : "spu-entries.txt";
        PS_LOG_INFO(kChan, "tracing the entries of program {:016x} into {}", c.trace_hash, c.trace_file);
    }
    if (c.mode != Mode::on)
        PS_LOG_INFO(kChan, "native SPU replacements {}", c.mode == Mode::off ? "off" : "in half A only (PSPOCKET_SPU_AB)");
    return c;
}

// ---- derived programs ----

struct DerivedFn {
    uint32_t addr = 0;
    uint32_t exit = 0; // a loop's exit (0: a function, returning to its link)
    Impl impl = nullptr;
    Registers exact{};
    ps_spu_fn lifted = nullptr;
    bool enabled = true;
    bool capture = false;
    std::atomic<uint64_t> native{0}, declined{0}, lifted_calls{0}, calls{0}, captured{0}, native_ns{0}, lifted_ns{0};
};

struct Derived {
    ps_spu_program program{};
    const ps_spu_program* original = nullptr;
    const Build* build = nullptr;
    bool traced = false;
    std::vector<ps_spu_entry> entries;
    std::vector<std::unique_ptr<DerivedFn>> fns;

    DerivedFn* fn(uint32_t addr) const
    {
        for (const auto& f : fns)
            if (f->addr == addr) return f.get();
        return nullptr;
    }
};

std::mutex g_mutex;
std::vector<std::unique_ptr<Derived>> g_derived; // never freed: contexts may run them
// Read without the lock by the dispatcher: published one by one, never removed.
constexpr uint32_t kMaxDerived = 64;
std::array<std::atomic<Derived*>, kMaxDerived> g_published{};
std::atomic<uint32_t> g_published_count{0};

Derived* published(const ps_spu_program* program)
{
    const uint32_t n = g_published_count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; i++) {
        Derived* d = g_published[i].load(std::memory_order_acquire);
        if (d && &d->program == program) return d;
    }
    return nullptr;
}

bool native_now(const Config& c)
{
    switch (c.mode) {
    case Mode::off: return false;
    case Mode::on: return true;
    case Mode::ab: return ((ps_host_now_ns() / c.ab_period_ns) & 1) == 0; // half A (D-950's halves)
    }
    return false;
}

void capture(SPUContext* ctx, uint8_t* ls, const Derived& d, DerivedFn& f, uint64_t seq);

// PSPOCKET_SPU_TRACE_ENTRIES: the (entry, link register) pairs a traced program's entries were entered with, in the
// program's own addresses: for a call through a register (bisl), the link is the call's return address, which names
// the call. The input of spu-live's --call (D-965). A table filled without locks.
constexpr uint32_t kTraceSlots = 1u << 16;
std::array<std::atomic<uint64_t>, kTraceSlots> g_trace{};
std::atomic<uint64_t> g_trace_dropped{0};

void trace(const SPUContext* ctx, uint32_t addr)
{
    const uint32_t link = (ctx->gpr[0].u32[3] - ctx->reloc) & PS_SPU_LS_MASK;
    const uint64_t key = (uint64_t{addr} << 32) | link | (1ull << 63);
    uint32_t h = static_cast<uint32_t>((key * 0x9E3779B97F4A7C15ull) >> 48);
    for (uint32_t probe = 0; probe < 64; probe++, h = (h + 1) & (kTraceSlots - 1)) {
        uint64_t v = g_trace[h].load(std::memory_order_relaxed);
        if (v == key) return;
        if (v == 0 && g_trace[h].compare_exchange_strong(v, key, std::memory_order_relaxed)) return;
        if (v == key) return;
    }
    g_trace_dropped.fetch_add(1, std::memory_order_relaxed);
}

// Every entry of a traced program that is not listed comes here.
void trace_dispatch(SPUContext* ctx, uint8_t* ls)
{
    const Derived* d = published(ctx->program);
    if (!d) PS_FATAL(kChan, "SPU entry trace: the attached program is not a derived one");
    const uint32_t addr = (ctx->pc - ctx->reloc) & PS_SPU_PC_MASK;
    trace(ctx, addr);
    d->original->table[(addr - d->original->code_start) >> 2](ctx, ls);
}

// The registers outside a function's contract, garbage (PSPOCKET_SPU_NATIVE_SCRAMBLE): a different pattern each call,
// so a reader of one sees the difference.
void scramble(SPUContext* ctx, const Registers& exact)
{
    static std::atomic<uint32_t> round{0};
    const uint32_t k = round.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B9u;
    for (uint32_t r = 0; r < 128; r++)
        if (!exact.test(r))
            for (uint32_t w = 0; w < 4; w++) ctx->gpr[r].u32[w] = 0xDEAD0000u ^ k ^ (r << 4) ^ w;
}

// Every listed entry of a derived program comes here (a plain ps_spu_fn has no closure: the function is found by the
// program and the pc, among a handful).
void dispatch(SPUContext* ctx, uint8_t* ls)
{
    const Derived* d = published(ctx->program);
    const uint32_t addr = (ctx->pc - ctx->reloc) & PS_SPU_PC_MASK;
    DerivedFn* f = d ? d->fn(addr) : nullptr;
    if (!f) PS_FATAL(kChan, "native SPU dispatch: no listed function at 0x{:05x} of the attached program", addr);
    if (d->traced) trace(ctx, addr);
    if (f->capture) {
        const uint64_t seq = f->calls.fetch_add(1, std::memory_order_relaxed);
        const Config& c = config();
        if (seq % c.capture_every == 0 && f->captured.load(std::memory_order_relaxed) < c.capture_max) {
            capture(ctx, ls, *d, *f, seq);
            return;
        }
        f->lifted_calls.fetch_add(1, std::memory_order_relaxed);
        f->lifted(ctx, ls);
        return;
    }
    const Config& c = config();
    const uint32_t ret = f->exit ? (f->exit + ctx->reloc) & PS_SPU_PC_MASK : ctx->gpr[0].u32[3] & PS_SPU_PC_MASK;
    bool native = false;
    const uint64_t t0 = c.timed ? ps_host_now_ns() : 0;
    if (f->impl && f->enabled && native_now(c)) {
        native = f->impl(ctx, ls) != 0;
        (native ? f->native : f->declined).fetch_add(1, std::memory_order_relaxed);
    } else {
        f->lifted_calls.fetch_add(1, std::memory_order_relaxed);
    }
    if (!native) f->lifted(ctx, ls);
    if (c.timed) (native ? f->native_ns : f->lifted_ns).fetch_add(ps_host_now_ns() - t0, std::memory_order_relaxed);
    // The check of the contract (D-965): what callers may not read is garbage. After a native run, or a lifted one that
    // returned (not one that left for the run loop in the middle, to stop: its registers are all still needed).
    if (c.scramble && (native || (ctx->stop_reason == PS_SPU_STOP_NONE && ctx->pc == ret))) scramble(ctx, f->exact);
}

// ---- capture ----

constexpr uint32_t kLines = PS_SPU_LS_SIZE / 16;

void capture(SPUContext* ctx, uint8_t* ls, const Derived& d, DerivedFn& f, uint64_t seq)
{
    thread_local std::vector<uint8_t> before;
    before.assign(ls, ls + PS_SPU_LS_SIZE);
    // The decrementer stands still through a captured call (as if the call took no time), so a function that reads it
    // gives the same values when replayed (D-974: the MP3 module times itself); it runs on afterwards as before.
    const ps_spu_channels ch = ctx->ch;
    uint32_t dec = 0;
    if (!ch.dec_frozen) {
        ps_spu_rdch(ctx, ctx->pc, PS_SPU_RdDec, &dec);
        ctx->ch.dec_value = dec;
        ctx->ch.dec_frozen = 1;
    }
    CaptureState in{}, out{};
    save_state(*ctx, in);
    f.lifted(ctx, ls);
    save_state(*ctx, out);
    if (!ch.dec_frozen && ctx->ch.dec_frozen && ctx->ch.dec_value == dec) {
        ctx->ch.dec_frozen = 0;
        ctx->ch.dec_value = ch.dec_value;
        ctx->ch.dec_start = ch.dec_start;
    }
    const Config& c = config();
    if (f.captured.fetch_add(1, std::memory_order_relaxed) >= c.capture_max) return;
    CaptureHeader h{};
    std::memcpy(h.magic, "PSNCAP01", 8);
    h.hash = d.original->hash;
    h.code_start = d.original->code_start;
    h.code_size = d.original->code_size;
    h.addr = f.addr;
    h.reloc = ctx->reloc;
    h.seq = seq;
    const std::string path = std::format("{}/{:016x}_{:05x}_{:07}.cap", c.capture_dir, h.hash, h.addr, seq);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        PS_LOG_WARN(kChan, "capture: cannot write {}", path);
        return;
    }
    std::fwrite(&h, sizeof h, 1, file);
    std::fwrite(&in, sizeof in, 1, file);
    std::fwrite(before.data(), 1, PS_SPU_LS_SIZE, file);
    std::fwrite(&out, sizeof out, 1, file);
    uint32_t changed = 0;
    for (uint32_t i = 0; i < kLines; i++) changed += std::memcmp(before.data() + i * 16, ls + i * 16, 16) != 0;
    std::fwrite(&changed, 4, 1, file);
    for (uint32_t i = 0; i < kLines; i++) {
        if (std::memcmp(before.data() + i * 16, ls + i * 16, 16) == 0) continue;
        const uint32_t at = i * 16;
        std::fwrite(&at, 4, 1, file);
        std::fwrite(ls + at, 1, 16, file);
    }
    std::fclose(file);
}

} // namespace

bool listed(uint64_t hash, uint32_t code_start, uint32_t addr)
{
    return each_build([&](const Build& b) {
        if (b.hash != hash || b.code_start != code_start) return false;
        for (const Function& f : b.functions)
            if (f.addr == addr) return true;
        return false;
    }) != nullptr;
}

std::vector<uint32_t> listed_addresses(uint64_t hash, uint32_t code_start)
{
    std::vector<uint32_t> out;
    each_build([&](const Build& b) {
        if (b.hash == hash && b.code_start == code_start)
            for (const Function& f : b.functions) out.push_back(f.addr);
        return false;
    });
    return out;
}

void add_build(const char* name, uint64_t hash, uint32_t code_start, uint32_t code_size, std::vector<KnownFunction> functions)
{
    auto a = std::make_unique<AddedBuild>();
    a->name = name;
    a->functions = std::move(functions);
    a->build = Build{a->name.c_str(), hash, code_start, code_size, a->functions};
    std::lock_guard lock(g_builds_mutex);
    g_added.push_back(std::move(a));
}

Impl native(uint64_t hash, uint32_t code_start, uint32_t code_size, uint32_t addr)
{
    if (const Build* b = find_build(hash, code_start, code_size))
        for (const Function& f : b->functions)
            if (f.addr == addr) return f.impl;
    return nullptr;
}

Registers exact_registers(uint64_t hash, uint32_t code_start, uint32_t code_size, uint32_t addr)
{
    Registers r = abi_registers();
    if (const Build* b = find_build(hash, code_start, code_size))
        for (const Function& f : b->functions)
            if (f.addr == addr) {
                if (f.exit) r = Registers{};
                r.bits[0] |= f.live.bits[0];
                r.bits[1] |= f.live.bits[1];
            }
    return r;
}

std::vector<Region> regions(uint64_t hash, uint32_t code_start, uint32_t code_size, uint32_t addr, const SPUContext& entry,
    const uint8_t* ls_entry)
{
    std::vector<Region> out;
    if (const Build* b = find_build(hash, code_start, code_size))
        for (const Function& f : b->functions)
            if (f.addr == addr && f.regions) f.regions(entry, ls_entry, out);
    return out;
}

void set_mode(Mode mode)
{
    std::lock_guard lock(g_config_mutex);
    g_configured = true;
    g_config.mode = mode;
}

Mode mode() { return config().mode; }

ps_spu_program* derive(const ps_spu_program* program)
{
    if (!program || original_of(program)) return nullptr;
    const Build* build = find_build(program->hash, program->code_start, program->code_size);
    if (!build) return nullptr;
    const Config& c = config();
    const bool capturing = c.capture_hash == program->hash && !c.capture_addrs.empty();
    const bool tracing = c.trace_hash == program->hash;
    if (c.mode == Mode::off && !capturing && !tracing) return nullptr;

    std::lock_guard lock(g_mutex);
    for (const auto& d : g_derived)
        if (d->original == program) return &d->program;
    if (g_derived.size() >= kMaxDerived) {
        PS_LOG_WARN(kChan, "native SPU replacements: more than {} derived programs; {} runs lifted", kMaxDerived,
            program->name);
        return nullptr;
    }
    auto d = std::make_unique<Derived>();
    d->original = program;
    d->build = build;
    d->entries.assign(program->entries, program->entries + program->entry_count);
    for (const Function& lf : build->functions) {
        const auto it = std::find_if(d->entries.begin(), d->entries.end(), [&](const ps_spu_entry& e) { return e.addr == lf.addr; });
        if (it == d->entries.end()) {
            PS_LOG_WARN(kChan, "native SPU replacements: {} has no entry at 0x{:05x}", program->name, lf.addr);
            continue;
        }
        auto f = std::make_unique<DerivedFn>();
        f->addr = lf.addr;
        f->impl = lf.impl;
        f->exact = lf.exit ? Registers{} : abi_registers();
        f->exit = lf.exit;
        f->exact.bits[0] |= lf.live.bits[0];
        f->exact.bits[1] |= lf.live.bits[1];
        f->lifted = it->fn;
        f->enabled = c.only.empty() || std::find(c.only.begin(), c.only.end(), lf.addr) != c.only.end();
        f->capture = capturing && std::find(c.capture_addrs.begin(), c.capture_addrs.end(), lf.addr) != c.capture_addrs.end();
        it->fn = dispatch;
        d->fns.push_back(std::move(f));
    }
    if (tracing) {
        d->traced = true;
        for (ps_spu_entry& e : d->entries)
            if (e.fn != dispatch) e.fn = trace_dispatch;
    }
    d->program = *program;
    d->program.entries = d->entries.data();
    d->program.entry_count = static_cast<uint32_t>(d->entries.size());
    d->program.table = nullptr; // ps_spu_register_program builds the derived program's own
    Derived* raw = d.get();
    g_derived.push_back(std::move(d));
    g_published[g_published_count.load(std::memory_order_relaxed)].store(raw, std::memory_order_release);
    g_published_count.fetch_add(1, std::memory_order_acq_rel);
    uint32_t natives = 0;
    for (const auto& f : raw->fns) natives += f->impl != nullptr && f->enabled;
    PS_LOG_INFO(kChan, "{}: {} as {} ({} of {} listed functions native{})", program->name, build->name,
        "a derived program", natives, raw->fns.size(), capturing ? ", capturing" : "");
    return &raw->program;
}

const ps_spu_program* original_of(const ps_spu_program* program)
{
    const Derived* d = published(program);
    return d ? d->original : nullptr;
}

ps_spu_program* derived_of(const ps_spu_program* program)
{
    std::lock_guard lock(g_mutex);
    for (const auto& d : g_derived)
        if (d->original == program) return &d->program;
    return nullptr;
}

std::vector<FunctionStats> stats()
{
    std::vector<FunctionStats> out;
    std::lock_guard lock(g_mutex);
    for (const auto& d : g_derived)
        for (const auto& f : d->fns)
            out.push_back({d->build->name, f->addr, f->native.load(), f->declined.load(), f->lifted_calls.load(),
                f->captured.load(), f->native_ns.load(), f->lifted_ns.load()});
    return out;
}

void write_trace()
{
    const Config& c = config();
    if (!c.trace_hash) return;
    std::FILE* f = std::fopen(c.trace_file.c_str(), "w");
    if (!f) return;
    std::vector<uint64_t> keys;
    for (const auto& slot : g_trace)
        if (const uint64_t v = slot.load(std::memory_order_relaxed)) keys.push_back(v & ~(1ull << 63));
    std::sort(keys.begin(), keys.end());
    std::fprintf(f, "# entry link (program %016llx; %llu pairs dropped)\n", static_cast<unsigned long long>(c.trace_hash),
        static_cast<unsigned long long>(g_trace_dropped.load()));
    for (const uint64_t k : keys) std::fprintf(f, "%05x %05x\n", static_cast<unsigned>(k >> 32), static_cast<unsigned>(k & 0x3FFFF));
    std::fclose(f);
}

void save_state(const SPUContext& ctx, CaptureState& out)
{
    std::memcpy(out.gpr, ctx.gpr, sizeof out.gpr);
    out.fpscr = ctx.fpscr;
    out.pc = ctx.pc;
    out.srr0 = ctx.srr0;
    out.stop_reason = ctx.stop_reason;
    out.stop_code = ctx.stop_code;
    out.exception = ctx.exception;
    out.interrupts_enabled = ctx.interrupts_enabled;
    out.xfloat = ctx.xfloat;
    out.ch = ctx.ch;
}

void load_state(const CaptureState& in, SPUContext& ctx)
{
    std::memcpy(ctx.gpr, in.gpr, sizeof in.gpr);
    ctx.fpscr = in.fpscr;
    ctx.pc = in.pc;
    ctx.srr0 = in.srr0;
    ctx.stop_reason = in.stop_reason;
    ctx.stop_code = in.stop_code;
    ctx.exception = in.exception;
    ctx.interrupts_enabled = in.interrupts_enabled;
    ctx.xfloat = in.xfloat;
    ctx.ch = in.ch;
}

} // namespace pspocket::spu::replace
