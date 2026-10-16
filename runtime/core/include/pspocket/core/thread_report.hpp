#pragma once

// Host threads' CPU time, for the status lines (CLAUDE.md rule 8: measure before optimizing). On the phone six cores
// serve six SPU threads, the PPU threads, the RSX thread and the sound: which of them holds a slow stretch back is
// read off the log instead of guessed (docs/improve.md).

#include <string>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket {

// One line: the CPU time the process used since the previous call (at the first call, since each thread started),
// as cores over `seconds`, then the `top` busiest threads by name ("SPU 3 4.12 s"); threads that used under 10 ms are
// left out. One caller at a time (the status line). Empty where the platform has no per-thread times.
std::string thread_cpu_report(double seconds, usize top = 8);

// Every host thread of the process now: its system ID, name and the CPU time it used since it started (user and
// system). Any number of callers (no state kept); empty where the platform has no per-thread times.
struct ThreadCpu {
    u64 id = 0;
    std::string name;
    u64 ns = 0;
};
std::vector<ThreadCpu> thread_cpu_times();

} // namespace pspocket
