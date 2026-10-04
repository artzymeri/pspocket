// pspocket-boot: runs a PS3 executable under the PPU interpreter on the host, with lv2 and the HLE modules, until it
// exits, a timeout passes or (with --stop-at-first) it reaches the first unimplemented syscall or firmware function.
// This is the Phase 2 bring-up tool (plan section 10): the log names every gap the game hits, in order.
//
//   pspocket-boot <EBOOT.elf> [--bdvd DIR|IMAGE.iso] [--hdd0 DIR] [--app-home DIR] [--timeout SECONDS]
//                 [--stop-at-first] [--log trace|debug|info|warn|error] [--imports]
//   pspocket-boot --probe-disc IMAGE.iso
//
// --bdvd takes the game's disc as a directory or as its disc image, read in place (/dev_bdvd from the image root,
// /app_home from PS3_GAME/USRDIR unless --app-home says otherwise).
// --imports lists every imported function by module (implemented ones marked) and exits without running.
// A host fault inside guest memory (an access to an unmapped guest address) is reported with the guest address and
// the faulting PPU thread's instruction address before the process dies (CLAUDE.md rule 5).
//
// The executable must be a decrypted ELF (RPCS3's --decrypt writes one). Game files stay outside the repository.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/hle/process.hpp"
#include "pspocket/loader/disc.hpp"
#include "pspocket/loader/loader.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/ppu/disasm.hpp"
#include "pspocket/ppu/runtime.h"

using namespace pspocket;

namespace {

int usage()
{
    std::fprintf(stderr,
        "usage: pspocket-boot <EBOOT.elf> [--bdvd DIR|IMAGE.iso] [--hdd0 DIR] [--app-home DIR] [--timeout SECONDS]\n"
        "                     [--stop-at-first] [--log trace|debug|info|warn|error] [--imports]\n"
        "       pspocket-boot --probe-disc IMAGE.iso\n");
    return 64;
}

u8* g_guest_base = nullptr;
GuestMemory* g_memory = nullptr;
hle::HleProcess* g_hle = nullptr;

void write_err(const char* text, int n)
{
    if (n > 0) (void)!write(STDERR_FILENO, text, static_cast<size_t>(n));
}

// After the fault line: the argument registers, the firmware function running (when the PPU is in an HLE stub) and
// the caller's last instructions before LR, so a crash in an HLE function shows what the game passed in each
// register. Best effort: it formats with the heap, which is fine for a fault in guest memory (the case reported).
void report_call_site(const PPUContext& c)
{
    char buf[256];
    int n = std::snprintf(buf, sizeof buf, "  r4..r10 = 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx\n",
        static_cast<unsigned long long>(c.gpr[4]), static_cast<unsigned long long>(c.gpr[5]),
        static_cast<unsigned long long>(c.gpr[6]), static_cast<unsigned long long>(c.gpr[7]),
        static_cast<unsigned long long>(c.gpr[8]), static_cast<unsigned long long>(c.gpr[9]),
        static_cast<unsigned long long>(c.gpr[10]));
    write_err(buf, std::min<int>(n, sizeof buf - 1));
    if (g_hle) {
        if (const hle::LinkedFunction* f = g_hle->linked_at(c.cia)) {
            n = std::snprintf(buf, sizeof buf, "  in firmware function %s::%s (0x%08X), called from 0x%08x:\n",
                f->module.c_str(), f->name.empty() ? "?" : f->name.c_str(), f->nid, static_cast<u32>(c.lr) - 4);
            write_err(buf, std::min<int>(n, sizeof buf - 1));
        }
    }
    constexpr u32 kInstructions = 16;
    const u32 ret = static_cast<u32>(c.lr);
    if (!g_memory || ret < 4 * kInstructions || !g_memory->is_committed(ret - 4 * kInstructions) ||
        !g_memory->is_committed(ret - 4)) {
        return;
    }
    for (u32 pc = ret - 4 * kInstructions; pc < ret; pc += 4) {
        const std::string text = ppu::disassemble(ps_read32(g_guest_base, pc), pc);
        n = std::snprintf(buf, sizeof buf, "    0x%08x: %s\n", pc, text.c_str());
        write_err(buf, std::min<int>(n, sizeof buf - 1));
    }
}

// SIGSEGV/SIGBUS: name the guest address and the PPU thread's position, then exit with 128 + signal (a debugger
// attached to the process stops at the fault before this handler runs).
void on_fault(int sig, siginfo_t* info, void*)
{
    const auto addr = reinterpret_cast<uintptr_t>(info->si_addr);
    const auto base = reinterpret_cast<uintptr_t>(g_guest_base);
    const lv2::PpuThread* t = lv2::current_thread();
    const PPUContext* c = t ? t->ctx : nullptr;
    char buf[384];
    int n;
    if (base && addr >= base && addr < base + GuestMemory::kGuestSize + GuestMemory::kGuardSize) {
        n = std::snprintf(buf, sizeof buf,
            "\n[fatal] boot: %s: guest access to 0x%08llx (not mapped, or protected) by PPU thread 0x%x at cia 0x%08x "
            "(lr 0x%llx, r1 0x%llx, r2 0x%llx, r3 0x%llx). cia is exact for interpreted code.\n",
            sig == SIGBUS ? "SIGBUS" : "SIGSEGV", static_cast<unsigned long long>(addr - base), t ? t->id : 0,
            c ? c->cia : 0, c ? static_cast<unsigned long long>(c->lr) : 0ull,
            c ? static_cast<unsigned long long>(c->gpr[1]) : 0ull, c ? static_cast<unsigned long long>(c->gpr[2]) : 0ull,
            c ? static_cast<unsigned long long>(c->gpr[3]) : 0ull);
    } else {
        n = std::snprintf(buf, sizeof buf, "\n[fatal] boot: %s at host address %p (outside guest memory); PPU thread 0x%x "
                                           "at cia 0x%08x\n",
            sig == SIGBUS ? "SIGBUS" : "SIGSEGV", info->si_addr, t ? t->id : 0, c ? c->cia : 0);
    }
    write_err(buf, std::min<int>(n, sizeof buf - 1));
    if (c) report_call_site(*c);
    _exit(128 + sig);
}

void install_fault_reporter(GuestMemory& memory)
{
    g_guest_base = memory.base();
    g_memory = &memory;
    struct sigaction sa{};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

void print_imports(const hle::HleProcess& hle)
{
    const auto linked = hle.linked();
    std::vector<std::string> modules;
    for (const auto* f : linked)
        if (std::find(modules.begin(), modules.end(), f->module) == modules.end()) modules.push_back(f->module);
    usize implemented = 0;
    for (const auto& m : modules) {
        usize count = 0, done = 0;
        for (const auto* f : linked)
            if (f->module == m) count++, done += f->impl != nullptr;
        std::printf("%s: %zu imported, %zu implemented\n", m.c_str(), count, done);
        for (const auto* f : linked)
            if (f->module == m)
                std::printf("  %c %s (0x%08X)\n", f->impl ? '*' : ' ', f->name.empty() ? "?" : f->name.c_str(), f->nid);
        implemented += done;
    }
    std::printf("total: %zu imported functions, %zu implemented (*)\n", linked.size(), implemented);
}

bool read_file(const char* path, std::vector<u8>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    // --probe-disc IMAGE: is it a readable PS3 game disc? Prints its title ID (exit 0) or why not (exit 1).
    if (argc == 3 && std::string_view(argv[1]) == "--probe-disc") {
        loader::DiscInfo disc;
        std::string error;
        if (!loader::open_disc_image(argv[2], &disc, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("%s %s\n", disc.title_id.c_str(), disc.title.c_str());
        return 0;
    }
    if (argc < 2) return usage();
    const char* elf_path = nullptr;
    std::string bdvd, hdd0, app_home;
    double timeout_s = 30;
    bool stop_at_first = false;
    bool list_imports = false;
    for (int i = 1; i < argc; i++) {
        const std::string_view a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--bdvd") { const char* v = value(); if (!v) return usage(); bdvd = v; }
        else if (a == "--hdd0") { const char* v = value(); if (!v) return usage(); hdd0 = v; }
        else if (a == "--app-home") { const char* v = value(); if (!v) return usage(); app_home = v; }
        else if (a == "--timeout") { const char* v = value(); if (!v) return usage(); timeout_s = std::atof(v); }
        else if (a == "--stop-at-first") stop_at_first = true;
        else if (a == "--imports") list_imports = true;
        else if (a == "--log") {
            const char* v = value();
            if (!v) return usage();
            const std::string_view l = v;
            log::set_min_level(l == "trace" ? log::Level::Trace : l == "debug" ? log::Level::Debug
                : l == "warn" ? log::Level::Warn : l == "error" ? log::Level::Error : log::Level::Info);
        } else if (a.starts_with("--")) return usage();
        else if (!elf_path) elf_path = argv[i];
        else return usage();
    }
    if (!elf_path) return usage();

    std::vector<u8> bytes;
    if (!read_file(elf_path, bytes)) {
        std::fprintf(stderr, "pspocket-boot: cannot read %s\n", elf_path);
        return 66;
    }
    if (bytes.size() >= 4 && bytes[0] == 'S' && bytes[1] == 'C' && bytes[2] == 'E' && bytes[3] == 0) {
        std::fprintf(stderr, "pspocket-boot: %s is a SELF; decrypt it to an ELF first (docs/phase0.md)\n", elf_path);
        return 65;
    }
    auto exe = loader::Executable::parse(std::move(bytes));
    if (!exe) {
        std::fprintf(stderr, "pspocket-boot: %s: %s\n", elf_path, exe.error().message.c_str());
        return 65;
    }

    GuestMemory memory;
    if (!memory.reserve()) return 71;
    install_fault_reporter(memory);

    // No lifted code is linked into this host: every guest function runs in the interpreter.
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);

    lv2::Config cfg = (*exe)->kernel_config();
    cfg.abort_on_unimplemented = stop_at_first;
    std::atomic<bool> exited{false};
    cfg.on_process_exit = [&](s32 status) {
        PS_LOG_INFO("boot", "process exit, status {}", status);
        exited = true;
    };
    loader::BootState state;
    loader::prepare_kernel_config(cfg, state);

    lv2::Kernel kernel(memory, cfg);
    kernel.install();
    hle::GameInfo game;
    if (!bdvd.empty() && std::filesystem::is_regular_file(bdvd)) {
        // A disc image: read in place; /dev_bdvd and /app_home come from it.
        loader::DiscInfo disc;
        std::string error;
        if (!loader::mount_disc_image(kernel, bdvd, &disc, &error)) {
            std::fprintf(stderr, "pspocket-boot: %s\n", error.c_str());
            return 66;
        }
        game.title_id = disc.title_id;
        game.param_sfo = std::move(disc.param_sfo);
    } else if (!bdvd.empty()) {
        if (!kernel.vfs_mount("/dev_bdvd", bdvd, true)) return 66;
        std::vector<u8> sfo;
        if (read_file((bdvd + "/PS3_GAME/PARAM.SFO").c_str(), sfo)) game.param_sfo = std::move(sfo);
    }
    if (!hdd0.empty() && !kernel.vfs_mount("/dev_hdd0", hdd0, false)) return 66;
    if (!app_home.empty() && !kernel.vfs_mount("/app_home", app_home, false)) return 66;

    hle::HleProcess hle(kernel, PS_MEM_MAIN_BASE, cfg.main_area_end, {stop_at_first, std::move(game)});
    g_hle = &hle;
    struct ForgetHle {
        ~ForgetHle() { g_hle = nullptr; } // before `hle` is destroyed
    } forget_hle;
    hle.install();

    u32 main_id = 0;
    loader::LoadReport report;
    if (const lv2::Result r = loader::load(**exe, kernel, hle, &report); r != lv2::CELL_OK) {
        std::fprintf(stderr, "pspocket-boot: load failed: %s\n", lv2::error_name(r));
        return 70;
    }
    if (list_imports) {
        print_imports(hle);
        return 0;
    }
    if (const lv2::Result r = loader::start_main_thread(*exe, kernel, state, {}, main_id); r != lv2::CELL_OK) {
        std::fprintf(stderr, "pspocket-boot: cannot start the main thread: %s\n", lv2::error_name(r));
        return 70;
    }

    const auto start = std::chrono::steady_clock::now();
    const bool done = kernel.wait_for_exit(std::chrono::milliseconds(static_cast<s64>(timeout_s * 1000)));
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    // Summary: which gaps the run hit, most-called first.
    std::vector<const hle::LinkedFunction*> missing;
    for (const auto* f : hle.linked())
        if (!f->impl && f->calls.load() > 0) missing.push_back(f);
    std::sort(missing.begin(), missing.end(), [](auto* a, auto* b) { return a->calls.load() > b->calls.load(); });
    std::printf("\n%s after %.1f s\n", done ? "process exited" : "timeout", secs);
    if (done) std::printf("exit status: %d\n", kernel.exit_status());
    std::printf("imports: %u functions (%u implemented), %u unresolved variables\n", report.functions_linked,
        report.functions_implemented, report.variables_unresolved);
    std::printf("unimplemented syscalls called: %llu\n", static_cast<unsigned long long>(kernel.unimplemented_count()));
    std::printf("unimplemented functions called: %zu\n", missing.size());
    for (const auto* f : missing)
        std::printf("  %8llu  %s::%s (0x%08X)\n", static_cast<unsigned long long>(f->calls.load()), f->module.c_str(),
            f->name.empty() ? "?" : f->name.c_str(), f->nid);

    if (!kernel.shutdown()) {
        std::fprintf(stderr, "pspocket-boot: guest threads still running at shutdown\n");
        std::fflush(stdout);
        std::_Exit(done ? 0 : 75);
    }
    return done ? 0 : 75;
}
