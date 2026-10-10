#pragma once

// Checks that may run on a thread other than the test's own: a kernel thread's entry, an SPU thread's log sink, a
// test's std::thread (D-585). Catch2's assertions are not thread-safe (D-571): two threads asserting at once race on
// its counters and its last-assertion record. THREAD_CHECK and THREAD_REQUIRE are CHECK and REQUIRE on the test's
// thread. Elsewhere a check that fails is kept, with its text and place, and reported on the test's thread by
// report_thread_checks() (the fixtures' destructors call it); a THREAD_REQUIRE that fails also ends the process, as
// REQUIRE's exception would on that thread. A check that passes takes no lock, so it orders nothing between threads
// and hides no race from ThreadSanitizer.

#include <catch_amalgamated.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pspocket::test {

struct ThreadChecks {
    std::thread::id test_thread = std::this_thread::get_id(); // initialised before main, on the thread Catch2 runs on
    std::mutex mutex;
    std::vector<std::string> failures; // guarded by mutex
};
inline ThreadChecks g_thread_checks;

inline bool on_test_thread() { return std::this_thread::get_id() == g_thread_checks.test_thread; }

inline void thread_check_failed(bool required, const char* expr, const char* file, int line)
{
    std::string text = std::format("{}:{}: {}({}) failed on a thread other than the test's", file, line,
        required ? "THREAD_REQUIRE" : "THREAD_CHECK", expr);
    if (required) {
        std::fprintf(stderr, "%s\n", text.c_str());
        std::abort();
    }
    std::lock_guard lock(g_thread_checks.mutex);
    g_thread_checks.failures.push_back(std::move(text));
}

// On the test's thread, once the other threads are done: what failed on them, as failed checks.
inline void report_thread_checks()
{
    std::vector<std::string> failures;
    {
        std::lock_guard lock(g_thread_checks.mutex);
        failures.swap(g_thread_checks.failures);
    }
    if (std::uncaught_exceptions() != 0) return;
    for (const std::string& f : failures) {
        try { // a destructor must not throw: Catch2 throws even from FAIL_CHECK once --abort is stopping the run
            FAIL_CHECK(f);
        } catch (...) {
        }
    }
}

} // namespace pspocket::test

#define PS_THREAD_CHECK_(required, assertion, ...)                                                                     \
    do {                                                                                                               \
        if (::pspocket::test::on_test_thread()) {                                                                      \
            assertion(__VA_ARGS__);                                                                                    \
        } else if (!(__VA_ARGS__)) {                                                                                   \
            ::pspocket::test::thread_check_failed(required, #__VA_ARGS__, __FILE__, __LINE__);                         \
        }                                                                                                              \
    } while (false)
#define THREAD_CHECK(...) PS_THREAD_CHECK_(false, CHECK, __VA_ARGS__)
#define THREAD_REQUIRE(...) PS_THREAD_CHECK_(true, REQUIRE, __VA_ARGS__)
