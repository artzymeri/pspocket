// spu-check: the game's translated SPU functions against the interpreter, from random states (CLAUDE.md rule 3 on the
// game's own code, D-132).
//
//   spu-check DUMP [--trials N] [--steps N] [--seed S] [--program PREFIX] [--entries N]
//
// For each SPU image in DUMP (what scripts/lift-spu.sh lifts: spu_<hash>_<base>.bin and .txt) whose lifted program is
// built in, and for each of the program's entries (every block start), runs N trials. A trial fills local store with
// random bytes, puts the image's code at its base, points the link register at a stop planted outside the code and
// the stack pointer into local store, fills the other registers with random values and fixes the timebase; then the
// interpreter runs it (at most --steps instructions) and the lifted program runs from the same state. Registers,
// channels, pc, the stop and local store must agree. Each run has main memory of its own that reads as zeros until
// written (a page is committed when first touched), so a DMA does the same in both. A trial whose stores reached the
// code is not compared (the lifted code would run what was translated, the
// interpreter what is there now), nor one the interpreter did not finish.
//
// Built only with -DPSPOCKET_GAME_SPU, like pspocket-boot: it runs the game's translated code; nothing of the game is
// in it.

#include <signal.h>
#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/runtime.h"

extern "C" void pspocket_game_spu_register(void); // the game's lifted SPU programs (scripts/lift-spu.sh, D-076)

using namespace pspocket;

namespace {

constexpr u32 kStopCode = 0x3F1;    // the stop planted at the link register's target
constexpr u32 kLsSize = PS_SPU_LS_SIZE;

struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    u64 next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    u32 below(u32 n) { return static_cast<u32>(next() % n); }
};

struct Image {
    std::string name;
    std::string kind;
    u32 base = 0;
    std::vector<u8> code;
};

std::vector<Image> load_images(const std::filesystem::path& dir, const std::string& prefix)
{
    std::vector<Image> out;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() != ".txt") continue;
        const std::string name = e.path().stem().string();
        if (name.rfind("spu_", 0) != 0 || name.rfind(prefix, 0) != 0) continue;
        std::ifstream meta(e.path());
        Image im;
        im.name = name;
        std::string key;
        while (meta >> key) {
            std::string value;
            meta >> value;
            if (key == "kind") im.kind = value;
            if (key == "base") im.base = static_cast<u32>(std::stoul(value, nullptr, 16));
        }
        std::ifstream bin(dir / (name + ".bin"), std::ios::binary);
        if (!bin) continue;
        im.code.assign(std::istreambuf_iterator<char>(bin), {});
        if (im.code.empty() || im.base + im.code.size() > kLsSize) continue;
        out.push_back(std::move(im));
    }
    std::sort(out.begin(), out.end(), [](const Image& a, const Image& b) { return a.name < b.name; });
    return out;
}

u64 fixed_timebase() { return 0x123456789ull; }

struct LocalStore {
    u8* p = static_cast<u8*>(std::aligned_alloc(128, kLsSize));
    ~LocalStore() { std::free(p); }
};

std::string hex128(const ps_spu_v128& v)
{
    return std::format("{:08x}{:08x}{:08x}{:08x}", v.u32[3], v.u32[2], v.u32[1], v.u32[0]);
}

std::string differences(const SPUContext& a, const SPUContext& b, const u8* ls_a, const u8* ls_b)
{
    std::string d;
    for (u32 i = 0; i < 128; i++)
        if (std::memcmp(&a.gpr[i], &b.gpr[i], 16) != 0)
            d += std::format(" r{}: {} (interpreter) vs {} (lifted);", i, hex128(a.gpr[i]), hex128(b.gpr[i]));
    if (std::memcmp(&a.fpscr, &b.fpscr, 16) != 0) d += " fpscr;";
    if (a.pc != b.pc) d += std::format(" pc: {:05x} vs {:05x};", a.pc, b.pc);
    if (a.stop_reason != b.stop_reason) d += std::format(" stop_reason: {} vs {};", a.stop_reason, b.stop_reason);
    if (a.stop_code != b.stop_code) d += std::format(" stop_code: {:#x} vs {:#x};", a.stop_code, b.stop_code);
    if (a.exception != b.exception) d += std::format(" exception: {} vs {};", a.exception, b.exception);
    if (a.srr0 != b.srr0) d += " srr0;";
    // The reservation version is the shared table's (the two runs move it in turn): everything else of the channels.
    ps_spu_channels ca = a.ch, cb = b.ch;
    ca.res_version = cb.res_version = 0;
    if (std::memcmp(&ca, &cb, sizeof ca) != 0) d += " channels;";
    for (u32 i = 0; i < kLsSize; i += 16)
        if (std::memcmp(ls_a + i, ls_b + i, 16) != 0) {
            d += std::format(" local store differs first at {:05x};", i);
            break;
        }
    return d;
}

// Main memory of one run: a guest space whose pages commit on first touch (from the fault handler) and are thrown away
// after each trial, so every trial starts from zeros.
struct Sandbox {
    GuestMemory mem;
    static constexpr u32 kMaxPages = 1u << 16;
    std::atomic<u32> touched_count{0};
    u64 touched[kMaxPages] = {};
};
Sandbox* g_boxes[2] = {nullptr, nullptr};
struct sigaction g_previous_bus{}, g_previous_segv{};

void on_fault(int sig, siginfo_t* info, void* uctx)
{
    const auto a = reinterpret_cast<uintptr_t>(info->si_addr);
    const usize page = GuestMemory::host_page_size();
    for (Sandbox* box : g_boxes) {
        if (!box) continue;
        const auto b = reinterpret_cast<uintptr_t>(box->mem.base());
        if (a < b || a - b >= GuestMemory::kGuestSize) continue;
        const uintptr_t p = (a - b) & ~(page - 1);
        if (::mprotect(box->mem.base() + p, page, PROT_READ | PROT_WRITE) != 0) break;
        const u32 n = box->touched_count.fetch_add(1);
        if (n < Sandbox::kMaxPages) box->touched[n] = p;
        return; // the access runs again on a zero page
    }
    const struct sigaction& prev = sig == SIGBUS ? g_previous_bus : g_previous_segv;
    if (prev.sa_flags & SA_SIGINFO && prev.sa_sigaction) {
        prev.sa_sigaction(sig, info, uctx);
        return;
    }
    signal(sig, SIG_DFL);
}

// Throws away what the trial touched: the pages read as zeros again.
void reset(Sandbox& box)
{
    const usize page = GuestMemory::host_page_size();
    const u32 n = std::min(box.touched_count.exchange(0), Sandbox::kMaxPages);
    for (u32 i = 0; i < n; i++)
        ::mmap(box.mem.base() + box.touched[i], page, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: spu-check DUMP [--trials N] [--steps N] [--seed S] [--program PREFIX] [--entries N]\n");
        return 64;
    }
    const std::filesystem::path dump = argv[1];
    u32 trials = 4, max_entries = 0;
    u64 steps = 200'000, seed = 1;
    std::string prefix = "spu_";
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string a = argv[i], v = argv[i + 1];
        if (a == "--trials") trials = static_cast<u32>(std::stoul(v));
        else if (a == "--steps") steps = std::stoull(v);
        else if (a == "--seed") seed = std::stoull(v);
        else if (a == "--program") prefix = v;
        else if (a == "--entries") max_entries = static_cast<u32>(std::stoul(v));
        else {
            std::fprintf(stderr, "spu-check: unknown option %s\n", a.c_str());
            return 64;
        }
    }

    static Sandbox box_i, box_l; // the interpreter's main memory, the lifted run's
    if (!box_i.mem.reserve() || !box_l.mem.reserve()) {
        std::fprintf(stderr, "spu-check: cannot reserve guest memory\n");
        return 1;
    }
    g_boxes[0] = &box_i;
    g_boxes[1] = &box_l;
    struct sigaction sa{};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &g_previous_bus);
    sigaction(SIGSEGV, &sa, &g_previous_segv);
    log::set_min_level(log::Level::Fatal); // random states raise exceptions and misses by the thousand
    pspocket_game_spu_register();
    ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
    ps_spu_set_timebase_source(fixed_timebase);

    const std::vector<Image> images = load_images(dump, prefix);
    LocalStore ls_i, ls_l, ls_0;
    u64 compared = 0, unfinished = 0, overwrote = 0, failures = 0, programs = 0;
    std::map<std::string, bool> seen; // relocatable copies of one program: checked once
    for (const Image& im : images) {
        const u64 hash = ps_spu_hash64(im.code.data(), im.code.size());
        const ps_spu_program* p = ps_spu_find_program(hash, im.base, static_cast<u32>(im.code.size()));
        if (!p || seen[p->name]) continue;
        seen[p->name] = true;
        programs++;
        const u32 at = im.base;
        const u32 reloc = (at - p->code_start) & PS_SPU_LS_MASK;
        // The stop the link register points at: below the code if there is room, else above it.
        const u32 trap = at >= 0x200 ? 0x100 : (at + static_cast<u32>(im.code.size()) + 0x100) & ~15u;
        const u32 n_entries = max_entries ? std::min(max_entries, p->entry_count) : p->entry_count;
        u64 program_failures = 0;
        for (u32 e = 0; e < n_entries; e++) {
            const u32 entry = (p->entries[e].addr + reloc) & PS_SPU_LS_MASK;
            for (u32 t = 0; t < trials; t++) {
                const u64 trial_seed = seed ^ (hash * 31 + u64{e} * 977 + t);
                Rng rng(trial_seed);
                for (u32 i = 0; i < kLsSize; i += 8) {
                    const u64 v = rng.next();
                    std::memcpy(ls_0.p + i, &v, 8);
                }
                std::memcpy(ls_0.p + at, im.code.data(), im.code.size());
                ps_st32(ls_0.p + trap, kStopCode); // stop kStopCode
                std::memcpy(ls_i.p, ls_0.p, kLsSize);
                std::memcpy(ls_l.p, ls_0.p, kLsSize);

                SPUContext ci{}, cl{};
                reset(box_i);
                reset(box_l);
                ps_spu_context_init(&ci, ls_i.p, box_i.mem.base());
                ps_spu_context_init(&cl, ls_l.p, box_l.mem.base());
                for (u32 r = 0; r < 128; r++)
                    for (u32 w = 0; w < 4; w++) ci.gpr[r].u32[w] = static_cast<u32>(rng.next());
                // The link register on the stop, the stack pointer in local store with room below it.
                ci.gpr[0] = ps_spu_v128{};
                ci.gpr[0].u32[3] = trap;
                ci.gpr[1].u32[3] = (0x30000u + rng.below(0xF000)) & ~15u;
                ci.gpr[1].u32[2] = 0x8000;
                std::memcpy(cl.gpr, ci.gpr, sizeof ci.gpr);
                ps_spu_attach_program_at(&cl, p, at);
                ci.xfloat = cl.xfloat;
                ci.pc = cl.pc = entry;

                ps_spu_interpret(&ci, steps);
                if (ci.stop_reason == PS_SPU_STOP_LIMIT) {
                    unfinished++;
                    continue;
                }
                // The lifted run, with a watchdog: what the interpreter finished must finish here too.
                std::atomic<bool> done{false};
                std::thread watchdog([&] {
                    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                    while (!done.load() && std::chrono::steady_clock::now() < until)
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    if (!done.load()) __atomic_fetch_or(&cl.attention, PS_SPU_ATTN_STOP, __ATOMIC_ACQ_REL);
                });
                ps_spu_run(&cl);
                done = true;
                watchdog.join();

                if (std::memcmp(ls_i.p + at, im.code.data(), im.code.size()) != 0 ||
                    std::memcmp(ls_l.p + at, im.code.data(), im.code.size()) != 0) {
                    overwrote++;
                    continue;
                }
                compared++;
                const std::string d = differences(ci, cl, ls_i.p, ls_l.p);
                if (!d.empty()) {
                    failures++;
                    if (++program_failures <= 3) {
                        std::printf("DIFF %s entry %05x (lifted %05x) seed %llu:%s\n", p->name, entry, p->entries[e].addr,
                            static_cast<unsigned long long>(trial_seed), d.c_str());
                    }
                }
            }
        }
        std::printf("%s: %u entries, %llu differences\n", p->name, n_entries, static_cast<unsigned long long>(program_failures));
        std::fflush(stdout);
    }
    std::printf("spu-check: %llu programs, %llu trials compared, %llu differ; %llu not finished in %llu steps, %llu wrote "
                "over their code\n",
        static_cast<unsigned long long>(programs), static_cast<unsigned long long>(compared),
        static_cast<unsigned long long>(failures), static_cast<unsigned long long>(unfinished),
        static_cast<unsigned long long>(steps), static_cast<unsigned long long>(overwrote));
    return failures ? 1 : 0;
}
