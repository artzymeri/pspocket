#pragma once

// Shake mode (D-1045): seeded perturbations at the points where a PS3 game's threads meet, so races a host runs past
// most of the time show in a few runs on the Mac instead of on someone's phone. Off by default: each point costs one
// relaxed load of a mask (chaos::on). PSPOCKET_SHAKE configures it at the process start:
//
//   PSPOCKET_SHAKE="seed=42;legal"                 every PS3-legal point at its default rate and delay
//   PSPOCKET_SHAKE="seed=42;all"                   every point, the host-adversarial ones too
//   PSPOCKET_SHAKE="seed=7;S1=20000:300;S8"        syscall entry 2% (20000 per million), up to 300 us; S8 at default
//
// Two families, labelled in the log and the summary:
// - PS3-legal: what a PS3 also does, so a game must survive it: a thread delayed at a call (an interrupt, a more urgent
//   thread), a conditional store failing spuriously (the architecture allows it), SPURS picking another ready
//   workload, a disc read or a dialog taking longer, a timer or the audio callback late;
// - host-adversarial: what the PS3 never does but the host can: the timebase running a little fast or slow, other
//   processes taking the cores, the woken thread overtaking the thread that woke it (what D-999 and D-1041 forbid).
// A failure under host-adversarial points only is a robustness gap of pspocket's, not necessarily the game's.
//
// Every decision comes from the calling thread's own stream, seeded from the seed and the thread's name (or its
// order of first use), and every injection is counted by point and kept in a log (PSPOCKET_SHAKE_LOG=FILE: written at
// exit; scripts/shake.sh reads it).

#include <array>
#include <atomic>
#include <string>
#include <string_view>

#include "pspocket/core/types.hpp"

// The mask of enabled points, for C callers (bit p: point p). 0: shake mode off.
extern "C" uint32_t ps_chaos_mask;

namespace pspocket::chaos {

enum Point : u8 {
    S1_Syscall = 1,       // an lv2 syscall's entry: a delay
    S2_HleEntry,          // a library function's entry (HLE): a delay
    S3_AfterWake,         // the thread that woke another, after its call: a delay (legal: preempted)
    S4_AfterPark,         // a thread woken from a wait, before it goes back to guest code: a delay
    S5_ThreadStart,       // a new thread, before its first instruction: a delay
    S6_Timer,             // an lv2 timer's event or a usleep's end: late
    S7_TimebaseRate,      // the timebase a little fast or slow, a while at a time (host-adversarial)
    S8_StoreConditional,  // stwcx./stdcx./PUTLLC fails without a reason (legal by the architecture)
    S9_SpuPoll,           // an SPU's channel read that would wait (mailbox, signal, event): a delay first
    S10_SpursSelect,      // SPURS's choice among ready workloads: another one first
    S11_RsxFifo,          // the RSX thread between command batches: a delay
    S12_FsAio,            // a file read (sys_fs, AIO): a delay
    S13_SysutilDialog,    // a system dialog's or save data callback's answer: later
    S14_Audio,            // cellAudio's mixing period: late
    S15_HostLoad,         // busy host threads competing for the cores (host-adversarial)
    S16_WakeOvertake,     // the waker-first rule skipped: the woken thread goes first (host-adversarial)
    kPoints
};

enum class Family : u8 { Ps3Legal, HostAdversarial };

struct PointInfo {
    const char* id;   // "S1"
    const char* name; // "syscall entry"
    Family family;
    u32 ppm;    // default chance a check injects, per million
    u32 max_us; // default largest delay
};
const PointInfo& info(Point p);
const char* family_name(Family f);

inline bool on(Point p) { return (__atomic_load_n(&ps_chaos_mask, __ATOMIC_RELAXED) >> p) & 1u; }
inline bool any() { return __atomic_load_n(&ps_chaos_mask, __ATOMIC_RELAXED) != 0; }

// One decision on the calling thread's stream: true with the point's chance; counted and logged when true.
bool hit(Point p, u64 detail = 0);
// hit(), then a delay of up to the point's largest (uniform), slept here; returns it in microseconds (0: none).
u32 delay(Point p, u64 detail = 0);
// A number from the calling thread's stream (shuffles); not counted.
u64 next();
// The point's configured largest delay, microseconds.
u32 max_us(Point p);

// "seed=N;legal|all|adversarial;S3=PPM:MAXUS;S8;..." (the top of the file). Points not named are off unless a family
// is named. Returns false, with `error`, on text it does not understand; nothing changes then.
bool configure(std::string_view spec, std::string* error = nullptr);
// From PSPOCKET_SHAKE (once, at the first call); logs what it enabled. Returns whether shake mode is on.
bool configure_from_env();
void disable();
u64 seed();
std::string description(); // "seed 42: S1 syscall entry 2% <=200us, ..."

struct Counts {
    std::array<u64, kPoints> hits{};
    std::array<u64, kPoints> delay_us{};
};
Counts counts();
// The injection log: the first entries (point, the thread's stream ID, microseconds since the start, detail), as text.
std::string log_text(usize max_lines = 100000);
// Writes it to PSPOCKET_SHAKE_LOG when set (pspocket-boot at exit). Returns the path, or "".
std::string write_log();

// A seed for code that would read std::random_device: the shake seed's stream while shaking (so a run repeats), else
// the device.
u64 random_seed();
// 32 random bits for the guest (sys_get_random_number, sys_ss_random_number_generator): a device per thread, or the
// calling thread's shake stream while shaking.
u32 random32();

// The timebase's rate while S7 is on: 1 otherwise; and the timebase it makes of the true one (timebase.cpp).
double timebase_rate();
u64 shaken_timebase(u64 raw);

} // namespace pspocket::chaos
