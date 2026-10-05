// Route B's go/no-go (D-147, D-150): does iOS let this app run code it did not ship with? Each library in the app's
// Library/Application Support/pspocket-codegen/ (pushed from the Mac: built by clang or by us, signed by Apple's
// codesign or by us) is loaded as LiveContainer loads its own: copied to a new file (the kernel caches a signature per
// file), its signature registered (F_ADDFILESIGS_RETURN), library validation asked (F_CHECK_LV), then dlopen, dlsym
// and a call that must return 42. With the development identity there too (identity.p12, identity.pass and the chain's
// .cer files, from scripts/ios.sh push-signing-identity), the app also writes the library itself, signs it and loads it:
// the case route B needs.

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "pspocket/codesign/codesign.hpp"
#include "pspocket/codesign/macho.hpp"
#include "probe.hpp"

namespace pspocket::selftest {
namespace {

namespace fs = std::filesystem;

bool read_file(const fs::path& p, std::vector<u8>& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Loads `path` the way route B will, reporting each step. Returns what ps_answer() returned, or -1.
int load_and_call(ProbeContext& ctx, const fs::path& path, const std::string& label)
{
    // A fresh copy: a file whose signature the kernel saw before keeps that signature.
    static int counter = 0;
    const fs::path copy = fs::temp_directory_path() / ("codegen-" + std::to_string(getpid()) + "-" +
        std::to_string(++counter) + ".dylib");
    std::error_code ec;
    fs::copy_file(path, copy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        ctx.info("{}: cannot copy: {}", label, ec.message());
        return -1;
    }
    int result = -1;
    std::string steps;
#if defined(__APPLE__)
    std::vector<u8> bytes;
    read_file(copy, bytes);
    const auto layout = codesign::read_layout(bytes);
    const int fd = open(copy.c_str(), O_RDONLY);
    if (fd >= 0 && layout) {
        // The signature's place in the file, as dyld passes it (LiveContainer's checkCodeSignature does the same).
        fsignatures_t sigs{};
        sigs.fs_file_start = 0;
        sigs.fs_blob_start = reinterpret_cast<void*>(static_cast<uintptr_t>(layout->signature_offset));
        sigs.fs_blob_size = layout->signature_size;
        const int add = fcntl(fd, F_ADDFILESIGS_RETURN, &sigs);
        steps += add == -1 ? std::string("F_ADDFILESIGS_RETURN refused (") + std::strerror(errno) + ")" :
                             "signature registered";
#if defined(F_CHECK_LV)
        fchecklv_t lv{};
        char message[1024] = {};
        lv.lv_file_start = 0;
        lv.lv_error_message_size = sizeof message;
        lv.lv_error_message = message;
        const int check = fcntl(fd, F_CHECK_LV, &lv);
        steps += check == -1 ? std::string(", library validation refused: ") + (message[0] ? message : std::strerror(errno)) :
                               ", library validation passed";
#endif
    }
    if (fd >= 0) close(fd);
#endif
    void* lib = dlopen(copy.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        const char* why = dlerror();
        ctx.info("{}: {}; dlopen refused: {}", label, steps, why ? why : "?");
    } else if (const auto fn = reinterpret_cast<int (*)()>(dlsym(lib, "ps_answer"))) {
        result = fn();
        ctx.info("{}: {}; loaded, ps_answer() = {}", label, steps, result);
    } else {
        ctx.info("{}: {}; loaded, but no ps_answer", label, steps);
    }
    return result;
}

} // namespace

void probe_codegen_dlopen(ProbeContext& ctx)
{
    const char* home = std::getenv("HOME");
    if (!home) {
        ctx.skip("no home directory");
        return;
    }
    const fs::path dir = fs::path(home) / "Library/Application Support/pspocket-codegen";
    std::error_code ec;
    int tried = 0, answered = 0;
    std::vector<fs::path> libraries;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".dylib") libraries.push_back(e.path());
    }
    std::sort(libraries.begin(), libraries.end());
    for (const fs::path& p : libraries) {
        tried++;
        if (load_and_call(ctx, p, p.filename().string()) == 42) answered++;
    }

    // The app's own library: written here, signed here with the development identity, loaded here.
    std::vector<u8> p12, pass;
    if (read_file(dir / "identity.p12", p12) && read_file(dir / "identity.pass", pass)) {
        std::string password(pass.begin(), pass.end());
        while (!password.empty() && (password.back() == '\n' || password.back() == '\r')) password.pop_back();
        std::vector<std::vector<u8>> chain;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::vector<u8> der;
            if (e.path().extension() == ".cer" && read_file(e.path(), der)) chain.push_back(std::move(der));
        }
        std::string error;
        const auto key = codesign::key_from_p12(p12, password, chain, &error);
        if (!key) {
            ctx.info("own library: the identity does not load: {}", error);
        } else {
#if defined(__APPLE__) && TARGET_OS_IPHONE
            const auto platform = codesign::Platform::IOS;
            const u32 min_os = 0x00110000;
#else
            const auto platform = codesign::Platform::MacOS;
            const u32 min_os = 0x000D0000;
#endif
            auto dylib = codesign::answer_dylib("ps_answer", 42, platform, min_os, "@rpath/own.dylib");
            codesign::SignOptions options;
            const char* bundle = std::getenv("PSPOCKET_BUNDLE_ID");
            options.identifier = bundle ? bundle : "com.artzymeri.pspocket.testhost";
            options.key = &*key;
            if (!codesign::sign(dylib, options, &error)) {
                ctx.info("own library: signing failed: {}", error);
            } else {
                const fs::path own = fs::temp_directory_path() / "codegen-own.dylib";
                std::ofstream(own, std::ios::binary | std::ios::trunc)
                    .write(reinterpret_cast<const char*>(dylib.data()), static_cast<std::streamsize>(dylib.size()));
                tried++;
                ctx.info("own library: {} bytes, team {}, {} certificates in the chain", dylib.size(), key->team_id,
                    key->chain.size());
                if (load_and_call(ctx, own, "own library (written and signed on this device)") == 42) answered++;
            }
        }
    }

    if (tried == 0) {
        ctx.skip("no libraries in Library/Application Support/pspocket-codegen (scripts/codegen-probe.sh pushes them)");
        return;
    }
    ctx.metric("libraries_loaded = {} of {}", answered, tried);
    ctx.set_summary(std::format("{} of {} libraries ran", answered, tried));
    if (answered == 0) ctx.fail("iOS ran none of them");
}

} // namespace pspocket::selftest

#if PSPOCKET_HAVE_CODEGEN
#include <atomic>
#include <chrono>
#include <thread>

#include "pspocket/codegen/toolchain.hpp"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#endif

namespace pspocket::selftest {
namespace {

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

// The highest footprint while it lives (sampled every 5 ms): what a compile costs at its peak, which is what iOS's
// memory limit sees.
class PeakFootprint {
public:
    PeakFootprint() : peak_(footprint_mb()), sampler_([this] {
        while (!stop_.load(std::memory_order_relaxed)) {
            peak_ = std::max(peak_.load(), footprint_mb());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }) {}
    ~PeakFootprint() { finish(); }
    double finish()
    {
        if (sampler_.joinable()) {
            stop_ = true;
            sampler_.join();
        }
        return std::max(peak_.load(), footprint_mb());
    }

private:
    std::atomic<double> peak_;
    std::atomic<bool> stop_{false};
    std::thread sampler_;
};

// Where the app keeps what the compiler reads: clang's headers, our sysroot, the runtime's ABI headers (the bundle's
// codegen folder on the phone; the source tree and the LLVM build on the Mac).
struct CodegenPaths {
    std::string resource_dir, sysroot;
    std::vector<std::string> runtime_include; // the runtime's ABI headers the translated code includes
};

CodegenPaths codegen_paths()
{
    CodegenPaths p;
#if defined(__APPLE__)
    // The app: its bundle's codegen folder (scripts/ios.sh puts clang's headers, the sysroot and the ABI headers
    // there).
    if (CFBundleRef bundle = CFBundleGetMainBundle()) {
        if (CFURLRef url = CFBundleCopyResourcesDirectoryURL(bundle)) {
            char path[1024] = {};
            if (CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(path), sizeof path)) {
                const std::string codegen = std::string(path) + "/codegen";
                std::error_code ec;
                if (fs::exists(codegen + "/clang/include/arm_neon.h", ec)) {
                    p.resource_dir = codegen + "/clang";
                    p.sysroot = codegen + "/sysroot";
                    p.runtime_include = {codegen + "/include"};
                }
            }
            CFRelease(url);
        }
    }
#endif
#if defined(PSPOCKET_CODEGEN_RESOURCE_DIR) && defined(PSPOCKET_CODEGEN_SYSROOT) && defined(PSPOCKET_SOURCE_DIR)
    if (p.resource_dir.empty()) { // the Mac: the LLVM build and the source tree
        p.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
        p.sysroot = PSPOCKET_CODEGEN_SYSROOT;
        p.runtime_include = {PSPOCKET_SOURCE_DIR "/runtime/ppu/include", PSPOCKET_SOURCE_DIR "/runtime/spu/include",
            PSPOCKET_SOURCE_DIR "/runtime/core/include"};
    }
#endif
    return p;
}

// The whole chain on a small program: compile, link, sign (with the development identity when the app has it), load
// and call. Returns what the program returned, or -1.
int small_program_chain(ProbeContext& ctx, const codegen::CompileOptions& base, const CodegenPaths& paths,
    const fs::path& work, const fs::path& dir, const std::string& platform, const std::string& min_os)
{
    std::error_code ec;
    const fs::path source = work / "answer.c", object = work / "answer.o", library = work / "answer.dylib";
    std::ofstream(source) << "#include <string.h>\n#include <math.h>\n"
                             "int ps_answer(void) { char b[4] = {42, 0, 0, 0}; int v; memcpy(&v, b, 4);"
                             " return v + (int)sqrt(0.0); }\n";
    std::string diagnostics;
    const auto t0 = std::chrono::steady_clock::now();
    if (!codegen::compile_c(source.string(), object.string(), base, &diagnostics)) {
        ctx.fail("the small program did not compile: {}", diagnostics.substr(0, 300));
        return -1;
    }
    codegen::LinkOptions link;
    link.platform = platform;
    link.min_os = min_os;
    link.exports = {"_ps_answer"};
    link.libraries = {paths.sysroot + "/lib/libSystem.tbd"};
    if (!codegen::link_dylib({object.string()}, library.string(), link, &diagnostics)) {
        ctx.fail("the small program did not link: {}", diagnostics.substr(0, 300));
        return -1;
    }
    const double chain_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ctx.metric("small_program_compile_and_link = {:.0f} ms", chain_ms);
    std::vector<u8> bytes;
    read_file(library, bytes);
    std::vector<u8> p12, pass;
    std::optional<codesign::SigningKey> key;
    if (read_file(dir / "identity.p12", p12) && read_file(dir / "identity.pass", pass)) {
        std::string password(pass.begin(), pass.end());
        while (!password.empty() && (password.back() == '\n' || password.back() == '\r')) password.pop_back();
        std::vector<std::vector<u8>> chain;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::vector<u8> der;
            if (e.path().extension() == ".cer" && read_file(e.path(), der)) chain.push_back(std::move(der));
        }
        key = codesign::key_from_p12(p12, password, chain, &diagnostics);
    }
    codesign::SignOptions sign;
    const char* bundle_id = std::getenv("PSPOCKET_BUNDLE_ID");
    sign.identifier = bundle_id ? bundle_id : "com.artzymeri.pspocket.testhost";
    sign.key = key ? &*key : nullptr;
    if (!codesign::sign(bytes, sign, &diagnostics)) {
        ctx.fail("signing failed: {}", diagnostics);
        return -1;
    }
    const fs::path signed_lib = work / "answer-signed.dylib";
    std::ofstream(signed_lib, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return load_and_call(ctx, signed_lib, "compiled, linked and signed here");
}

} // namespace

// Stage 2's measurement on the device (D-151): the in-process compiler on real translated code (a PPU unit and an SPU
// program the Mac pushed: scripts/codegen-bench.sh) and the whole chain on a small program (compile, link, sign, load).
void probe_codegen_compile(ProbeContext& ctx)
{
    const CodegenPaths paths = codegen_paths();
    if (paths.resource_dir.empty()) {
        ctx.skip("no compiler headers (the app's codegen folder)");
        return;
    }
    const fs::path work = fs::temp_directory_path() / ("codegen-compile-" + std::to_string(getpid()));
    std::error_code ec;
    fs::create_directories(work, ec);
#if defined(__APPLE__) && TARGET_OS_IPHONE
    const std::string target = "arm64-apple-ios17.0", platform = "ios", min_os = "17.0", cpu = "apple-a12";
#else
    const std::string target = "arm64-apple-macos13.0", platform = "macos", min_os = "13.0", cpu = "apple-m1";
#endif
    codegen::CompileOptions base;
    base.target = target;
    base.cpu = cpu;
    base.resource_dir = paths.resource_dir;
    base.system_dirs = {paths.sysroot + "/include"};
    base.include_dirs = paths.runtime_include;
    const fs::path dir = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") /
        "Library/Application Support/pspocket-codegen";
    const int answer = small_program_chain(ctx, base, paths, work, dir, platform, min_os);
    ctx.check(answer == 42, "the compiled program did not return 42");
    std::string diagnostics;
    auto t0 = std::chrono::steady_clock::now();

    // Real translated code, when the Mac pushed some (bench/<name>/<name>.c with its headers beside it): each file
    // alone at -O1 and -O2 (seconds, MB of C per second, peak memory), then the first file on 2, 4 and 6 threads at
    // once (how far the phone's 2 performance and 4 efficiency cores carry a whole game).
    std::vector<fs::path> sources;
    for (const auto& d : fs::directory_iterator(dir / "bench", ec))
        for (const auto& e : fs::directory_iterator(d.path(), ec))
            if (e.path().extension() == ".c") sources.push_back(e.path());
    std::sort(sources.begin(), sources.end());
    auto options_for = [&](const fs::path& file, int level) {
        codegen::CompileOptions o = base;
        o.include_dirs.push_back(file.parent_path().string());
        o.opt_level = level;
        return o;
    };
    for (const fs::path& file : sources) {
        const double mb = static_cast<double>(fs::file_size(file, ec)) / 1048576.0;
        for (const int level : {1, 2}) {
            const double before = footprint_mb();
            PeakFootprint peak;
            t0 = std::chrono::steady_clock::now();
            const bool ok = codegen::compile_c(file.string(), (work / "bench.o").string(),
                options_for(file, level), &diagnostics);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            ctx.metric("compile {} ({:.1f} MB of C) -O{} = {:.2f} s ({:.2f} MB/s){}, memory {:.0f} MB, peak +{:.0f} MB",
                file.filename().string(), mb, level, seconds, mb / seconds, ok ? "" : " FAILED", before,
                peak.finish() - before);
            if (!ok) ctx.info("{}", diagnostics.substr(0, 300));
        }
    }
    if (!sources.empty()) {
        const fs::path& file = sources.front();
        const double mb = static_cast<double>(fs::file_size(file, ec)) / 1048576.0;
        for (const int threads : {2, 4, 6}) {
            const double before = footprint_mb();
            PeakFootprint peak;
            std::atomic<int> failed{0};
            t0 = std::chrono::steady_clock::now();
            std::vector<std::thread> workers;
            for (int i = 0; i < threads; ++i)
                workers.emplace_back([&, i] {
                    std::string d;
                    if (!codegen::compile_c(file.string(), (work / std::format("bench-{}.o", i)).string(),
                            options_for(file, 2), &d))
                        ++failed;
                });
            for (std::thread& w : workers) w.join();
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            ctx.metric("{} at once ({}, -O2) = {:.2f} s, {:.2f} MB of C per second{}, peak +{:.0f} MB", threads,
                file.filename().string(), seconds, mb * threads / seconds,
                failed ? std::format(", {} FAILED", failed.load()) : "", peak.finish() - before);
        }
    }
    fs::remove_all(work, ec);
    ctx.set_summary(std::format("compile + link + sign + load: {}", answer == 42 ? "ok" : "failed"));
}

} // namespace pspocket::selftest
#endif
