// The game in the iOS app (pspocket/platform/game.h): pspocket-boot's boot sequence (runtime/loader/tools/
// boot_main.cpp) with the Metal backend presenting into the app's CAMetalLayer, the log in a file and on screen, and
// the virtual controller driven by the app's buttons. Objective-C++ with ARC.

#import <AVFoundation/AVFoundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include "pspocket/platform/game.h"

#include <mach/mach.h>
#include <os/proc.h>
#include <signal.h>

#include <thread>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/core/mmio.hpp"
#include "pspocket/core/png.hpp"
#include "pspocket/core/thread_report.hpp"
#include "pspocket/game/host.hpp"
#include "pspocket/hle/audio.hpp"
#include "pspocket/hle/input.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/report.hpp"
#include "pspocket/hle/spu_programs.hpp"
#include "pspocket/loader/disc.hpp"
#include "pspocket/loader/game_modules.hpp"
#include "pspocket/loader/hdd_game.hpp"
#include "pspocket/loader/lle.hpp"
#include "pspocket/loader/loader.hpp"
#include "pspocket/loader/system.hpp"
#include "pspocket/recomp/firmware.hpp"
#include "pspocket/recomp/hash.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/platform/audio_output.hpp"
#include "pspocket/platform/ios.h"
#include "pspocket/ppu/interpreter.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/rsx/metal/metal_backend.hpp"
#include "pspocket/rsx/write_tracker.hpp"
#include "pspocket/spu/runtime.h"
#include "session_translator.hpp"

using namespace pspocket;

#if PSPOCKET_HAVE_GAME_PPU
extern "C" {
void game_ppu_register(void); // the game's PPU code translated ahead of time (ppu-lift, D-092)
extern const uint32_t game_ppu_image_words[];
extern const uint32_t game_ppu_image_word_count;
}
#endif
#if PSPOCKET_HAVE_GAME_SPU
extern "C" void pspocket_game_spu_register(void); // the game's lifted SPU programs (scripts/lift-spu.sh, D-076)
#endif

namespace {

constexpr std::string_view kChan = "game";
constexpr size_t kTailLines = 200;

// Everything a running game needs, alive until the process exits.
struct Session {
    std::shared_ptr<loader::Executable> exe;
    std::unique_ptr<GuestMemory> memory;
    std::unique_ptr<rsx::WriteTracker> tracker;
    std::unique_ptr<lv2::Kernel> kernel;
    std::unique_ptr<hle::HleProcess> hle;
    std::unique_ptr<loader::LleModules> lle; // the firmware modules it runs (D-157)
    std::unique_ptr<loader::SystemLibrary> system; // their translation (D-178)
    std::unique_ptr<loader::RuntimeModules> modules; // the modules it loads while it runs (D-159)
    std::unique_ptr<platform::AudioOutput> audio; // the game's sound (cellAudio's mix) on the phone's output
    loader::BootState state;
    std::chrono::steady_clock::time_point start;
    std::atomic<bool> running{false};
    std::atomic<u64> flips{0};
    CAMetalLayer* layer = nil;
    // The frame rate line in the log (written from the present callback, on the RSX thread only).
    std::chrono::steady_clock::time_point rate_since;
    u64 rate_flips = 0;
    u64 rate_underruns = 0; // the speaker's, at the last line
    u64 rate_ppu_interpreted = 0, rate_spu_interpreted = 0; // instructions, at the last line (D-228)
    // The disc's title ID, which keeps each game's shader cache and coverage apart (D-118), and whether the build's
    // translated PPU code is this game's: the shaders recorded on the Mac for it come with it.
    std::string title_id;
    bool translated = false;
    // The game's Metal backend, once the RSX made it: asked to trim when memory runs low (D-148).
    std::atomic<rsx::metal::MetalBackend*> backend{nullptr};
    // SPU programs translated while it runs (D-236), when the app carries the compiler.
    std::shared_ptr<prepare::SessionTranslator> spu_translator;
    // Its own PRX modules translated ahead of time (D-238): after `modules`, which it is installed on.
    std::unique_ptr<loader::GameModules> game_modules;
    std::string coverage; // where the SPU images it meets are saved
    // The app's Documents folder (the log's), where a frame trace is asked for and kept (D-947): an empty file
    // Documents/pspocket-trace, put there over USB, has the backend trace the next frame into Documents/trace-<flip>/.
    std::string documents;
    std::string trace_dir;
};

constexpr auto kRateInterval = std::chrono::seconds(5);

Session* g_session = nullptr; // one game per process, never destroyed

// ---- the log: a file and the last lines for the screen ----

std::mutex g_log_mutex;
int g_log_fd = -1;
std::deque<std::string> g_tail;

void on_log(void*, ps_ios_log_level level, const char* channel, const char* message)
{
    static constexpr const char* kLevel[] = {"trace", "debug", "info", "warn", "error", "fatal"};
    std::string line = std::string("[") + kLevel[static_cast<unsigned>(level) % 6] + "] " + channel + ": " + message;
    std::lock_guard lock(g_log_mutex);
    if (g_log_fd >= 0) {
        const std::string out = line + "\n";
        (void)!write(g_log_fd, out.data(), out.size());
    }
    g_tail.push_back(std::move(line));
    while (g_tail.size() > kTailLines) g_tail.pop_front();
}

// ---- faults: one line in the log, then the default action (the system's crash report) ----

void on_fault(int sig, siginfo_t* info, void* uctx)
{
    // A store to a page the texture cache's write tracker made read-only (D-134): resolved, the store runs again.
    if (ps_rsx_handle_write_fault(info->si_addr)) return;
    // A raw SPU's problem-state register (D-528): the load or store is performed on it, the thread goes on.
    if (pspocket::mmio_handle_fault(info->si_addr, uctx)) return;
    char buf[320];
    const auto addr = reinterpret_cast<uintptr_t>(info->si_addr);
    const auto base = g_session && g_session->memory ? reinterpret_cast<uintptr_t>(g_session->memory->base()) : 0;
    const lv2::PpuThread* t = lv2::current_thread();
    const SPUContext* spu = ps_spu_current();
    int n;
    if (base && addr >= base && addr < base + PS_GUEST_SPACE_SIZE) {
        n = std::snprintf(buf, sizeof buf, "[fatal] game: %s: guest access to 0x%08llx by %s 0x%x at 0x%05x\n",
            sig == SIGBUS ? "SIGBUS" : "SIGSEGV", static_cast<unsigned long long>(addr - base),
            t ? "PPU thread" : spu ? "SPU code, pc" : "host thread", t ? t->id : 0u,
            t && t->ctx ? t->ctx->cia : spu ? spu->pc : 0u);
    } else {
        n = std::snprintf(buf, sizeof buf, "[fatal] game: %s at host address %p\n", sig == SIGBUS ? "SIGBUS" : "SIGSEGV",
            info->si_addr);
    }
    if (g_log_fd >= 0 && n > 0) {
        (void)!write(g_log_fd, buf, static_cast<size_t>(std::min<int>(n, sizeof buf - 1)));
        fsync(g_log_fd);
    }
    signal(sig, SIG_DFL); // returning re-runs the access, which now crashes with a system crash report
}

// A frozen game: frames came, then none for 10 s. What every PPU thread and the RSX's FIFO wait on goes to the log,
// once per freeze (D-135): the phone froze with nothing logged, and gone before a debugger could attach.
void watch_for_freezes(Session* s)
{
    std::thread([s] {
        constexpr auto kQuiet = std::chrono::seconds(10);
        u64 seen = 0;
        auto since = std::chrono::steady_clock::now();
        bool reported = false;
        u32 ticks = 0;
        while (s->running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            // Every minute: where the PPU interpreter ran code, for the next preparation's continuations (D-255).
            if (++ticks % 30 == 0 && !s->coverage.empty()) {
                if (const u32 added = ppu::save_landings(s->coverage + "/ppu-landings.txt"))
                    PS_LOG_INFO(kChan, "{} new PPU landings saved: the next preparation translates them (D-255)", added);
            }
            const u64 flips = s->flips.load(std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now();
            if (flips != seen) {
                seen = flips;
                since = now;
                reported = false;
                continue;
            }
            if (!flips || reported || now - since < kQuiet) continue;
            reported = true;
            lv2::ReportOptions options;
            options.title = std::format("no frame for {:.0f} s after frame {}",
                std::chrono::duration<double>(now - since).count(), flips);
            options.describe_address = [s](u32 addr) -> std::string {
                const hle::LinkedFunction* f = s->hle ? s->hle->linked_at(addr) : nullptr;
                if (!f) return {};
                return f->module + "::" + (f->name.empty() ? std::format("{:#010x}", f->nid) : f->name);
            };
            const std::string report = lv2::format_report(*s->kernel, options);
            // Line by line: the system log cuts long messages.
            usize at = 0;
            while (at < report.size()) {
                const usize end = report.find('\n', at);
                const usize n = (end == std::string::npos ? report.size() : end) - at;
                if (n) PS_LOG_WARN(kChan, "freeze: {}", std::string_view(report).substr(at, n));
                if (end == std::string::npos) break;
                at = end + 1;
            }
        }
    }).detach();
}

bool debugger_attached()
{
    kinfo_proc info{};
    size_t size = sizeof info;
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    return sysctl(mib, 4, &info, &size, nullptr, 0) == 0 && (info.kp_proc.p_flag & P_TRACED) != 0;
}

void install_fault_handler()
{
    struct sigaction sa{};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

// Every instruction an interpreter has run so far, from its per-site counts (D-207).
template <typename Site>
u64 interpreted_total(uint32_t (*sites_of)(Site*, uint32_t))
{
    std::vector<Site> sites(4096);
    const u32 n = sites_of(sites.data(), static_cast<u32>(sites.size()));
    u64 total = 0;
    for (u32 i = 0; i < n; i++) total += sites[i].steps;
    return total;
}

int fail(char* error, size_t size, const std::string& message)
{
    if (error && size) std::snprintf(error, size, "%s", message.c_str());
    PS_LOG_ERROR(kChan, "{}", message);
    return 1;
}

bool read_file(const char* path, std::vector<u8>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// The Mac's PSPOCKET_* switches for a run on the phone (D-947): Documents/pspocket-env.txt, a NAME=VALUE a line ('#'
// starts a comment), put there over USB (afcclient). Only PSPOCKET_ names; each one set is logged.
void set_env_from_file(const std::filesystem::path& path)
{
    std::ifstream f(path);
    if (!f) return;
    for (std::string line; std::getline(f, line);) {
        if (const usize hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        const usize eq = line.find('=');
        if (eq == std::string::npos || !line.starts_with("PSPOCKET_")) continue;
        const std::string name = line.substr(0, eq), value = line.substr(eq + 1);
        setenv(name.c_str(), value.c_str(), 1);
        PS_LOG_INFO(kChan, "{}={} (from {})", name, value, path.filename().string());
    }
}

} // namespace

extern "C" int ps_game_boot(const ps_game_options* o, char* error, size_t error_size)
{
    if (g_session) return fail(error, error_size, "a game is already running; quit the app to start another");
    if (!o || !o->elf_path || !o->hdd0_dir || (!o->iso_path && !o->game_dir)) {
        return fail(error, error_size, "missing paths");
    }

    if (o->log_path) {
        // The last boot's log stays beside it (game-prev.log): a game iOS stopped is looked at after the next launch.
        std::string previous = o->log_path;
        if (previous.ends_with(".log")) previous.resize(previous.size() - 4);
        std::rename(o->log_path, (previous + "-prev.log").c_str());
        std::lock_guard lock(g_log_mutex);
        g_log_fd = open(o->log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    ps_ios_install_log_sink(on_log, nullptr);
    if (o->log_path) set_env_from_file(std::filesystem::path(o->log_path).parent_path() / "pspocket-env.txt");

    std::vector<u8> bytes;
    if (!read_file(o->elf_path, bytes)) return fail(error, error_size, std::string("cannot read ") + o->elf_path);
    if (bytes.size() >= 4 && bytes[0] == 'S' && bytes[1] == 'C' && bytes[2] == 'E' && bytes[3] == 0) {
        return fail(error, error_size, "the executable is still an encrypted SELF; it has to be decrypted to an ELF");
    }
    const std::string exe_sha256 = recomp::to_hex(recomp::Sha256::hash(std::span<const u8>(bytes)));
    auto exe = loader::Executable::parse(std::move(bytes));
    if (!exe) return fail(error, error_size, "the executable: " + exe.error().message);
    // A game in /dev_hdd0/game (D-939): an installed PSN game, or a disc game's update, which runs with its disc.
    std::optional<loader::HddGame> hdd_game;
    if (o->game_dir) {
        std::string why;
        hdd_game = loader::read_hdd_game(o->hdd0_dir, o->game_dir, &why);
        if (!hdd_game) return fail(error, error_size, "the installed game: " + why);
        if (hdd_game->update() && !o->iso_path) {
            return fail(error, error_size, hdd_game->title_id + "'s update runs with its disc, and there is none");
        }
    }

    auto s = std::make_unique<Session>();
    s->exe = *exe;
    if (o->log_path) s->documents = std::filesystem::path(o->log_path).parent_path().string();
    s->memory = std::make_unique<GuestMemory>();
    if (!s->memory->reserve()) return fail(error, error_size, "cannot reserve the 4 GB guest address space");
    // PPU code the translation has not reached (or all of it, in a build without the game's translated code) is
    // interpreted.
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
    // SPU code without a lifted program, or outside it, is interpreted (D-076). The game's lifted SPU programs, when
    // the build has them (scripts/ios.sh), run its SPURS tasks, jobs and the modules they load (D-079, D-081).
    ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
    const std::string game_library = o->game_library ? o->game_library : "";
    bool spu_modules = false; // hle::enable_spu_modules registers the worker's stubs: once
    auto enable_spu_modules = [&spu_modules] {
        if (!std::exchange(spu_modules, true)) hle::enable_spu_modules();
    };

    lv2::Config cfg = s->exe->kernel_config();
    Session* raw = s.get();
    cfg.on_process_exit = [raw](s32 status) {
        PS_LOG_INFO(kChan, "the game exited, status {}", status);
        raw->running = false;
    };
    if (hdd_game) s->state.options.argv[0] = hdd_game->boot_path;
    loader::prepare_kernel_config(cfg, s->state);
    // A FIFO the RSX cannot read (a desync) is logged and skipped to put, as RPCS3 recovers (CLAUDE.md rule 5: the
    // app falls back and logs; pspocket-boot on the Mac stops there).
    cfg.rsx_log_failures = true;

    // The frames: the Metal backend renders into its output texture (1152x640, D-016), copied into the layer.
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return fail(error, error_size, "no Metal device");
    if (o->metal_layer) {
        CAMetalLayer* layer = (__bridge CAMetalLayer*)o->metal_layer;
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        layer.drawableSize = CGSizeMake(1152, 640);
        s->layer = layer;
    }
    s->tracker = std::make_unique<rsx::WriteTracker>(*s->memory);
    cfg.rsx_backend = [raw, device]() -> std::unique_ptr<rsx::Backend> {
        rsx::metal::MetalBackendConfig mc;
        mc.device = (__bridge void*)device;
        // The shader cache (D-114): what this phone compiled before for this game (D-118), and what the Mac recorded
        // for the game the app was built for (in the app, scripts/ios.sh), made ahead on background threads while the
        // game loads; the GPU code it compiles is kept in binary archives for the next launch (D-143).
        if (NSString* caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject) {
            mc.shader_cache = std::string(caches.UTF8String) + "/pspocket-shaders/" + raw->title_id;
        }
        NSString* seed = [[NSBundle mainBundle] pathForResource:@"shaders" ofType:nil];
        if (seed && raw->translated) mc.shader_seeds.push_back(seed.UTF8String);
        mc.shader_cache_threads = 0; // half the cores (D-143)
        // The compiled GPU code kept for the next launch (D-143): off. With 3, 6 and then 9 archives loaded iOS sent
        // memory warnings as they loaded, more each time, and stopped the app twice with 9 (D-145).
        mc.shader_archives = false;
        // Anything new compiles in the background; a frame waits for it 4 ms at most, then draws without it (D-158):
        // a game the phone has never seen plays from the start without stalls.
        mc.async_shaders = true;
        mc.compile_wait_ms = 4.0;
        // A frame trace asked for over USB (D-947): its targets and textures as PNG files beside its log lines.
        mc.trace_surface = [raw](const std::string& name, const u8* bgra, u32 w, u32 h) {
            if (!raw->trace_dir.empty()) write_png_rgb(raw->trace_dir + "/" + name + ".png", bgra, w, h, w * 4, true);
        };
        mc.present = [raw](const rsx::metal::PresentFrame& f) {
            const u64 flips = raw->flips.fetch_add(1, std::memory_order_relaxed) + 1;
            const auto now = std::chrono::steady_clock::now();
            if (now - raw->rate_since >= kRateInterval) {
                const double dt = std::chrono::duration<double>(now - raw->rate_since).count();
                const u64 underruns = raw->audio ? raw->audio->stats().underruns : 0;
                // iOS's heat level: from "serious" the system slows the chips down.
                static constexpr const char* kThermal[] = {"nominal", "fair", "serious", "critical"};
                const auto thermal = static_cast<usize>([[NSProcessInfo processInfo] thermalState]);
                // The memory iOS counts against the app, and what is left before it stops the app (it does so without
                // a crash report: a game that vanished at 17:06 left only "Received memory warning" in the log).
                task_vm_info_data_t vm{};
                mach_msg_type_number_t vm_count = TASK_VM_INFO_COUNT;
                const double footprint = task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm),
                                             &vm_count) == KERN_SUCCESS ? static_cast<double>(vm.phys_footprint) : 0.0;
                PS_LOG_INFO(kChan, "{:.1f} fps ({} frames in {:.1f} s; {} since the boot); the speaker ran dry {} times; "
                                   "heat {}; memory {:.2f} GB, {:.2f} GB left",
                    static_cast<double>(flips - raw->rate_flips) / dt, flips - raw->rate_flips, dt, flips,
                    underruns - raw->rate_underruns, kThermal[std::min<usize>(thermal, 3)], footprint / 1e9,
                    static_cast<double>(os_proc_available_memory()) / 1e9);
                PS_LOG_INFO(kChan, "threads: {}", thread_cpu_report(dt)); // which thread holds a slow stretch back
                // Code the translation did not cover, run by the interpreters (D-207, D-228): the goal is none.
                const u64 ppu = interpreted_total(&ps_ppu_interp_sites), spu = interpreted_total(&ps_spu_interp_sites);
                if (ppu != raw->rate_ppu_interpreted || spu != raw->rate_spu_interpreted) {
                    PS_LOG_INFO(kChan, "interpreted in {:.1f} s: {} PPU and {} SPU instructions", dt,
                        ppu - raw->rate_ppu_interpreted, spu - raw->rate_spu_interpreted);
                    raw->rate_ppu_interpreted = ppu;
                    raw->rate_spu_interpreted = spu;
                }
                // A frame trace asked for (D-947): Documents/pspocket-trace, taken away as the trace starts.
                if (!raw->documents.empty()) {
                    std::error_code ec;
                    const std::filesystem::path ask = std::filesystem::path(raw->documents) / "pspocket-trace";
                    if (std::filesystem::exists(ask, ec) && std::filesystem::remove(ask, ec)) {
                        if (rsx::metal::MetalBackend* b = raw->backend.load(std::memory_order_acquire)) {
                            raw->trace_dir = std::format("{}/trace-{}", raw->documents, flips + 1);
                            std::filesystem::create_directories(raw->trace_dir, ec);
                            b->trace_next_frame();
                            PS_LOG_INFO(kChan, "a frame trace asked for: the frame after flip {}, into {}", flips,
                                raw->trace_dir);
                        }
                    }
                }
                // Under 1.5 GB left before iOS stops the app: the backend frees what it can make again (D-148).
                if (os_proc_available_memory() < (1536ull << 20)) {
                    if (rsx::metal::MetalBackend* b = raw->backend.load(std::memory_order_acquire)) b->trim_memory();
                }
                raw->rate_underruns = underruns;
                raw->rate_since = now;
                raw->rate_flips = flips;
            }
            CAMetalLayer* layer = raw->layer;
            if (!layer) return;
            id<MTLTexture> src = (__bridge id<MTLTexture>)f.texture;
            id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)f.command_buffer;
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable || drawable.texture.width != src.width || drawable.texture.height != src.height) return;
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:src toTexture:drawable.texture];
            [blit endEncoding];
            [cb presentDrawable:drawable];
        };
        auto backend = std::make_unique<rsx::metal::MetalBackend>(*raw->memory, *raw->tracker, std::move(mc));
        if (!backend->valid()) return nullptr;
        raw->backend.store(backend.get(), std::memory_order_release);
        return backend;
    };

    s->kernel = std::make_unique<lv2::Kernel>(*s->memory, cfg);
    s->kernel->install();

    hle::GameInfo game;
    std::string title = hdd_game ? hdd_game->title : std::string();
    if (o->iso_path && (!hdd_game || hdd_game->update())) {
        loader::DiscInfo disc;
        std::string disc_error;
        if (!loader::mount_disc_image(*s->kernel, o->iso_path, &disc, &disc_error)) {
            return fail(error, error_size, "the disc image: " + disc_error);
        }
        if (hdd_game && disc.title_id != hdd_game->title_id) {
            return fail(error, error_size, "the update is " + hdd_game->title_id + "'s, the disc " + disc.title_id);
        }
        game.title_id = disc.title_id;
        game.param_sfo = std::move(disc.param_sfo);
        title = disc.title;
    }
    // Its folder's PARAM.SFO is what cellGame reads (an update's: category GD), its USRDIR /app_home (below).
    if (hdd_game) game = hdd_game->game_info();
    for (const char c : game.title_id) { // a path component: letters and digits only
        if (std::isalnum(static_cast<unsigned char>(c))) s->title_id += c;
    }
    if (s->title_id.empty()) s->title_id = "unknown";
    // Coverage for the Mac (D-117), per game (D-118): the SPU programs this phone meets without lifted code (saved as
    // pspocket-boot --spu-dump saves them) and the addresses lifted code did not reach, kept for scripts/ios.sh
    // pull-coverage, which hands them to the next translation. Application Support: the system does not purge it,
    // Files does not show it.
    if (NSString* support = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)
                                .firstObject) {
        const std::string coverage = std::string(support.UTF8String) + "/pspocket-coverage/" + s->title_id;
        std::error_code cov_ec;
        std::filesystem::create_directories(coverage + "/spu", cov_ec);
        if (!cov_ec) {
            s->coverage = coverage;
            hle::set_spu_dump_dir(coverage + "/spu");
            ps_spu_set_miss_log((coverage + "/spu-misses.log").c_str());
            enable_spu_modules(); // the images SPURS loads are only seen with the module hooks in (as --spu-dump)
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(o->hdd0_dir) / "game", ec);
    if (!s->kernel->vfs_mount("/dev_hdd0", o->hdd0_dir, false)) {
        return fail(error, error_size, std::string("cannot mount ") + o->hdd0_dir + " as /dev_hdd0");
    }
    if (hdd_game) {
        std::string why;
        if (!loader::mount_app_home(*s->kernel, *hdd_game, &why)) return fail(error, error_size, why);
        PS_LOG_INFO(kChan, "{} {} \"{}\" version {} from /dev_hdd0/game/{}",
            hdd_game->update() ? "the disc game's update:" : "an installed game:", hdd_game->title_id, hdd_game->title,
            hdd_game->app_ver.empty() ? "?" : hdd_game->app_ver, hdd_game->dir);
    }
    if (o->flash_dir && std::filesystem::is_directory(o->flash_dir, ec)) {
        // An install from before D-177 also holds the XMB, the Blu-ray player and the emulators: 150 MB no game reads.
        if (const u64 freed = recomp::prune_firmware(o->flash_dir))
            PS_LOG_INFO(kChan, "/dev_flash: {:.0f} MB of no use to games removed", static_cast<double>(freed) / 1048576.0);
        if (!s->kernel->vfs_mount("/dev_flash", o->flash_dir, true)) {
            return fail(error, error_size, std::string("cannot mount ") + o->flash_dir + " as /dev_flash");
        }
    } else {
        PS_LOG_WARN(kChan, "no /dev_flash: system files the game loads itself (its MP3 codec) are missing");
    }

    hle::ProcessConfig process_config;
    process_config.game = std::move(game);
    s->hle = std::make_unique<hle::HleProcess>(*s->kernel, PS_MEM_MAIN_BASE, cfg.main_area_end,
        std::move(process_config));
    s->hle->install();

    // Sound: the playback category (it plays with the ring/silent switch on) and a speaker that takes a cellAudio
    // period at a time (iOS's default, about 20 ms per callback, would swing the 34 ms buffer by more than half), then
    // cellAudio's mix to the speaker, paced by what it holds (D-074) and stretched while the game is behind (D-075).
    NSError* audio_error = nil;
    AVAudioSession* session = [AVAudioSession sharedInstance];
    if (![session setCategory:AVAudioSessionCategoryPlayback error:&audio_error] ||
        ![session setPreferredIOBufferDuration:static_cast<double>(hle::kAudioBlockFrames) / hle::kAudioSampleRate
                                         error:&audio_error] ||
        ![session setActive:YES error:&audio_error]) {
        PS_LOG_WARN(kChan, "audio session: {}", audio_error ? audio_error.localizedDescription.UTF8String : "?");
    }
    PS_LOG_INFO(kChan, "audio session: {} Hz, {:.1f} ms per callback", session.sampleRate,
        session.IOBufferDuration * 1000.0);
    s->audio = std::make_unique<platform::AudioOutput>();
    if (s->audio->valid()) {
        platform::AudioOutput* out = s->audio.get();
        hle::set_audio_sink(
            *s->hle, [out](const f32* samples, u32 frames) { out->push(samples, frames); },
            [out] { return hle::AudioBufferLevel{out->queued(), out->device_frames()}; },
            [out](bool on, f32 tempo) { out->set_stretch(on, tempo); });
    }

    // Firmware modules (D-157): with the system software installed, the import libraries pspocket has nothing of are
    // served by the PS3's own modules. Indexed once (lle-index.txt beside /dev_flash).
    if (o->flash_dir && std::filesystem::is_directory(o->flash_dir, ec)) {
        s->lle = std::make_unique<loader::LleModules>(o->flash_dir);
        std::string why;
        if (!s->lle->index(&why)) {
            PS_LOG_WARN(kChan, "no firmware modules: {}", why);
            s->lle.reset();
        }
    }
    // Their translation, made once on this iPhone (D-178): the folder "system" beside /dev_flash's.
    if (s->lle) {
        std::filesystem::path flash = std::filesystem::path(o->flash_dir).lexically_normal();
        if (!flash.has_filename()) flash = flash.parent_path();
        std::string why;
        s->system = loader::SystemLibrary::load((flash.parent_path() / "system").string(), *s->lle, *s->memory, &why);
        if (!s->system) PS_LOG_INFO(kChan, "{}", why);
    }
    loader::LoadReport report;
    if (const lv2::Result r = loader::load(*s->exe, *s->kernel, *s->hle, &report, s->lle.get()); r != lv2::CELL_OK) {
        return fail(error, error_size, std::string("loading the executable failed: ") + lv2::error_name(r));
    }
    if (s->lle) s->state.module_starts = s->lle->take_starts();
    // Modules the game loads while it runs (sys_prx_load_module, D-159).
    s->modules = std::make_unique<loader::RuntimeModules>(*s->hle, s->exe, s->lle.get());
    s->hle->set_module_loader(s->modules.get());
    // The game's own modules: saved for the next preparation when they have no translation, to their slots when
    // they have (D-238).
    if (!s->coverage.empty()) s->modules->set_capture_dir(s->coverage + "/prx");
    if (!game_library.empty()) {
        std::string why;
        s->game_modules = loader::GameModules::load(
            (std::filesystem::path(game_library).parent_path() / "modules").string(), exe_sha256, *s->modules,
            s->lle.get(), *s->memory, &why);
        if (!s->game_modules && why != "none") PS_LOG_INFO(kChan, "the game's modules library: {}", why);
    }
#if PSPOCKET_HAVE_GAME_SPU
    // The lifted SPU programs built into the app (scripts/ios.sh), first: a program is found by its code's hash, the
    // first registered winning, so these (with the entries the Mac's runs collected, D-099) run where the library has
    // the same program, and the library's fill in the ones the app lacks (D-155). Never for another game: the hash.
    pspocket_game_spu_register();
    enable_spu_modules();
#endif
    if (!game_library.empty()) {
        // The game's translated code from its game library (route B, D-152), checked against this runtime and this
        // executable before anything in it runs.
        game::LoadedLibrary loaded;
        std::string why;
        if (!game::load_library(game_library, *s->memory, &loaded, &why))
            return fail(error, error_size, "the game library cannot run: " + why);
        if (loaded.library->spu_register) enable_spu_modules();
        s->translated = loaded.library->ppu_function_count > 0;
    }
    // What the game meets that no preparation knew, translated while it runs (D-236).
    if (!s->coverage.empty()) s->spu_translator = ios::start_session_translator(s->title_id, s->coverage + "/spu", *s->memory);
#if PSPOCKET_HAVE_GAME_PPU
    // The game's PPU code translated ahead of time (D-092), for the executable it was translated from only (D-098).
    if (game_library.empty() &&
        loader::register_translated_ppu(*s->memory, game_ppu_image_words, game_ppu_image_word_count, &game_ppu_register)) {
        s->translated = true;
        PS_LOG_INFO(kChan, "the game's PPU code runs translated ahead of time");
    }
#endif
    install_fault_handler();
    // Textures are watched by page protection from here on (D-134): the handler resolves the tracker's faults. Not
    // under a debugger (it would stop at each one), nor with PSPOCKET_RSX_HASH_TEXTURES set (the hashing of before).
    if (s->tracker && !std::getenv("PSPOCKET_RSX_HASH_TEXTURES") && !debugger_attached()) {
        s->tracker->install();
        s->tracker->set_protection(true);
    }
    u32 main_id = 0;
    s->start = std::chrono::steady_clock::now();
    s->rate_since = s->start;
    s->running = true;
    g_session = s.release();
    // iOS's memory warning: the backend frees what it can make again at its next flip (D-148; run 089: nine warnings
    // in 2.4 s, then the app was stopped).
    [[NSNotificationCenter defaultCenter] addObserverForName:UIApplicationDidReceiveMemoryWarningNotification
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification*) {
                                                      if (rsx::metal::MetalBackend* b = g_session->backend.load())
                                                          b->trim_memory();
                                                  }];
    const loader::BootOptions boot_options = g_session->state.options; // argv[0]: the executable's guest path
    if (const lv2::Result r = loader::start_main_thread(g_session->exe, *g_session->kernel, g_session->state,
            boot_options, main_id);
        r != lv2::CELL_OK) {
        g_session->running = false;
        return fail(error, error_size, std::string("cannot start the main thread: ") + lv2::error_name(r));
    }
    PS_LOG_INFO(kChan, "{} \"{}\" booted from {}: {} imported functions ({} implemented, {} RPCS3 stubs)",
        g_session->hle->config().game.title_id, title,
        hdd_game ? g_session->state.options.argv[0] : std::string(o->iso_path), report.functions_linked,
        report.functions_implemented, report.functions_stubbed);
    watch_for_freezes(g_session);
    return 0;
}

extern "C" int ps_game_probe_disc(const char* iso_path, char* title_id, size_t title_id_size, char* title,
    size_t title_size, char* error, size_t error_size)
{
    loader::DiscInfo info;
    std::string message;
    if (!iso_path || !loader::open_disc_image(iso_path, &info, &message)) {
        if (error && error_size) std::snprintf(error, error_size, "%s", message.empty() ? "no image" : message.c_str());
        return 1;
    }
    if (title_id && title_id_size) std::snprintf(title_id, title_id_size, "%s", info.title_id.c_str());
    if (title && title_size) std::snprintf(title, title_size, "%s", info.title.c_str());
    return 0;
}

extern "C" int ps_game_probe_installed(const char* hdd0_dir, const char* game_dir, char* title_id,
    size_t title_id_size, char* title, size_t title_size, char* category, size_t category_size, char* version,
    size_t version_size, char* error, size_t error_size)
{
    std::string why = "missing paths";
    const auto game = hdd0_dir && game_dir ? loader::read_hdd_game(hdd0_dir, game_dir, &why) : std::nullopt;
    if (!game) {
        if (error && error_size) std::snprintf(error, error_size, "%s", why.c_str());
        return 1;
    }
    if (title_id && title_id_size) std::snprintf(title_id, title_id_size, "%s", game->game_info().title_id.c_str());
    if (title && title_size) std::snprintf(title, title_size, "%s", game->title.c_str());
    if (category && category_size) std::snprintf(category, category_size, "%s", game->category.c_str());
    if (version && version_size) std::snprintf(version, version_size, "%s", game->app_ver.c_str());
    return 0;
}

extern "C" int ps_game_find_update(const char* hdd0_dir, const char* title_id, char* game_dir, size_t game_dir_size,
    char* version, size_t version_size)
{
    if (!hdd0_dir || !title_id) return 0;
    const auto update = loader::find_update(hdd0_dir, title_id);
    if (!update) return 0;
    if (game_dir && game_dir_size) std::snprintf(game_dir, game_dir_size, "%s", update->host_dir.c_str());
    if (version && version_size) std::snprintf(version, version_size, "%s", update->app_ver.c_str());
    return 1;
}

extern "C" int ps_game_read_disc_file(const char* iso_path, const char* path, void* buffer, size_t buffer_size,
    size_t* size, char* error, size_t error_size)
{
    const auto fail = [&](const std::string& why) {
        if (error && error_size) std::snprintf(error, error_size, "%s", why.c_str());
        return 1;
    };
    if (size) *size = 0;
    if (!iso_path || !path) return fail("no image or path");
    std::string message;
    const auto image = loader::open_disc_image(iso_path, nullptr, &message);
    if (!image) return fail(message.empty() ? "no image" : message);
    const auto file = image->open(path);
    if (!file) return fail(std::string("no ") + path + " on the disc");
    const u64 n = file->size();
    if (size) *size = static_cast<size_t>(n);
    if (buffer && buffer_size) {
        const usize take = static_cast<usize>(std::min<u64>(n, buffer_size));
        if (!file->read(0, buffer, take)) return fail(std::string("reading ") + path + " failed");
    }
    return 0;
}

extern "C" void ps_game_set_pad(const ps_game_pad* pad)
{
    if (!g_session || !g_session->hle || !pad) return;
    hle::PadInput in;
    in.digital1 = pad->digital1;
    in.digital2 = pad->digital2;
    in.right_x = pad->right_x;
    in.right_y = pad->right_y;
    in.left_x = pad->left_x;
    in.left_y = pad->left_y;
    std::memcpy(in.pressure, pad->pressure, sizeof in.pressure);
    hle::set_pad_input(*g_session->hle, in);
}

extern "C" void ps_game_get_stats(ps_game_stats* out)
{
    if (!out) return;
    *out = {};
    if (!g_session) return;
    out->running = g_session->running ? 1 : 0;
    out->flips = g_session->flips.load(std::memory_order_relaxed);
    out->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_session->start).count();
}

extern "C" void ps_game_log_tail(char* out, size_t size)
{
    if (!out || !size) return;
    std::string text;
    {
        std::lock_guard lock(g_log_mutex);
        for (const std::string& line : g_tail) text += line + "\n";
    }
    if (text.size() >= size) text.erase(0, text.size() - (size - 1));
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = 0;
}
