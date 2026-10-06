// The shader cache (D-114). Every program the backend compiles and every render pipeline it makes is recorded in a
// directory; when the backend starts, the ones recorded before (there and in read-only seed directories, such as the
// app's copy of the Mac's) are made again on background threads while the game loads. A draw that needs them later
// finds them ready instead of stopping the RSX thread to compile them: the first mission compiled 533 programs and
// pipelines in its first minutes, 8.3 s of stalls on an M4 Mac and several times that on an iPhone (run 082).
//
// A program is a file programs/<hash>.txt: its key, its translation's entry and masks, its ucode (the collision
// check) and its MSL source, under a first line naming the translation's version (shader::kTranslationVersion); records
// of another version are not loaded. A pipeline is a line of pipelines.txt: its two program keys and PipelineParams.
//
// D-143: the pipelines are made in the order they were first needed (pipelines.txt grows in that order), each with
// its two programs, then the programs no pipeline names: the first ones a game needs are ready first, where all
// 7,500 programs used to come before the first pipeline. Each worker keeps the GPU binaries it compiled in a Metal
// binary archive in the writable directory (archive-<launch>-<worker>.bin); the next run loads them all and finds its
// pipelines there instead of compiling them: the iPhone compiled them again at every launch, for longer than the game
// takes to load (macOS keeps them in its own cache). An archive is only ever added to by the worker that made it;
// the loaded ones are only read.

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

// The first line of a program record: the record format and the translation's version. A record of another version is
// skipped: its MSL is an older translation's, under a key the current one also makes (D-174).
const std::string kMagic = std::format("pspocket-shader {}", shader::kTranslationVersion);

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

ShaderCache::ShaderCache(id<MTLDevice> device, std::string dir, std::vector<std::string> seeds, u32 threads,
    bool archives)
    : device_(device), dir_(std::move(dir)), seeds_(std::move(seeds)), use_archives_(archives)
{
    if (!dir_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir_) / "programs", ec);
        if (ec) PS_LOG_WARN(kChan, "shader cache: cannot create {}: {}", dir_, ec.message());
    }
    // Half the cores (3 on an iPhone's 6, 4 on a Mac), at user-initiated QoS: at utility the iPhone ran them on its
    // efficiency cores, and the game needs what they make within its first minute (D-143).
    if (threads == 0) threads = std::clamp<u32>(std::thread::hardware_concurrency() / 2, 2, 4);
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
    compiled_cv_.notify_all();
    for (std::thread& t : workers_) t.join();
}

void ShaderCache::load_and_start(u32 threads)
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0); // while the game loads
#endif
    load_archives();
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
            record_index_[r.key] = records_.size();
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
        PS_LOG_INFO(kChan, "shader cache: making {} programs and {} pipelines ahead, on {} threads ({} GPU binary "
            "archives)", records_.size(), pipeline_records_.size(), threads, archives_.count);
        last_report_ = std::chrono::steady_clock::now();
    }
    workers_left_ = threads;
    for (u32 i = 0; i < threads; i++) workers_.emplace_back([this, i] { work(i); });
}

// The archives earlier runs kept. One that does not load (another device, an OS update) is deleted: its pipelines
// are compiled again into a new one. Without archives, those of earlier runs are deleted (D-145).
void ShaderCache::load_archives()
{
    if (dir_.empty()) return;
    NSMutableArray<id<MTLBinaryArchive>>* loaded = [NSMutableArray array];
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir_, ec)) {
        const std::string name = e.path().filename().string();
        if (!e.is_regular_file() || !name.starts_with("archive-") || !name.ends_with(".bin")) continue;
        if (!use_archives_) {
            std::filesystem::remove(e.path(), ec);
            continue;
        }
        MTLBinaryArchiveDescriptor* d = [MTLBinaryArchiveDescriptor new];
        d.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:e.path().c_str()]];
        NSError* error = nil;
        id<MTLBinaryArchive> archive = [device_ newBinaryArchiveWithDescriptor:d error:&error];
        if (archive) {
            [loaded addObject:archive];
        } else {
            PS_LOG_WARN(kChan, "shader cache: archive {} does not load ({}); deleted", name,
                error ? error.localizedDescription.UTF8String : "?");
            std::filesystem::remove(e.path(), ec);
        }
    }
    std::lock_guard lock(mutex_);
    archives_ = loaded;
}

NSArray<id<MTLBinaryArchive>>* ShaderCache::archives()
{
    std::lock_guard lock(mutex_);
    return archives_;
}

// A program's function, compiled once: a worker that needs one another worker is compiling waits for it.
id<MTLFunction> ShaderCache::program(const std::string& key, MTLCompileOptions* options)
{
    usize index;
    {
        std::unique_lock lock(mutex_);
        compiled_cv_.wait(lock, [&] { return !compiling_.contains(key) || stop_.load(std::memory_order_acquire); });
        if (auto it = functions_.find(key); it != functions_.end()) return it->second;
        const auto r = record_index_.find(key);
        if (r == record_index_.end() || stop_.load(std::memory_order_acquire)) return nil;
        index = r->second;
        compiling_.insert(key);
    }
    id<MTLFunction> fn = nil;
    @autoreleasepool {
        const Record& r = records_[index];
        NSError* error = nil;
        id<MTLLibrary> lib = [device_ newLibraryWithSource:[NSString stringWithUTF8String:r.shader.source.c_str()]
                                                   options:options
                                                     error:&error];
        fn = lib ? [lib newFunctionWithName:[NSString stringWithUTF8String:r.shader.entry.c_str()]] : nil;
    }
    {
        std::lock_guard lock(mutex_);
        compiling_.erase(key);
        functions_[key] = fn; // nil too: a program that does not compile is not tried again
        if (fn) {
            const Record& r = records_[index];
            CompiledProgram p;
            p.function = fn;
            p.shader = r.shader;
            p.ucode = r.ucode;
            p.key = r.key;
            ready_programs_.push_back(std::move(p));
            ready_.store(true, std::memory_order_release);
            made_programs_++;
        }
    }
    compiled_cv_.notify_all();
    return fn;
}

// Every 10 s while it runs: how far the cache is (the iPhone's log shows whether it was ready for the game).
void ShaderCache::report_progress()
{
#if defined(__APPLE__)
    // After 45 s the game is playing, not loading: what is left yields to it (on the iPhone the game's sound fell to
    // half speed while three workers at user-initiated QoS compiled on; D-143).
    thread_local bool lowered = false;
    if (!lowered && std::chrono::steady_clock::now() - started_ > std::chrono::seconds(45)) {
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        lowered = true;
    }
#endif
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report_ < std::chrono::seconds(10)) return;
    last_report_ = now;
    PS_LOG_INFO(kChan, "shader cache: {} of {} pipelines and {} of {} programs made ({} from the GPU binary archives) "
        "in {:.0f} s", made_pipelines_, pipeline_records_.size(), made_programs_, records_.size(), archive_hits_,
        std::chrono::duration<double>(now - started_).count());
}

void ShaderCache::work(u32 worker)
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    MTLCompileOptions* options = shader_compile_options();
    NSArray<id<MTLBinaryArchive>>* loaded = archives();
    // This worker's own archive, for the pipelines the loaded ones miss.
    id<MTLBinaryArchive> mine = nil;
    std::string mine_path;
    u64 mine_added = 0, mine_saved = 0;
    if (!dir_.empty() && use_archives_) {
        mine_path = (std::filesystem::path(dir_) / std::format("archive-{:x}-{}.bin",
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count(), worker)).string();
        mine = [device_ newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:nil];
    }
    const auto save = [&] {
        if (!mine || mine_added == mine_saved) return;
        NSError* error = nil;
        if (![mine serializeToURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:mine_path.c_str()]] error:&error]) {
            PS_LOG_WARN(kChan, "shader cache: cannot save {}: {}", mine_path,
                error ? error.localizedDescription.UTF8String : "?");
        }
        mine_saved = mine_added;
    };

    // Pipelines first, in the order they were first needed, each with its two programs.
    for (usize i; (i = next_pipeline_.fetch_add(1)) < pipeline_records_.size();) {
        if (stop_.load(std::memory_order_acquire)) break;
        const PipelineRecord& r = pipeline_records_[i];
        id<MTLFunction> vp = program(r.vp_key, options);
        id<MTLFunction> fp = program(r.fp_key, options);
        if (!vp || !fp) continue;
        @autoreleasepool {
            MTLRenderPipelineDescriptor* d = pipeline_descriptor(r.params, vp, fp);
            NSError* error = nil;
            id<MTLRenderPipelineState> pso = nil;
            bool hit = false;
            if (loaded.count) {
                d.binaryArchives = loaded;
                pso = [device_ newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                         reflection:nil error:&error];
                hit = pso != nil;
            }
            // A pipeline that writes no colour (depth only: shadow maps) loses its fragment stage when compiled, and
            // an archive holding one cannot be saved ("expecting 'fragment' stage in pipeline"; the iPhone, D-143):
            // those are compiled each run instead.
            bool writes_color = false;
            for (u32 c = 0; c < r.params.color_count; c++) writes_color |= r.params.write_masks[c] != 0;
            if (!pso && mine && writes_color) {
                // Compiled into this worker's archive, then made from it.
                if ([mine addRenderPipelineFunctionsWithDescriptor:d error:&error]) {
                    mine_added++;
                    d.binaryArchives = loaded.count ? [loaded arrayByAddingObject:mine] : @[ mine ];
                }
            }
            if (!pso) pso = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
            if (!pso) continue;
            std::lock_guard lock(mutex_);
            ready_pipelines_.push_back({r.vp_key, r.fp_key, r.params, vp, fp, pso});
            ready_.store(true, std::memory_order_release);
            made_pipelines_++;
            if (hit) archive_hits_++;
        }
        if (mine_added - mine_saved >= 400) save();
        report_progress();
    }
    // Then the programs no pipeline named.
    for (usize i; (i = next_program_.fetch_add(1)) < records_.size();) {
        if (stop_.load(std::memory_order_acquire)) break;
        program(records_[i].key, options);
        report_progress();
    }
    save();
    std::lock_guard lock(mutex_);
    archived_ += mine_added;
    if (--workers_left_ == 0) {
        done_ = true;
        PS_LOG_INFO(kChan, "shader cache: {} programs and {} pipelines made ahead in {:.1f} s ({} from the GPU binary "
            "archives, {} added to them)", made_programs_, made_pipelines_,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count(), archive_hits_,
            archived_);
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
