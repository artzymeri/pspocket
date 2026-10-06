// Route B's signer (D-150) against Apple's own tools on the Mac: the minimal library it writes, signed ad hoc, passes
// codesign --verify and loads with dlopen; signed with a (self-signed) identity, codesign reads its team, authority,
// identifier and designated requirement back, and Security.framework's CMS decoder verifies the signature over the
// CodeDirectory.

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <unistd.h>

#if defined(__APPLE__)
#include <Security/CMSDecoder.h>
#include <Security/Security.h>
#endif

#include "pspocket/codesign/codesign.hpp"
#include "pspocket/codesign/macho.hpp"

using namespace pspocket;
using namespace pspocket::codesign;

namespace {

[[maybe_unused]] std::string temp_path(const std::string& name)
{
    return (std::filesystem::temp_directory_path() / std::format("pspocket-codesign-{}-{}", getpid(), name)).string();
}

[[maybe_unused]] void write(const std::string& path, const std::vector<u8>& data)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

// A command's exit status and its output (stdout and stderr).
[[maybe_unused]] std::pair<int, std::string> run(const std::string& command)
{
    std::string out;
    FILE* p = popen((command + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[512];
    while (fgets(buf, sizeof buf, p)) out += buf;
    const int status = pclose(p);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, out};
}

u32 be32(const std::vector<u8>& b, usize at)
{
    return static_cast<u32>(b[at]) << 24 | static_cast<u32>(b[at + 1]) << 16 | static_cast<u32>(b[at + 2]) << 8 | b[at + 3];
}

// The blob in slot `slot` of the file's code signature (its bytes, with its own header).
[[maybe_unused]] std::vector<u8> signature_slot(const std::vector<u8>& file, u32 slot)
{
    const auto l = read_layout(file);
    if (!l) return {};
    const usize base = l->signature_offset;
    for (u32 i = 0; i < be32(file, base + 8); i++) {
        if (be32(file, base + 12 + i * 8) != slot) continue;
        const usize at = base + be32(file, base + 16 + i * 8);
        return std::vector<u8>(file.begin() + static_cast<std::ptrdiff_t>(at),
            file.begin() + static_cast<std::ptrdiff_t>(at + be32(file, at + 4)));
    }
    return {};
}

} // namespace

#if defined(__APPLE__) && defined(__aarch64__)

TEST_CASE("codesign: the minimal library, signed ad hoc, passes codesign --verify and returns its answer")
{
    auto dylib = answer_dylib("ps_answer", 42, Platform::MacOS, 0x000D0000, "@rpath/answer.dylib");
    std::string error;
    REQUIRE(sign(dylib, {.identifier = "com.example.answer"}, &error));
    const std::string path = temp_path("answer.dylib");
    write(path, dylib);
    const auto [status, out] = run("/usr/bin/codesign --verify --verbose=4 " + path);
    INFO(out);
    CHECK(status == 0);
    void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    INFO((lib ? "" : dlerror()));
    REQUIRE(lib);
    const auto fn = reinterpret_cast<int (*)()>(dlsym(lib, "ps_answer"));
    REQUIRE(fn);
    CHECK(fn() == 42);
    std::filesystem::remove(path);
}

TEST_CASE("codesign: signing a signed file again gives a valid file of the same answer")
{
    auto dylib = answer_dylib("ps_answer", 7, Platform::MacOS, 0x000D0000, "@rpath/answer7.dylib");
    REQUIRE(sign(dylib, {.identifier = "com.example.one"}));
    REQUIRE(sign(dylib, {.identifier = "com.example.two"}));
    const std::string path = temp_path("answer7.dylib");
    write(path, dylib);
    const auto [status, out] = run("/usr/bin/codesign --verify --verbose=4 " + path);
    INFO(out);
    CHECK(status == 0);
    const auto [dstatus, details] = run("/usr/bin/codesign -dvvv " + path);
    CHECK(details.find("Identifier=com.example.two") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("codesign: with an identity, codesign reads the team, authority and requirement, and the CMS verifies")
{
    std::string error;
    const auto key = key_for_tests("TESTTEAM01", &error);
    INFO(error);
    REQUIRE(key);
    auto dylib = answer_dylib("ps_answer", 42, Platform::IOS, 0x00110000, "@rpath/answer.dylib");
    REQUIRE(sign(dylib, {.identifier = "com.example.answer", .key = &*key}, &error));
    const std::string path = temp_path("answer-signed.dylib");
    write(path, dylib);

    const auto [status, out] = run("/usr/bin/codesign -dvvvv -r- " + path);
    INFO(out);
    CHECK(status == 0);
    CHECK(out.find("Identifier=com.example.answer") != std::string::npos);
    CHECK(out.find("TeamIdentifier=TESTTEAM01") != std::string::npos);
    // The authority is "(unavailable)" for a certificate no root vouches for; the CMS check below covers the signer.
    CHECK(out.find("Signature size=") != std::string::npos);
    CHECK(out.find("Hash type=sha256") != std::string::npos);
    CHECK(out.find("designated => identifier \"com.example.answer\" and anchor apple generic and certificate "
                   "leaf[subject.CN] = \"pspocket test\" and certificate 1[field.1.2.840.113635.100.6.2.1] /* exists */") !=
        std::string::npos);
    const auto cdhash = code_directory_hash(dylib);
    REQUIRE(cdhash);
    std::string hex;
    for (int i = 0; i < 20; i++) hex += std::format("{:02x}", (*cdhash)[i]);
    CHECK(out.find("CDHash=" + hex) != std::string::npos);

    // The CMS: the signer's signature over the CodeDirectory checks out (trust is not evaluated: a test certificate).
    const auto cms_blob = signature_slot(dylib, 0x10000);
    const auto cd_blob = signature_slot(dylib, 0);
    REQUIRE(cms_blob.size() > 8);
    REQUIRE(cd_blob.size() > 8);
    CMSDecoderRef decoder = nullptr;
    REQUIRE(CMSDecoderCreate(&decoder) == errSecSuccess);
    CFDataRef detached = CFDataCreate(nullptr, cd_blob.data(), static_cast<CFIndex>(cd_blob.size()));
    CHECK(CMSDecoderSetDetachedContent(decoder, detached) == errSecSuccess);
    CHECK(CMSDecoderUpdateMessage(decoder, cms_blob.data() + 8, cms_blob.size() - 8) == errSecSuccess);
    CHECK(CMSDecoderFinalizeMessage(decoder) == errSecSuccess);
    size_t signers = 0;
    CHECK(CMSDecoderGetNumSigners(decoder, &signers) == errSecSuccess);
    CHECK(signers == 1);
    CMSSignerStatus signer = kCMSSignerUnsigned;
    SecPolicyRef policy = SecPolicyCreateBasicX509();
    CHECK(CMSDecoderCopySignerStatus(decoder, 0, policy, false, &signer, nullptr, nullptr) == errSecSuccess);
    CHECK(signer == kCMSSignerValid);
    CFRelease(policy);
    CFRelease(detached);
    CFRelease(decoder);
    std::filesystem::remove(path);
}

#else

TEST_CASE("codesign: the minimal library has a code signature for sign() to fill")
{
    auto dylib = answer_dylib("ps_answer", 42, Platform::IOS, 0x00110000, "@rpath/answer.dylib");
    REQUIRE(read_layout(dylib));
    REQUIRE(sign(dylib, {.identifier = "com.example.answer"}));
    CHECK(code_directory_hash(dylib));
}

#endif
