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
#include <string>
#include <thread>
#include <vector>

#include "pspocket/core/log.hpp"

#if PSPOCKET_HAVE_CODEGEN
#include "pspocket/codesign/codesign.hpp"
#include "pspocket/prepare/prepare.hpp"
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
    g_status.phase = p.phase == Phase::Translating ? PS_PREPARE_TRANSLATING
        : p.phase == Phase::Compiling             ? PS_PREPARE_COMPILING
        : p.phase == Phase::Linking               ? PS_PREPARE_LINKING
        : p.phase == Phase::Signing               ? PS_PREPARE_SIGNING
        : p.phase == Phase::Done                  ? PS_PREPARE_DONE
                                                  : PS_PREPARE_FAILED;
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
    const bool ok = !sha.empty() && pspocket::prepare::library_is_current(game_dir, sha, &reason, spu);
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
    std::string why;
    auto key = signing_key(&why);
    if (!key) {
        copy_text(error, error_size, why);
        return 1;
    }
    const std::string codegen = resources() + "/codegen";
    std::error_code ec;
    if (!fs::exists(codegen + "/clang/include/arm_neon.h", ec)) {
        copy_text(error, error_size, "this build of pspocket has no compiler headers");
        return 1;
    }
    {
        std::lock_guard lock(g_mutex);
        if (g_running) {
            copy_text(error, error_size, "a game is being prepared already");
            return 1;
        }
        g_running = true;
        g_status = {};
        std::error_code e;
        fs::create_directories(game_dir, e);
        if (g_trace) std::fclose(g_trace);
        g_trace = std::fopen((fs::path(game_dir) / "prepare-progress.csv").c_str(), "w");
        if (g_trace) std::fprintf(g_trace, "seconds,phase,fraction,seconds_left,files_done,files_total\n");
        g_traced_at = -10;
        g_status.phase = PS_PREPARE_TRANSLATING;
        g_status.seconds_left = -1;
        copy_text(g_status.detail, sizeof g_status.detail, "Starting");
    }
    g_cancel = false;

    pspocket::prepare::Options o;
    o.executable = executable;
    o.title_id = title_id;
    o.game_dir = game_dir;
    o.toolchain.resource_dir = codegen + "/clang";
    o.toolchain.sysroot = codegen + "/sysroot";
    o.toolchain.include_dirs = {codegen + "/include"};
    o.toolchain.entry_source = codegen + "/library/library_entry.c";
    o.toolchain.cpu = "native"; // compiled on this phone, for this phone
    // The SPU programs the game met without a translation (saved while it ran), with the misses for more entries.
    const std::string coverage = coverage_dir(title_id);
    o.spu_images_dir = coverage + "/spu";
    o.spu_logs = {coverage + "/spu-misses.log"};
    // Every core; compiles start while their predicted peaks fit in half of what iOS lets the app have now.
    o.workers = std::max(1u, std::thread::hardware_concurrency());
    o.memory_mb = static_cast<double>(os_proc_available_memory()) / 1048576.0 * 0.5;
    NSString* bundle = [[NSBundle mainBundle] bundleIdentifier];
    o.identifier = bundle ? bundle.UTF8String : "com.artzymeri.pspocket.testhost"; // library validation wants the app's
    // How fast this phone compiled last time, per kind of file (this game's manifest, else the last game's).
    pspocket::prepare::Manifest last;
    if (pspocket::prepare::read_manifest(game_dir, &last) ||
        pspocket::prepare::read_manifest((fs::path(game_dir).parent_path() / "last-preparation").string(), &last))
        for (int k = 0; k < 3; k++) o.prior_rates[k] = last.rates[k];
    o.progress = set_status;
    o.cancel = &g_cancel;
    PS_LOG_INFO(kChan, "{}: preparing with {} workers, {:.0f} MB for compiles, rates {:.2f}/{:.2f}", title_id, o.workers,
        o.memory_mb, o.prior_rates[0], o.prior_rates[1]);

    std::thread([o = std::move(o), key = std::move(*key)]() mutable {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0); // the compiles it starts inherit it
        o.key = &key;
        const pspocket::prepare::Result r = pspocket::prepare::prepare_game(o);
        if (r.ok) { // the rates for the next game's time left
            std::error_code e;
            const fs::path last_dir = fs::path(o.game_dir).parent_path() / "last-preparation";
            fs::create_directories(last_dir, e);
            fs::copy_file(fs::path(o.game_dir) / "manifest.json", last_dir / "manifest.json",
                fs::copy_options::overwrite_existing, e);
        }
        std::lock_guard lock(g_mutex);
        g_running = false;
        if (g_trace) {
            std::fprintf(g_trace, "# %s in %.1f s (translate %.1f, compile %.1f, link %.1f, sign %.1f), %zu files, rates "
                                  "%.3f %.3f %.3f%s%s\n",
                r.ok ? "done" : "failed", r.seconds, r.translate_seconds, r.compile_seconds, r.link_seconds,
                r.sign_seconds, r.files_compiled, r.rates[0], r.rates[1], r.rates[2], r.ok ? "" : ": ",
                r.ok ? "" : r.error.c_str());
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
