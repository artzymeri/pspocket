#include "pspocket/codegen/build.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

namespace pspocket::codegen {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

bool read_text(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// FNV-1a, 64 bits: what names an object (its source's text and everything that compiles it).
struct Fnv {
    uint64_t h = 0xcbf29ce484222325ull;
    void add(std::string_view s)
    {
        for (const char c : s) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
        h = (h ^ 0xff) * 0x100000001b3ull; // a separator, so ("ab", "c") and ("a", "bc") differ
    }
};

std::string settings_text(const CompileOptions& o)
{
    // The CPU as compiled for: "native" is this device's, and an object made for another chip is not this one's.
    std::string s = std::format("{}|{}|{}|{}", o.target, o.cpu == "native" ? host_cpu() : o.cpu, o.opt_level,
        o.resource_dir);
    for (const auto* list : {&o.system_dirs, &o.include_dirs, &o.defines, &o.extra_args})
        for (const std::string& v : *list) s += "|" + v;
    return s;
}

} // namespace

const TranslatedCodeSettings& translated_code_settings()
{
    static const TranslatedCodeSettings settings = {
        .llvm_options = {"-grow-region-complexity-budget=1000"},
        .ppu_args = {"-fno-slp-vectorize"},
        .spu_args = {},
    };
    return settings;
}

CostEstimate estimate_cost(const std::string& text, FileKind kind)
{
    // Per function: its lines times its block starts (labels "L_xxxxx:" at the start of a line). Functions start with
    // "void " at the start of a line (every translated function is void f(ctx, base or ls)).
    double lines_times_labels = 0, lines = 0, labels = 0;
    auto end_function = [&] {
        lines_times_labels += lines * labels;
        lines = labels = 0;
    };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string_view line(text.data() + pos, eol - pos);
        if (line.starts_with("void ") && line.find('(') != std::string_view::npos) end_function();
        else if (line.starts_with("L_") && line.find(':') != std::string_view::npos) labels += 1;
        lines += 1;
        pos = eol + 1;
    }
    end_function();
    const double mb = static_cast<double>(text.size()) / 1e6;
    // Least squares over two builds of GTA V's files, ten at a time on the M4 with translated_code_settings(), each
    // build's own speed taken out (run 094).
    CostEstimate e;
    switch (kind) {
    case FileKind::SpuCode:
        e.seconds = 6.767 * mb + 9.180 * lines_times_labels / 1e6;
        e.memory_mb = 200 + 100 * mb;
        break;
    case FileKind::PpuCode:
        e.seconds = 2.599 * mb + 1.385 * lines_times_labels / 1e6;
        e.memory_mb = 80 + 70 * mb;
        break;
    case FileKind::Table:
        e.seconds = 0.852 * mb;
        e.memory_mb = 50 + 20 * mb;
        break;
    }
    e.seconds = std::max(e.seconds, 0.02);
    return e;
}

void TimeLeft::finished(int group, double predicted, double seconds)
{
    const int g = std::clamp(group, 0, kGroups - 1);
    predicted_done_[g] += predicted;
    seconds_done_[g] += seconds;
}

double TimeLeft::rate(int group) const
{
    const int g = std::clamp(group, 0, kGroups - 1);
    return (prior_rate_[g] * prior_weight_ + seconds_done_[g]) / (prior_weight_ + predicted_done_[g]);
}

double TimeLeft::seconds_left(const std::vector<File>& running, std::vector<File> waiting) const
{
    std::vector<double> free(workers_, 0.0);
    for (size_t i = 0; i < running.size() && i < free.size(); i++) {
        const double expected = running[i].predicted * rate(running[i].group);
        // A file past its prediction is taken to need a tenth of it more, whatever it has run.
        free[i] = std::max(expected - running[i].elapsed, 0.1 * expected);
    }
    std::sort(waiting.begin(), waiting.end(), [](const File& a, const File& b) { return a.predicted > b.predicted; });
    for (const File& f : waiting) {
        auto earliest = std::min_element(free.begin(), free.end());
        *earliest += f.predicted * rate(f.group);
    }
    return *std::max_element(free.begin(), free.end());
}

BuildResult build_library(std::vector<BuildJob> jobs, const std::vector<std::string>& extra_objects,
    const std::string& output, const BuildOptions& options)
{
    BuildResult result;
    const auto t0 = Clock::now();
    std::error_code ec;
    fs::create_directories(options.object_dir, ec);
    if (ec) {
        result.error = std::format("cannot make {}: {}", options.object_dir, ec.message());
        return result;
    }

    // Each object is named by everything that makes it: kept ones are used again, the others compiled.
    const std::string settings = settings_text(options.compile) + "|" + options.cache_key;
    struct Work {
        BuildJob job;
        std::string object;
        bool done = false;
    };
    std::vector<Work> work;
    std::vector<std::string> objects;
    double cost_total = 0, cost_done = 0;
    for (BuildJob& job : jobs) {
        std::string text;
        if (!read_text(job.source, text)) {
            result.error = std::format("cannot read {}", job.source);
            return result;
        }
        Fnv h;
        h.add(settings);
        for (const std::string& d : job.include_dirs) h.add(d);
        for (const std::string& d : job.defines) h.add(d);
        for (const std::string& a : job.extra_args) h.add(a);
        h.add(text);
        const std::string object = (fs::path(options.object_dir) /
            std::format("{}-{:016x}.o", fs::path(job.source).stem().string(), h.h)).string();
        cost_total += job.cost;
        objects.push_back(object);
        const bool kept = fs::exists(object, ec) && fs::file_size(object, ec) > 0;
        if (kept) {
            cost_done += job.cost;
            result.reused++;
        }
        work.push_back({std::move(job), object, kept});
    }
    std::vector<size_t> order; // the files to compile, slowest first
    for (size_t i = 0; i < work.size(); i++)
        if (!work[i].done) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return work[a].job.cost > work[b].job.cost; });

    const unsigned workers = std::max(1u, options.workers ? options.workers : std::thread::hardware_concurrency());
    std::mutex m;
    std::condition_variable cv;
    size_t next = 0, files_done = result.reused, running = 0;
    double memory_in_use = 0;
    std::vector<std::pair<size_t, Clock::time_point>> active; // (work index, start)
    TimeLeft left(workers, options.prior_rates, std::min(1000.0, 0.25 * cost_total));
    double kind_predicted[TimeLeft::kGroups] = {}, kind_seconds[TimeLeft::kGroups] = {};
    std::string first_error;

    // Under m. `i` is the file that started or finished (SIZE_MAX: none), `seconds` how long it took (-1: started).
    auto report = [&](BuildProgress::Phase phase, size_t i, double seconds, const std::string& file) {
        if (!options.progress) return;
        BuildProgress p;
        p.phase = phase;
        p.files_done = files_done;
        p.files_total = work.size();
        p.cost_done = cost_done;
        p.cost_total = cost_total;
        p.seconds = since(t0);
        p.file = file;
        if (i < work.size()) {
            p.file_cost = work[i].job.cost;
            p.file_group = work[i].job.group;
            p.file_seconds = seconds;
        }
        if (phase == BuildProgress::Phase::Compiling) {
            std::vector<TimeLeft::File> now, waiting;
            for (const auto& [a, start] : active) now.push_back({work[a].job.group, work[a].job.cost, since(start)});
            for (size_t k = next; k < order.size(); k++)
                waiting.push_back({work[order[k]].job.group, work[order[k]].job.cost, 0});
            p.seconds_left = left.seconds_left(now, std::move(waiting));
        }
        options.progress(p);
    };

    auto worker = [&] {
        std::unique_lock lock(m);
        for (;;) {
            // The slowest file left whose memory fits, or any file when nothing runs (one bigger than the budget).
            size_t pick = order.size();
            while (first_error.empty() && !(options.cancel && options.cancel->load()) && next < order.size()) {
                for (size_t k = next; k < order.size(); k++) {
                    const BuildJob& j = work[order[k]].job;
                    if (running == 0 || options.memory_mb <= 0 || memory_in_use + j.memory_mb <= options.memory_mb) {
                        pick = k;
                        break;
                    }
                }
                if (pick != order.size()) break;
                cv.wait(lock);
            }
            if (pick == order.size()) return;
            std::swap(order[next], order[pick]); // started files stay in front of `next`
            const size_t i = order[next++];
            std::stable_sort(order.begin() + static_cast<std::ptrdiff_t>(next), order.end(),
                [&](size_t a, size_t b) { return work[a].job.cost > work[b].job.cost; });
            running++;
            memory_in_use += work[i].job.memory_mb;
            active.emplace_back(i, Clock::now());
            report(BuildProgress::Phase::Compiling, i, -1, work[i].job.source);
            lock.unlock();

            CompileOptions o = options.compile;
            o.include_dirs.insert(o.include_dirs.end(), work[i].job.include_dirs.begin(), work[i].job.include_dirs.end());
            o.defines.insert(o.defines.end(), work[i].job.defines.begin(), work[i].job.defines.end());
            o.extra_args.insert(o.extra_args.end(), work[i].job.extra_args.begin(), work[i].job.extra_args.end());
            std::string diagnostics;
            const auto s0 = Clock::now();
            const bool ok = compile_c(work[i].job.source, work[i].object, o, &diagnostics);
            const double seconds = since(s0);

            lock.lock();
            running--;
            memory_in_use -= work[i].job.memory_mb;
            std::erase_if(active, [&](const auto& a) { return a.first == i; });
            result.compile_thread_seconds += seconds;
            if (ok) {
                work[i].done = true;
                files_done++;
                cost_done += work[i].job.cost;
                result.compiled++;
                left.finished(work[i].job.group, work[i].job.cost, seconds);
                const int g = std::clamp(work[i].job.group, 0, TimeLeft::kGroups - 1);
                kind_predicted[g] += work[i].job.cost;
                kind_seconds[g] += seconds;
            } else if (first_error.empty()) {
                first_error = std::format("{} did not compile:\n{}", work[i].job.source, diagnostics.substr(0, 4000));
            }
            report(BuildProgress::Phase::Compiling, i, seconds, work[i].job.source);
            cv.notify_all();
        }
    };
    {
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < std::min<size_t>(workers, std::max<size_t>(order.size(), 1)); t++)
            threads.emplace_back(worker);
        for (std::thread& t : threads) t.join();
    }
    result.compile_seconds = since(t0);
    result.predicted = cost_total;
    for (int g = 0; g < TimeLeft::kGroups; g++)
        if (kind_predicted[g] > 1.0) result.rates[g] = kind_seconds[g] / kind_predicted[g];
    if (!first_error.empty() || (options.cancel && options.cancel->load())) {
        result.error = first_error.empty() ? "cancelled" : first_error;
        std::lock_guard lock(m);
        report(BuildProgress::Phase::Failed, SIZE_MAX, -1, "");
        return result;
    }

    {
        std::lock_guard lock(m);
        report(BuildProgress::Phase::Linking, SIZE_MAX, -1, output);
    }
    const auto l0 = Clock::now();
    std::vector<std::string> all = objects;
    all.insert(all.end(), extra_objects.begin(), extra_objects.end());
    std::string diagnostics;
    const bool linked = link_dylib(all, output, options.link, &diagnostics);
    result.link_seconds = since(l0);
    std::lock_guard lock(m);
    if (!linked) {
        result.error = std::format("the link failed:\n{}", diagnostics.substr(0, 4000));
        report(BuildProgress::Phase::Failed, SIZE_MAX, -1, "");
        return result;
    }
    result.ok = true;
    report(BuildProgress::Phase::Done, SIZE_MAX, -1, output);
    return result;
}

} // namespace pspocket::codegen
