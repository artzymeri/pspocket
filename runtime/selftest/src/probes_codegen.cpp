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
