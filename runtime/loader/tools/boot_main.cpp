// pspocket-boot: runs a PS3 executable under the PPU interpreter on the host, with lv2 and the HLE modules, until it
// exits, a timeout passes or (with --stop-at-first) it reaches the first unimplemented syscall or firmware function.
// This is the Phase 2 bring-up tool (plan section 10): the log names every gap the game hits, in order.
//
//   pspocket-boot <EBOOT.elf> [--bdvd DIR|IMAGE.iso] [--hdd0 DIR] [--flash DIR] [--app-home DIR]
//                 [--timeout SECONDS] [--report-every SECONDS] [--stop-at-first]
//                 [--log trace|debug|info|warn|error] [--imports] [--gpu null|metal] [--frames DIR]
//                 [--frame-every N] [--dump-on-fault FILE]
//   pspocket-boot --probe-disc IMAGE.iso
//
// --bdvd takes the game's disc as a directory or as its disc image, read in place (/dev_bdvd from the image root,
// /app_home from PS3_GAME/USRDIR unless --app-home says otherwise).
// --flash mounts DIR read-only as /dev_flash: the system files a game reads itself (GTA V's MultiStream loads its MP3
// codec, sys/external/flashMP3.pic), taken from the user's own firmware and kept out of the repository (D-067).
// --imports lists every imported function by module (implemented ones marked) and exits without running.
// A host fault inside guest memory (an access to an unmapped guest address) is reported with the guest address and
// the faulting PPU thread's instruction address before the process dies (CLAUDE.md rule 5).
// A run that ends at its timeout, or on such a fault, prints the thread report (lv2/report.hpp): every PPU thread's
// state, what it waits on and its guest backtrace, and the RSX's FIFO. --report-every prints it while the game runs.
// --gpu metal draws the game's frames with the Metal backend (Apple platforms; the default null backend runs the
// commands and draws nothing). --frames DIR (which implies Metal) saves every Nth presented frame (--frame-every,
// 60 by default) as DIR/frame-NNNNNN.png: what the game shows, at the backend's output size.
// --dump-on-fault FILE writes the guest's committed memory below the RSX's local memory to FILE when a guest fault
// ends the run, for offline analysis (game data: keep it in inventory/). The file is a sequence of blocks: a
// big-endian u32 guest address, a u32 byte count, then the bytes. A fault in SPU code also writes that SPU's local
// store (256 KB) to FILE.ls.
//
// The executable must be a decrypted ELF (RPCS3's --decrypt writes one). Game files stay outside the repository.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <pthread.h>
#include <unistd.h>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/hle/process.hpp"
#include "pspocket/loader/disc.hpp"
#include "pspocket/loader/loader.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/report.hpp"
#include "pspocket/core/png.hpp"
#if defined(PSPOCKET_HAVE_METAL)
#include "pspocket/rsx/metal/metal_backend.hpp"
#include "pspocket/rsx/write_tracker.hpp"
#endif
#include "pspocket/ppu/disasm.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/spu/disasm.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;

namespace {

int usage()
{
    std::fprintf(stderr,
        "usage: pspocket-boot <EBOOT.elf> [--bdvd DIR|IMAGE.iso] [--hdd0 DIR] [--flash DIR] [--app-home DIR]\n"
        "                     [--timeout SECONDS] [--report-every SECONDS] [--stop-at-first]\n"
        "                     [--log trace|debug|info|warn|error] [--imports] [--gpu null|metal] [--frames DIR]\n"
        "                     [--frame-every N] [--dump-on-fault FILE]\n"
        "       pspocket-boot --probe-disc IMAGE.iso\n");
    return 64;
}

u8* g_guest_base = nullptr;
GuestMemory* g_memory = nullptr;
hle::HleProcess* g_hle = nullptr;
std::atomic<lv2::Kernel*> g_kernel{nullptr};
volatile sig_atomic_t g_fault_signal = 0;
std::string g_dump_path; // --dump-on-fault

void write_err(const char* text, int n)
{
    if (n > 0) (void)!write(STDERR_FILENO, text, static_cast<size_t>(n));
}

// Writes the committed guest memory below the RSX's local memory to g_dump_path (--dump-on-fault): blocks of a
// big-endian address and byte count, then the bytes. Best effort, from the fault handler.
void dump_guest_memory()
{
    if (g_dump_path.empty() || !g_memory) return;
    std::FILE* f = std::fopen(g_dump_path.c_str(), "wb");
    if (!f) return;
    // Runs of committed 4 KB pages, each written as one block.
    u64 total = 0;
    u64 addr = 0;
    while (addr < PS_MEM_RSX_LOCAL_BASE) {
        if (!g_memory->is_committed(static_cast<u32>(addr))) {
            addr += PS_GUEST_PAGE_SIZE;
            continue;
        }
        u64 end = addr + PS_GUEST_PAGE_SIZE;
        while (end < PS_MEM_RSX_LOCAL_BASE && g_memory->is_committed(static_cast<u32>(end))) end += PS_GUEST_PAGE_SIZE;
        const u32 a = static_cast<u32>(addr), size = static_cast<u32>(end - addr);
        const u8 header[8] = {static_cast<u8>(a >> 24), static_cast<u8>(a >> 16), static_cast<u8>(a >> 8),
            static_cast<u8>(a), static_cast<u8>(size >> 24), static_cast<u8>(size >> 16), static_cast<u8>(size >> 8),
            static_cast<u8>(size)};
        std::fwrite(header, 1, sizeof header, f);
        std::fwrite(g_guest_base + a, 1, size, f);
        total += size;
        addr = end;
    }
    std::fclose(f);
    char buf[256];
    int n = std::snprintf(buf, sizeof buf, "  guest memory (%llu MB) written to %s\n",
        static_cast<unsigned long long>(total >> 20), g_dump_path.c_str());
    write_err(buf, std::min<int>(n, sizeof buf - 1));
    // The faulting SPU's local store, when the fault is in SPU code.
    if (const SPUContext* spu = ps_spu_current()) {
        const std::string ls_path = g_dump_path + ".ls";
        if (std::FILE* l = std::fopen(ls_path.c_str(), "wb")) {
            std::fwrite(spu->ls, 1, 0x40000, l);
            std::fclose(l);
            n = std::snprintf(buf, sizeof buf, "  the SPU's local store written to %s\n", ls_path.c_str());
            write_err(buf, std::min<int>(n, sizeof buf - 1));
        }
    }
}

// The thread report (lv2/report.hpp), with the HLE function behind each import stub named.
std::string thread_report(lv2::Kernel& kernel, std::string title)
{
    lv2::ReportOptions options;
    options.title = std::move(title);
    options.describe_address = [](u32 addr) -> std::string {
        const hle::LinkedFunction* f = g_hle ? g_hle->linked_at(addr) : nullptr;
        if (!f) return {};
        return f->module + "::" + (f->name.empty() ? std::format("{:#010x}", f->nid) : f->name);
    };
    return lv2::format_report(kernel, options);
}

void print_thread_report(lv2::Kernel& kernel, std::string title)
{
    const std::string report = thread_report(kernel, std::move(title));
    std::fwrite(report.data(), 1, report.size(), stdout);
    std::fflush(stdout);
}

// The thread report after a guest fault takes locks the faulting thread might hold: if it has not finished within a
// few seconds, end the process as the fault would have.
void on_report_timeout(int) { _exit(128 + g_fault_signal); }

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
    const bool guest = base && addr >= base && addr < base + GuestMemory::kGuestSize + GuestMemory::kGuardSize;
    if (guest) {
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
    if (!t) {
        // Not a PPU thread: name the host thread (SPURS workers, the RSX, cellAudio...) and the SPU code it runs.
        char name[64] = "?";
        pthread_getname_np(pthread_self(), name, sizeof name);
        n = std::snprintf(buf, sizeof buf, "  on host thread \"%s\"\n", name);
        write_err(buf, std::min<int>(n, sizeof buf - 1));
        if (const SPUContext* spu = ps_spu_current()) {
            const u32 pc = spu->pc & 0x3FFFC;
            const std::string insn = spu::disassemble(ps_read32(spu->ls, pc), pc);
            n = std::snprintf(buf, sizeof buf, "  SPU code at 0x%05x: %s; MFC parameters: LSA 0x%05x, EA 0x%08x%08x, "
                                               "size 0x%x, tag %u\n",
                pc, insn.c_str(), spu->ch.mfc_lsa, spu->ch.mfc_eah, spu->ch.mfc_eal, spu->ch.mfc_size, spu->ch.mfc_tag);
            write_err(buf, std::min<int>(n, sizeof buf - 1));
            // The registers' preferred words ($0 is the link register, $1 the stack pointer).
            for (u32 row = 0; row < 32; row += 8) {
                n = std::snprintf(buf, sizeof buf, "  $%u..$%u = %08x %08x %08x %08x %08x %08x %08x %08x\n", row, row + 7,
                    spu->gpr[row].u32[3], spu->gpr[row + 1].u32[3], spu->gpr[row + 2].u32[3], spu->gpr[row + 3].u32[3],
                    spu->gpr[row + 4].u32[3], spu->gpr[row + 5].u32[3], spu->gpr[row + 6].u32[3], spu->gpr[row + 7].u32[3]);
                write_err(buf, std::min<int>(n, sizeof buf - 1));
            }
        }
    }
    if (guest) dump_guest_memory();
    // What every thread was doing. Best effort, like the call site: it locks and allocates, so a watchdog ends the
    // process if the faulting thread held a lock the report needs.
    if (lv2::Kernel* kernel = g_kernel.load(); guest && kernel) {
        g_fault_signal = sig;
        signal(SIGALRM, on_report_timeout);
        alarm(5);
        const std::string report =
            thread_report(*kernel, std::format("guest fault in PPU thread {:#x}", t ? t->id : 0));
        write_err(report.data(), static_cast<int>(report.size()));
    }
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
    std::string bdvd, hdd0, flash, app_home;
    double timeout_s = 30;
    double report_every_s = 0; // 0: only at the end
    bool stop_at_first = false;
    bool list_imports = false;
    std::string gpu = "null", frames_dir;
    u32 frame_every = 60;
    for (int i = 1; i < argc; i++) {
        const std::string_view a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--bdvd") { const char* v = value(); if (!v) return usage(); bdvd = v; }
        else if (a == "--hdd0") { const char* v = value(); if (!v) return usage(); hdd0 = v; }
        else if (a == "--flash") { const char* v = value(); if (!v) return usage(); flash = v; }
        else if (a == "--app-home") { const char* v = value(); if (!v) return usage(); app_home = v; }
        else if (a == "--timeout") { const char* v = value(); if (!v) return usage(); timeout_s = std::atof(v); }
        else if (a == "--report-every") {
            const char* v = value();
            if (!v || std::atof(v) <= 0) return usage();
            report_every_s = std::atof(v);
        }
        else if (a == "--gpu") {
            const char* v = value();
            if (!v || (std::string_view(v) != "null" && std::string_view(v) != "metal")) return usage();
            gpu = v;
        } else if (a == "--frames") {
            const char* v = value();
            if (!v) return usage();
            frames_dir = v;
            gpu = "metal";
        } else if (a == "--frame-every") {
            const char* v = value();
            if (!v || std::atoi(v) <= 0) return usage();
            frame_every = static_cast<u32>(std::atoi(v));
        }
        else if (a == "--dump-on-fault") { const char* v = value(); if (!v) return usage(); g_dump_path = v; }
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

    // The GPU backend sys_rsx gives the RSX (the null backend unless --gpu metal). The write tracker outlives the
    // kernel, which owns the backend.
    std::atomic<u32> frames_written{0};
#if defined(PSPOCKET_HAVE_METAL)
    std::unique_ptr<rsx::WriteTracker> tracker;
    if (gpu == "metal") {
        if (!frames_dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(frames_dir, ec);
            if (ec) {
                std::fprintf(stderr, "pspocket-boot: cannot create %s: %s\n", frames_dir.c_str(), ec.message().c_str());
                return 73;
            }
        }
        tracker = std::make_unique<rsx::WriteTracker>(memory);
        cfg.rsx_backend = [&]() -> std::unique_ptr<rsx::Backend> {
            rsx::metal::MetalBackendConfig mc;
            if (!frames_dir.empty()) {
                mc.capture_every = frame_every;
                mc.capture = [&](const u8* bgra, u32 w, u32 h, u64 flip) {
                    const std::string path = std::format("{}/frame-{:06}.png", frames_dir, flip);
                    if (write_png_rgb(path, bgra, w, h, w * 4, true) && frames_written.fetch_add(1) == 0) {
                        PS_LOG_INFO("boot", "first frame saved: {}", path);
                    }
                };
            }
            auto backend = std::make_unique<rsx::metal::MetalBackend>(memory, *tracker, std::move(mc));
            if (!backend->valid()) return nullptr; // no Metal device: the null backend
            return backend;
        };
    }
#else
    if (gpu == "metal") {
        std::fprintf(stderr, "pspocket-boot: this build has no Metal backend (--gpu metal, --frames)\n");
        return 64;
    }
#endif

    lv2::Kernel kernel(memory, cfg);
    kernel.install();
    g_kernel = &kernel;
    struct ForgetKernel {
        ~ForgetKernel() { g_kernel = nullptr; } // before `kernel` is destroyed
    } forget_kernel;
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
    if (!flash.empty() && !kernel.vfs_mount("/dev_flash", flash, true)) return 66;
    if (!app_home.empty() && !kernel.vfs_mount("/app_home", app_home, false)) return 66;

    hle::ProcessConfig process_config;
    process_config.abort_on_unimplemented = stop_at_first;
    process_config.game = std::move(game);
    hle::HleProcess hle(kernel, PS_MEM_MAIN_BASE, cfg.main_area_end, std::move(process_config));
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

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto seconds = [](double s) {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(s));
    };
    const auto deadline = start + seconds(timeout_s);
    auto next_report = start + seconds(report_every_s);
    bool done = false;
    for (;;) {
        const auto until = report_every_s > 0 ? std::min(deadline, next_report) : deadline;
        done = kernel.wait_for_exit(std::chrono::ceil<std::chrono::milliseconds>(std::max(until - Clock::now(),
            Clock::duration::zero())));
        const auto now = Clock::now();
        if (done || now >= deadline) break;
        if (report_every_s > 0 && now >= next_report) {
            const double elapsed = std::chrono::duration<double>(now - start).count();
            print_thread_report(kernel, std::format("after {:.1f} s", elapsed));
            next_report += seconds(report_every_s);
        }
    }
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();

    // At a timeout, what every thread waits for (the summary stays the last thing printed).
    if (!done) print_thread_report(kernel, std::format("timeout after {:.1f} s", secs));

    // Summary: which gaps the run hit, most-called first.
    std::vector<const hle::LinkedFunction*> missing;
    for (const auto* f : hle.linked())
        if (!f->impl && f->calls.load() > 0) missing.push_back(f);
    std::sort(missing.begin(), missing.end(), [](auto* a, auto* b) { return a->calls.load() > b->calls.load(); });
    std::printf("\n%s after %.1f s\n", done ? "process exited" : "timeout", secs);
    if (done) std::printf("exit status: %d\n", kernel.exit_status());
    std::printf("imports: %u functions (%u implemented), %u unresolved variables\n", report.functions_linked,
        report.functions_implemented, report.variables_unresolved);
    if (!frames_dir.empty()) std::printf("frames saved: %u in %s\n", frames_written.load(), frames_dir.c_str());
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
