// pspocket-cc: the device's compiler (runtime/codegen, D-151) as a Mac command, to time and size a whole game's
// translated code with the code the phone runs (clang in process, the bundled headers, no SDK).
//
//   pspocket-cc [-j N] [-O L] [-I DIR]... [-X ARG]... [--mllvm OPT]... [--repeat K] [--cpu NAME] -o OUTDIR FILE.c...
//
// -X passes one more argument to clang's driver (-X -fno-slp-vectorize); --mllvm sets one of LLVM's own options for
// every compile (--mllvm -grow-region-complexity-budget=1000).
//
// Each file compiles to OUTDIR/<name>.o on one of N threads (largest first). Prints each file's seconds and the
// process's footprint after it, then the total CPU seconds, the wall time and the peak footprint.

#include <mach/mach.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pspocket/codegen/toolchain.hpp"

namespace fs = std::filesystem;

namespace {

double footprint_mb()
{
    task_vm_info_data_t vm{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count) != KERN_SUCCESS) return 0;
    return static_cast<double>(vm.phys_footprint) / 1048576.0;
}

int usage()
{
    std::fputs("usage: pspocket-cc [-j N] [-O L] [-I DIR]... [-X ARG]... [--mllvm OPT]... [--repeat K] [--cpu NAME] "
               "-o OUTDIR FILE.c...\n",
        stderr);
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace pspocket;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    int repeat = 1;
    codegen::CompileOptions base;
    base.target = "arm64-apple-macos13.0";
    base.cpu = "apple-m1";
    base.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
    base.system_dirs = {std::string(PSPOCKET_CODEGEN_SYSROOT) + "/include"};
    fs::path out;
    std::vector<fs::path> files;
    std::vector<std::string> llvm_options;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "-j") jobs = static_cast<unsigned>(std::max(1, std::atoi(next().c_str())));
        else if (a.starts_with("-O")) base.opt_level = std::atoi(a.c_str() + 2);
        else if (a == "-I") base.include_dirs.push_back(next());
        else if (a.starts_with("-I")) base.include_dirs.push_back(a.substr(2));
        else if (a == "-X") base.extra_args.push_back(next());
        else if (a == "--mllvm") llvm_options.push_back(next());
        else if (a == "--repeat") repeat = std::max(1, std::atoi(next().c_str()));
        else if (a == "--cpu") base.cpu = next();
        else if (a == "-o") out = next();
        else if (a.starts_with("-")) return usage();
        else files.emplace_back(a);
    }
    if (out.empty() || files.empty()) return usage();
    if (std::string error; !llvm_options.empty() && !codegen::set_llvm_options(llvm_options, &error)) {
        std::fprintf(stderr, "pspocket-cc: %s\n", error.c_str());
        return 2;
    }
    fs::create_directories(out);
    std::vector<fs::path> work;
    for (int r = 0; r < repeat; ++r) work.insert(work.end(), files.begin(), files.end());
    std::stable_sort(work.begin(), work.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code ec;
        return fs::file_size(a, ec) > fs::file_size(b, ec);
    });

    std::atomic<size_t> next_file{0};
    std::atomic<int> failed{0};
    std::atomic<double> peak{footprint_mb()};
    std::atomic<bool> done{false};
    std::mutex print;
    double cpu_seconds = 0;
    std::thread sampler([&] {
        while (!done) {
            peak = std::max(peak.load(), footprint_mb());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    for (unsigned j = 0; j < std::min<size_t>(jobs, work.size()); ++j)
        workers.emplace_back([&] {
            for (size_t i; (i = next_file++) < work.size();) {
                const fs::path& file = work[i];
                codegen::CompileOptions o = base;
                o.include_dirs.push_back(file.parent_path().string());
                std::string diagnostics;
                const auto s0 = std::chrono::steady_clock::now();
                const bool ok = codegen::compile_c(file.string(), (out / file.filename()).replace_extension(".o"),
                    o, &diagnostics);
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
                std::error_code ec;
                std::lock_guard lock(print);
                cpu_seconds += s;
                std::printf("%8.2f s  %6.2f MB  %s%s  (footprint %.0f MB)\n", s,
                    static_cast<double>(fs::file_size(file, ec)) / 1048576.0, file.filename().c_str(),
                    ok ? "" : "  FAILED", footprint_mb());
                if (!ok) {
                    ++failed;
                    std::fprintf(stderr, "%s\n", diagnostics.substr(0, 2000).c_str());
                }
                std::fflush(stdout);
            }
        });
    for (std::thread& w : workers) w.join();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    done = true;
    sampler.join();
    std::printf("%zu files, %d failed: %.1f CPU seconds, %.1f s wall on %u threads, peak footprint %.0f MB, now %.0f "
                "MB\n",
        work.size(), failed.load(), cpu_seconds, wall, jobs, peak.load(), footprint_mb());
    return failed ? 1 : 0;
}
