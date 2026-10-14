// Asynchronous program and pipeline compilation (D-158): see AsyncCompiler in metal_internal.hpp. RPCS3 does the same
// with its asynchronous shader mode ("Async with Shader Interpreter" or skipped draws, rpcs3/Emu/RSX/Program/
// ShaderCache and VKAsyncScheduler); pspocket skips the draw past a per-frame wait.

#include "metal_internal.hpp"

#include <algorithm>
#include <format>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

namespace pspocket::rsx::metal {

AsyncCompiler::AsyncCompiler(id<MTLDevice> device, u32 threads) : device_(device)
{
    // Half the cores, as the shader cache's (D-143): a scene the game shows for the first time needs hundreds at once
    // (GTA V's first mission: 300 programs and 260 pipelines in one stretch, run 098).
    if (threads == 0) threads = std::clamp<u32>(std::thread::hardware_concurrency() / 2, 2, 4);
    for (u32 i = 0; i < threads; i++) workers_.emplace_back([this] { work(); });
}

AsyncCompiler::~AsyncCompiler()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    work_cv_.notify_all();
    for (std::thread& t : workers_) t.join();
}

bool AsyncCompiler::queued(const std::string& program_key)
{
    std::lock_guard lock(mutex_);
    return program_keys_.contains(program_key);
}

bool AsyncCompiler::queued(const PipelineKey& key)
{
    std::lock_guard lock(mutex_);
    return pipeline_keys_.contains(key);
}

void AsyncCompiler::queue(ProgramJob job)
{
    {
        std::lock_guard lock(mutex_);
        if (!program_keys_.insert(job.key).second) return;
        programs_.push_back(std::move(job));
    }
    work_cv_.notify_one();
}

void AsyncCompiler::queue(PipelineJob job)
{
    {
        std::lock_guard lock(mutex_);
        if (!pipeline_keys_.emplace(job.key, true).second) return;
        pipelines_.push_back(std::move(job));
    }
    work_cv_.notify_one();
}

bool AsyncCompiler::wait_until(std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock(mutex_);
    return ready_cv_.wait_until(lock, deadline, [this] { return has_ready_.load(std::memory_order_relaxed); });
}

void AsyncCompiler::wait_idle()
{
    std::unique_lock lock(mutex_);
    ready_cv_.wait(lock, [this] { return programs_.empty() && pipelines_.empty() && busy_ == 0; });
}

bool AsyncCompiler::take(Ready& out)
{
    if (!has_ready_.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(mutex_);
    out = std::move(ready_);
    ready_ = {};
    has_ready_.store(false, std::memory_order_release);
    return true;
}

void AsyncCompiler::work()
{
#if defined(__APPLE__)
    // As the shader cache's workers while the game loads (D-143): a draw is waiting for this.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    MTLCompileOptions* options = shader_compile_options();
    for (;;) {
        std::optional<ProgramJob> program;
        std::optional<PipelineJob> pipeline;
        {
            std::unique_lock lock(mutex_);
            work_cv_.wait(lock, [this] { return stop_ || !programs_.empty() || !pipelines_.empty(); });
            if (stop_) return;
            // Pipelines first: one is queued only once its two programs are made, and it is the last thing its draw
            // waits for. Programs first left the draws of a new scene waiting for every program of the scene (run 098).
            if (!pipelines_.empty()) {
                pipeline = std::move(pipelines_.front());
                pipelines_.pop_front();
            } else {
                program = std::move(programs_.front());
                programs_.pop_front();
            }
            busy_++;
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto elapsed = [&] {
            return static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        };
        @autoreleasepool {
            if (program) {
                NSError* error = nil;
                id<MTLLibrary> lib = [device_ newLibraryWithSource:[NSString stringWithUTF8String:program->shader.source.c_str()]
                                                           options:options
                                                             error:&error];
                id<MTLFunction> fn = lib ? [lib newFunctionWithName:[NSString stringWithUTF8String:program->shader.entry.c_str()]]
                                         : nil;
                const u64 ns = elapsed();
                std::lock_guard lock(mutex_);
                if (fn) {
                    CompiledProgram p;
                    p.function = fn;
                    p.shader = std::move(program->shader);
                    p.ucode = std::move(program->ucode);
                    p.key = program->key;
                    ready_.programs.emplace_back(std::move(p), ns);
                } else {
                    ready_.failed_programs.emplace_back(program->key,
                        std::format("MSL compilation of {} {} failed: {}\nsource:\n{}", program->what, program->shader.entry,
                            error ? error.localizedDescription.UTF8String : "entry missing", program->shader.source));
                }
                program_keys_.erase(program->key);
            } else {
                NSError* error = nil;
                id<MTLRenderPipelineState> pso = [device_ newRenderPipelineStateWithDescriptor:pipeline->descriptor
                                                                                         error:&error];
                const u64 ns = elapsed();
                std::lock_guard lock(mutex_);
                if (pso) {
                    ready_.pipelines.push_back({pipeline->key, pso, pipeline->vp_key, pipeline->fp_key, pipeline->params, ns});
                } else {
                    ready_.failed_pipelines.emplace_back(pipeline->key, std::format("render pipeline creation failed: {}",
                        error ? error.localizedDescription.UTF8String : "?"));
                }
                pipeline_keys_.erase(pipeline->key);
            }
        }
        {
            std::lock_guard lock(mutex_);
            busy_--;
            has_ready_.store(true, std::memory_order_release);
        }
        ready_cv_.notify_all();
    }
}

// ---- the RSX thread's side ----------------------------------------------------------------------------------------

// What the background threads finished joins the backend's programs and pipelines, recorded in the shader cache as if
// the RSX thread had made them.
void MetalBackend::Impl::take_async()
{
    if (!async) return;
    AsyncCompiler::Ready r;
    if (!async->take(r)) return;
    for (auto& [p, ns] : r.programs) {
        auto [it, inserted] = programs.insert_or_assign(p.key, std::move(p));
        if (shader_cache) shader_cache->record_program(it->second);
        if (!inserted) forget_program_lookups(); // the key's program changed under the lookups that hold it
        stats.programs_compiled++;
        stats.async_programs++;
        stats.async_compile_ns += ns;
    }
    for (auto& pl : r.pipelines) {
        add_pipeline(pl.key, pl.pso);
        if (shader_cache) shader_cache->record_pipeline(pl.vp_key, pl.fp_key, pl.params);
        stats.pipelines_created++;
        stats.async_pipelines++;
        stats.async_compile_ns += pl.ns;
    }
    for (auto& [key, why] : r.failed_programs) {
        async_failed_programs.insert(key);
        fail(std::move(why)); // a translator bug, not a guest problem: loud, once
    }
    for (auto& [key, why] : r.failed_pipelines) {
        async_failed_pipelines.emplace(key, true);
        fail(std::move(why));
    }
}

bool MetalBackend::Impl::wait_async(const std::function<bool()>& done)
{
    for (;;) {
        take_async();
        if (done()) return true;
        if (compile_wait_left_ns <= 0) return false;
        const auto t0 = std::chrono::steady_clock::now();
        async->wait_until(t0 + std::chrono::nanoseconds(compile_wait_left_ns));
        const s64 waited = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        compile_wait_left_ns -= std::max<s64>(waited, 1);
        stats.compile_wait_ns += static_cast<u64>(waited);
    }
}

} // namespace pspocket::rsx::metal
