// pspocket-boot: runs a PS3 executable under the PPU interpreter on the host, with lv2 and the HLE modules, until it
// exits, a timeout passes or (with --stop-at-first) it reaches the first unimplemented syscall or firmware function.
// This is the Phase 2 bring-up tool (plan section 10): the log names every gap the game hits, in order.
//
//   pspocket-boot <EBOOT.elf> [--bdvd DIR|IMAGE.iso] [--hdd0 DIR] [--flash DIR] [--app-home DIR]
//                 [--timeout SECONDS] [--report-every SECONDS] [--stop-at-first]
//                 [--log trace|debug|info|warn|error] [--imports] [--gpu null|metal] [--frames DIR]
//                 [--frame-every N] [--dump-on-fault FILE] [--press BUTTON@START[+EVERY][:HOLD]]... [--audio]
//                 [--spu-dump DIR] [--trace-frame N] [--rsx-recover]
//   pspocket-boot --probe-disc IMAGE.iso
//
// --bdvd takes the game's disc as a directory or as its disc image, read in place (/dev_bdvd from the image root,
// /app_home from PS3_GAME/USRDIR unless --app-home says otherwise).
// --flash mounts DIR read-only as /dev_flash: the system files a game reads itself (GTA V's MultiStream loads its MP3
// codec, sys/external/flashMP3.pic), taken from the user's own firmware and kept out of the repository (D-067).
// --imports lists every imported function by module (implemented ones marked) and exits without running.
// --audio plays the game's sound (cellAudio's mix) on the Mac's default output, through AVAudioEngine.
// --spu-dump DIR saves every SPU program SPURS loads that has no lifted program into DIR, for scripts/lift-spu.sh
// (D-076; game data: keep it in inventory/). A build configured with -DPSPOCKET_GAME_SPU=OUT runs the programs lifted
// into OUT natively; the rest are interpreted, and the run ends with how many loads were which.
// --press presses a button of the virtual controller during the run (repeatable; pspocket/hle/pad_script.hpp):
// cross@60 holds Cross from 60 s for 0.25 s, cross@60+10 again every 10 s, start@30:1.5 holds Start for 1.5 s,
// ls-up@100:5 pushes the left stick up (forward) for 5 s (ls-/rs- up, down, left, right).
// A host fault inside guest memory (an access to an unmapped guest address) is reported with the guest address and
// the faulting PPU thread's instruction address before the process dies (CLAUDE.md rule 5).
// A run that ends at its timeout, or on such a fault, prints the thread report (lv2/report.hpp): every PPU thread's
// state, what it waits on and its guest backtrace, and the RSX's FIFO. --report-every prints it while the game runs.
// --gpu metal draws the game's frames with the Metal backend (Apple platforms; the default null backend runs the
// commands and draws nothing). --frames DIR (which implies Metal) saves every Nth presented frame (--frame-every, 60 by
// default; --frame-from N: none before frame N) as DIR/frame-NNNNNN.png: what the game shows, at the backend's output
// size. --trace-frame N (with --frames) logs every clear and draw of frame N (channel rsx.trace: targets, programs,
// textures, blending) and saves each colour target it used as DIR/trace-N/<address>-fmt<format>-<width>x<height>.png at
// its flip. --rsx-recover logs a FIFO error (with the words around it and the last jumps) and skips to put, as the app
// does, instead of stopping the run. --shader-cache DIR (with Metal) records every program and pipeline the backend
// makes in DIR and makes the ones recorded there before ahead, on background threads, while the game loads (D-114; game
// data: keep it in inventory/, scripts/ios.sh puts it in the app).
// --press-file FILE reads presses from FILE, as --press takes them (whitespace or lines apart; # starts a comment): a
// scripted walk (games/<title>/walk-*.txt). --frame-jumps compares each saved frame (every --frame-every, from
// --frame-from) with the one before as 64x36 thumbnails and counts the jumps (mean difference over 25 of 255, big over
// 60): the colour flicker of run 088 in a number, without writing a frame (--frames still saves them). --summary FILE
// writes the run as JSON when it ends (scripts/gate.sh reads it, D-149): how it ended, fps per 5 s, the Metal
// backend's report windows, the peak memory footprint, RSX failures, unimplemented calls, SPU code interpreted, frame
// jumps and every warning and error by shape (numbers as #).
// --game-library FILE runs the game's translated code from a game library (route B, D-152: pspocket-prepare makes one
// as the phone does) instead of the translation built into this program: its PPU functions and SPU programs, for the
// executable it was made from only; a library made for another runtime or executable stops the run.
// --dump-on-fault FILE writes the guest's committed memory below the RSX's local memory to FILE when a guest fault
// ends the run, for offline analysis (game data: keep it in inventory/). The file is a sequence of blocks: a
// big-endian u32 guest address, a u32 byte count, then the bytes. A fault in SPU code also writes that SPU's local
// store (256 KB) to FILE.ls.
//
// The executable must be a decrypted ELF (RPCS3's --decrypt writes one). Game files stay outside the repository.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sys/sysctl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <sys/ucontext.h>
#endif

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/hle/audio.hpp"
#include "pspocket/hle/spu_programs.hpp"
#include "pspocket/hle/input.hpp"
#include "pspocket/hle/pad_script.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/loader/disc.hpp"
#include "pspocket/loader/loader.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/report.hpp"
#include "pspocket/core/png.hpp"
#include "pspocket/core/thread_report.hpp"
#include "pspocket/game/host.hpp"
#if defined(PSPOCKET_HAVE_AUDIO_OUTPUT)
#include "pspocket/platform/audio_output.hpp"
#endif
#if defined(PSPOCKET_HAVE_METAL)
#include "pspocket/rsx/metal/metal_backend.hpp"
#include "pspocket/rsx/write_tracker.hpp"
#endif
#include "pspocket/ppu/disasm.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/rsx/rsx.hpp"
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
        "                     [--frame-every N] [--frame-from N] [--dump-on-fault FILE]\n"
        "                     [--press BUTTON@START[+EVERY][:HOLD]]...\n"
        "                     [--audio] [--spu-dump DIR] [--trace-frame N] [--rsx-recover] [--shader-cache DIR]\n"
        "                     [--press-file FILE] [--frame-jumps] [--summary FILE.json] [--game-library FILE]\n"
        "       pspocket-boot --probe-disc IMAGE.iso\n");
    return 64;
}

#if defined(PSPOCKET_HAVE_GAME_SPU)
extern "C" void pspocket_game_spu_register(void); // the game's lifted SPU programs (scripts/lift-spu.sh, D-076)
#endif
#if defined(PSPOCKET_HAVE_GAME_PPU)
extern "C" {
void game_ppu_register(void); // the game's PPU code translated ahead of time (ppu-lift, D-092)
extern const uint32_t game_ppu_image_words[];  // (address, word) pairs of the image it was translated from
extern const uint32_t game_ppu_image_word_count;
}
#endif

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
// The host code that faulted and its callers, for translated code (cia is stale there): the program counter, the link
// register and the frame-pointer chain, with the image slide for "atos -o pspocket-boot -s SLIDE ADDR...".
void report_host_frames(void* uctx)
{
#if defined(__APPLE__) && defined(__aarch64__)
    const auto* uc = static_cast<const ucontext_t*>(uctx);
    if (!uc || !uc->uc_mcontext) return;
    const auto& ss = uc->uc_mcontext->__ss;
    char buf[160];
    int n = std::snprintf(buf, sizeof buf, "  host frames (slide 0x%llx): 0x%llx 0x%llx",
        static_cast<unsigned long long>(_dyld_get_image_vmaddr_slide(0)), static_cast<unsigned long long>(ss.__pc),
        static_cast<unsigned long long>(ss.__lr));
    write_err(buf, std::min<int>(n, sizeof buf - 1));
    const auto lo = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self())) -
        pthread_get_stacksize_np(pthread_self());
    const auto hi = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
    uintptr_t fp = ss.__fp;
    for (int i = 0; i < 48 && fp >= lo && fp + 16 <= hi && (fp & 15) == 0; i++) {
        const auto* frame = reinterpret_cast<const uintptr_t*>(fp);
        n = std::snprintf(buf, sizeof buf, " 0x%llx", static_cast<unsigned long long>(frame[1]));
        write_err(buf, std::min<int>(n, sizeof buf - 1));
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
    write_err("\n", 1);
#else
    (void)uctx;
#endif
}

void on_fault(int sig, siginfo_t* info, void* uctx)
{
#if defined(PSPOCKET_HAVE_METAL)
    // A store to a page the texture cache's write tracker made read-only (D-134): resolved, the store runs again.
    if (ps_rsx_handle_write_fault(info->si_addr)) return;
#endif
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
    report_host_frames(uctx);
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

bool debugger_attached()
{
    kinfo_proc info{};
    size_t size = sizeof info;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    return sysctl(mib, 4, &info, &size, nullptr, 0) == 0 && (info.kp_proc.p_flag & P_TRACED) != 0;
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

// The memory the system counts against the process (the iOS app's status line has it too, D-145), in MB; 0 elsewhere.
double footprint_mb()
{
#if defined(__APPLE__)
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) == KERN_SUCCESS)
        return static_cast<double>(vm.phys_footprint) / 1048576.0;
#endif
    return 0;
}

// A log message with its numbers taken out ("#" for each run starting with a digit, hex and decimals included):
// warnings and errors of one kind count as one in the summary, whatever their addresses (D-149).
std::string message_shape(std::string_view channel, std::string_view message)
{
    std::string out(channel);
    out += ": ";
    for (usize i = 0; i < message.size() && out.size() < 200;) {
        const char c = message[i];
        if (c >= '0' && c <= '9') {
            while (i < message.size() && (std::isxdigit(static_cast<unsigned char>(message[i])) || message[i] == 'x' ||
                                          message[i] == 'X' || message[i] == '.'))
                i++;
            out += '#';
        } else {
            out += c;
            i++;
        }
    }
    return out;
}

std::string json_string(std::string_view v)
{
    std::string out = "\"";
    for (const char c : v) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
        } else {
            out += c;
        }
    }
    return out + "\"";
}

// What --summary writes, gathered while the game runs (D-149).
struct RunSummary {
    std::mutex mutex;
    std::vector<double> fps; // per 5 s window
    double peak_footprint_mb = 0;
    std::map<std::string, u64> messages; // warnings and errors by shape
#if defined(PSPOCKET_HAVE_METAL)
    std::vector<rsx::metal::WindowStats> windows;
#endif
    // --frame-jumps: each compared frame's 64x36 thumbnail against the one before.
    u64 frames_compared = 0, jumps = 0, big_jumps = 0;
    std::vector<std::pair<u64, double>> jump_at; // the first jumps: flip, mean difference
    std::vector<int> previous;                   // the last thumbnail, B G R per pixel
};
RunSummary g_summary;

// The log on stderr as the default sink writes it, the warnings and errors also counted by shape.
void summary_sink(void*, log::Level level, std::string_view channel, std::string_view message)
{
    std::fprintf(stderr, "[%.*s] %.*s: %.*s\n", static_cast<int>(log::level_name(level).size()),
        log::level_name(level).data(), static_cast<int>(channel.size()), channel.data(), static_cast<int>(message.size()),
        message.data());
    if (level < log::Level::Warn) return;
    std::string shape = message_shape(channel, message);
    std::lock_guard lock(g_summary.mutex);
    g_summary.messages[std::move(shape)]++;
}

// A frame for --frame-jumps: its mean difference from the frame before, 64x36 box-filtered, over 25 a jump (run 088).
void compare_frame(const u8* bgra, u32 w, u32 h, u64 flip)
{
    constexpr u32 kW = 64, kH = 36;
    std::vector<int> thumb(kW * kH * 3);
    for (u32 ty = 0; ty < kH; ty++) {
        const u32 y0 = ty * h / kH, y1 = std::max(y0 + 1, (ty + 1) * h / kH);
        for (u32 tx = 0; tx < kW; tx++) {
            const u32 x0 = tx * w / kW, x1 = std::max(x0 + 1, (tx + 1) * w / kW);
            u64 sum[3] = {};
            for (u32 y = y0; y < y1; y++) {
                const u8* row = bgra + (static_cast<usize>(y) * w + x0) * 4;
                for (u32 x = x0; x < x1; x++, row += 4) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                }
            }
            const u64 n = u64{y1 - y0} * (x1 - x0);
            for (u32 c = 0; c < 3; c++) thumb[(ty * kW + tx) * 3 + c] = static_cast<int>(sum[c] / n);
        }
    }
    std::lock_guard lock(g_summary.mutex);
    if (!g_summary.previous.empty()) {
        u64 total = 0;
        for (usize i = 0; i < thumb.size(); i++) total += static_cast<u64>(std::abs(thumb[i] - g_summary.previous[i]));
        const double diff = static_cast<double>(total) / static_cast<double>(thumb.size());
        g_summary.frames_compared++;
        if (diff > 25) {
            g_summary.jumps++;
            if (diff > 60) g_summary.big_jumps++;
            if (g_summary.jump_at.size() < 20) g_summary.jump_at.emplace_back(flip, diff);
        }
    }
    g_summary.previous = std::move(thumb);
}

// Saves --frames on threads of its own: compressing a PNG takes about as long as a frame, and the capture callback
// runs on the GPU's completion thread, where it would hold up the frames after it (and the game's pace with them).
class FrameWriter {
public:
    explicit FrameWriter(u32 threads)
    {
        for (u32 i = 0; i < threads; i++) threads_.emplace_back([this] { run(); });
    }
    ~FrameWriter() { finish(); }
    void push(std::string path, const u8* bgra, u32 w, u32 h)
    {
        std::vector<u8> pixels(bgra, bgra + static_cast<usize>(w) * h * 4);
        {
            std::lock_guard lock(mutex_);
            if (done_) return;
            queue_.push_back({std::move(path), std::move(pixels), w, h});
        }
        cv_.notify_one();
    }
    // Writes what is queued, then stops.
    void finish()
    {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
        for (std::thread& t : threads_) {
            if (t.joinable()) t.join();
        }
    }
    u32 written() const { return written_.load(); }

private:
    struct Job {
        std::string path;
        std::vector<u8> pixels;
        u32 w, h;
    };
    void run()
    {
        std::unique_lock lock(mutex_);
        while (true) {
            cv_.wait(lock, [this] { return done_ || !queue_.empty(); });
            if (queue_.empty()) return; // finished, and nothing left
            Job j = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            if (write_png_rgb(j.path, j.pixels.data(), j.w, j.h, j.w * 4, true) && written_.fetch_add(1) == 0)
                PS_LOG_INFO("boot", "first frame saved: {}", j.path);
            lock.lock();
        }
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    bool done_ = false;
    std::atomic<u32> written_{0};
    std::vector<std::thread> threads_; // last: started once the rest exists
};

// --summary: the run as JSON (D-149), for scripts/gate.sh.
bool write_summary(const std::string& path, bool exited, int status, double seconds, lv2::Kernel& kernel,
    usize unimplemented_functions, const hle::SpuProgramStats& spu)
{
    std::lock_guard lock(g_summary.mutex);
    std::string j = "{\n";
    j += std::format("  \"exit\": {},\n  \"exit_status\": {},\n  \"seconds\": {:.1f},\n",
        json_string(exited ? "exited" : "timeout"), status, seconds);
    rsx::Rsx* r = kernel.rsx();
    j += std::format("  \"flips\": {},\n  \"rsx_failures\": {},\n", r ? r->status().flips : 0, r ? r->failure_count() : 0);
    j += "  \"fps\": [";
    for (usize i = 0; i < g_summary.fps.size(); i++) j += std::format("{}{:.1f}", i ? ", " : "", g_summary.fps[i]);
    j += std::format("],\n  \"footprint_peak_mb\": {:.0f},\n", g_summary.peak_footprint_mb);
    j += std::format("  \"unimplemented_syscalls\": {},\n  \"unimplemented_functions\": {},\n",
        kernel.unimplemented_count(), unimplemented_functions);
    j += std::format("  \"spu_loads\": {{\"lifted\": {}, \"interpreted\": {}, \"images_without_lifted\": {}}},\n",
        spu.attached, spu.unknown, spu.images);
    j += std::format("  \"frame_jumps\": {{\"compared\": {}, \"jumps\": {}, \"big\": {}, \"at\": [",
        g_summary.frames_compared, g_summary.jumps, g_summary.big_jumps);
    for (usize i = 0; i < g_summary.jump_at.size(); i++)
        j += std::format("{}[{}, {:.1f}]", i ? ", " : "", g_summary.jump_at[i].first, g_summary.jump_at[i].second);
    j += "]},\n  \"gpu_windows\": [";
#if defined(PSPOCKET_HAVE_METAL)
    for (usize i = 0; i < g_summary.windows.size(); i++) {
        const rsx::metal::WindowStats& w = g_summary.windows[i];
        std::string ends;
        for (usize k = 0; k < w.pass_ends.size(); k++)
            ends += std::format("{}{}: {:.2f}", k ? ", " : "", json_string(rsx::metal::kPassEndNames[k]), w.pass_ends[k]);
        j += std::format("{}\n    {{\"last_flip\": {}, \"gpu_ms\": {:.2f}, \"draws\": {:.0f}, \"render_passes\": {:.1f}, "
                         "\"pass_ends\": {{{}}}, \"readbacks\": {:.2f}, \"readback_wait_ms\": {:.2f}, "
                         "\"texture_checks\": {:.0f}, \"write_faults\": {:.1f}, \"feedback_copies\": {:.2f}, "
                         "\"corner_textures\": {:.2f}, \"surface_reloads\": {:.2f}, \"gpu_wait_ms\": {:.2f}, "
                         "\"programs_made\": {}, \"pipelines_made\": {}, \"compile_ms\": {:.1f}, \"textures\": {}, "
                         "\"surfaces\": {}, \"gpu_allocated_mb\": {:.0f}, \"deferred_queue\": {}}}",
            i ? "," : "", w.last_flip, w.gpu_ms, w.draws, w.render_passes, ends, w.readbacks, w.readback_wait_ms,
            w.texture_checks, w.write_faults, w.feedback_copies, w.corner_textures, w.surface_reloads, w.gpu_wait_ms,
            w.programs_made, w.pipelines_made, w.compile_ms, w.textures, w.surfaces, w.gpu_allocated_mb, w.deferred_queue);
    }
#endif
    j += "\n  ],\n  \"messages\": {";
    bool first = true;
    for (const auto& [shape, count] : g_summary.messages) {
        j += std::format("{}\n    {}: {}", first ? "" : ",", json_string(shape), count);
        first = false;
    }
    j += "\n  }\n}\n";
    std::ofstream out(path, std::ios::trunc);
    out << j;
    return static_cast<bool>(out);
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
    std::string shader_cache; // --shader-cache (D-114)
    u32 frame_every = 60;
    u64 frame_from = 0;
    u64 trace_frame = 0;
    bool rsx_recover = false;
    std::vector<hle::PadPress> presses;
    bool audio = false;
    std::string spu_dump;
    std::string game_library; // --game-library (D-152)
    bool frame_jumps = false;
    std::string summary_path;
    const auto add_press = [&](std::string_view spec) {
        std::string error;
        const auto press = hle::parse_pad_press(spec, &error);
        if (!press) {
            std::fprintf(stderr, "pspocket-boot: %s\n", error.c_str());
            return false;
        }
        presses.push_back(*press);
        return true;
    };
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
        } else if (a == "--frame-from") {
            const char* v = value();
            if (!v || std::atoll(v) < 0) return usage();
            frame_from = static_cast<u64>(std::atoll(v));
        } else if (a == "--rsx-recover") {
            rsx_recover = true;
        } else if (a == "--shader-cache") {
            const char* v = value();
            if (!v) return usage();
            shader_cache = v;
        } else if (a == "--trace-frame") {
            const char* v = value();
            if (!v || std::atoll(v) <= 0) return usage();
            trace_frame = static_cast<u64>(std::atoll(v));
        }
        else if (a == "--dump-on-fault") { const char* v = value(); if (!v) return usage(); g_dump_path = v; }
        else if (a == "--press") {
            const char* v = value();
            if (!v) return usage();
            if (!add_press(v)) return 64;
        } else if (a == "--press-file") {
            const char* v = value();
            if (!v) return usage();
            std::ifstream in(v);
            if (!in) {
                std::fprintf(stderr, "pspocket-boot: cannot read %s\n", v);
                return 66;
            }
            for (std::string line; std::getline(in, line);) {
                if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
                std::istringstream words(line);
                for (std::string w; words >> w;)
                    if (!add_press(w)) return 64;
            }
        } else if (a == "--frame-jumps") {
            frame_jumps = true;
            gpu = "metal";
        } else if (a == "--summary") {
            const char* v = value();
            if (!v) return usage();
            summary_path = v;
        }
        else if (a == "--audio") audio = true;
        else if (a == "--spu-dump") { const char* v = value(); if (!v) return usage(); spu_dump = v; }
        else if (a == "--game-library") { const char* v = value(); if (!v) return usage(); game_library = v; }
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
    if (!summary_path.empty()) log::set_sink(summary_sink, nullptr); // warnings and errors counted by shape

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

    // Guest code the build has no translation for (a build without -DPSPOCKET_GAME_PPU: all of it) runs in the
    // interpreter, which enters translated functions where it reaches them (D-092).
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
    cfg.rsx_log_failures = rsx_recover;

    // The GPU backend sys_rsx gives the RSX (the null backend unless --gpu metal). The write tracker outlives the
    // kernel, which owns the backend.
    FrameWriter frame_writer(frames_dir.empty() ? 0 : 3);
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
        // Textures are watched by page protection (D-134): on_fault resolves the tracker's faults. Not under a
        // debugger, nor with PSPOCKET_RSX_HASH_TEXTURES set (the hashing of before).
        if (!std::getenv("PSPOCKET_RSX_HASH_TEXTURES") && !debugger_attached()) {
            tracker->install();
            tracker->set_protection(true);
        }
        cfg.rsx_backend = [&]() -> std::unique_ptr<rsx::Backend> {
            rsx::metal::MetalBackendConfig mc;
            mc.shader_cache = shader_cache;
            if (!summary_path.empty()) {
                mc.on_window = [](const rsx::metal::WindowStats& w) {
                    std::lock_guard lock(g_summary.mutex);
                    g_summary.windows.push_back(w);
                };
            }
            if (!frames_dir.empty() || frame_jumps) {
                mc.capture_every = frame_every;
                mc.capture = [&](const u8* bgra, u32 w, u32 h, u64 flip) {
                    if (flip < frame_from) return;
                    if (frame_jumps) compare_frame(bgra, w, h, flip);
                    if (!frames_dir.empty())
                        frame_writer.push(std::format("{}/frame-{:06}.png", frames_dir, flip), bgra, w, h);
                };
            }
            if (!frames_dir.empty()) {
                if (trace_frame) {
                    mc.trace_frame = trace_frame;
                    mc.trace_surface = [&frames_dir, trace_frame](const std::string& name, const u8* bgra, u32 w, u32 h) {
                        const std::string dir = std::format("{}/trace-{}", frames_dir, trace_frame);
                        std::error_code ec;
                        std::filesystem::create_directories(dir, ec);
                        write_png_rgb(dir + "/" + name + ".png", bgra, w, h, w * 4, true);
                    };
                }
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
    // The game's SPU programs (D-076): the lifted ones run natively; the others, and code a program reaches outside
    // its lifted range (the SPURS syscall stubs, overlays), are interpreted, in every build, until they are collected.
    ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
#if defined(PSPOCKET_HAVE_GAME_SPU)
    if (game_library.empty()) { // a game library brings its own (registered after the executable is loaded)
        pspocket_game_spu_register();
        hle::enable_spu_modules();
    }
#endif
    if (!spu_dump.empty()) {
        hle::set_spu_dump_dir(spu_dump);
        hle::enable_spu_modules();
    }
#if defined(PSPOCKET_HAVE_AUDIO_OUTPUT)
    std::unique_ptr<platform::AudioOutput> audio_output;
    if (audio) {
        audio_output = std::make_unique<platform::AudioOutput>();
        platform::AudioOutput* out = audio_output.get();
        hle::set_audio_sink(
            hle, [out](const f32* samples, u32 frames) { out->push(samples, frames); },
            [out] { return hle::AudioBufferLevel{out->queued(), out->device_frames()}; },
            [out](bool on, f32 tempo) { out->set_stretch(on, tempo); });
    }
    struct DetachAudio {
        hle::HleProcess& hle;
        ~DetachAudio() { hle::set_audio_sink(hle, {}); } // before the output is destroyed
    } detach_audio{hle};
#else
    if (audio) {
        std::fprintf(stderr, "pspocket-boot: this build has no audio output (--audio)\n");
        return 64;
    }
#endif

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
    bool translated = false; // the game's PPU code runs translated (built in or from a library)
    if (!game_library.empty()) {
        // The game's translated code from a game library (route B, D-152), checked against this runtime and this
        // executable before anything in it runs.
        game::LoadedLibrary loaded;
        std::string error;
        if (!game::load_library(game_library, memory, &loaded, &error)) {
            std::fprintf(stderr, "pspocket-boot: %s\n", error.c_str());
            return 70;
        }
        if (loaded.library->spu_register) hle::enable_spu_modules();
        translated = loaded.library->ppu_function_count > 0;
    }
#if defined(PSPOCKET_HAVE_GAME_PPU)
    // The game's PPU code translated ahead of time (D-092), for the executable it was translated from only (D-098):
    // branches into it from the interpreter run it. A game library replaces it.
    if (game_library.empty())
        translated = loader::register_translated_ppu(memory, game_ppu_image_words, game_ppu_image_word_count,
            &game_ppu_register);
#endif
    // PSPOCKET_PPU_ONLY=LO-HI[,LO-HI...] keeps the translated functions in those ranges only, the rest interpreted:
    // bisecting a translation fault down to the function.
    if (const char* only = std::getenv("PSPOCKET_PPU_ONLY"); only && translated) {
        std::vector<std::pair<u64, u64>> ranges;
        for (const char* p = only;;) {
            char* end = nullptr;
            const u64 lo = std::strtoull(p, &end, 0);
            if (end == p) break;
            u64 hi = lo + 4;
            if (*end == '-') hi = std::strtoull(end + 1, &end, 0);
            ranges.emplace_back(lo, hi);
            if (*end != ',') break;
            p = end + 1;
        }
        u64 kept = 0;
        for (u32 i = 0; i < ps_ppu_dispatch.count; i++) {
            const u32 addr = ps_ppu_dispatch.base + i * 4;
            if (ps_ppu_dispatch.slots[i] == ps_ppu_dispatch_miss_stub) continue;
            const bool keep = std::any_of(ranges.begin(), ranges.end(), [&](auto r) { return addr >= r.first && addr < r.second; });
            if (!keep) ps_ppu_dispatch_unregister(addr);
            else kept++;
        }
        PS_LOG_INFO("boot", "PSPOCKET_PPU_ONLY={}: {} translated functions kept", only, kept);
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
    // The frame rate, every 5 s once the RSX runs (as the iOS app's log line).
    constexpr double kRateEvery = 5.0;
    auto next_rate = start + seconds(kRateEvery);
    auto rate_since = start;
    u64 rate_flips = 0;
    bool done = false;
    // Scripted presses (--press): which buttons are down is checked every 20 ms while any press is scheduled.
    std::string pad_text;
    for (;;) {
        auto until = std::min(next_rate, report_every_s > 0 ? std::min(deadline, next_report) : deadline);
        if (!presses.empty()) until = std::min(until, Clock::now() + std::chrono::milliseconds(20));
        done = kernel.wait_for_exit(std::chrono::ceil<std::chrono::milliseconds>(std::max(until - Clock::now(),
            Clock::duration::zero())));
        const auto now = Clock::now();
        if (done || now >= deadline) break;
        if (!presses.empty()) {
            const double t = std::chrono::duration<double>(now - start).count();
            std::string text; // the buttons down, which decide the controller's state
            for (const hle::PadPress& p : presses)
                if (p.down_at(t) && text.find(p.button) == std::string::npos) text += (text.empty() ? "" : "+") + p.button;
            if (text != pad_text) {
                hle::set_pad_input(hle, hle::pad_state_at(presses, t));
                PS_LOG_INFO("boot", "controller at {:.2f} s: {}", t, text.empty() ? "released" : text);
                pad_text = text;
            }
        }
        if (now >= next_rate) {
            if (rsx::Rsx* r = kernel.rsx()) {
                const u64 flips = r->status().flips;
                const double dt = std::chrono::duration<double>(now - rate_since).count();
                if (flips != rate_flips) {
                    const double fps = static_cast<double>(flips - rate_flips) / dt;
                    const double footprint = footprint_mb();
                    PS_LOG_INFO("boot", "{:.1f} fps ({} frames in {:.1f} s; {} since the boot); memory {:.2f} GB", fps,
                        flips - rate_flips, dt, flips, footprint / 1024);
                    PS_LOG_INFO("boot", "threads: {}", thread_cpu_report(dt));
                    std::lock_guard lock(g_summary.mutex);
                    g_summary.fps.push_back(fps);
                    g_summary.peak_footprint_mb = std::max(g_summary.peak_footprint_mb, footprint);
                }
                rate_flips = flips;
            }
            rate_since = now;
            next_rate = now + seconds(kRateEvery);
        }
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
    if (!frames_dir.empty()) {
        frame_writer.finish();
        std::printf("frames saved: %u in %s\n", frame_writer.written(), frames_dir.c_str());
    }
#if defined(PSPOCKET_HAVE_AUDIO_OUTPUT)
    if (audio_output) {
        const auto st = audio_output->stats();
        std::printf("sound out: %.1f s played, %llu underruns, %llu overruns\n",
            static_cast<double>(st.played) / hle::kAudioSampleRate, static_cast<unsigned long long>(st.underruns),
            static_cast<unsigned long long>(st.overruns));
    }
#endif
    const hle::SpuProgramStats spu = hle::spu_program_stats();
    std::printf("SPU program loads: %llu lifted, %llu interpreted (%llu images without a lifted program); %llu modules "
                "lifted, %llu changed before they ran\n",
        static_cast<unsigned long long>(spu.attached), static_cast<unsigned long long>(spu.unknown),
        static_cast<unsigned long long>(spu.images), static_cast<unsigned long long>(spu.modules),
        static_cast<unsigned long long>(spu.modified));
    // Where SPU code was interpreted most (the program current when the interpreter took over, and where).
    ps_spu_interp_site sites[12];
    const u32 nsites = ps_spu_interp_sites(sites, 12);
    for (u32 i = 0; i < nsites; i++) {
        std::printf("SPU interpreted: %llu instructions from %llu entries at LS 0x%05x in %s\n",
            static_cast<unsigned long long>(sites[i].steps), static_cast<unsigned long long>(sites[i].entries), sites[i].pc,
            sites[i].program ? sites[i].program->name : "no program");
    }
    for (u32 mb = 0; mb < (PS_MEM_RSX_LOCAL_SIZE >> 20); mb++) {
        const u64 got = ps_spu_video_dma(mb, 0), put = ps_spu_video_dma(mb, 1);
        if (got || put) {
            std::printf("SPU DMA in RSX local memory %#010x: %llu bytes read, %llu written\n", PS_MEM_RSX_LOCAL_BASE + (mb << 20),
                static_cast<unsigned long long>(got), static_cast<unsigned long long>(put));
        }
    }
    std::printf("unimplemented syscalls called: %llu\n", static_cast<unsigned long long>(kernel.unimplemented_count()));
    std::printf("unimplemented functions called: %zu\n", missing.size());
    for (const auto* f : missing)
        std::printf("  %8llu  %s::%s (0x%08X)\n", static_cast<unsigned long long>(f->calls.load()), f->module.c_str(),
            f->name.empty() ? "?" : f->name.c_str(), f->nid);
    if (!summary_path.empty()) {
        if (!write_summary(summary_path, done, done ? kernel.exit_status() : 0, secs, kernel, missing.size(), spu))
            std::fprintf(stderr, "pspocket-boot: cannot write %s\n", summary_path.c_str());
        else
            std::printf("summary: %s\n", summary_path.c_str());
    }

    if (!kernel.shutdown()) {
        std::fprintf(stderr, "pspocket-boot: guest threads still running at shutdown\n");
        std::fflush(stdout);
        std::_Exit(done ? 0 : 75);
    }
    return done ? 0 : 75;
}
