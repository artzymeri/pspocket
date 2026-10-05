// thread_cpu_report: a busy thread shows up by name with the CPU time it used.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <pthread.h>
#include <string>
#include <thread>

#include "pspocket/core/thread_report.hpp"

using namespace pspocket;

TEST_CASE("thread_cpu_report names a busy thread with the CPU time it used")
{
#if defined(__APPLE__)
    (void)thread_cpu_report(1.0); // the threads that exist now: only what they use from here counts
    std::atomic<bool> spun{false}, done{false};
    std::thread busy([&] {
        pthread_setname_np("busy-report-test");
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        volatile u64 x = 0;
        while (std::chrono::steady_clock::now() < end) x = x + 1;
        spun = true;
        while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(1)); // alive while it is reported
    });
    while (!spun) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const std::string report = thread_cpu_report(0.3);
    done = true;
    busy.join();
    INFO(report);
    const auto at = report.find("busy-report-test ");
    REQUIRE(at != std::string::npos);
    const double used = std::stod(report.substr(at + std::string("busy-report-test ").size()));
    CHECK(used > 0.2);
    CHECK(used < 0.6);
    CHECK(report.starts_with("CPU "));
#else
    CHECK(thread_cpu_report(1.0).empty());
#endif
}
