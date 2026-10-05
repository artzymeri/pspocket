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
};

// What a translated C file will cost to compile, from its size and the shape of its functions (lines times block
// starts, which is what LLVM's register allocator pays for: run 092). Fitted on GTA V's 813 files on the M4.
struct CostEstimate {
    double seconds = 0;
    double memory_mb = 0;
};
CostEstimate estimate_cost(const std::string& source_text, bool spu);

// The time left of a build, from the predicted costs and what the files done so far really took (D-152). The rate
// (real seconds per predicted second) starts from a prior and follows the files as they finish; the files left are
// laid out over the workers, slowest first, as the builder runs them.
class TimeLeft {
public:
    explicit TimeLeft(unsigned workers, double prior_rate = 1.0, double prior_weight = 30.0)
        : workers_(workers ? workers : 1), prior_rate_(prior_rate), prior_weight_(prior_weight)
    {
    }
    void finished(double predicted, double seconds); // a file compiled
    double rate() const;                             // real seconds per predicted second
    // running: (predicted, seconds so far) of the files compiling now; waiting: predicted costs of the files left.
    double seconds_left(const std::vector<std::pair<double, double>>& running, std::vector<double> waiting) const;

private:
    unsigned workers_;
    double prior_rate_, prior_weight_;
    double predicted_done_ = 0, seconds_done_ = 0;
};

struct BuildProgress {
    enum class Phase { Compiling, Linking, Done, Failed };
    Phase phase = Phase::Compiling;
    size_t files_done = 0, files_total = 0;
    double cost_done = 0, cost_total = 0; // predicted seconds: the fraction done is cost_done / cost_total
    double seconds = 0;                   // since the build started
    double seconds_left = -1;             // -1: not known yet
    std::string file;                     // the file that just started or finished
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
