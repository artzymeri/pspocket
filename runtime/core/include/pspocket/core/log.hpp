#pragma once

#include <format>
#include <string>
#include <string_view>

#include "pspocket/core/types.hpp"

// Structured logging. Every line has a level and a channel ("mem", "ppu", "lv2", "hle", ...).
// Hosts install a sink (stderr by default; the iOS app routes to its UI and os_log).
namespace pspocket::log {

enum class Level : u8 { Trace, Debug, Info, Warn, Error, Fatal };

using Sink = void (*)(void* user, Level level, std::string_view channel, std::string_view message);

void set_sink(Sink sink, void* user);
void set_min_level(Level level);
Level min_level();
std::string_view level_name(Level level);

void write(Level level, std::string_view channel, std::string_view message);

template <class... Args>
void print(Level level, std::string_view channel, std::format_string<Args...> fmt, Args&&... args)
{
    if (level < min_level()) return;
    write(level, channel, std::format(fmt, std::forward<Args>(args)...));
}

// Logs at Fatal and aborts. Dev builds use this for "fail loudly" (CLAUDE.md rule 5).
[[noreturn]] void fatal_message(std::string_view channel, std::string_view message);

template <class... Args>
[[noreturn]] void fatal(std::string_view channel, std::format_string<Args...> fmt, Args&&... args)
{
    fatal_message(channel, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace pspocket::log

#define PS_LOG_TRACE(chan, ...) ::pspocket::log::print(::pspocket::log::Level::Trace, chan, __VA_ARGS__)
#define PS_LOG_DEBUG(chan, ...) ::pspocket::log::print(::pspocket::log::Level::Debug, chan, __VA_ARGS__)
#define PS_LOG_INFO(chan, ...) ::pspocket::log::print(::pspocket::log::Level::Info, chan, __VA_ARGS__)
#define PS_LOG_WARN(chan, ...) ::pspocket::log::print(::pspocket::log::Level::Warn, chan, __VA_ARGS__)
#define PS_LOG_ERROR(chan, ...) ::pspocket::log::print(::pspocket::log::Level::Error, chan, __VA_ARGS__)
#define PS_FATAL(chan, ...) ::pspocket::log::fatal(chan, __VA_ARGS__)

// Internal invariant. Not for guest-triggerable conditions: those log and return an error code.
#define PS_CHECK(cond)                                                                                  \
    do {                                                                                                \
        if (!(cond)) [[unlikely]]                                                                       \
            ::pspocket::log::fatal("check", "{}:{}: check failed: {}", __FILE__, __LINE__, #cond);      \
    } while (0)
