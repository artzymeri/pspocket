// iOS host glue (pspocket/platform/ios.h): log routing and memory numbers for the app shell.

#include "pspocket/platform/ios.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <mach/mach.h>
#include <os/log.h>
#include <os/proc.h>

#include "pspocket/core/log.hpp"

namespace {

using pspocket::log::Level;

static_assert(static_cast<int>(Level::Trace) == PS_IOS_LOG_TRACE);
static_assert(static_cast<int>(Level::Debug) == PS_IOS_LOG_DEBUG);
static_assert(static_cast<int>(Level::Info) == PS_IOS_LOG_INFO);
static_assert(static_cast<int>(Level::Warn) == PS_IOS_LOG_WARN);
static_assert(static_cast<int>(Level::Error) == PS_IOS_LOG_ERROR);
static_assert(static_cast<int>(Level::Fatal) == PS_IOS_LOG_FATAL);

constexpr const char* kSubsystem = "com.artzymeri.pspocket";

std::mutex g_mutex;
ps_ios_log_callback g_callback = nullptr;
void* g_user = nullptr;
std::unordered_map<std::string, os_log_t> g_logs; // one os_log per channel, created on first use

os_log_t channel_log(const std::string& channel)
{
    auto it = g_logs.find(channel);
    if (it == g_logs.end()) it = g_logs.emplace(channel, os_log_create(kSubsystem, channel.c_str())).first;
    return it->second;
}

os_log_type_t to_os_log_type(Level level)
{
    switch (level) {
    case Level::Trace:
    case Level::Debug: return OS_LOG_TYPE_DEBUG;
    case Level::Info:
    case Level::Warn: return OS_LOG_TYPE_DEFAULT; // info-type lines are not shown by default; these should be
    case Level::Error: return OS_LOG_TYPE_ERROR;
    case Level::Fatal: return OS_LOG_TYPE_FAULT;
    }
    return OS_LOG_TYPE_DEFAULT;
}

void ios_sink(void*, Level level, std::string_view channel, std::string_view message)
{
    const std::string chan(channel);
    const std::string msg(message);
    os_log_t log;
    ps_ios_log_callback callback;
    void* user;
    {
        std::lock_guard lock(g_mutex);
        log = channel_log(chan);
        callback = g_callback;
        user = g_user;
    }
    os_log_with_type(log, to_os_log_type(level), "%{public}s", msg.c_str());
    if (callback) {
        callback(user, static_cast<ps_ios_log_level>(level), chan.c_str(), msg.c_str());
    } else {
        const std::string_view name = pspocket::log::level_name(level);
        std::fprintf(stderr, "[%.*s] %s: %s\n", static_cast<int>(name.size()), name.data(), chan.c_str(), msg.c_str());
    }
}

} // namespace

extern "C" void ps_ios_install_log_sink(ps_ios_log_callback callback, void* user)
{
    {
        std::lock_guard lock(g_mutex);
        g_callback = callback;
        g_user = user;
    }
    pspocket::log::set_sink(ios_sink, nullptr);
}

extern "C" uint64_t ps_ios_memory_footprint(void)
{
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.phys_footprint;
}

extern "C" uint64_t ps_ios_available_memory(void) { return os_proc_available_memory(); }
