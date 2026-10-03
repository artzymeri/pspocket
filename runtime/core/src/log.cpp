#include "pspocket/core/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace pspocket::log {
namespace {

void stderr_sink(void*, Level level, std::string_view channel, std::string_view message)
{
    std::fprintf(stderr, "[%.*s] %.*s: %.*s\n", static_cast<int>(level_name(level).size()), level_name(level).data(),
        static_cast<int>(channel.size()), channel.data(), static_cast<int>(message.size()), message.data());
}

std::mutex g_sink_mutex;
Sink g_sink = stderr_sink;
void* g_sink_user = nullptr;
std::atomic<Level> g_min_level{Level::Info};

} // namespace

void set_sink(Sink sink, void* user)
{
    std::lock_guard lock(g_sink_mutex);
    g_sink = sink ? sink : stderr_sink;
    g_sink_user = sink ? user : nullptr;
}

void set_min_level(Level level) { g_min_level.store(level, std::memory_order_relaxed); }

Level min_level() { return g_min_level.load(std::memory_order_relaxed); }

std::string_view level_name(Level level)
{
    switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    case Level::Fatal: return "fatal";
    }
    return "?";
}

void write(Level level, std::string_view channel, std::string_view message)
{
    std::lock_guard lock(g_sink_mutex);
    g_sink(g_sink_user, level, channel, message);
}

void fatal_message(std::string_view channel, std::string_view message)
{
    write(Level::Fatal, channel, message);
    std::abort();
}

} // namespace pspocket::log
