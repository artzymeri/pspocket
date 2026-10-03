// pspocket-boot: runs a PS3 executable under the PPU interpreter on the host, with lv2 and the HLE modules, until it
// exits, a timeout passes or (with --stop-at-first) it reaches the first unimplemented syscall or firmware function.
// This is the Phase 2 bring-up tool (plan section 10): the log names every gap the game hits, in order.
//
//   pspocket-boot <EBOOT.elf> [--bdvd DIR] [--hdd0 DIR] [--app-home DIR] [--timeout SECONDS] [--stop-at-first]
//                 [--log trace|debug|info|warn|error]
//
// The executable must be a decrypted ELF (RPCS3's --decrypt writes one). Game files stay outside the repository.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/hle/process.hpp"
#include "pspocket/loader/loader.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/ppu/runtime.h"

using namespace pspocket;

namespace {

int usage()
{
    std::fprintf(stderr,
        "usage: pspocket-boot <EBOOT.elf> [--bdvd DIR] [--hdd0 DIR] [--app-home DIR] [--timeout SECONDS]\n"
        "                     [--stop-at-first] [--log trace|debug|info|warn|error]\n");
    return 64;
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
    if (argc < 2) return usage();
    const char* elf_path = nullptr;
    std::string bdvd, hdd0, app_home;
    double timeout_s = 30;
    bool stop_at_first = false;
    for (int i = 1; i < argc; i++) {
        const std::string_view a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--bdvd") { const char* v = value(); if (!v) return usage(); bdvd = v; }
        else if (a == "--hdd0") { const char* v = value(); if (!v) return usage(); hdd0 = v; }
        else if (a == "--app-home") { const char* v = value(); if (!v) return usage(); app_home = v; }
        else if (a == "--timeout") { const char* v = value(); if (!v) return usage(); timeout_s = std::atof(v); }
        else if (a == "--stop-at-first") stop_at_first = true;
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
    if (!bdvd.empty() && !kernel.vfs_mount("/dev_bdvd", bdvd, true)) return 66;
    if (!hdd0.empty() && !kernel.vfs_mount("/dev_hdd0", hdd0, false)) return 66;
    if (!app_home.empty() && !kernel.vfs_mount("/app_home", app_home, false)) return 66;

    hle::HleProcess hle(kernel, PS_MEM_MAIN_BASE, cfg.main_area_end, {stop_at_first});
    hle.install();

    u32 main_id = 0;
    loader::LoadReport report;
    if (const lv2::Result r = loader::boot(*exe, kernel, hle, state, {}, main_id, &report); r != lv2::CELL_OK) {
        std::fprintf(stderr, "pspocket-boot: boot failed: %s\n", lv2::error_name(r));
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
