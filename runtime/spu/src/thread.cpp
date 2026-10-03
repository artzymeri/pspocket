// SPU host threads (pspocket/spu/thread.hpp).

#include "pspocket/spu/thread.hpp"

#include <pthread.h>

#include "pspocket/core/log.hpp"
#include "pspocket/spu/runtime.h"

#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace pspocket::spu {

Thread::Thread(SPUContext& ctx, std::string name) : ctx_(ctx), name_(std::move(name))
{
    if (!ctx_.host) ps_spu_host_attach(&ctx_);
}

Thread::~Thread()
{
    if (joinable_) {
        request_stop();
        join();
    }
    delete static_cast<pthread_t*>(handle_);
}

bool Thread::start(ExitCallback on_exit)
{
    if (joinable_) return false;
    on_exit_ = std::move(on_exit);
    ps_spu_clear_stop(&ctx_);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kStackSize);
#if defined(__APPLE__)
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INITIATED, 0);
#endif
    if (!handle_) handle_ = new pthread_t{};
    running_.store(true, std::memory_order_release);
    const int rc = pthread_create(static_cast<pthread_t*>(handle_), &attr, &Thread::entry, this);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        running_.store(false, std::memory_order_release);
        PS_LOG_ERROR("spu", "cannot create the host thread for {} (error {})", name_, rc);
        return false;
    }
    joinable_ = true;
    return true;
}

void Thread::request_stop() { ps_spu_request_stop(&ctx_); }

void Thread::join()
{
    if (!joinable_) return;
    pthread_join(*static_cast<pthread_t*>(handle_), nullptr);
    joinable_ = false;
}

void* Thread::entry(void* arg)
{
    static_cast<Thread*>(arg)->body();
    return nullptr;
}

void Thread::body()
{
#if defined(__APPLE__)
    pthread_setname_np(name_.c_str());
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), name_.substr(0, 15).c_str());
#endif
    const u32 reason = ps_spu_run(&ctx_);
    if (on_exit_) {
        on_exit_(ctx_, reason);
    } else if (reason == PS_SPU_STOP_EXCEPTION || reason == PS_SPU_STOP_HALT) {
        PS_FATAL("spu", "{} stopped by {} at 0x{:05x} (exception {}, detail 0x{:x}) and no exit callback handles it",
            name_, reason == PS_SPU_STOP_HALT ? "a halt" : "an exception", ctx_.pc, ctx_.exception, ctx_.stop_code);
    }
    running_.store(false, std::memory_order_release);
}

} // namespace pspocket::spu
