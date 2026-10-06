// thread_cpu_report: a busy thread shows up by name with the CPU time it used.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <ctime>
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
        // 300 ms of this thread's own CPU time, however loaded the machine is (a wall-clock spin got less than 0.2 s of
        // CPU while builds ran beside it), within 20 s.
        const auto cpu = [] {
            timespec ts{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
        };
        const double start = cpu();
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        volatile u64 x = 0;
        while (cpu() - start < 0.3 && std::chrono::steady_clock::now() < give_up) x = x + 1;
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
