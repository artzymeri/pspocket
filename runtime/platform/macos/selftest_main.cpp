// pspocket-selftest: runs the device self-test probes on the Mac (the same probes the iOS test host runs).
//
//   pspocket-selftest            run every probe
//   pspocket-selftest --list     list probes
//   pspocket-selftest <name>...  run the named probes

#include <cstdio>
#include <cstring>

#include "pspocket/selftest.h"

namespace {

void print_event(void*, ps_selftest_event event, const char* probe, const char* message)
{
    switch (event) {
    case PS_SELFTEST_BEGIN: std::printf("\n== %s: %s\n", probe, message); break;
    case PS_SELFTEST_INFO: std::printf("   %s\n", message); break;
    case PS_SELFTEST_METRIC: std::printf("   * %s\n", message); break;
    case PS_SELFTEST_PASS: std::printf("   PASS  %s\n", message); break;
    case PS_SELFTEST_FAIL: std::printf("   FAIL  %s\n", message); break;
    case PS_SELFTEST_SKIP: std::printf("   SKIP  %s\n", message); break;
    }
    std::fflush(stdout);
}

int find_probe(const char* name)
{
    for (int i = 0; i < ps_selftest_probe_count(); i++)
        if (std::strcmp(ps_selftest_probe_name(i), name) == 0) return i;
    return -1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--list") == 0) {
        for (int i = 0; i < ps_selftest_probe_count(); i++)
            std::printf("%-20s %s\n", ps_selftest_probe_name(i), ps_selftest_probe_description(i));
        return 0;
    }

    std::printf("%s\n", ps_selftest_build_info());
    ps_selftest_summary total{};
    if (argc > 1) {
        for (int a = 1; a < argc; a++) {
            const int index = find_probe(argv[a]);
            if (index < 0) {
                std::fprintf(stderr, "unknown probe: %s (see --list)\n", argv[a]);
                return 2;
            }
            const ps_selftest_summary s = ps_selftest_run_one(index, print_event, nullptr);
            total.passed += s.passed;
            total.failed += s.failed;
            total.skipped += s.skipped;
            total.seconds += s.seconds;
        }
    } else {
        total = ps_selftest_run_all(print_event, nullptr);
    }
    std::printf("\n%d passed, %d failed, %d skipped (%.2f s)\n", total.passed, total.failed, total.skipped, total.seconds);
    return total.failed == 0 ? 0 : 1;
}
