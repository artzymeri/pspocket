// Route B's compiler (D-151) on the Mac, as the phone will use it: C compiled in this process by clang against only
// clang's own headers and runtime/codegen/sysroot (no SDK), linked by lld against sysroot/lib/libSystem.tbd, signed by
// runtime/codesign, loaded and called.

#include <catch_amalgamated.hpp>

#include <dlfcn.h>
#include <unistd.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pspocket/codegen/toolchain.hpp"
#include "pspocket/codesign/codesign.hpp"
#include "pspocket/codesign/macho.hpp"

using namespace pspocket;

namespace {

namespace fs = std::filesystem;

fs::path temp_dir()
{
    const fs::path d = fs::temp_directory_path() / std::format("pspocket-codegen-{}", getpid());
    fs::create_directories(d);
    return d;
}

// What the translated code leans on: memcpy, the maths library, NEON vectors, fixed-width integers.
constexpr const char* kSource = R"(
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <arm_neon.h>

static uint32_t load_be32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }

int ps_answer(void)
{
    const uint8_t bytes[4] = {0, 0, 0, 40};
    float32x4_t v = vdupq_n_f32(1.0f);
    v = vaddq_f32(v, v); /* 2.0 in every lane */
    return (int)load_be32(bytes) + (int)vgetq_lane_f32(v, 3) + (int)sqrt(0.25) + (int)exp2f(0.0f) - 1;
}
)";

} // namespace

TEST_CASE("codegen: C compiled and linked in process against the phone's headers loads and runs")
{
    const fs::path dir = temp_dir();
    const fs::path source = dir / "answer.c", object = dir / "answer.o", library = dir / "answer.dylib";
    std::ofstream(source) << kSource;

    codegen::CompileOptions c;
    c.target = "arm64-apple-macos13.0";
    c.cpu = "apple-m1";
    c.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
    c.system_dirs = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/include"};
    std::string diagnostics;
    const bool compiled = codegen::compile_c(source.string(), object.string(), c, &diagnostics);
    INFO(diagnostics);
    REQUIRE(compiled);

    codegen::LinkOptions l;
    l.platform = "macos";
    l.min_os = "13.0";
    l.install_name = "@rpath/answer.dylib";
    l.exports = {"_ps_answer"};
    l.libraries = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/lib/libSystem.tbd"};
    const bool linked = codegen::link_dylib({object.string()}, library.string(), l, &diagnostics);
    INFO(diagnostics);
    REQUIRE(linked);

    // Signed as the phone signs it (ad hoc here: the Mac runs that).
    std::ifstream in(library, std::ios::binary);
    std::vector<u8> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    std::string error;
    REQUIRE(codesign::sign(bytes, {.identifier = "com.example.codegen"}, &error));
    const fs::path signed_path = dir / "answer-signed.dylib";
    std::ofstream(signed_path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));

    void* lib = dlopen(signed_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    INFO((lib ? "" : dlerror()));
    REQUIRE(lib);
    const auto fn = reinterpret_cast<int (*)()>(dlsym(lib, "ps_answer"));
    REQUIRE(fn);
    CHECK(fn() == 42);
    fs::remove_all(dir);
}

// Run 092: clang::createInvocation adds -fsyntax-only, which drops -o, so the object was written beside the source (a
// read-only folder on the phone). The object must land where asked, in another directory, and nowhere else.
TEST_CASE("codegen: the object goes where -o says, not beside the source")
{
    const fs::path dir = temp_dir();
    fs::create_directories(dir / "src");
    fs::create_directories(dir / "out");
    const fs::path source = dir / "src" / "unit.c", object = dir / "out" / "elsewhere.o";
    std::ofstream(source) << "int ps_unit(int x) { return x * 3; }\n";
    codegen::CompileOptions c;
    c.target = "arm64-apple-ios17.0";
    c.cpu = "apple-a12";
    c.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
    c.system_dirs = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/include"};
    std::string diagnostics;
    const bool compiled = codegen::compile_c(source.string(), object.string(), c, &diagnostics);
    INFO(diagnostics);
    REQUIRE(compiled);
    CHECK(fs::file_size(object) > 0);
    CHECK_FALSE(fs::exists(dir / "src" / "unit.o"));
    CHECK(diagnostics.find("warning") == std::string::npos); // no "argument unused during compilation: '-c'"
    fs::remove_all(dir);
}

// Run 092: lld reserves a code signature unasked only for macOS; an iOS library must have one for codesign::sign.
TEST_CASE("codegen: a library linked for iOS has room for its signature")
{
    const fs::path dir = temp_dir();
    const fs::path source = dir / "ios.c", object = dir / "ios.o", library = dir / "ios.dylib";
    std::ofstream(source) << "int ps_answer(void) { return 42; }\n";
    codegen::CompileOptions c;
    c.target = "arm64-apple-ios17.0";
    c.cpu = "apple-a12";
    c.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
    c.system_dirs = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/include"};
    std::string diagnostics;
    REQUIRE(codegen::compile_c(source.string(), object.string(), c, &diagnostics));
    codegen::LinkOptions l; // platform ios, as on the phone
    l.exports = {"_ps_answer"};
    l.libraries = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/lib/libSystem.tbd"};
    const bool linked = codegen::link_dylib({object.string()}, library.string(), l, &diagnostics);
    INFO(diagnostics);
    REQUIRE(linked);
    std::ifstream in(library, std::ios::binary);
    std::vector<u8> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto layout = codesign::read_layout(bytes);
    REQUIRE(layout);
    CHECK(layout->code_signature_cmd != 0);
    std::string error;
    CHECK(codesign::sign(bytes, {.identifier = "com.example.codegen"}, &error));
    fs::remove_all(dir);
}

// pspocket-prepare's first GTA V build: an empty define made "-D" take the next argument ("-fvisibility=hidden") as a
// macro name, and the file failed. Empty defines and include directories are skipped.
TEST_CASE("codegen: empty defines and include directories are skipped")
{
    const fs::path dir = temp_dir();
    const fs::path source = dir / "plain.c", object = dir / "plain.o";
    std::ofstream(source) << "int ps_plain(void) { return 7; }\n";
    codegen::CompileOptions c;
    c.target = "arm64-apple-macos13.0";
    c.cpu = "apple-m1";
    c.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
    c.system_dirs = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/include"};
    c.defines = {""};
    c.include_dirs = {""};
    c.extra_args = {"-fvisibility=hidden"};
    std::string diagnostics;
    const bool compiled = codegen::compile_c(source.string(), object.string(), c, &diagnostics);
    INFO(diagnostics);
    CHECK(compiled);
    fs::remove_all(dir);
}
