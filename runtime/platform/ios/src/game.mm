// The game in the iOS app (pspocket/platform/game.h): pspocket-boot's boot sequence (runtime/loader/tools/
// boot_main.cpp) with the Metal backend presenting into the app's CAMetalLayer, the log in a file and on screen, and
// the virtual controller driven by the app's buttons. Objective-C++ with ARC.

#import <AVFoundation/AVFoundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include "pspocket/platform/game.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <os/proc.h>
#include <signal.h>

#include <thread>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <ctime>
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
#include "pspocket/hle/display.hpp"
#include "pspocket/hle/input.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/report.hpp"
#include "pspocket/hle/spu_programs.hpp"
#include "pspocket/loader/disc.hpp"
#include "pspocket/loader/frame_watch.hpp"
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

// The screen's refreshes for the game's vblanks (D-971): a display link at 60 Hz reports each one, in steady_clock
// time.
@interface PSPocketDisplayClock : NSObject
- (void)refresh:(CADisplayLink*)link;
@end

@implementation PSPocketDisplayClock
- (void)refresh:(CADisplayLink*)link
{
    using std::chrono::duration;
    using Clock = std::chrono::steady_clock;
    const Clock::time_point now = Clock::now();
    // The link's times are CACurrentMediaTime's (mach absolute time); steady_clock counts the time asleep too, so the
    // refresh is placed by how long ago it was.
    const double ago = CACurrentMediaTime() - link.timestamp;
    const double period = link.targetTimestamp - link.timestamp;
    pspocket::hle::display_vsync(now - std::chrono::duration_cast<Clock::duration>(duration<double>(ago)),
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration<double>(period)));
}
@end

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
    // TASK_POWER_INFO_V2's counters at the last line (D-970): CPU and performance-core time (mach time units), energy
    // (nJ), moves between the kinds of cores.
    u64 rate_cpu = 0, rate_ptime = 0, rate_energy = 0, rate_switches = 0;
    // Presenting (RSX thread only, D-971): the flips' pace (a moving average of the time between them), whether frames
    // are held for 1/30 s at least, and the time spent waiting for a drawable since the last line.
    std::chrono::steady_clock::time_point last_present;
    double flip_ms = 0;
    bool pace_30 = false;
    bool pacing = true; // PSPOCKET_NO_PACING: off
    u64 drawable_wait_ns = 0, held_frames = 0;
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
    // Glitch reports (D-1065): the detectors over each frame's thumbnail, the folder reports go to, and how many this
    // session wrote (at most kMaxReports).
    std::unique_ptr<loader::FrameWatch> watch;
    std::string reports_dir;
    std::atomic<int> reports_written{0};
    // The app's Documents folder (the log's), where a frame trace is asked for and kept (D-947): an empty file
    // Documents/pspocket-trace, put there over USB, has the backend trace the next frame into Documents/trace-<flip>/.
    std::string documents;
    std::string trace_dir;
};

constexpr auto kRateInterval = std::chrono::seconds(5);

// iOS's heat levels (NSProcessInfoThermalState): from "serious" the system slows the chips down.
constexpr const char* kThermal[] = {"nominal", "fair", "serious", "critical"};
const char* thermal_name()
{
    return kThermal[std::min<usize>(static_cast<usize>([[NSProcessInfo processInfo] thermalState]), 3)];
}

struct PowerCounters {
    u64 cpu = 0, ptime = 0, energy = 0, switches = 0;
};

// The process's CPU time, the part of it on the performance cores, its energy and its moves between the kinds of cores
// (TASK_POWER_INFO_V2, D-970).
std::optional<PowerCounters> power_counters()
{
    task_power_info_v2_data_t p{};
    mach_msg_type_number_t n = TASK_POWER_INFO_V2_COUNT;
    if (task_info(mach_task_self(), TASK_POWER_INFO_V2, reinterpret_cast<task_info_t>(&p), &n) != KERN_SUCCESS) return {};
    return PowerCounters{p.cpu_energy.total_user + p.cpu_energy.total_system, p.task_ptime, p.task_energy,
        p.task_pset_switches};
}

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

// The playback category (it plays with the ring/silent switch on), a cellAudio period a callback, active (D-074;
// again after iOS's media services were reset, D-997).
void set_up_audio_session(AVAudioSession* session)
{
    NSError* audio_error = nil;
    if (![session setCategory:AVAudioSessionCategoryPlayback error:&audio_error] ||
        ![session setPreferredIOBufferDuration:static_cast<double>(hle::kAudioBlockFrames) / hle::kAudioSampleRate
                                         error:&audio_error] ||
        ![session setActive:YES error:&audio_error]) {
        PS_LOG_WARN(kChan, "audio session: {}", audio_error ? audio_error.localizedDescription.UTF8String : "?");
    }
}

// Where the sound goes now: the outputs' names and kinds ("ArtZ's MacBook Pro (AirPlay)", "Speaker (Speaker)").
std::string audio_route(AVAudioSession* session)
{
    std::string r;
    for (AVAudioSessionPortDescription* port in session.currentRoute.outputs)
        r += std::format("{}{} ({})", r.empty() ? "" : ", ", port.portName.UTF8String, port.portType.UTF8String);
    return r.empty() ? "nothing" : r;
}

// The app's Settings › Experiments switches (D-996): a UserDefaults key the app's toggle writes, read once at boot into
// the PSPOCKET_ switch it stands for. A value the switch file set first wins (a test the Mac set up stays as it is).
void set_env_from_settings()
{
    struct Switch {
        NSString* key;
        const char* name;
        const char* on;
    };
    static const Switch kSwitches[] = {
        {@"edgeGPU", "PSPOCKET_EDGE_GPU", "1"}, // Sony's EDGE geometry on the GPU (D-987)
        {@"startMissing", "PSPOCKET_START_MISSING", "1"}, // start a game whose imports pspocket lacks (D-1061)
    };
    for (const Switch& s : kSwitches) {
        if (![[NSUserDefaults standardUserDefaults] boolForKey:s.key] || std::getenv(s.name)) continue;
        setenv(s.name, s.on, 0);
        PS_LOG_INFO(kChan, "{}={} (from Settings)", s.name, s.on);
    }
}

// ---- glitch reports (D-1065) ----
//
// A detector's event (FrameWatch: partly black, flicker, hitch, frozen) or the player's flag writes
// Documents/reports/<stamp>-<kind>/: report.json (what, when, the build, the switches, heat), log.txt (the log's last
// lines) and thumbs.png (the last 32 frames' 64x36 thumbnails, 8 a row, oldest first). At most 30 a session; the folder
// keeps the newest 50 MB. scripts/ios.sh pull-reports copies them to the Mac (inventory/phone/reports/).
constexpr int kMaxReports = 30;
constexpr u64 kReportsBytes = 50ull << 20;

void trim_reports(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> runs;
    u64 total = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        u64 bytes = 0;
        for (const auto& f : std::filesystem::recursive_directory_iterator(e.path(), ec))
            if (f.is_regular_file(ec)) bytes += f.file_size(ec);
        total += bytes;
        runs.emplace_back(e.last_write_time(ec), e.path());
    }
    std::sort(runs.begin(), runs.end());
    for (const auto& [when, path] : runs) {
        if (total <= kReportsBytes) break;
        u64 bytes = 0;
        for (const auto& f : std::filesystem::recursive_directory_iterator(path, ec))
            if (f.is_regular_file(ec)) bytes += f.file_size(ec);
        std::filesystem::remove_all(path, ec);
        total -= std::min(total, bytes);
    }
}

void write_report(Session* s, const loader::FrameWatch::Event& e, std::vector<loader::FrameWatch::Thumb> thumbs)
{
    if (!s || s->reports_dir.empty() || s->reports_written.fetch_add(1) >= kMaxReports) return;
    std::string log;
    {
        std::lock_guard lock(g_log_mutex);
        for (const std::string& line : g_tail) log += line + "\n";
    }
    const std::string kind = loader::FrameWatch::name(e.kind);
    const u64 flip = e.flip;
    const double value = e.value;
    const std::string dir = s->reports_dir;
    // Off the thread that saw it (the RSX thread's present, or the main thread's button): files take milliseconds.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        const std::time_t now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
        const std::filesystem::path run = std::filesystem::path(dir) / (std::string(stamp) + "-" + kind);
        std::error_code ec;
        std::filesystem::create_directories(run, ec);
        std::string env;
        for (const char* name : {"PSPOCKET_EDGE_GPU", "PSPOCKET_EDGE_GPU_EXACT", "PSPOCKET_RSX_NO_SURFACE_POOL",
                 "PSPOCKET_START_MISSING", "PSPOCKET_SPU_NATIVE"})
            if (const char* v = std::getenv(name)) env += std::format("{}\"{}\": \"{}\"", env.empty() ? "" : ", ", name, v);
        std::ofstream(run / "report.json") << std::format(
            "{{\n  \"kind\": \"{}\",\n  \"flip\": {},\n  \"value\": {:.3f},\n  \"time\": \"{}\",\n  \"heat\": \"{}\",\n"
            "  \"switches\": {{{}}}\n}}\n", kind, flip, value, stamp, thermal_name(), env);
        std::ofstream(run / "log.txt") << log;
        if (!thumbs.empty()) {
            constexpr u32 kCols = 8, kW = loader::FrameWatch::kW, kH = loader::FrameWatch::kH;
            const u32 rows = static_cast<u32>((thumbs.size() + kCols - 1) / kCols);
            std::vector<u8> px(static_cast<usize>(kCols * kW) * rows * kH * 4, 0);
            for (usize i = 0; i < thumbs.size(); i++) {
                const u32 ox = static_cast<u32>(i % kCols) * kW, oy = static_cast<u32>(i / kCols) * kH;
                for (u32 y = 0; y < kH; y++)
                    for (u32 x = 0; x < kW; x++) {
                        u8* d = &px[((oy + y) * kCols * kW + ox + x) * 4];
                        const u8* t = &thumbs[i][(y * kW + x) * 3];
                        d[0] = t[0], d[1] = t[1], d[2] = t[2], d[3] = 255;
                    }
            }
            write_png_rgb((run / "thumbs.png").string(), px.data(), kCols * kW, rows * kH, kCols * kW * 4, true);
        }
        trim_reports(dir);
    });
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
    set_env_from_settings();

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
        // Metal's performance HUD (Info.plist MetalHUDEnabled) only when asked: PSPOCKET_HUD (Documents/pspocket-env.txt)
        // shows it and logs a "metal-HUD" line a second (present intervals, frame misses; D-970).
        if (@available(iOS 16.0, *)) {
            layer.developerHUDProperties = std::getenv("PSPOCKET_HUD") ? @{@"mode" : @"default", @"logging" : @"default"} : @{};
        }
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
                // The memory iOS counts against the app, and what is left before it stops the app (it does so without
                // a crash report: a game that vanished at 17:06 left only "Received memory warning" in the log).
                task_vm_info_data_t vm{};
                mach_msg_type_number_t vm_count = TASK_VM_INFO_COUNT;
                const double footprint = task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm),
                                             &vm_count) == KERN_SUCCESS ? static_cast<double>(vm.phys_footprint) : 0.0;
                PS_LOG_INFO(kChan, "{:.1f} fps ({} frames in {:.1f} s; {} since the boot); the speaker ran dry {} times; "
                                   "heat {}; memory {:.2f} GB, {:.2f} GB left",
                    static_cast<double>(flips - raw->rate_flips) / dt, flips - raw->rate_flips, dt, flips,
                    underruns - raw->rate_underruns, thermal_name(), footprint / 1e9,
                    static_cast<double>(os_proc_available_memory()) / 1e9);
                PS_LOG_INFO(kChan, "threads: {}", thread_cpu_report(dt)); // which thread holds a slow stretch back
                // How much of the work iOS gives the two performance cores, and the power behind the heat (D-970).
                // Low Power Mode slows the CPUs and caps the display at 60 Hz.
                if (const auto p = power_counters()) {
                    mach_timebase_info_data_t tb{};
                    mach_timebase_info(&tb);
                    const u64 cpu = p->cpu - raw->rate_cpu, ptime = p->ptime - raw->rate_ptime;
                    PS_LOG_INFO(kChan, "power: CPU {:.1f} s, {:.0f}% of it on the performance cores, {:.1f} W, "
                                       "{} moves between the kinds of cores{}",
                        static_cast<double>(cpu) * tb.numer / tb.denom / 1e9,
                        cpu ? 100.0 * static_cast<double>(ptime) / static_cast<double>(cpu) : 0.0,
                        static_cast<double>(p->energy - raw->rate_energy) / 1e9 / dt, p->switches - raw->rate_switches,
                        [[NSProcessInfo processInfo] isLowPowerModeEnabled] ? "; Low Power Mode on" : "");
                    raw->rate_cpu = p->cpu;
                    raw->rate_ptime = p->ptime;
                    raw->rate_energy = p->energy;
                    raw->rate_switches = p->switches;
                }
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
                PS_LOG_INFO(kChan, "present: a flip every {:.1f} ms, {} frames held 1/30 s, {:.1f} ms waiting for "
                                   "drawables", raw->flip_ms, raw->held_frames,
                    static_cast<double>(raw->drawable_wait_ns) / 1e6);
                raw->held_frames = 0;
                raw->drawable_wait_ns = 0;
                raw->rate_underruns = underruns;
                raw->rate_since = now;
                raw->rate_flips = flips;
            }
            CAMetalLayer* layer = raw->layer;
            if (!layer) return;
            id<MTLTexture> src = (__bridge id<MTLTexture>)f.texture;
            id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)f.command_buffer;
            // The flips' pace (D-971): a game that flips every second vblank (30 fps) has each frame shown for 1/30 s
            // at least, so one the GPU finishes a little late is not shown a refresh late while the next is on time
            // (a 3-1 cadence of refreshes, a judder). Switched with some hysteresis; a 60 fps game is never held.
            if (raw->last_present.time_since_epoch().count()) {
                const double ms = std::chrono::duration<double, std::milli>(now - raw->last_present).count();
                if (ms < 200) raw->flip_ms = raw->flip_ms > 0 ? raw->flip_ms * 0.95 + ms * 0.05 : ms;
            }
            raw->last_present = now;
            raw->pace_30 = raw->pacing && (raw->pace_30 ? raw->flip_ms > 24 : raw->flip_ms > 30);
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            raw->drawable_wait_ns += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - now).count());
            if (!drawable || drawable.texture.width != src.width || drawable.texture.height != src.height) return;
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:src toTexture:drawable.texture];
            [blit endEncoding];
            if (raw->pace_30) {
                [cb presentDrawable:drawable afterMinimumDuration:1.0 / 30];
                raw->held_frames++;
            } else {
                [cb presentDrawable:drawable];
            }
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
    AVAudioSession* session = [AVAudioSession sharedInstance];
    set_up_audio_session(session);
    PS_LOG_INFO(kChan, "audio session: {} Hz, {:.1f} ms per callback, out to {}", session.sampleRate,
        session.IOBufferDuration * 1000.0, audio_route(session));
    s->audio = std::make_unique<platform::AudioOutput>();
    if (s->audio->valid()) {
        platform::AudioOutput* out = s->audio.get();
        hle::set_audio_sink(
            *s->hle, [out](const f32* samples, u32 frames) { out->push(samples, frames); },
            [out] { return hle::AudioBufferLevel{out->queued(), out->device_frames()}; },
            [out](bool on, f32 tempo) { out->set_stretch(on, tempo); });
    }
    // The sound follows wherever iOS sends it (D-997): AirPlay screen mirroring to a Mac moves it there and back, a
    // call or Siri interrupts it, headphones come and go. The engine starts itself again when its output changes
    // (AudioOutput); these are what it is not told of. The session lives as long as the app, as these observers do.
    {
        platform::AudioOutput* out = s->audio.get();
        NSNotificationCenter* centre = [NSNotificationCenter defaultCenter];
        [centre addObserverForName:AVAudioSessionInterruptionNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification* n) {
                            const auto type = static_cast<AVAudioSessionInterruptionType>(
                                [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue]);
                            PS_LOG_INFO(kChan, "sound interrupted: {}",
                                type == AVAudioSessionInterruptionTypeBegan ? "began" : "ended");
                            if (type != AVAudioSessionInterruptionTypeEnded) return;
                            [[AVAudioSession sharedInstance] setActive:YES error:nil];
                            out->restart("an interruption ended");
                        }];
        [centre addObserverForName:AVAudioSessionRouteChangeNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification* n) {
                            const auto reason = [n.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
                            PS_LOG_INFO(kChan, "sound now out to {} (route change, reason {})",
                                audio_route([AVAudioSession sharedInstance]), reason);
                            out->restart("the route changed");
                        }];
        [centre addObserverForName:AVAudioSessionMediaServicesWereResetNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification*) {
                            PS_LOG_WARN(kChan, "iOS's media services were reset: the sound is set up again");
                            set_up_audio_session([AVAudioSession sharedInstance]);
                            out->restart("the media services were reset", true);
                        }];
        [centre addObserverForName:UIApplicationDidBecomeActiveNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification*) {
                            if (out->running()) return;
                            [[AVAudioSession sharedInstance] setActive:YES error:nil];
                            out->restart("the app came back");
                        }];
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
    // Glitch reports (D-1065): the detectors, fed each frame's thumbnail once the backend has them.
    if (o->log_path) s->reports_dir = (std::filesystem::path(o->log_path).parent_path() / "reports").string();
    s->watch = std::make_unique<loader::FrameWatch>();
    {
        Session* sp = s.get(); // the session lives as long as the app
        s->watch->on_event = [sp](const loader::FrameWatch::Event& e) {
            PS_LOG_WARN(kChan, "glitch report: {} at flip {} ({:.2f})", loader::FrameWatch::name(e.kind), e.flip, e.value);
            write_report(sp, e, sp->watch->recent());
        };
    }
    loader::LoadReport report;
    if (const lv2::Result r = loader::load(*s->exe, *s->kernel, *s->hle, &report, s->lle.get()); r != lv2::CELL_OK) {
        return fail(error, error_size, std::string("loading the executable failed: ") + lv2::error_name(r));
    }
    // A system function the game imports that nothing answers (D-1061): the game would set up its state through it and
    // crash later (GTA IV, run 333). It does not start, and says what it needs, unless Settings › Experiments asks to
    // start such games anyway (PSPOCKET_START_MISSING).
    if (!report.missing.empty() && !std::getenv("PSPOCKET_START_MISSING")) {
        std::string list;
        for (usize i = 0; i < report.missing.size() && i < 6; i++) {
            const auto& m = report.missing[i];
            list += std::format("{}{} {}", i ? ", " : "", m.library,
                m.name.empty() ? std::format("{:#010x}", m.nid) : m.name);
        }
        if (report.missing.size() > 6) list += std::format(" and {} more", report.missing.size() - 6);
        return fail(error, error_size, std::format("This game needs {} system function{} pspocket doesn't have yet "
            "({}). It won't start until they are added; Settings › Experiments can start it anyway.",
            report.missing.size(), report.missing.size() == 1 ? "" : "s", list));
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
    if (const auto p = power_counters()) {
        s->rate_cpu = p->cpu;
        s->rate_ptime = p->ptime;
        s->rate_energy = p->energy;
        s->rate_switches = p->switches;
    }
    {
        // The kinds of cores (D-970): hw.perflevel0 the performance cores, hw.perflevel1 the efficiency cores.
        int perf = 0, eff = 0;
        size_t size = sizeof(int);
        sysctlbyname("hw.perflevel0.logicalcpu", &perf, &size, nullptr, 0);
        size = sizeof(int);
        sysctlbyname("hw.perflevel1.logicalcpu", &eff, &size, nullptr, 0);
        PS_LOG_INFO(kChan, "cores: {} performance, {} efficiency; heat {}; Low Power Mode {}", perf, eff, thermal_name(),
            [[NSProcessInfo processInfo] isLowPowerModeEnabled] ? "on" : "off");
    }
    // The game's vblanks on the screen's refreshes (D-971): a 60 Hz display link on the main thread reports them
    // (PSPOCKET_NO_DISPLAY_CLOCK: the free-running 59.94 Hz timer, as before). PSPOCKET_NO_PACING: frames not held.
    s->pacing = !std::getenv("PSPOCKET_NO_PACING");
    if (!std::getenv("PSPOCKET_NO_DISPLAY_CLOCK")) {
        dispatch_async(dispatch_get_main_queue(), ^{
            PSPocketDisplayClock* clock = [PSPocketDisplayClock new];
            CADisplayLink* link = [CADisplayLink displayLinkWithTarget:clock selector:@selector(refresh:)];
            link.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
            [link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
        });
    }
    PS_LOG_INFO(kChan, "vblanks on {}; frames {}", std::getenv("PSPOCKET_NO_DISPLAY_CLOCK") ? "a 59.94 Hz timer" :
        "the screen's refreshes", s->pacing ? "held 1/30 s at 30 fps" : "not held");
    // Each change of the heat level and of Low Power Mode when it happens (D-970): the 5-s lines show only the level.
    [[NSNotificationCenter defaultCenter] addObserverForName:NSProcessInfoThermalStateDidChangeNotification
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification*) {
                                                      PS_LOG_INFO(kChan, "heat now {}", thermal_name());
                                                  }];
    [[NSNotificationCenter defaultCenter] addObserverForName:NSProcessInfoPowerStateDidChangeNotification
                                                      object:nil
                                                       queue:nil
                                                  usingBlock:^(NSNotification*) {
                                                      PS_LOG_INFO(kChan, "Low Power Mode now {}",
                                                          [[NSProcessInfo processInfo] isLowPowerModeEnabled] ? "on" : "off");
                                                  }];
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

extern "C" void ps_game_flag(void)
{
    if (!g_session || !g_session->watch) return;
    const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    g_session->watch->flag(g_session->flips.load(std::memory_order_relaxed), now);
}

extern "C" int ps_game_reports_written(void) { return g_session ? g_session->reports_written.load() : 0; }

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
