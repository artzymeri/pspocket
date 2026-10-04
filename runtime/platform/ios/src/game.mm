// The game in the iOS app (pspocket/platform/game.h): pspocket-boot's boot sequence (runtime/loader/tools/
// boot_main.cpp) with the Metal backend presenting into the app's CAMetalLayer, the log in a file and on screen, and
// the virtual controller driven by the app's buttons. Objective-C++ with ARC.

#import <AVFoundation/AVFoundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "pspocket/platform/game.h"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
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
#include <string>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/hle/audio.hpp"
#include "pspocket/hle/input.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/hle/spu_programs.hpp"
#include "pspocket/loader/disc.hpp"
#include "pspocket/loader/loader.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/platform/audio_output.hpp"
#include "pspocket/platform/ios.h"
#include "pspocket/ppu/runtime.h"
#include "pspocket/rsx/metal/metal_backend.hpp"
#include "pspocket/rsx/write_tracker.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;

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

void on_fault(int sig, siginfo_t* info, void*)
{
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

void install_fault_handler()
{
    struct sigaction sa{};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
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

} // namespace

extern "C" int ps_game_boot(const ps_game_options* o, char* error, size_t error_size)
{
    if (g_session) return fail(error, error_size, "a game is already running; quit the app to start another");
    if (!o || !o->elf_path || !o->iso_path || !o->hdd0_dir) return fail(error, error_size, "missing paths");

    if (o->log_path) {
        std::lock_guard lock(g_log_mutex);
        g_log_fd = open(o->log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    ps_ios_install_log_sink(on_log, nullptr);

    std::vector<u8> bytes;
    if (!read_file(o->elf_path, bytes)) return fail(error, error_size, std::string("cannot read ") + o->elf_path);
    if (bytes.size() >= 4 && bytes[0] == 'S' && bytes[1] == 'C' && bytes[2] == 'E' && bytes[3] == 0) {
        return fail(error, error_size, "the executable is still an encrypted SELF; it has to be decrypted to an ELF");
    }
    auto exe = loader::Executable::parse(std::move(bytes));
    if (!exe) return fail(error, error_size, "the executable: " + exe.error().message);

    auto s = std::make_unique<Session>();
    s->exe = *exe;
    s->memory = std::make_unique<GuestMemory>();
    if (!s->memory->reserve()) return fail(error, error_size, "cannot reserve the 4 GB guest address space");
    ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); // no lifted code in the app yet: the interpreter runs the game
    // SPU code without a lifted program, or outside it, is interpreted (D-076). The game's lifted SPU programs, when
    // the build has them (scripts/ios.sh), run its SPURS tasks, jobs and the modules they load (D-079, D-081).
    ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
#if PSPOCKET_HAVE_GAME_SPU
    pspocket_game_spu_register();
    hle::enable_spu_modules();
#endif

    lv2::Config cfg = s->exe->kernel_config();
    Session* raw = s.get();
    cfg.on_process_exit = [raw](s32 status) {
        PS_LOG_INFO(kChan, "the game exited, status {}", status);
        raw->running = false;
    };
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
        mc.present = [raw](const rsx::metal::PresentFrame& f) {
            const u64 flips = raw->flips.fetch_add(1, std::memory_order_relaxed) + 1;
            const auto now = std::chrono::steady_clock::now();
            if (now - raw->rate_since >= kRateInterval) {
                const double dt = std::chrono::duration<double>(now - raw->rate_since).count();
                const u64 underruns = raw->audio ? raw->audio->stats().underruns : 0;
                PS_LOG_INFO(kChan, "{:.1f} fps ({} frames in {:.1f} s; {} since the boot); the speaker ran dry {} times",
                    static_cast<double>(flips - raw->rate_flips) / dt, flips - raw->rate_flips, dt, flips,
                    underruns - raw->rate_underruns);
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
        return backend;
    };

    s->kernel = std::make_unique<lv2::Kernel>(*s->memory, cfg);
    s->kernel->install();

    hle::GameInfo game;
    loader::DiscInfo disc;
    std::string disc_error;
    if (!loader::mount_disc_image(*s->kernel, o->iso_path, &disc, &disc_error)) {
        return fail(error, error_size, "the disc image: " + disc_error);
    }
    game.title_id = disc.title_id;
    game.param_sfo = std::move(disc.param_sfo);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(o->hdd0_dir) / "game", ec);
    if (!s->kernel->vfs_mount("/dev_hdd0", o->hdd0_dir, false)) {
        return fail(error, error_size, std::string("cannot mount ") + o->hdd0_dir + " as /dev_hdd0");
    }
    if (o->flash_dir && std::filesystem::is_directory(o->flash_dir, ec)) {
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

    loader::LoadReport report;
    if (const lv2::Result r = loader::load(*s->exe, *s->kernel, *s->hle, &report); r != lv2::CELL_OK) {
        return fail(error, error_size, std::string("loading the executable failed: ") + lv2::error_name(r));
    }
    install_fault_handler();
    u32 main_id = 0;
    s->start = std::chrono::steady_clock::now();
    s->rate_since = s->start;
    s->running = true;
    g_session = s.release();
    if (const lv2::Result r = loader::start_main_thread(g_session->exe, *g_session->kernel, g_session->state, {},
            main_id);
        r != lv2::CELL_OK) {
        g_session->running = false;
        return fail(error, error_size, std::string("cannot start the main thread: ") + lv2::error_name(r));
    }
    PS_LOG_INFO(kChan, "{} \"{}\" booted from {}: {} imported functions ({} implemented)", disc.title_id, disc.title,
        o->iso_path, report.functions_linked, report.functions_implemented);
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
