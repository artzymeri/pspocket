// spu-native-check: native replacements of SPU middleware functions against the lifted functions, on states captured
// from a game (D-965; pspocket/spu/replace.hpp, PSPOCKET_SPU_CAPTURE).
//
//   spu-native-check CAPTURE... [--library FILE] [--time N] [--max N] [--callees native|lifted] [--coverage FILE]
//                    [--quiet]
//
// CAPTURE: capture files (.cap) or directories of them. For each capture the lifted function runs from the state at
// entry and must give the state at exit the capture holds (the capture is of the lifted function: this checks that the
// replay is faithful); then the native implementation runs from the same state and must give the same, bit for bit:
// the registers of its contract (pspocket/spu/replace.hpp: $0, $1, $3, $80-$127 and the volatile ones its callers may
// read after it), the pc, the stop, the channels and all 256 KB of local store. The other volatile registers it may
// leave otherwise: their count is reported. A native implementation that declines
// the inputs must leave everything as it was. --time N runs each function N more times on each capture and reports the
// time a call of each: the fastest of the N runs of each capture (local store and registers restored before each run,
// outside the timing), averaged over the captures. --coverage FILE runs each capture in the interpreter and writes how
// many times each instruction ran ("function address count" lines): the paths and loops the game uses.
//
// The function checked calls the other listed functions through the derived program, natively where they have native
// code (--callees lifted: the lifted program, every callee lifted). The game's lifted programs come built in (-DPSPOCKET_GAME_SPU, as spu-check) or from a game library (--library). The
// captures hold the game's data: they stay in the gitignored inventory/ (CLAUDE.md rule 1); nothing of the game is in
// this source.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/game/host.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/replace.hpp"
#include "pspocket/spu/runtime.h"

#if PSPOCKET_HAVE_GAME_SPU
extern "C" void pspocket_game_spu_register(void); // the game's lifted SPU programs (scripts/lift-spu.sh, D-076)
#endif

using namespace pspocket;
namespace rp = pspocket::spu::replace;

namespace {

constexpr u32 kLsSize = PS_SPU_LS_SIZE;

struct Capture {
    std::string path;
    rp::CaptureHeader header{};
    rp::CaptureState in{}, out{};
    std::vector<u8> ls_in, ls_out;
};

bool load(const std::filesystem::path& path, Capture& c, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open";
        return false;
    }
    c.path = path.string();
    c.ls_in.resize(kLsSize);
    f.read(reinterpret_cast<char*>(&c.header), sizeof c.header);
    if (!f || std::memcmp(c.header.magic, "PSNCAP01", 8) != 0) {
        error = "not a capture (PSNCAP01)";
        return false;
    }
    f.read(reinterpret_cast<char*>(&c.in), sizeof c.in);
    f.read(reinterpret_cast<char*>(c.ls_in.data()), kLsSize);
    f.read(reinterpret_cast<char*>(&c.out), sizeof c.out);
    u32 lines = 0;
    f.read(reinterpret_cast<char*>(&lines), 4);
    if (!f || lines > kLsSize / 16) {
        error = "truncated";
        return false;
    }
    c.ls_out = c.ls_in;
    for (u32 i = 0; i < lines; i++) {
        u32 at = 0;
        f.read(reinterpret_cast<char*>(&at), 4);
        if (!f || at > kLsSize - 16) {
            error = "truncated";
            return false;
        }
        f.read(reinterpret_cast<char*>(c.ls_out.data() + at), 16);
    }
    if (!f) {
        error = "truncated";
        return false;
    }
    return true;
}

std::string hex128(const ps_spu_v128& v)
{
    return std::format("{:08x}{:08x}{:08x}{:08x}", v.u32[3], v.u32[2], v.u32[1], v.u32[0]);
}

// What differs between the state a run left (ctx, ls) and the expected one in what must be exact (`exact`: the
// registers; everything else always); empty when nothing does. *loose counts the other registers that differ.
std::string differences(const rp::CaptureState& want, const u8* ls_want, const SPUContext& ctx, const u8* ls,
    const rp::Registers& exact = {{~0ull, ~0ull}}, u32* loose = nullptr)
{
    rp::CaptureState got{};
    rp::save_state(ctx, got);
    std::string d;
    u32 regs = 0;
    for (u32 i = 0; i < 128; i++) {
        if (std::memcmp(&want.gpr[i], &got.gpr[i], 16) == 0) continue;
        if (!exact.test(i)) {
            if (loose) ++*loose;
            continue;
        }
        if (regs++ < 6) d += std::format(" r{}: {} (want) vs {};", i, hex128(want.gpr[i]), hex128(got.gpr[i]));
    }
    if (regs > 6) d += std::format(" {} registers in all;", regs);
    if (std::memcmp(&want.fpscr, &got.fpscr, 16) != 0) d += " fpscr;";
    if (want.pc != got.pc) d += std::format(" pc: {:05x} vs {:05x};", want.pc, got.pc);
    if (want.stop_reason != got.stop_reason || want.stop_code != got.stop_code || want.exception != got.exception)
        d += std::format(" stop: {}/{:#x}/{} vs {}/{:#x}/{};", want.stop_reason, want.stop_code, want.exception,
            got.stop_reason, got.stop_code, got.exception);
    if (want.srr0 != got.srr0) d += " srr0;";
    ps_spu_channels a = want.ch, b = got.ch;
    a.res_version = b.res_version = 0;
    if (std::memcmp(&a, &b, sizeof a) != 0) d += " channels;";
    u32 lines = 0, first = 0;
    for (u32 i = 0; i < kLsSize; i += 16)
        if (std::memcmp(ls_want + i, ls + i, 16) != 0 && lines++ == 0) first = i;
    if (lines) d += std::format(" local store: {} lines differ, the first at {:05x};", lines, first);
    return d;
}

struct Totals {
    u64 captures = 0, replay_bad = 0, native_ok = 0, native_bad = 0, declined = 0, decline_bad = 0;
    double lifted_ns = 0, native_ns = 0;
    u64 timed = 0, lifted_timed = 0, loose_regs = 0;
};

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::filesystem::path> inputs;
    std::string library;
    u32 time_runs = 0;
    u64 max = 0;
    bool quiet = false, verbose = false, bench = false, callees_native = true;
    std::string coverage_path;
    std::map<std::pair<u32, u32>, u64> coverage; // (function, address) -> instructions run
    u32 bench_runs = 1000;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "spu-native-check: %s needs a value\n", a.c_str());
                std::exit(64);
            }
            return argv[++i];
        };
        if (a == "--library") library = value();
        else if (a == "--time") time_runs = static_cast<u32>(std::stoul(value()));
        else if (a == "--max") max = std::stoull(value());
        else if (a == "--quiet") quiet = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--callees") callees_native = value() != "lifted";
        else if (a == "--coverage") coverage_path = value();
        else if (a == "--bench") bench = true;
        else if (a == "--bench-runs") bench_runs = static_cast<u32>(std::stoul(value()));
        else if (a.rfind("--", 0) == 0) {
            std::fprintf(stderr, "usage: spu-native-check CAPTURE... [--library FILE] [--time N] [--max N] [--quiet]\n");
            return 64;
        } else inputs.emplace_back(a);
    }
    if (inputs.empty()) {
        std::fprintf(stderr, "usage: spu-native-check CAPTURE... [--library FILE] [--time N] [--max N] [--quiet]\n");
        return 64;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& in : inputs) {
        if (std::filesystem::is_directory(in)) {
            for (const auto& e : std::filesystem::directory_iterator(in))
                if (e.path().extension() == ".cap") files.push_back(e.path());
        } else {
            files.push_back(in);
        }
    }
    std::sort(files.begin(), files.end());
    if (max && files.size() > max) files.resize(max);

    // Derived programs (the listed functions through the dispatcher) unless --callees lifted: the function checked calls
    // the others as the game does (natively where there is native code); the function itself is run directly.
    rp::set_mode(callees_native ? rp::Mode::on : rp::Mode::off);
    static GuestMemory memory; // DMA from a captured function reads zeros (the functions checked make none)
    if (!memory.reserve()) {
        std::fprintf(stderr, "spu-native-check: cannot reserve guest memory\n");
        return 1;
    }
    if (!library.empty()) {
        game::LoadOptions options;
        options.check_image_words = false;
        std::string error;
        if (!game::load_library(library, memory, nullptr, &error, options)) {
            std::fprintf(stderr, "spu-native-check: %s\n", error.c_str());
            return 1;
        }
    } else {
#if PSPOCKET_HAVE_GAME_SPU
        pspocket_game_spu_register();
#else
        std::fprintf(stderr, "spu-native-check: this build has no game's SPU programs built in: give --library\n");
        return 64;
#endif
    }
    log::set_min_level(log::Level::Warn);

    u8* ls = static_cast<u8*>(std::aligned_alloc(128, kLsSize));
    std::map<std::pair<u64, u32>, Totals> totals;
    const u64 fp = ps_spu_fpenv_enter();
    u64 failures = 0;
    for (const auto& file : files) {
        Capture c;
        std::string error;
        if (!load(file, c, error)) {
            std::printf("SKIP %s: %s\n", file.string().c_str(), error.c_str());
            continue;
        }
        const rp::CaptureHeader& h = c.header;
        Totals& t = totals[{h.hash, h.addr}];
        t.captures++;
        const ps_spu_program* run = ps_spu_find_program(h.hash, h.code_start, h.code_size);
        const ps_spu_program* p = run && rp::original_of(run) ? rp::original_of(run) : run;
        if (!p || !p->table || h.addr < p->code_start || h.addr - p->code_start >= p->code_size) {
            std::printf("SKIP %s: program %016llx is not built in\n", file.string().c_str(),
                static_cast<unsigned long long>(h.hash));
            continue;
        }
        const ps_spu_fn lifted = p->table[(h.addr - p->code_start) >> 2];
        const rp::Impl native = rp::native(h.hash, h.code_start, h.code_size, h.addr);
        SPUContext ctx{};
        auto reset = [&] {
            std::memcpy(ls, c.ls_in.data(), kLsSize);
            ctx = SPUContext{};
            ps_spu_context_init(&ctx, ls, memory.base());
            ps_spu_attach_program_at(&ctx, run, (h.code_start + h.reloc) & PS_SPU_LS_MASK);
            rp::load_state(c.in, ctx);
        };
        // --coverage FILE: the interpreter from the captured entry to the captured exit, every instruction counted by
        // address (the program's own): which paths the game takes, how hot each loop is.
        if (!coverage_path.empty()) {
            reset();
            for (u64 steps = 0; steps < 50'000'000; steps++) {
                const u32 at = (ctx.pc - h.reloc) & PS_SPU_LS_MASK;
                coverage[{h.addr, at}]++;
                if (ps_spu_interpret(&ctx, 1) != PS_SPU_STOP_LIMIT) break;
                if (ctx.pc == c.out.pc) break;
            }
        }
        // The replay: the lifted function from the captured entry gives the captured exit.
        reset();
        lifted(&ctx, ls);
        if (const std::string d = differences(c.out, c.ls_out.data(), ctx, ls); !d.empty()) {
            t.replay_bad++;
            failures++;
            std::printf("REPLAY %s: the lifted function does not give the captured exit:%s\n", c.path.c_str(), d.c_str());
            continue;
        }
        // No native implementation, or one that declines these inputs: the lifted function's time alone (what there
        // is to win).
        // The fastest of the runs: other processes preempt this one (a loaded Mac), never make it faster.
        const auto time_lifted = [&] {
            if (!time_runs) return;
            double best = 1e30;
            for (u32 r = 0; r < time_runs; r++) {
                reset();
                const auto t0 = std::chrono::steady_clock::now();
                lifted(&ctx, ls);
                best = std::min(best, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count());
            }
            t.lifted_ns += best;
            t.lifted_timed++;
        };
        if (!native) {
            time_lifted();
            continue;
        }
        reset();
        if (!native(&ctx, ls)) {
            t.declined++;
            if (const std::string d = differences(c.in, c.ls_in.data(), ctx, ls); !d.empty()) {
                t.decline_bad++;
                failures++;
                std::printf("DECLINE %s: the native function declined but changed:%s\n", c.path.c_str(), d.c_str());
            }
            time_lifted();
            continue;
        }
        const rp::Registers exact = rp::exact_registers(h.hash, h.code_start, h.code_size, h.addr);
        u32 loose = 0;
        const std::string d = differences(c.out, c.ls_out.data(), ctx, ls, exact, &loose);
        t.loose_regs += loose;
        if (!d.empty()) {
            t.native_bad++;
            failures++;
            if (!quiet || t.native_bad <= 5) std::printf("DIFF %s:%s\n", c.path.c_str(), d.c_str());
            continue;
        }
        t.native_ok++;
        if (time_runs) {
            double ln = 1e30, nn = 1e30;
            for (u32 r = 0; r < time_runs; r++) {
                reset();
                auto t0 = std::chrono::steady_clock::now();
                lifted(&ctx, ls);
                auto t1 = std::chrono::steady_clock::now();
                ln = std::min(ln, std::chrono::duration<double, std::nano>(t1 - t0).count());
                reset();
                t0 = std::chrono::steady_clock::now();
                native(&ctx, ls);
                t1 = std::chrono::steady_clock::now();
                nn = std::min(nn, std::chrono::duration<double, std::nano>(t1 - t0).count());
            }
            t.lifted_ns += ln;
            t.native_ns += nn;
            t.timed++;
            if (verbose) std::printf("TIME %s: %.0f ns lifted, %.0f ns native\n", c.path.c_str(), ln, nn);
            if (bench) {
                // Back to back without restoring anything between the runs (the state they leave is not checked).
                const rp::CaptureState in = c.in;
                for (int which = 0; which < 2; which++) {
                    reset();
                    const auto t0 = std::chrono::steady_clock::now();
                    for (u32 r = 0; r < bench_runs; r++) {
                        rp::load_state(in, ctx);
                        if (which == 0) lifted(&ctx, ls);
                        else native(&ctx, ls);
                    }
                    std::printf("BENCH %s: %.0f ns a call %s\n", c.path.c_str(),
                        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / static_cast<double>(bench_runs),
                        which ? "native" : "lifted");
                }
            }
        }
    }
    ps_spu_fpenv_leave(fp);
    if (!coverage_path.empty()) {
        std::ofstream out(coverage_path);
        for (const auto& [key, n] : coverage) out << std::format("{:05x} {:05x} {}\n", key.first, key.second, n);
    }
    for (const auto& [key, t] : totals) {
        std::printf("%016llx %05x: %llu captures; replay differs %llu; native: %llu exact, %llu differ, %llu declined (%llu of "
                    "them changed something)",
            static_cast<unsigned long long>(key.first), key.second, static_cast<unsigned long long>(t.captures),
            static_cast<unsigned long long>(t.replay_bad), static_cast<unsigned long long>(t.native_ok),
            static_cast<unsigned long long>(t.native_bad), static_cast<unsigned long long>(t.declined),
            static_cast<unsigned long long>(t.decline_bad));
        if (t.native_ok + t.native_bad)
            std::printf("; volatile registers left otherwise: %.1f a call",
                static_cast<double>(t.loose_regs) / static_cast<double>(t.native_ok + t.native_bad));
        if (t.timed)
            std::printf("; a call %.0f ns lifted, %.0f ns native (x%.2f)", t.lifted_ns / static_cast<double>(t.timed),
                t.native_ns / static_cast<double>(t.timed), t.lifted_ns / std::max(1.0, t.native_ns));
        else if (t.lifted_timed)
            std::printf("; a call %.0f ns lifted", t.lifted_ns / static_cast<double>(t.lifted_timed));
        std::printf("\n");
    }
    std::free(ls);
    return failures ? 1 : 0;
}
