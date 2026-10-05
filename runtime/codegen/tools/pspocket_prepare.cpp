// pspocket-prepare: makes a game library (route B, D-152) from a game's translated C on the Mac, with the compiler,
// the build and the library entry the phone uses (runtime/codegen, runtime/game), so the gate runs what the phone
// will run.
//
//   pspocket-prepare --title ID --exe EBOOT.elf [--ppu DIR] [--spu DIR] --out game.dylib [--objects DIR]
//                    [-j N] [--memory MB] [-O L] [-X ARG]... [--ppu-arg ARG]... [--spu-arg ARG]...
//                    [--mllvm OPT]... [--quiet]
//
// --ppu is ppu-lift's output (--name game_ppu), --spu scripts/lift-spu.sh's; --exe the decrypted executable they were
// translated from (its SHA-256 goes into the library). Objects are kept in --objects (default: beside the library),
// so a second run compiles only what changed. -X passes an argument to clang for every file, --ppu-arg and --spu-arg
// for PPU or SPU files only (-fno-slp-vectorize); --mllvm sets one of LLVM's options for all of them. Progress, the
// time left and, at the end, how right the time left was.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

#include "pspocket/codegen/build.hpp"
#include "pspocket/codegen/toolchain.hpp"
#include "pspocket/codesign/codesign.hpp"
#include "pspocket/game/host.hpp"
#include "pspocket/recomp/hash.hpp"

namespace fs = std::filesystem;
using namespace pspocket;

namespace {

int usage()
{
    std::fputs("usage: pspocket-prepare --title ID --exe EBOOT.elf [--ppu DIR] [--spu DIR] --out game.dylib\n"
               "                        [--objects DIR] [-j N] [--memory MB] [-O L] [-X ARG]... [--ppu-arg ARG]...\n"
               "                        [--spu-arg ARG]... [--mllvm OPT]... [--quiet]\n",
        stderr);
    return 64;
}

bool read_bytes(const fs::path& p, std::string& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// FNV-1a over the headers in a directory: part of the object's name (BuildJob::defines), so a unit whose game header
// changed is compiled again although its own text did not.
std::string headers_hash(const fs::path& dir)
{
    std::vector<fs::path> headers;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".h") headers.push_back(e.path());
    std::sort(headers.begin(), headers.end());
    uint64_t h = 0xcbf29ce484222325ull;
    for (const fs::path& p : headers) {
        std::string text;
        read_bytes(p, text);
        for (const char c : text) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
    }
    return std::format("PSPOCKET_HEADERS_HASH=0x{:016x}ull", h);
}

std::vector<std::string> g_ppu_args, g_spu_args; // --ppu-arg, --spu-arg

codegen::BuildJob job_for(const fs::path& source, bool spu, const std::string& headers)
{
    std::string text;
    read_bytes(source, text);
    const codegen::CostEstimate e = codegen::estimate_cost(text, spu);
    codegen::BuildJob j;
    j.source = source.string();
    j.include_dirs = {source.parent_path().string()};
    if (!headers.empty()) j.defines = {headers};
    j.extra_args = spu ? g_spu_args : g_ppu_args;
    j.cost = e.seconds;
    j.memory_mb = e.memory_mb;
    return j;
}

std::string clock_text(double s)
{
    const long t = static_cast<long>(s + 0.5);
    return t >= 3600 ? std::format("{}:{:02}:{:02}", t / 3600, t / 60 % 60, t % 60) : std::format("{}:{:02}", t / 60, t % 60);
}

} // namespace

int main(int argc, char** argv)
{
    std::string title, exe, ppu, spu, out, objects;
    unsigned jobs = 0;
    double memory_mb = 0;
    int opt = 2;
    bool quiet = false;
    std::vector<std::string> extra, llvm_options;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--quiet") quiet = true;
        else if (a.starts_with("-O") && a.size() == 3) opt = a[2] - '0';
        else if (!(v = value())) return usage();
        else if (a == "--title") title = v;
        else if (a == "--exe") exe = v;
        else if (a == "--ppu") ppu = v;
        else if (a == "--spu") spu = v;
        else if (a == "--out") out = v;
        else if (a == "--objects") objects = v;
        else if (a == "-j") jobs = static_cast<unsigned>(std::max(1, std::atoi(v)));
        else if (a == "--memory") memory_mb = std::atof(v);
        else if (a == "-X") extra.emplace_back(v);
        else if (a == "--ppu-arg") g_ppu_args.emplace_back(v);
        else if (a == "--spu-arg") g_spu_args.emplace_back(v);
        else if (a == "--mllvm") llvm_options.emplace_back(v);
        else return usage();
    }
    if (title.empty() || exe.empty() || out.empty() || (ppu.empty() && spu.empty())) return usage();
    if (objects.empty()) objects = (fs::path(out).parent_path() / (fs::path(out).stem().string() + "-objects")).string();
    if (std::string error; !llvm_options.empty() && !codegen::set_llvm_options(llvm_options, &error)) {
        std::fprintf(stderr, "pspocket-prepare: %s\n", error.c_str());
        return 64;
    }

    // The jobs: every PPU unit, every SPU program, and the library's entry.
    std::vector<codegen::BuildJob> work;
    size_t ppu_files = 0, spu_files = 0, spu_programs = 0;
    std::error_code ec;
    if (!ppu.empty()) {
        const std::string headers = headers_hash(ppu);
        for (const auto& e : fs::directory_iterator(ppu, ec))
            if (e.path().extension() == ".c") {
                work.push_back(job_for(e.path(), false, headers));
                ppu_files++;
            }
    }
    if (!spu.empty()) {
        for (const auto& d : fs::directory_iterator(spu, ec)) {
            if (d.is_regular_file() && d.path().extension() == ".c") { // game_spu.c
                work.push_back(job_for(d.path(), true, ""));
                spu_files++;
                continue;
            }
            if (!d.is_directory()) continue;
            spu_programs++;
            const std::string headers = headers_hash(d.path());
            for (const auto& e : fs::directory_iterator(d.path(), ec))
                if (e.path().extension() == ".c") {
                    work.push_back(job_for(e.path(), true, headers));
                    spu_files++;
                }
        }
    }
    std::string exe_bytes;
    if (!read_bytes(exe, exe_bytes)) {
        std::fprintf(stderr, "pspocket-prepare: cannot read %s\n", exe.c_str());
        return 66;
    }
    const auto sha = recomp::Sha256::hash(std::string_view(exe_bytes));
    std::string sha_list;
    for (const u8 b : sha) sha_list += std::format("{}0x{:02x}", sha_list.empty() ? "" : ",", b);
    const fs::path info_dir = fs::path(objects) / "info";
    fs::create_directories(info_dir, ec);
    const std::string info = std::format(
        "/* Written by pspocket-prepare: what this game library was made from (pspocket/game/library.h). */\n"
        "#define PS_GAME_ABI_HASH 0x{:016x}ull\n#define PS_GAME_TRANSLATOR_HASH 0x{:016x}ull\n"
        "#define PS_GAME_TITLE_ID \"{}\"\n#define PS_GAME_MADE_BY \"pspocket {} (pspocket-prepare)\"\n"
        "#define PS_GAME_EXECUTABLE_SHA256 {{{}}}\n#define PS_GAME_HAS_PPU {}\n#define PS_GAME_HAS_SPU {}\n"
        "#define PS_GAME_SPU_PROGRAM_COUNT {}u\n",
        ps_game_abi_hash, ps_game_translator_hash, title, PSPOCKET_VERSION, sha_list, ppu.empty() ? 0 : 1,
        spu.empty() ? 0 : 1, spu_programs);
    std::ofstream(info_dir / "game_library_info.h") << info;
    {
        codegen::BuildJob entry;
        entry.source = PSPOCKET_GAME_LIBRARY_ENTRY_SOURCE;
        entry.include_dirs = {info_dir.string()};
        uint64_t h = 0xcbf29ce484222325ull;
        for (const char c : info) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
        entry.defines = {std::format("PSPOCKET_INFO_HASH=0x{:016x}ull", h)};
        entry.cost = 0.05;
        entry.memory_mb = 50;
        work.push_back(std::move(entry));
    }

    codegen::BuildOptions o;
    o.compile.target = "arm64-apple-macos13.0";
    o.compile.cpu = "apple-m1";
    o.compile.opt_level = opt;
    o.compile.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
    o.compile.system_dirs = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/include"};
    o.compile.include_dirs = {PSPOCKET_SOURCE_DIR "/runtime/ppu/include", PSPOCKET_SOURCE_DIR "/runtime/spu/include",
        PSPOCKET_SOURCE_DIR "/runtime/core/include", PSPOCKET_SOURCE_DIR "/runtime/game/include"};
    o.compile.defines = {"PSPOCKET_GAME_LIBRARY"};
    o.compile.extra_args = {"-fvisibility=hidden"};
    o.compile.extra_args.insert(o.compile.extra_args.end(), extra.begin(), extra.end());
    o.link.platform = "macos";
    o.link.min_os = "13.0";
    o.link.install_name = "@rpath/game.dylib";
    o.link.exports = {"_pspocket_game_library"};
    o.link.libraries = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/lib/libSystem.tbd"};
    o.workers = jobs;
    o.memory_mb = memory_mb;
    o.object_dir = objects;
    o.cache_key = std::format("abi {:016x} llvm {}", ps_game_abi_hash, codegen::compiler_version());
    for (const std::string& l : llvm_options) o.cache_key += " " + l;

    // Progress: a line every few seconds, and the time left as it was then, to say at the end how right it was.
    struct Sample {
        double seconds, left, fraction;
    };
    std::vector<Sample> samples;
    double last_print = -10;
    o.progress = [&](const codegen::BuildProgress& p) {
        const double fraction = p.cost_total > 0 ? p.cost_done / p.cost_total : 0;
        if (p.phase == codegen::BuildProgress::Phase::Compiling && p.seconds_left >= 0)
            samples.push_back({p.seconds, p.seconds_left, fraction});
        const bool phase_line = p.phase != codegen::BuildProgress::Phase::Compiling;
        if (quiet && !phase_line) return;
        if (!phase_line && p.seconds - last_print < 3) return;
        last_print = p.seconds;
        const char* what = p.phase == codegen::BuildProgress::Phase::Linking ? "linking"
            : p.phase == codegen::BuildProgress::Phase::Done                ? "done"
            : p.phase == codegen::BuildProgress::Phase::Failed              ? "failed"
                                                                            : "compiling";
        std::printf("[%3.0f%%] %s: %zu/%zu files, %s%s\n", 100 * fraction, what, p.files_done, p.files_total,
            clock_text(p.seconds).c_str(),
            p.seconds_left >= 0 ? std::format(", about {} left", clock_text(p.seconds_left)).c_str() : "");
        std::fflush(stdout);
    };
    std::printf("%s: %zu PPU files, %zu SPU files (%zu programs), the library entry; objects in %s\n", title.c_str(),
        ppu_files, spu_files, spu_programs, objects.c_str());
    const codegen::BuildResult r = codegen::build_library(work, {}, out, o);
    if (!r.ok) {
        std::fprintf(stderr, "pspocket-prepare: %s\n", r.error.c_str());
        return 1;
    }

    // Signed as the phone signs it, ad hoc here (the Mac runs ad hoc code).
    std::string library_bytes, error;
    read_bytes(out, library_bytes);
    std::vector<u8> bytes(library_bytes.begin(), library_bytes.end());
    if (!codesign::sign(bytes, {.identifier = "com.pspocket.game." + title}, &error)) {
        std::fprintf(stderr, "pspocket-prepare: signing failed: %s\n", error.c_str());
        return 1;
    }
    const fs::path tmp = out + ".tmp";
    std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    fs::rename(tmp, out, ec);
    std::printf("%s: %zu files compiled, %zu kept; compile %s (thread time %s, predicted %.0f), link %.1f s; %.1f MB\n",
        out.c_str(), r.compiled, r.reused, clock_text(r.compile_seconds).c_str(),
        clock_text(r.compile_thread_seconds).c_str(), r.predicted, r.link_seconds,
        static_cast<double>(bytes.size()) / 1048576.0);
    // How right the time left to compile was, at 10, 25, 50 and 75% of the compile time, against the real end.
    if (r.compiled > 0 && !samples.empty()) {
        std::printf("time left, as said then against what it was:");
        for (const double at : {0.1, 0.25, 0.5, 0.75}) {
            const auto s = std::find_if(samples.begin(), samples.end(),
                [&](const Sample& x) { return x.seconds >= at * r.compile_seconds; });
            if (s == samples.end()) continue;
            const double real = r.compile_seconds - s->seconds;
            std::printf("  %.0f%%: %s vs %s (%+.0f%%)", 100 * at, clock_text(s->left).c_str(), clock_text(real).c_str(),
                real > 0 ? 100 * (s->left - real) / real : 0.0);
        }
        std::printf("\n");
    }
    return 0;
}
