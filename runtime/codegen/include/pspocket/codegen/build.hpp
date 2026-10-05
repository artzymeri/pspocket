#pragma once

// Building a game library from translated C (route B, D-152): every file compiled by the device's compiler
// (toolchain.hpp), as many at once as the cores and the memory budget allow, the slowest first, then linked into one
// library. Finished objects are kept, so a build that stops (the app sent to the background, a crash) resumes where
// it was. Progress and the time left count each file's predicted cost, not files: a few files take most of the time
// (run 092). The phone's "Preparing game…" screen and pspocket-prepare on the Mac both run this.

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "pspocket/codegen/toolchain.hpp"

namespace pspocket::codegen {

struct BuildJob {
    std::string source;                    // a .c file
    std::vector<std::string> include_dirs; // besides BuildOptions::compile's
    std::vector<std::string> defines;
    std::vector<std::string> extra_args; // more of clang's driver arguments for this file (-fno-slp-vectorize)
    double cost = 0;      // predicted seconds of compiling (estimate_cost), for order, progress and the time left
    double memory_mb = 0; // predicted peak memory of its compile
    int group = 0;        // files whose predictions err alike (FileKind): the time left learns a rate per group
};

// What a translated C file holds, for its cost: translated PPU or SPU functions, or a registration table (the
// lifters' *_reg.c and lift-spu.sh's game_spu.c: addresses and pointers, compiled in no time).
enum class FileKind { PpuCode = 0, SpuCode = 1, Table = 2 };

// How translated code is compiled (D-153), on the Mac and on the phone alike: PPU code without the SLP vectorizer
// (one GTA V unit took 35 s with it, 3.3 s without; the others 17% less), and LLVM's register allocator with a
// region-growing budget of 1,000 instead of 10,000 (SPU functions keep 90 vector registers live across thousands of
// blocks: the slowest program compiles 2.3 times faster, its code 30% smaller). GTA V compiles 19% faster and runs
// the gate as fast (run 094).
struct TranslatedCodeSettings {
    std::vector<std::string> llvm_options; // set_llvm_options, once, before compiles start
    std::vector<std::string> ppu_args, spu_args; // BuildJob::extra_args
};
const TranslatedCodeSettings& translated_code_settings();

// What a translated C file will cost to compile, from its size and the shape of its functions (lines times block
// starts, which is what LLVM's register allocator pays for: run 092). Fitted on two builds of GTA V's 815 files ten at
// a time on the M4 with translated_code_settings(), each build's speed taken out (run 094): seconds of one such
// thread. Mean error per file: PPU code 17%, SPU code 31%.
struct CostEstimate {
    double seconds = 0;
    double memory_mb = 0;
};
CostEstimate estimate_cost(const std::string& source_text, FileKind kind);

// The time left of a build, from the predicted costs and what the files done so far really took (D-152). A rate per
// group of files (real seconds per predicted second: PPU and SPU predictions err differently, run 093) starts from a
// prior and follows that group's files as they finish; the files left are laid out over the workers, slowest first,
// as the builder runs them.
class TimeLeft {
public:
    static constexpr int kGroups = 3; // FileKind
    // The prior weighs as much as `prior_weight` predicted seconds of files done (build_library: a quarter of the
    // build, at most 1,000): the first files to finish are the ones that ran fast, so trusting them early said 40%
    // too little a tenth of the way into GTA V's build (run 094).
    explicit TimeLeft(unsigned workers, double prior_rate = 1.0, double prior_weight = 1000.0)
        : workers_(workers ? workers : 1), prior_rate_(prior_rate), prior_weight_(prior_weight)
    {
    }
    void finished(int group, double predicted, double seconds); // a file compiled
    double rate(int group) const;                               // real seconds per predicted second
    struct File {
        int group = 0;
        double predicted = 0;
        double elapsed = 0; // seconds compiling so far (running files)
    };
    double seconds_left(const std::vector<File>& running, std::vector<File> waiting) const;

private:
    unsigned workers_;
    double prior_rate_, prior_weight_;
    double predicted_done_[kGroups] = {}, seconds_done_[kGroups] = {};
    double all_predicted_ = 0, all_seconds_ = 0;
};

struct BuildProgress {
    enum class Phase { Compiling, Linking, Done, Failed };
    Phase phase = Phase::Compiling;
    size_t files_done = 0, files_total = 0;
    double cost_done = 0, cost_total = 0; // predicted seconds: the fraction done is cost_done / cost_total
    double seconds = 0;                   // since the build started
    double seconds_left = -1;             // -1: not known yet
    std::string file;                     // the file that just started or finished
    double file_cost = 0;                 // its predicted cost
    int file_group = 0;
    double file_seconds = -1;             // how long it took, when it finished (-1: it started)
};

struct BuildOptions {
    CompileOptions compile; // what every file shares: target, CPU, headers, optimisation
    LinkOptions link;
    unsigned workers = 0;        // 0: one per core
    double memory_mb = 0;        // compiles start while their predicted peaks fit; 0: no limit
    std::string object_dir;      // objects, kept between builds
    std::string cache_key;       // what else makes an object (the ABI and compiler versions): part of its name
    double prior_rate = 1.0;     // this machine's speed against the cost model's (TimeLeft)
    std::function<void(const BuildProgress&)> progress; // called from the build's threads, one call at a time
    const std::atomic<bool>* cancel = nullptr;
};

struct BuildResult {
    bool ok = false;
    std::string error;
    size_t compiled = 0, reused = 0; // files compiled now, objects kept from an earlier build
    double compile_seconds = 0, link_seconds = 0;
    double predicted = 0, compile_thread_seconds = 0;
};

// Compiles the jobs (objects in options.object_dir) and links them, with `extra_objects`, into `output`.
BuildResult build_library(std::vector<BuildJob> jobs, const std::vector<std::string>& extra_objects,
    const std::string& output, const BuildOptions& options);

} // namespace pspocket::codegen
