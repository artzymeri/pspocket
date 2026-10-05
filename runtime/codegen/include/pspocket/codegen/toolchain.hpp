#pragma once

// Compiling and linking on the device (route B, D-147; stage 2, D-151): the lifters' C compiled to arm64 objects by
// clang, and the objects linked into a library by lld, both in this process from LLVM's libraries (iOS runs no other
// programs). The library is then signed (runtime/codesign, D-150) and loaded.
//
// The phone has no SDK: the code compiles against clang's own headers (stdint.h, stddef.h, arm_neon.h: the resource
// directory, shipped with the app) and runtime/codegen/sysroot (string.h and math.h, the few declarations the
// translated code needs), and links against sysroot/lib/libSystem.tbd (the symbols it uses, by name).

#include <string>
#include <vector>

namespace pspocket::codegen {

struct CompileOptions {
    std::string target = "arm64-apple-ios17.0";
    std::string cpu = "apple-a12";          // the oldest chip iOS 17 runs on
    std::string resource_dir;               // clang's resource directory (its include/ holds stdint.h, arm_neon.h)
    std::vector<std::string> system_dirs;   // -isystem: runtime/codegen/sysroot/include
    std::vector<std::string> include_dirs;  // -I: the runtime's ABI headers, the game's generated headers
    std::vector<std::string> defines;       // -D
    std::vector<std::string> extra_args;    // more of clang's driver arguments (-fno-slp-vectorize)
    int opt_level = 2;
};

// The compiler's name and version ("LLVM 22.1.8"): part of what names a kept object.
std::string compiler_version();

// LLVM's own options (what `clang -mllvm X` sets), for every compile after this call. They are process-wide: call it
// before any compile starts, not while one runs.
bool set_llvm_options(const std::vector<std::string>& options, std::string* diagnostics = nullptr);

// One C file to an object file. Thread-safe: each call has its own compiler and LLVM context.
bool compile_c(const std::string& source, const std::string& object, const CompileOptions& options,
    std::string* diagnostics = nullptr);

struct LinkOptions {
    std::string platform = "ios"; // ios or macos
    std::string min_os = "17.0";
    std::string install_name = "@rpath/game.dylib";
    std::vector<std::string> exports;   // the only exported symbols (C names with the leading underscore)
    std::vector<std::string> libraries; // .tbd stubs to link against (sysroot/lib/libSystem.tbd)
};

// Objects to a dynamic library (lld's Mach-O linker). The output carries lld's ad hoc signature for codesign::sign to
// replace. Not thread-safe: one link at a time.
bool link_dylib(const std::vector<std::string>& objects, const std::string& output, const LinkOptions& options,
    std::string* diagnostics = nullptr);

} // namespace pspocket::codegen
