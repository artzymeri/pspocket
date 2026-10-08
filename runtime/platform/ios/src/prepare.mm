// Preparing a game on the phone (pspocket/platform/prepare.h, D-154): runtime/prepare with the app's compiler folder,
// the user's development identity and this phone's CPU and memory.

#include "pspocket/platform/prepare.h"

#import <Foundation/Foundation.h>
#include <os/proc.h>
#include <pthread.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pspocket/core/log.hpp"
#include "session_translator.hpp"

#if PSPOCKET_HAVE_CODEGEN
#import <UIKit/UIKit.h>
#include <sys/qos.h>

#include "pspocket/codegen/toolchain.hpp"
#include "pspocket/codesign/codesign.hpp"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/game/host.hpp"
#include "pspocket/loader/lle.hpp"
#include "pspocket/loader/loader.hpp"
#include "pspocket/prepare/modules.hpp"
#include "pspocket/prepare/prepare.hpp"
#include "pspocket/prepare/session.hpp"
#include "pspocket/prepare/system.hpp"
#include "pspocket/spu/registry.hpp"
#endif

namespace {

namespace fs = std::filesystem;
constexpr const char* kChan = "prepare";

std::mutex g_mutex;
ps_prepare_status g_status{};
bool g_running = false;
std::atomic<bool> g_cancel{false};
FILE* g_trace = nullptr;   // game_dir/prepare-progress.csv: what the time left said, to check it after (D-153)
double g_traced_at = -10;

void copy_text(char* dst, size_t size, const std::string& s)
{
    if (dst && size) std::snprintf(dst, size, "%s", s.c_str());
}

#if PSPOCKET_HAVE_CODEGEN
bool read_file(const fs::path& p, std::vector<uint8_t>& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string resources()
{
    NSString* path = [[NSBundle mainBundle] resourcePath];
    return path ? std::string(path.UTF8String) : std::string();
}

// The user's development identity, which the app signs what it compiles with (D-150): identity.p12, identity.pass and
// the chain's .cer files in Library/Application Support/pspocket-codegen (scripts/ios.sh push-signing-identity).
std::optional<pspocket::codesign::SigningKey> signing_key(std::string* error)
{
    const char* home = std::getenv("HOME");
    const fs::path dir = fs::path(home ? home : "/tmp") / "Library/Application Support/pspocket-codegen";
    std::vector<uint8_t> p12, pass;
    if (!read_file(dir / "identity.p12", p12) || !read_file(dir / "identity.pass", pass)) {
        if (error) *error = "this iPhone has no signing identity yet (on the Mac: scripts/ios.sh push-signing-identity)";
        return std::nullopt;
    }
    std::string password(pass.begin(), pass.end());
    while (!password.empty() && (password.back() == '\n' || password.back() == '\r')) password.pop_back();
    std::vector<std::vector<uint8_t>> chain;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::vector<uint8_t> der;
        if (e.path().extension() == ".cer" && read_file(e.path(), der)) chain.push_back(std::move(der));
    }
    return pspocket::codesign::key_from_p12(p12, password, chain, error);
}

// Where the app keeps what a game met that no translation had (game.mm, D-117/D-118): its SPU images and misses.
std::string coverage_dir(const std::string& title_id)
{
    std::string id;
    for (const char c : title_id)
        if (std::isalnum(static_cast<unsigned char>(c))) id += c;
    NSString* support = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES)
                            .firstObject;
    return support ? std::string(support.UTF8String) + "/pspocket-coverage/" + (id.empty() ? "unknown" : id) : "";
}

void set_status(const pspocket::prepare::Progress& p)
{
    using Phase = pspocket::prepare::Progress::Phase;
    std::lock_guard lock(g_mutex);
    // Done and failed are finish()'s to say, with the library or the error: a step's own end is not the preparation's
    // (the game's library, then its modules, D-238; the app starts the game the moment it reads "done").
    if (p.phase != Phase::Done && p.phase != Phase::Failed)
        g_status.phase = p.phase == Phase::Translating ? PS_PREPARE_TRANSLATING
            : p.phase == Phase::Compiling             ? PS_PREPARE_COMPILING
            : p.phase == Phase::Linking               ? PS_PREPARE_LINKING
                                                      : PS_PREPARE_SIGNING;
    g_status.fraction = p.fraction;
    g_status.seconds = p.seconds;
    g_status.seconds_left = p.seconds_left;
    g_status.files_done = static_cast<uint32_t>(p.files_done);
    g_status.files_total = static_cast<uint32_t>(p.files_total);
    copy_text(g_status.detail, sizeof g_status.detail, p.detail);
    if (g_trace && (p.seconds - g_traced_at >= 2 || p.phase != Phase::Compiling)) {
        g_traced_at = p.seconds;
        std::fprintf(g_trace, "%.1f,%d,%.4f,%.1f,%zu,%zu\n", p.seconds, g_status.phase, p.fraction, p.seconds_left,
            p.files_done, p.files_total);
        std::fflush(g_trace);
    }
}
// What every preparation starts with: the signing identity, the compiler folder, one at a time. The toolchain into
// *toolchain; false with the reason in error.
bool begin(const std::string& dir, std::optional<pspocket::codesign::SigningKey>* key,
    pspocket::prepare::Toolchain* toolchain, char* error, size_t error_size)
{
    std::string why;
    *key = signing_key(&why);
    if (!*key) {
        copy_text(error, error_size, why);
        return false;
    }
    const std::string codegen = resources() + "/codegen";
    std::error_code ec;
    if (!fs::exists(codegen + "/clang/include/arm_neon.h", ec)) {
        copy_text(error, error_size, "this build of pspocket has no compiler headers");
        return false;
    }
    {
        std::lock_guard lock(g_mutex);
        if (g_running) {
            copy_text(error, error_size, "a game is being prepared already");
            return false;
        }
        g_running = true;
        g_status = {};
        std::error_code e;
        fs::create_directories(dir, e);
        if (g_trace) std::fclose(g_trace);
        g_trace = std::fopen((fs::path(dir) / "prepare-progress.csv").c_str(), "w");
        if (g_trace) std::fprintf(g_trace, "seconds,phase,fraction,seconds_left,files_done,files_total\n");
        g_traced_at = -10;
        g_status.phase = PS_PREPARE_TRANSLATING;
        g_status.seconds_left = -1;
        copy_text(g_status.detail, sizeof g_status.detail, "Starting");
    }
    g_cancel = false;
    toolchain->resource_dir = codegen + "/clang";
    toolchain->sysroot = codegen + "/sysroot";
    toolchain->include_dirs = {codegen + "/include"};
    toolchain->entry_source = codegen + "/library/library_entry.c";
    toolchain->cpu = "native"; // compiled on this phone, for this phone
    return true;
}

// The end of every preparation: the status the Swift side polls.
void finish(const pspocket::prepare::Result& r)
{
    std::lock_guard lock(g_mutex);
    g_running = false;
    if (g_trace) {
        std::fprintf(g_trace, "# %s in %.1f s (translate %.1f, compile %.1f, link %.1f, sign %.1f), %zu files, rates "
                              "%.3f %.3f %.3f%s%s\n",
            r.ok ? "done" : "failed", r.seconds, r.translate_seconds, r.compile_seconds, r.link_seconds, r.sign_seconds,
            r.files_compiled, r.rates[0], r.rates[1], r.rates[2], r.ok ? "" : ": ", r.ok ? "" : r.error.c_str());
        std::fclose(g_trace);
        g_trace = nullptr;
    }
    if (r.ok) {
        g_status.phase = PS_PREPARE_DONE;
        g_status.fraction = 1;
        g_status.seconds_left = 0;
        copy_text(g_status.library, sizeof g_status.library, r.library);
    } else {
        g_status.phase = PS_PREPARE_FAILED;
        copy_text(g_status.error, sizeof g_status.error, r.error);
    }
}
#endif

} // namespace

extern "C" int ps_prepare_check(const char* game_dir, const char* executable, char* why, size_t why_size)
{
#if PSPOCKET_HAVE_CODEGEN
    if (!game_dir || !executable) return 0;
    const std::string sha = pspocket::prepare::file_sha256(executable);
    std::string reason = "the executable cannot be read";
    // The title is the game folder's name (Application Support/pspocket-games/<TITLE_ID>).
    const std::string spu = coverage_dir(fs::path(game_dir).filename().string()) + "/spu";
    const std::string landings = coverage_dir(fs::path(game_dir).filename().string()) + "/ppu-landings.txt"; // D-255
    bool ok = !sha.empty() && pspocket::prepare::library_is_current(game_dir, sha, &reason, spu, landings);
    // And its own PRX modules: on its disc (D-239) and saved while it ran (D-238).
    if (ok)
        ok = pspocket::prepare::game_modules_are_current((fs::path(game_dir) / "modules").string(), sha,
            {coverage_dir(fs::path(game_dir).filename().string()) + "/prx", (fs::path(game_dir) / "disc-prx").string()},
            pspocket::prepare::kGameModuleFloor, &reason);
    // And the disc's SPU programs, when they were found after the library was made (D-239).
    if (ok) {
        std::error_code e;
        const auto made = fs::last_write_time(fs::path(game_dir) / "manifest.json", e);
        for (const auto& f : fs::directory_iterator(fs::path(game_dir) / "disc-spu", e))
            if (f.path().extension() == ".elf" && f.last_write_time(e) > made) {
                ok = false;
                reason = "the disc's SPU programs to translate";
                break;
            }
    }
    copy_text(why, why_size, reason);
    return ok ? 1 : 0;
#else
    (void)game_dir;
    (void)executable;
    copy_text(why, why_size, "this build of pspocket has no compiler");
    return 0;
#endif
}

extern "C" int ps_prepare_start(const char* executable, const char* title_id, const char* game_dir, char* error,
    size_t error_size)
{
#if PSPOCKET_HAVE_CODEGEN
    if (!executable || !title_id || !game_dir) {
        copy_text(error, error_size, "missing paths");
        return 1;
    }
    std::optional<pspocket::codesign::SigningKey> key;
    pspocket::prepare::Options o;
    if (!begin(game_dir, &key, &o.toolchain, error, error_size)) return 1;
    o.executable = executable;
    o.title_id = title_id;
    o.game_dir = game_dir;
    // The SPU programs the game met without a translation (saved while it ran), with the misses for more entries.
    const std::string coverage = coverage_dir(title_id);
    o.spu_images_dir = coverage + "/spu";
    o.spu_logs = {coverage + "/spu-misses.log"};
    o.ppu_landings = coverage + "/ppu-landings.txt"; // where it interpreted PPU code: continuations (D-255)
    o.spu_elf_dirs = {(fs::path(game_dir) / "disc-spu").string()}; // the disc's own (D-239)
    // Every core; compiles start while their predicted peaks fit in half of what iOS lets the app have now.
    o.workers = std::max(1u, std::thread::hardware_concurrency());
    o.memory_mb = static_cast<double>(os_proc_available_memory()) / 1048576.0 * 0.5;
    NSString* bundle = [[NSBundle mainBundle] bundleIdentifier];
    o.identifier = bundle ? bundle.UTF8String : "com.artzymeri.pspocket.testhost"; // library validation wants the app's
    // How fast this phone compiles, per kind of file, brought from the heat it was measured at to the heat now (a warm
    // phone compiles slower, D-154): this game's last preparation, else the phone's speed on the last game's PPU code
    // (D-164).
    o.thermal = static_cast<int>([[NSProcessInfo processInfo] thermalState]);
    pspocket::prepare::Manifest own, last;
    const bool have_own = pspocket::prepare::read_manifest(game_dir, &own);
    const bool have_last =
        pspocket::prepare::read_manifest((fs::path(game_dir).parent_path() / "last-preparation").string(), &last);
    const auto rates = pspocket::prepare::prior_rates(have_own ? &own : nullptr, have_last ? &last : nullptr, o.thermal);
    for (int k = 0; k < 3; k++) o.prior_rates[k] = rates[static_cast<size_t>(k)];
    o.progress = set_status;
    o.cancel = &g_cancel;
    PS_LOG_INFO(kChan, "{}: preparing with {} workers, {:.0f} MB for compiles, rates {:.2f}/{:.2f}", title_id, o.workers,
        o.memory_mb, o.prior_rates[0], o.prior_rates[1]);

    // The player waits for it. Threads made without a QoS class get the default one, not their creator's (sys/qos.h),
    // so the compiles are given theirs (D-235).
    o.qos_class = QOS_CLASS_USER_INITIATED;
    std::thread([o = std::move(o), key = std::move(*key)]() mutable {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
        o.key = &key;
        const pspocket::prepare::Result r = pspocket::prepare::prepare_game(o);
        if (r.ok) { // the rates for the next game's time left
            std::error_code e;
            const fs::path last_dir = fs::path(o.game_dir).parent_path() / "last-preparation";
            fs::create_directories(last_dir, e);
            fs::copy_file(fs::path(o.game_dir) / "manifest.json", last_dir / "manifest.json",
                fs::copy_options::overwrite_existing, e);
            // Then the game's own PRX modules it saved while it ran (D-238). A failure leaves them interpreted.
            pspocket::prepare::ModulesOptions m;
            m.inputs = {coverage_dir(o.title_id) + "/prx", (fs::path(o.game_dir) / "disc-prx").string()};
            m.title_id = o.title_id;
            m.executable_sha256 = pspocket::prepare::file_sha256(o.executable);
            m.dir = (fs::path(o.game_dir) / "modules").string();
            m.toolchain = o.toolchain;
            m.workers = o.workers;
            m.memory_mb = o.memory_mb;
            m.qos_class = o.qos_class;
            m.key = o.key;
            m.identifier = o.identifier;
            m.thermal = o.thermal;
            m.progress = o.progress;
            m.cancel = o.cancel;
            const pspocket::prepare::ModulesResult mr = pspocket::prepare::prepare_game_modules(m);
            if (!mr.library.ok)
                PS_LOG_WARN(kChan, "{}: its own modules stay under the interpreter: {}", o.title_id, mr.library.error);
            else if (!mr.none)
                PS_LOG_INFO(kChan, "{}: {} of its own modules translated", o.title_id, mr.modules);
        }
        finish(r);
    }).detach();
    return 0;
#else
    (void)executable;
    (void)title_id;
    (void)game_dir;
    copy_text(error, error_size, "this build of pspocket has no compiler (scripts/build-llvm-ios.sh)");
    return 1;
#endif
}

extern "C" void ps_prepare_get_status(ps_prepare_status* out)
{
    if (!out) return;
    std::lock_guard lock(g_mutex);
    *out = g_status;
}

extern "C" void ps_prepare_cancel(void) { g_cancel = true; }

#if PSPOCKET_HAVE_CODEGEN
namespace {

// The firmware modules the game whose decrypted executable is at `path` links (LleModules::choice_for, D-191): the ones
// whose SPU programs the system library is to hold for it. Empty when it cannot be read.
std::set<std::string> modules_for(pspocket::loader::LleModules& lle, const char* path)
{
    std::set<std::string> out;
    if (!path) return out;
    std::ifstream in(path, std::ios::binary);
    std::vector<pspocket::u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto exe = pspocket::loader::Executable::parse(std::move(bytes));
    if (!exe) {
        PS_LOG_WARN(kChan, "{}: {}: no system modules worked out for it", path, exe.error().message);
        return out;
    }
    for (const std::string& file : lle.choice_for(pspocket::loader::LleModules::needs_of((*exe)->module())))
        out.insert(file);
    return out;
}

} // namespace
#endif

extern "C" int ps_prepare_system_check(const char* flash_dir, const char* system_dir, const char* executable, char* why,
    size_t why_size)
{
#if PSPOCKET_HAVE_CODEGEN
    if (!flash_dir || !system_dir) return 0;
    pspocket::loader::LleModules lle(flash_dir);
    std::string reason;
    if (!lle.index(&reason)) {
        copy_text(why, why_size, "no system software: " + reason);
        return 0;
    }
    const bool ok = pspocket::prepare::system_library_is_current(system_dir, lle.identity_text(), &reason,
        modules_for(lle, executable));
    copy_text(why, why_size, reason);
    return ok ? 1 : 0;
#else
    (void)flash_dir;
    (void)system_dir;
    (void)executable;
    copy_text(why, why_size, "this build of pspocket has no compiler");
    return 0;
#endif
}

extern "C" int ps_prepare_system_start(const char* flash_dir, const char* system_dir, const char* executable,
    char* error, size_t error_size)
{
#if PSPOCKET_HAVE_CODEGEN
    if (!flash_dir || !system_dir) {
        copy_text(error, error_size, "missing paths");
        return 1;
    }
    std::optional<pspocket::codesign::SigningKey> key;
    pspocket::prepare::SystemOptions o;
    if (!begin(system_dir, &key, &o.toolchain, error, error_size)) return 1;
    o.dev_flash = flash_dir;
    o.dir = system_dir;
    {
        pspocket::loader::LleModules lle(flash_dir);
        if (lle.index()) o.spu_modules = modules_for(lle, executable);
    }
    o.workers = std::max(1u, std::thread::hardware_concurrency());
    o.memory_mb = static_cast<double>(os_proc_available_memory()) / 1048576.0 * 0.5;
    NSString* bundle = [[NSBundle mainBundle] bundleIdentifier];
    o.identifier = bundle ? bundle.UTF8String : "com.artzymeri.pspocket.testhost";
    o.thermal = static_cast<int>([[NSProcessInfo processInfo] thermalState]);
    pspocket::prepare::Manifest last;
    if (pspocket::prepare::read_manifest(system_dir, &last) ||
        pspocket::prepare::read_manifest((fs::path(system_dir).parent_path() / "last-preparation").string(), &last)) {
        const auto rates = pspocket::prepare::prior_rates(&last, nullptr, o.thermal);
        for (int k = 0; k < 3; k++) o.prior_rates[k] = rates[static_cast<size_t>(k)];
    }
    o.progress = set_status;
    o.cancel = &g_cancel;
    PS_LOG_INFO(kChan, "the PS3 system software: translating its modules with {} workers", o.workers);
    std::thread([o = std::move(o), key = std::move(*key)]() mutable {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
        o.key = &key;
        finish(pspocket::prepare::prepare_system(o));
    }).detach();
    return 0;
#else
    (void)flash_dir;
    (void)system_dir;
    (void)executable;
    copy_text(error, error_size, "this build of pspocket has no compiler (scripts/build-llvm-ios.sh)");
    return 1;
#endif
}

namespace pspocket::ios {

std::shared_ptr<prepare::SessionTranslator> start_session_translator(const std::string& title_id,
    const std::string& images_dir, GuestMemory& memory)
{
#if PSPOCKET_HAVE_CODEGEN
    if (const char* off = std::getenv("PSPOCKET_SPU_TRANSLATE"); off && std::string(off) == "0") return nullptr;
    {
        std::lock_guard lock(g_mutex);
        if (g_running) { // the compiler is the preparation's (it holds compiler_mutex too)
            PS_LOG_INFO(kChan, "SPU programs are not translated during this session: a preparation is running");
            return nullptr;
        }
    }
    // The identity signs every library the session makes: read once, kept for the process.
    static std::optional<codesign::SigningKey> key;
    if (!key) {
        std::string why;
        key = signing_key(&why);
        if (!key) {
            PS_LOG_INFO(kChan, "SPU programs are not translated during this session: {}", why);
            return nullptr;
        }
    }
    const std::string codegen = resources() + "/codegen";
    std::error_code ec;
    if (!fs::exists(codegen + "/clang/include/arm_neon.h", ec)) return nullptr;
    prepare::SessionOptions o;
    o.images_dir = images_dir;
    o.work_dir = (fs::temp_directory_path() / "pspocket-session" / title_id).string();
    prepare::Toolchain& t = o.library.toolchain;
    t.resource_dir = codegen + "/clang";
    t.sysroot = codegen + "/sysroot";
    t.include_dirs = {codegen + "/include"};
    t.entry_source = codegen + "/library/library_entry.c";
    t.cpu = "native";
    o.library.key = &*key;
    NSString* bundle = [[NSBundle mainBundle] bundleIdentifier];
    o.library.identifier = bundle ? bundle.UTF8String : "com.artzymeri.pspocket.testhost";
    o.library.title_id = title_id;
    o.library.workers = 1; // one core, behind the game's threads
    o.library.qos_class = QOS_CLASS_UTILITY;
    // Programs that would compile for longer wait for the next launch's preparation (the game would run hot meanwhile).
    // Assassin's Creed's slowest took 5.4 s on the M4 (run 219).
    o.max_program_seconds = 60;
    // Not after a memory warning (for the rest of the session), not with under 2 GB left, not hot, not in Low Power
    // Mode, not while the app is in the background.
    static std::atomic<bool> warned{false}, active{true};
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    [center addObserverForName:UIApplicationDidReceiveMemoryWarningNotification object:nil queue:nil
                    usingBlock:^(NSNotification*) { warned = true; }];
    [center addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:nil
                    usingBlock:^(NSNotification*) { active = false; }];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil
                    usingBlock:^(NSNotification*) { active = true; }];
    o.may_compile = [] {
        NSProcessInfo* info = [NSProcessInfo processInfo];
        return !warned && active && !info.lowPowerModeEnabled && info.thermalState < NSProcessInfoThermalStateSerious &&
            os_proc_available_memory() > (2048ull << 20);
    };
    o.load = [&memory](const std::string& path, std::string* error) {
        const spu::RegistryBatch batch; // its programs land at once (D-231)
        game::LoadedLibrary out;
        return game::load_library(path, memory, &out, error);
    };
    // Under the game's fault handler, installed after this (D-236): the game's faults must not reach LLVM's.
    codegen::install_llvm_signal_handlers();
    auto translator = std::make_shared<prepare::SessionTranslator>(std::move(o));
    if (std::string error; !translator->start(&error)) {
        PS_LOG_WARN(kChan, "SPU programs are not translated during this session: {}", error);
        return nullptr;
    }
    PS_LOG_INFO(kChan, "SPU programs the game meets untranslated are translated during the session");
    return translator;
#else
    (void)title_id;
    (void)images_dir;
    (void)memory;
    return nullptr;
#endif
}

} // namespace pspocket::ios
