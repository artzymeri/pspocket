// The shader cache (D-114). Every program the backend compiles and every render pipeline it makes is recorded in a
// directory; when the backend starts, the ones recorded before (there and in read-only seed directories, such as the
// app's copy of the Mac's) are made again on background threads while the game loads. A draw that needs them later
// finds them ready instead of stopping the RSX thread to compile them: the first mission compiled 533 programs and
// pipelines in its first minutes, 8.3 s of stalls on an M4 Mac and several times that on an iPhone (run 082).
//
// A program is a file programs/<hash>.txt: its key, its translation's entry and masks, its ucode (the collision
// check) and its MSL source. A pipeline is a line of pipelines.txt: its two program keys and PipelineParams.

#include "metal_internal.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

#include "pspocket/core/log.hpp"

namespace pspocket::rsx::metal {
namespace {

constexpr std::string_view kMagic = "pspocket-shader 1";

u64 fnv(std::string_view s, u64 h = 0xcbf29ce484222325ull)
{
    for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
    return h;
}

std::string params_text(const PipelineParams& p)
{
    std::string t = std::to_string(p.color_count);
    for (u32 i = 0; i < 4; i++) t += std::format(" {} {} {}", p.color_formats[i], p.write_masks[i], p.blend_on[i]);
    t += std::format(" {} {} {} {} {} {} {} {}", p.sfactor_rgb, p.dfactor_rgb, p.sfactor_a, p.dfactor_a, p.equation_rgb,
        p.equation_a, p.depth_format, p.stencil_format);
    return t;
}

bool parse_params(std::istringstream& in, PipelineParams& p)
{
    in >> p.color_count;
    for (u32 i = 0; i < 4; i++) in >> p.color_formats[i] >> p.write_masks[i] >> p.blend_on[i];
    in >> p.sfactor_rgb >> p.dfactor_rgb >> p.sfactor_a >> p.dfactor_a >> p.equation_rgb >> p.equation_a >> p.depth_format >>
        p.stencil_format;
    return !in.fail() && p.color_count <= 4;
}

// One program record, or false when the file is not one.
bool parse_program(const std::string& text, ShaderCache::Record& r)
{
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != kMagic) return false;
    while (std::getline(in, line)) {
        const usize sp = line.find(' ');
        const std::string field = line.substr(0, sp), value = sp == std::string::npos ? "" : line.substr(sp + 1);
        if (field == "key") r.key = value;
        else if (field == "entry") r.shader.entry = value;
        else if (field == "input_mask") r.shader.input_mask = static_cast<u32>(std::stoul(value));
        else if (field == "texture_mask") r.shader.texture_mask = static_cast<u32>(std::stoul(value));
        else if (field == "constant_count") r.shader.constant_count = static_cast<u32>(std::stoul(value));
        else if (field == "ucode") {
            std::istringstream words(value);
            usize n = 0;
            words >> n;
            r.ucode.resize(n);
            for (u32& w : r.ucode) words >> std::hex >> w;
            if (words.fail()) return false;
        } else if (field == "source") {
            const usize n = std::stoul(value);
            r.shader.source.resize(n);
            in.read(r.shader.source.data(), static_cast<std::streamsize>(n));
            return static_cast<usize>(in.gcount()) == n && !r.key.empty() && !r.shader.entry.empty();
        }
    }
    return false;
}

std::string read_file(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

} // namespace

ShaderCache::ShaderCache(id<MTLDevice> device, std::string dir, std::vector<std::string> seeds, u32 threads)
    : device_(device), dir_(std::move(dir)), seeds_(std::move(seeds))
{
    if (!dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir_) / "programs", ec);
        if (ec) PS_LOG_WARN(kChan, "shader cache: cannot create {}: {}", dir_, ec.message());
    }
    if (threads == 0) threads = std::clamp<u32>(std::thread::hardware_concurrency() / 3, 1, 3);
    started_ = std::chrono::steady_clock::now();
    loader_ = std::thread([this, threads] { load_and_start(threads); });
}

ShaderCache::~ShaderCache()
{
    stop_.store(true, std::memory_order_release);
    if (loader_.joinable()) loader_.join();
    {
        std::lock_guard lock(mutex_); // a worker between its check and its wait still sees stop_ when woken
    }
    programs_done_cv_.notify_all();
    for (std::thread& t : workers_) t.join();
}

void ShaderCache::load_and_start(u32 threads)
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0); // background work while the game loads
#endif
    // Seeds first, then the writable directory; a key seen once is enough.
    std::vector<std::string> dirs = seeds_;
    if (!dir_.empty()) dirs.push_back(dir_);
    std::unordered_set<std::string> keys;
    for (const std::string& d : dirs) {
        std::error_code ec;
        const std::filesystem::path programs = std::filesystem::path(d) / "programs";
        for (const auto& e : std::filesystem::directory_iterator(programs, ec)) {
            if (stop_.load(std::memory_order_acquire)) return;
            Record r;
            if (!e.is_regular_file() || !parse_program(read_file(e.path()), r)) continue;
            if (!keys.insert(r.key).second) continue;
            std::lock_guard lock(mutex_);
            recorded_programs_.insert(r.key);
            records_.push_back(std::move(r));
        }
        std::ifstream lines(std::filesystem::path(d) / "pipelines.txt");
        std::string line;
        while (std::getline(lines, line)) {
            const usize t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            PipelineRecord p{line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1), {}};
            std::istringstream in(line.substr(t2 + 1));
            if (!parse_params(in, p.params)) continue;
            std::lock_guard lock(mutex_);
            if (!recorded_pipelines_.insert(line).second) continue;
            pipeline_records_.push_back(std::move(p));
        }
    }
    {
        std::lock_guard lock(mutex_);
        if (records_.empty() && pipeline_records_.empty()) {
            done_ = true;
            done_cv_.notify_all();
            return;
        }
        PS_LOG_INFO(kChan, "shader cache: making {} programs and {} pipelines ahead, on {} threads", records_.size(),
            pipeline_records_.size(), threads);
    }
    workers_left_ = threads;
    for (u32 i = 0; i < threads; i++) workers_.emplace_back([this] { work(); });
}

void ShaderCache::work()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
    MTLCompileOptions* options = shader_compile_options();
    // Programs.
    for (usize i; (i = next_program_.fetch_add(1)) < records_.size();) {
        if (stop_.load(std::memory_order_acquire)) break;
        @autoreleasepool {
            const Record& r = records_[i];
            NSError* error = nil;
            id<MTLLibrary> lib = [device_ newLibraryWithSource:[NSString stringWithUTF8String:r.shader.source.c_str()]
                                                       options:options
                                                         error:&error];
            id<MTLFunction> fn = lib ? [lib newFunctionWithName:[NSString stringWithUTF8String:r.shader.entry.c_str()]] : nil;
            std::lock_guard lock(mutex_);
            if (fn) {
                CompiledProgram p;
                p.function = fn;
                p.shader = r.shader;
                p.ucode = r.ucode;
                p.key = r.key;
                functions_[r.key] = fn;
                ready_programs_.push_back(std::move(p));
                ready_.store(true, std::memory_order_release);
                made_programs_++;
            }
            if (++programs_finished_ == records_.size()) programs_done_cv_.notify_all();
        }
    }
    // Pipelines, once every program is made (a pipeline needs its two).
    {
        std::unique_lock lock(mutex_);
        programs_done_cv_.wait(lock, [this] {
            return programs_finished_ >= records_.size() || stop_.load(std::memory_order_acquire);
        });
    }
    for (usize i; (i = next_pipeline_.fetch_add(1)) < pipeline_records_.size();) {
        if (stop_.load(std::memory_order_acquire)) break;
        @autoreleasepool {
            const PipelineRecord& r = pipeline_records_[i];
            id<MTLFunction> vp = nil, fp = nil;
            {
                std::lock_guard lock(mutex_);
                if (auto it = functions_.find(r.vp_key); it != functions_.end()) vp = it->second;
                if (auto it = functions_.find(r.fp_key); it != functions_.end()) fp = it->second;
            }
            if (!vp || !fp) continue;
            NSError* error = nil;
            id<MTLRenderPipelineState> pso = [device_ newRenderPipelineStateWithDescriptor:pipeline_descriptor(r.params, vp, fp)
                                                                                    error:&error];
            if (!pso) continue;
            std::lock_guard lock(mutex_);
            ready_pipelines_.push_back({r.vp_key, r.fp_key, r.params, vp, fp, pso});
            ready_.store(true, std::memory_order_release);
            made_pipelines_++;
        }
    }
    std::lock_guard lock(mutex_);
    if (--workers_left_ == 0) {
        done_ = true;
        PS_LOG_INFO(kChan, "shader cache: {} programs and {} pipelines made ahead in {:.1f} s", made_programs_,
            made_pipelines_,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count());
        done_cv_.notify_all();
    }
}

bool ShaderCache::take(Ready& out)
{
    if (!ready_.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(mutex_);
    out.programs = std::move(ready_programs_);
    out.pipelines = std::move(ready_pipelines_);
    ready_programs_.clear();
    ready_pipelines_.clear();
    ready_.store(false, std::memory_order_release);
    return true;
}

void ShaderCache::wait()
{
    std::unique_lock lock(mutex_);
    done_cv_.wait(lock, [this] { return done_; });
}

void ShaderCache::record_program(const CompiledProgram& p)
{
    if (dir_.empty() || p.key.empty()) return;
    {
        std::lock_guard lock(mutex_);
        if (!recorded_programs_.insert(p.key).second) return;
    }
    std::string text = std::format("{}\nkey {}\nentry {}\ninput_mask {}\ntexture_mask {}\nconstant_count {}\nucode {}",
        kMagic, p.key, p.shader.entry, p.shader.input_mask, p.shader.texture_mask, p.shader.constant_count, p.ucode.size());
    for (u32 w : p.ucode) text += std::format(" {:x}", w);
    text += std::format("\nsource {}\n", p.shader.source.size());
    text += p.shader.source;
    const std::filesystem::path path = std::filesystem::path(dir_) / "programs" / std::format("{:016x}.txt", fnv(p.key));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) PS_LOG_WARN(kChan, "shader cache: cannot write {}", path.string());
}

void ShaderCache::record_pipeline(const std::string& vp_key, const std::string& fp_key, const PipelineParams& p)
{
    if (dir_.empty() || vp_key.empty() || fp_key.empty()) return;
    const std::string line = std::format("{}\t{}\t{}", vp_key, fp_key, params_text(p));
    {
        std::lock_guard lock(mutex_);
        if (!recorded_pipelines_.insert(line).second) return;
    }
    std::ofstream out(std::filesystem::path(dir_) / "pipelines.txt", std::ios::app);
    out << line << '\n';
}

// The RSX thread: what the shader cache made since the last call joins the backend's programs and pipelines. A program
// compiled meanwhile on demand stays (its pipelines are already keyed by its function); a pipeline joins only when
// both its programs are the ones it was made with.
void MetalBackend::Impl::take_shader_cache()
{
    if (!shader_cache) return;
    ShaderCache::Ready r;
    if (!shader_cache->take(r)) return;
    for (CompiledProgram& p : r.programs) {
        if (programs.contains(p.key)) continue;
        const std::string key = p.key;
        programs.emplace(key, std::move(p));
        stats.programs_prewarmed++;
    }
    for (ShaderCache::ReadyPipeline& pl : r.pipelines) {
        const auto vp = programs.find(pl.vp_key), fp = programs.find(pl.fp_key);
        if (vp == programs.end() || fp == programs.end() || vp->second.function != pl.vp || fp->second.function != pl.fp)
            continue;
        if (pipelines.emplace(pipeline_key(pl.params, pl.vp, pl.fp), pl.pso).second) stats.pipelines_prewarmed++;
    }
}

} // namespace pspocket::rsx::metal
