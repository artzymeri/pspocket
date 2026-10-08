// cellSpurs against libsre.sprx, Sony's module of the library, from the user's installed system software (D-193's
// method, D-505): the same calls with made-up arguments go to pspocket's native SPURS and to libsre, and the answers
// and every byte of the work area must be the same. Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH,
// firmware_compare.hpp); no game data.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

// The work area: a SPURS queue line, an output word, a semaphore, an LF queue, each 128-aligned, and bytes around them
// that nothing should touch.
constexpr u32 kArea = kData + 0x10000, kAreaSize = 0x10000;
constexpr u32 kQueue = kArea + 0x100, kOut = kArea + 0x400, kSem = kArea + 0x800, kLfq = kArea + 0xC00;
constexpr u32 kSpurs = kArea + 0x2000;   // a CellSpurs (8 KB)
constexpr u32 kTaskset = kArea + 0x4000; // a CellSpursTaskset2 (10496 bytes)
constexpr u32 kInfo = kArea + 0x7000;    // a CellSpursTasksetInfo (0x2490 bytes)
constexpr u32 kWinfo = kArea + 0x9800;   // a CellSpursWorkloadInfo
constexpr u32 kElf = kArea + 0xA000;     // SPU ELF images (4 of 0x400 bytes)
constexpr u8 kFill = 0xA5;

struct Call {
    const char* fn;
    std::vector<u64> args;
    std::function<void(u8*)> setup; // what to write into the reset work area first
    std::string what;
};

struct Answer {
    u32 r3 = 0;
    std::vector<u8> area;
};

u32 ours(GuestThread& t, const char* name, const std::vector<u64>& args)
{
    const Function& fn = GuestThread::find(name);
    for (usize i = 0; i < args.size(); i++) t.ctx.gpr[3 + i] = args[i];
    fn.fn(t.ctx, t.f.base());
    return static_cast<u32>(t.ctx.gpr[3]);
}

u32 theirs(GuestThread& t, const FirmwareLibrary& sony, const char* name, const std::vector<u64>& args)
{
    // A function known only by its NID is named "<library>_<NID>".
    const std::string_view n(name);
    const u32 nid = n.starts_with("cellSpurs_") ? static_cast<u32>(std::stoul(std::string(n.substr(10)), nullptr, 16))
                                                : recomp::nid_of(name);
    const auto it = sony.module->functions.find({"cellSpurs", nid});
    REQUIRE(it != sony.module->functions.end());
    std::array<u64, 8> a{};
    std::copy(args.begin(), args.end(), a.begin());
    return static_cast<u32>(call_guest(t.ctx, it->second, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
}

Answer run(u8* b, const Call& c, const std::function<u32(const char*, const std::vector<u64>&)>& call)
{
    std::memset(b + kArea, kFill, kAreaSize);
    if (c.setup) c.setup(b);
    Answer a;
    a.r3 = call(c.fn, c.args);
    a.area.assign(b + kArea, b + kArea + kAreaSize);
    return a;
}

void compare(GuestThread& t, const FirmwareLibrary& sony, const std::vector<Call>& calls)
{
    u8* b = t.f.base();
    usize differ = 0;
    for (const Call& c : calls) {
        const Answer o = run(b, c, [&](const char* n, const std::vector<u64>& a) { return ours(t, n, a); });
        const Answer s = run(b, c, [&](const char* n, const std::vector<u64>& a) { return theirs(t, sony, n, a); });
        if (o.r3 == s.r3 && o.area == s.area) continue;
        usize at = 0;
        while (at < o.area.size() && o.area[at] == s.area[at]) at++;
        if (++differ <= 50)
            UNSCOPED_INFO(std::format("{} {}: {:#x} (pspocket) vs {:#x} (Sony){}", c.fn, c.what, o.r3, s.r3,
                at < o.area.size() ? std::format("; area differs first at +{:#x} ({:#04x} vs {:#04x})", at, o.area[at],
                                         s.area[at])
                                   : std::string()));
    }
    INFO(std::format("{} of {} calls differ", differ, calls.size()));
    CHECK(differ == 0);
}

// A line of made-up words, then the fields a case sets.
void random_line(u8* b, u32 at, std::mt19937& rng)
{
    for (u32 i = 0; i < 128; i += 4) ps_write32(b, at + i, static_cast<u32>(rng()));
}

// The getters of one structure: NULL, misaligned and good structures and outputs, over made-up contents.
void getter_calls(std::vector<Call>& out, const char* fn, u32 object, std::mt19937& rng)
{
    for (u32 round = 0; round < 8; round++) {
        const u32 seed = static_cast<u32>(rng());
        const auto setup = [object, seed](u8* b) {
            std::mt19937 r(seed);
            random_line(b, object, r);
        };
        for (const u32 obj : {object, 0u, object + 4, object + 0x40, object + 0x10})
            for (const u32 o : {kOut, 0u})
                out.push_back({fn, {obj, o}, setup, std::format("({:#x}, {:#x}) round {}", obj, o, round)});
    }
}

// An SPU ELF at `at` (0x400 bytes): the header, `phnum` program headers from 0x40, segments from 0x100 (one loadable
// segment a GUID section starts when `guid`).
void put_spu_elf(u8* b, u32 at, bool guid, u16 machine, u32 phnum, std::mt19937& rng)
{
    std::memset(b + at, 0, 0x400);
    std::memcpy(b + at, "\x7F" "ELF\x01\x02\x01", 7);
    ps_write16(b, at + 16, 2);
    ps_write16(b, at + 18, machine);
    ps_write32(b, at + 28, 0x40); // e_phoff
    ps_write16(b, at + 42, 0x20); // e_phentsize
    ps_write16(b, at + 44, static_cast<u16>(phnum));
    for (u32 i = 0; i < phnum; i++) {
        const u32 ph = at + 0x40 + 0x20 * i;
        ps_write32(b, ph, i == 0 ? 4 : 1); // a note first, then loadable segments
        ps_write32(b, ph + 4, 0x100 + 0x40 * i);
        for (u32 k = 0; k < 16; k++) ps_write32(b, at + 0x100 + 0x40 * i + 4 * k, static_cast<u32>(rng()));
    }
    if (guid && phnum > 1) {
        const u32 seg = at + 0x100 + 0x40 * (phnum - 1); // the last loadable one
        for (u32 k = 0; k < 4; k++) {
            const u32 g = static_cast<u32>(rng()) & 0xFFFF;
            ps_write32(b, seg + 4 * k, 0x42000002u | (g << 9) | (k << 7)); // ila $2, (g << 2) | k
        }
    }
}

// A made-up CellSpurs: random bytes, 16 or 32 workloads, the enabled workloads given.
void put_spurs(u8* b, std::mt19937& rng, bool second, u32 enabled)
{
    for (u32 i = 0; i < 0x2000; i += 4) ps_write32(b, kSpurs + i, static_cast<u32>(rng()));
    b[kSpurs + 0x74] = static_cast<u8>(second ? b[kSpurs + 0x74] | 0x40 : b[kSpurs + 0x74] & ~0x40);
    ps_write32(b, kSpurs + 0xB0, enabled);
}

} // namespace

TEST_CASE("cellSpurs' workload and taskset information against libsre", "[hle][lle][spurs]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "cellSpurs", false);
    std::mt19937 rng(20261009);
    std::vector<Call> calls;
    // cellSpursGetWorkloadInfo: both kinds of instance, every workload ID around the limits, enabled or not.
    for (u32 round = 0; round < 6; round++) {
        const u32 seed = static_cast<u32>(rng());
        const bool second = round % 2;
        const u32 enabled = round < 2 ? 0xFFFFFFFFu : static_cast<u32>(rng());
        const auto setup = [=](u8* b) {
            std::mt19937 r(seed);
            put_spurs(b, r, second, enabled);
        };
        for (u32 wid = 0; wid < 34; wid++)
            calls.push_back({"cellSpursGetWorkloadInfo", {kSpurs, wid, kWinfo}, setup,
                std::format("wid {} second {} enabled {:#x}", wid, second, enabled)});
        for (const u32 sp : {0u, kSpurs + 0x40})
            calls.push_back({"cellSpursGetWorkloadInfo", {sp, 1, kWinfo}, setup, std::format("spurs {:#x}", sp)});
        calls.push_back({"cellSpursGetWorkloadInfo", {kSpurs, 1, 0}, setup, "no info"});
    }
    // cellSpursGetTasksetInfo: tasksets of both sizes on such an instance, their tasks in every state, with ELFs that
    // have a GUID, none, the wrong machine, no program headers, or are not there.
    for (u32 round = 0; round < 12; round++) {
        const u32 seed = static_cast<u32>(rng());
        const bool second = round % 2;
        const u32 wid = round < 8 ? (second ? 17 + round : round) : std::array<u32, 4>{0, 31, 32, 99}[round - 8];
        const u32 size = round % 3 == 0 ? 10496 : round % 3 == 1 ? 6400 : 10495;
        const auto setup = [=](u8* b) {
            std::mt19937 r(seed);
            put_spurs(b, r, second, round == 7 ? 0 : 0xFFFFFFFFu);
            for (u32 i = 0; i < 10496; i += 4) ps_write32(b, kTaskset + i, static_cast<u32>(r()));
            ps_write64(b, kTaskset + 0x60, kSpurs);
            ps_write32(b, kTaskset + 0x74, wid);
            ps_write32(b, kTaskset + 0x1890, size);
            put_spu_elf(b, kElf, true, 23, 3, r);
            put_spu_elf(b, kElf + 0x400, false, 23, 3, r);
            put_spu_elf(b, kElf + 0x800, true, 21, 3, r);
            put_spu_elf(b, kElf + 0xC00, true, 23, 0, r);
            const u32 elves[] = {kElf, kElf + 0x400, kElf + 0x800, kElf + 0xC00, kElf + 4, 0, 0x40000000u, kElf + 3,
                                 kArea + 0x100};
            for (u32 i = 0; i < 128; i++) {
                const u32 ti = kTaskset + 0x80 + 0x30 * i;
                ps_write64(b, ti + 0x10, (u64{r()} << 32) | elves[r() % std::size(elves)]);
            }
        };
        calls.push_back({"cellSpursGetTasksetInfo", {kTaskset, kInfo}, setup,
            std::format("round {} wid {} size {}", round, wid, size)});
        if (round == 0) {
            calls.push_back({"cellSpursGetTasksetInfo", {0, kInfo}, setup, "no taskset"});
            calls.push_back({"cellSpursGetTasksetInfo", {kTaskset, 0}, setup, "no info"});
            calls.push_back({"cellSpursGetTasksetInfo", {kTaskset + 0x10, kInfo}, setup, "misaligned"});
        }
    }
    compare(t, sony, calls);
}

namespace {

// A jobbin2 binary at kElf: `phnum` program headers (3 or 5) of the given types and flags, segments from 0x200, the
// tag "BIN2" at the first segment's +32.
struct Jobbin2 {
    u32 phnum = 3;
    u32 types[5] = {1, 1, 4, 0, 0};
    u32 flags[5] = {5, 6, 4, 0, 0};
    u32 tag = 0x42494E32;
    u32 phoff = 52;
    u32 first_offset = 0x200;
    u32 bss = 0x40;
};

void put_jobbin2(u8* b, const Jobbin2& j)
{
    std::memset(b + kElf, 0, 0x1000);
    std::memcpy(b + kElf, "\x7F" "ELF\x01\x02\x01", 7);
    ps_write16(b, kElf + 18, 23);
    ps_write32(b, kElf + 28, j.phoff);
    ps_write16(b, kElf + 42, 32);
    ps_write16(b, kElf + 44, static_cast<u16>(j.phnum));
    for (u32 i = 0; i < 5; i++) {
        const u32 ph = kElf + 52 + 32 * i;
        ps_write32(b, ph, j.types[i]);
        ps_write32(b, ph + 4, i == 0 ? j.first_offset : 0x200 + 0x100 * i);
        ps_write32(b, ph + 8, 0x3000 + 0x400 * i);   // vaddr
        ps_write32(b, ph + 16, 0x100);              // filesz
        ps_write32(b, ph + 20, 0x100 + (j.flags[i] & 2 ? j.bss : 0)); // memsz
        ps_write32(b, ph + 24, j.flags[i]);
    }
    ps_write32(b, kElf + j.first_offset + 32, j.tag);
}

} // namespace

TEST_CASE("cellSpurs' exception handlers, victim hints, jobbin2 headers and exit code containers against libsre",
    "[hle][lle][spurs]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "cellSpurs", false);
    std::mt19937 rng(20261010);
    std::vector<Call> calls;
    for (u32 round = 0; round < 8; round++) {
        const u32 seed = static_cast<u32>(rng());
        const bool second = round % 2, exception = round == 5;
        const u32 enabled = round < 4 ? 0xFFFFFFFFu : static_cast<u32>(rng());
        const auto setup = [=](u8* b) {
            std::mt19937 r(seed);
            put_spurs(b, r, second, enabled);
            ps_write32(b, kSpurs + 0xD6C, exception ? 1 : 0);
            for (u32 w = 0; w < 32; w++) { // every other F entry free
                const u32 fe = w < 16 ? kSpurs + 0x100 + 0x80 * w : kSpurs + 0x1200 + 0x80 * (w - 16);
                if (w % 2 == 0) ps_write64(b, fe, 0);
            }
        };
        for (const u32 wid : {0u, 1u, 2u, 15u, 16u, 17u, 31u, 32u}) {
            calls.push_back({"cellSpursSetExceptionEventHandler", {kSpurs, wid, 0x12345678, 0xABCDEF01}, setup,
                std::format("set wid {} round {}", wid, round)});
            calls.push_back({"cellSpursUnsetExceptionEventHandler", {kSpurs, wid}, setup,
                std::format("unset wid {} round {}", wid, round)});
        }
        for (const u32 sp : {0u, kSpurs + 0x20}) {
            calls.push_back({"cellSpursSetExceptionEventHandler", {sp, 2, 0x12345678, 1}, setup, "bad spurs"});
            calls.push_back({"cellSpursUnsetExceptionEventHandler", {sp, 2}, setup, "bad spurs"});
        }
        calls.push_back({"cellSpursSetExceptionEventHandler", {kSpurs, 2, 0, 1}, setup, "no handler"});
        // The victim hints up to the system call, which libsre's error report would follow into made-up memory.
        const auto no_system_workload = [=](u8* b) {
            setup(b);
            ps_write32(b, kSpurs + 0xD80, ps_read32(b, kSpurs + 0xD80) & ~0x02000000u);
        };
        calls.push_back({"cellSpursSetPreemptionVictimHints", {kSpurs, kOut}, no_system_workload, "no system workload"});
        calls.push_back({"cellSpursSetPreemptionVictimHints", {0, kOut}, setup, "NULL"});
        calls.push_back({"cellSpursSetPreemptionVictimHints", {kSpurs, 0}, setup, "NULL array"});
        calls.push_back({"cellSpursSetPreemptionVictimHints", {kSpurs + 8, kOut}, setup, "misaligned"});
        for (const u32 sp : {kSpurs, 0u, kSpurs + 4})
            for (const u32 o : {kOut, 0u})
                calls.push_back({"cellSpurs_E279681F", {sp, o}, setup, std::format("{:#x} {:#x}", sp, o)});
    }
    // jobbin2 headers: both shapes, and each way of not being one.
    std::vector<std::pair<Jobbin2, std::string>> bins;
    bins.push_back({Jobbin2{}, "three headers"});
    Jobbin2 five{5, {1, 1, 1, 1, 4}, {5, 5, 5, 6, 4}};
    bins.push_back({five, "five headers"});
    Jobbin2 j = five;
    j.tag = 0x62696E32;
    bins.push_back({j, "bin2"});
    j = five;
    j.tag = 0x42494E33;
    bins.push_back({j, "wrong tag"});
    j = five;
    j.flags[1] = 6;
    bins.push_back({j, "two writable"});
    j = five;
    j.types[4] = 1;
    bins.push_back({j, "no note"});
    j = Jobbin2{};
    j.phnum = 4;
    bins.push_back({j, "four headers"});
    j = Jobbin2{};
    j.phoff = 64;
    bins.push_back({j, "headers elsewhere"});
    j = Jobbin2{};
    j.first_offset = 0x208;
    bins.push_back({j, "misaligned code"});
    j = Jobbin2{};
    j.bss = 0;
    bins.push_back({j, "no bss"});
    j = Jobbin2{};
    j.flags[1] = 5;
    j.types[1] = 1;
    bins.push_back({j, "no writable segment"});
    for (const auto& [bin, what] : bins) {
        const auto setup = [bin](u8* b) { put_jobbin2(b, bin); };
        calls.push_back({"cellSpursJobHeaderSetJobbin2Param", {kOut, kElf}, setup, what});
    }
    calls.push_back({"cellSpursJobHeaderSetJobbin2Param", {0, kElf}, [](u8* b) { put_jobbin2(b, Jobbin2{}); }, "NULL"});
    calls.push_back({"cellSpursJobHeaderSetJobbin2Param", {kOut, 0}, nullptr, "NULL binary"});
    calls.push_back({"cellSpursJobHeaderSetJobbin2Param", {kOut, kElf + 8}, nullptr, "misaligned binary"});
    // Exit code containers: the attribute setter and the initializer.
    for (const u32 revision : {1u, 0u, 2u}) {
        const auto setup = [revision](u8* b) {
            ps_write32(b, kArea + 0x1000, revision);
            ps_write64(b, kArea + 0x1040, 0x5555);
        };
        for (const u32 attr : {kArea + 0x1000, 0u, kArea + 0x1004})
            for (const u32 container : {kArea + 0x1800u, 0u, kArea + 0x1808u})
                calls.push_back({"cellSpursTaskAttributeSetExitCodeContainer", {attr, container}, setup,
                    std::format("revision {} attr {:#x} container {:#x}", revision, attr, container)});
    }
    for (const u32 container : {kArea + 0x1800u, 0u, kArea + 0x1840u})
        calls.push_back({"cellSpursTaskExitCodeInitialize", {container}, nullptr, std::format("{:#x}", container)});
    compare(t, sony, calls);
}

TEST_CASE("cellSpurs' queue, semaphore and LF queue accessors against libsre", "[hle][lle][spurs]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "cellSpurs", false);
    std::mt19937 rng(20261008);
    std::vector<Call> calls;
    for (const char* fn : {"cellSpursQueueDepth", "cellSpursQueueGetEntrySize", "cellSpursQueueGetDirection",
             "cellSpursQueueGetTasksetAddress", "cellSpursQueueSize"})
        getter_calls(calls, fn, kQueue, rng);
    getter_calls(calls, "cellSpursSemaphoreGetTasksetAddress", kSem, rng);
    getter_calls(calls, "cellSpursLFQueueGetTasksetAddress", kLfq, rng);
    // An LF queue's signal address with bit 0 set or clear.
    for (const u32 signal : {0u, 1u, 0x12345680u, 0x12345681u, 0xFFFFFFFFu}) {
        calls.push_back({"cellSpursLFQueueGetTasksetAddress", {kLfq, kOut},
            [signal](u8* b) { ps_write64(b, kLfq + 0x70, 0xABCD000000000000ull | signal); },
            std::format("signal {:#x}", signal)});
    }
    // Clear: every combination of the indices marked or not and tasks waiting to pop or push.
    for (const u32 pop : {0u, 3u, ~3u, 0x7FFFFFFFu, 0x80000000u})
        for (const u32 push : {0u, 5u, ~5u})
            for (const u8 pop_waiters : {0, 1})
                for (const u8 push_waiters : {0, 2}) {
                    const auto setup = [=](u8* b) {
                        std::memset(b + kQueue, 0, 128);
                        ps_write32(b, kQueue, pop);
                        ps_write32(b, kQueue + 4, push);
                        ps_write32(b, kQueue + 8, 0x40);
                        ps_write32(b, kQueue + 12, 4);
                        b[kQueue + 0x20] = pop_waiters;
                        b[kQueue + 0x30] = push_waiters;
                    };
                    for (const u32 q : {kQueue, 0u, kQueue + 8})
                        calls.push_back({"cellSpursQueueClear", {q}, setup,
                            std::format("({:#x}) pop {:#x} push {:#x} waiters {} {}", q, pop, push, pop_waiters,
                                push_waiters)});
                }
    compare(t, sony, calls);
}

TEST_CASE("cellSpursGetTasksetInfo: a task's state, context size and its ELF's GUID (D-505)", "[hle][spurs]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    std::mt19937 rng(7);
    std::memset(b + kArea, 0, kAreaSize);
    put_spurs(b, rng, false, 0x80000000u >> 3);
    ps_write32(b, kSpurs + 0xB00 + 3 * 0x20 + 0x10, 0x1E40); // workload 3's size
    std::memset(b + kTaskset, 0, 10496);
    ps_write64(b, kTaskset + 0x60, kSpurs);
    ps_write32(b, kTaskset + 0x74, 3);
    ps_write32(b, kTaskset + 0x1890, 6400);
    put_spu_elf(b, kElf, false, 23, 3, rng);
    const u32 seg = kElf + 0x100 + 0x40 * 2;
    const u16 g[4] = {0x1234, 0x5678, 0x9ABC, 0xDEF0};
    for (u32 k = 0; k < 4; k++) ps_write32(b, seg + 4 * k, 0x42000002u | (u32{g[k]} << 9) | (k << 7));
    // Task 5: enabled and ready, signalled, with its ELF and a context of two saved LS blocks.
    ps_write32(b, kTaskset + 0x30, 0x80000000u >> 5); // enabled
    ps_write32(b, kTaskset + 0x10, 0x80000000u >> 5); // ready
    ps_write32(b, kTaskset + 0x40, 0x80000000u >> 5); // signalled
    const u32 ti = kTaskset + 0x80 + 0x30 * 5;
    ps_write64(b, ti + 0x10, kElf | 3);         // the low bits are flags
    ps_write64(b, ti + 0x18, 0x00800000u | 2); // context | saved blocks
    REQUIRE(t.call("cellSpursGetTasksetInfo", kTaskset, kInfo) == 0);
    const u32 out = kInfo + 5 * 0x48;
    CHECK(ps_read32(b, out + 0x20) == kElf);
    CHECK(ps_read32(b, out + 0x24) == 0x00800000u);
    CHECK(ps_read32(b, out + 0x28) == 0x400 + 2 * 0x800);
    CHECK(b[out + 0x2C] == 3); // ready
    CHECK(b[out + 0x2D] == 1);
    const u8 want[8] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
    CHECK(std::memcmp(b + out + 0x34, want, 8) == 0);
    CHECK(b[kInfo + 4 * 0x48 + 0x2C] == 0); // no task 4
    CHECK(ps_read32(b, kInfo + 0x2408) == 3);
    CHECK(ps_read32(b, kInfo + 0x241C) == 6400);
}

TEST_CASE("cellSpursSetPreemptionVictimHints needs the system workload; jobbin2 headers (D-506)", "[hle][spurs]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    std::memset(b + kSpurs, 0, 0x2000);
    b[kSpurs + 0x76] = 5;
    CHECK(t.call("cellSpursSetPreemptionVictimHints", kSpurs, kOut) == r3_of_code(0x80410709)); // PERM
    ps_write32(b, kSpurs + 0xD80, 0x02000000);
    CHECK(t.call("cellSpursSetPreemptionVictimHints", kSpurs, kOut) == 0);
    // Three program headers: code at 0x3000 (0x100), data at 0x3400 (0x100 + 0x40 bss), a note.
    put_jobbin2(b, Jobbin2{});
    std::memset(b + kOut, 0, 48);
    REQUIRE(t.call("cellSpursJobHeaderSetJobbin2Param", kOut, kElf) == 0);
    CHECK(ps_read16(b, kOut + 2) == 0x40 / 16);
    CHECK(ps_read32(b, kOut + 4) == kElf + 0x200);
    CHECK(ps_read16(b, kOut + 8) == (0x3400 + 0x140 - 0x3000 - 0x40) / 16);
    CHECK(b[kOut + 44] == 4);
}

TEST_CASE("cellSpudll against libsre (D-508)", "[hle][lle][spurs]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "cellSpudll", false);
    u8* b = f.base();
    // An SPU shared object: its header, then program headers of the types given (vaddr 0x100 * i, memsz 0x180).
    const auto so = [&](u16 type, u8 cls, u8 data, u16 machine, u16 phentsize, std::vector<u32> types, u32 vbase) {
        std::memset(b + kElf, 0, 0x400);
        ps_write32(b, kElf, 0x7F454C46);
        b[kElf + 4] = cls;
        b[kElf + 5] = data;
        ps_write16(b, kElf + 16, type);
        ps_write16(b, kElf + 18, machine);
        ps_write32(b, kElf + 28, 0x40);
        ps_write16(b, kElf + 42, phentsize);
        ps_write16(b, kElf + 44, static_cast<u16>(types.size()));
        for (u32 i = 0; i < types.size(); i++) {
            ps_write32(b, kElf + 0x40 + 32 * i, types[i]);
            ps_write32(b, kElf + 0x40 + 32 * i + 8, vbase + 0x100 * i);
            ps_write32(b, kElf + 0x40 + 32 * i + 20, 0x180);
        }
    };
    struct Shape {
        u16 type = 3;
        u8 cls = 1, data = 2;
        u16 machine = 23, phentsize = 32;
        std::vector<u32> types{1, 1, 2};
        u32 vbase = 0x80;
    };
    std::vector<std::pair<Shape, std::string>> shapes = {{Shape{}, "good"}};
    Shape s;
    s.type = 2;
    shapes.push_back({s, "ET_EXEC"});
    s = Shape{};
    s.cls = 2;
    shapes.push_back({s, "ELF64"});
    s = Shape{};
    s.data = 1;
    shapes.push_back({s, "little endian"});
    s = Shape{};
    s.machine = 21;
    shapes.push_back({s, "PPU"});
    s = Shape{};
    s.phentsize = 56;
    shapes.push_back({s, "phentsize"});
    s = Shape{};
    s.types = {1, 1};
    shapes.push_back({s, "no dynamic"});
    s = Shape{};
    s.types = {1, 1, 1, 1, 2};
    shapes.push_back({s, "four loads"});
    s = Shape{};
    s.types = {1, 1, 1, 1, 1, 2};
    shapes.push_back({s, "five loads"});
    s = Shape{};
    s.types = {2, 6, 1};
    s.vbase = 0x3FF00;
    shapes.push_back({s, "high"});
    s = Shape{};
    s.types = {2};
    shapes.push_back({s, "dynamic only"});
    s = Shape{};
    s.types = {};
    shapes.push_back({s, "no headers"});
    s = Shape{};
    s.vbase = 0;
    s.types = {2, 1};
    shapes.push_back({s, "dynamic at 0"});
    std::vector<Call> calls;
    for (const auto& [shape, what] : shapes) {
        const auto setup = [&, shape](u8* bb) {
            so(shape.type, shape.cls, shape.data, shape.machine, shape.phentsize, shape.types, shape.vbase);
            for (u32 i = 0; i < 64; i += 4) ps_write32(bb, kOut + 0x40 + i, 0);
            ps_write32(bb, kOut + 0x48, 5);
            ps_write32(bb, kOut + 0x4C, 33);
        };
        calls.push_back({"cellSpudllGetImageSize", {kOut, kElf, 0}, setup, what + ", default config"});
        calls.push_back({"cellSpudllGetImageSize", {kOut, kElf, kOut + 0x40}, setup, what + ", a config"});
    }
    calls.push_back({"cellSpudllGetImageSize", {0, kElf, 0}, nullptr, "no size"});
    calls.push_back({"cellSpudllGetImageSize", {kOut, 0, 0}, nullptr, "no ELF"});
    calls.push_back({"cellSpudllHandleConfigSetDefaultValues", {kOut}, nullptr, "defaults"});
    calls.push_back({"cellSpudllHandleConfigSetDefaultValues", {0}, nullptr, "NULL"});
    usize differ = 0;
    for (const Call& c : calls) {
        const auto call = [&](bool theirs_side) {
            std::memset(b + kArea, kFill, kAreaSize);
            if (c.setup) c.setup(b);
            u32 r3 = 0;
            if (theirs_side) {
                const auto it = sony.module->functions.find({"cellSpudll", recomp::nid_of(c.fn)});
                REQUIRE(it != sony.module->functions.end());
                std::array<u64, 3> a{};
                std::copy(c.args.begin(), c.args.end(), a.begin());
                r3 = static_cast<u32>(call_guest(t.ctx, it->second, {a[0], a[1], a[2]}));
            } else {
                r3 = ours(t, c.fn, c.args);
            }
            return std::pair{r3, std::vector<u8>(b + kArea, b + kArea + kAreaSize)};
        };
        const auto o = call(false), sy = call(true);
        if (o != sy) {
            ++differ;
            UNSCOPED_INFO(std::format("{} {}: {:#x} vs {:#x} (Sony)", c.fn, c.what, o.first, sy.first));
        }
    }
    CHECK(differ == 0);
}

TEST_CASE("cellSpurs' SPU sharing functions against libsre (D-509)", "[hle][lle][spurs]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Fixture f;
    GuestThread t(f, 0, "ppu");
    FirmwareLibrary sony(f, t, *flash, "cellSpurs", false);
    std::vector<Call> calls;
    const u32 attr = kArea + 0x1000;
    for (const u32 flags : {0u, 0x00800000u, 0x01000000u, 0x80000000u, 0x80000002u, 0x00400001u})
        for (const u32 nspus : {1u, 2u}) {
            const auto setup = [=](u8* b) {
                for (u32 i = 0; i < 0x200; i += 4) ps_write32(b, attr + i, 0x11111111u * (i / 4 % 7));
                ps_write32(b, attr + 0x8, nspus);
                ps_write32(b, attr + 0x28, flags);
            };
            const std::string what = std::format("flags {:#x} nSpus {}", flags, nspus);
            for (const u32 x : {0u, 1u}) {
                calls.push_back({"cellSpurs_5C3A614C", {attr, x}, setup, what});
                for (const u32 y : {0u, 1u}) calls.push_back({"cellSpurs_B55782F9", {attr, x, y}, setup, what});
            }
        }
    for (const u32 a : {0u, attr + 4}) {
        calls.push_back({"cellSpurs_5C3A614C", {a, 1}, nullptr, std::format("attr {:#x}", a)});
        calls.push_back({"cellSpurs_B55782F9", {a, 1, 1}, nullptr, std::format("attr {:#x}", a)});
    }
    // The root system calls' wrappers: refused without SPU sharing; with it, lv2 answers both sides alike.
    for (const u32 flags : {0u, 0x00800000u, 0x01000000u}) {
        const auto setup = [flags](u8* b) {
            std::memset(b + kSpurs, 0, 0x1000);
            ps_write32(b, kSpurs + 0xD80, flags);
            ps_write32(b, kSpurs + 0xD30, 0x12345);
        };
        calls.push_back({"cellSpurs_BAC24478", {kSpurs, 1, 2, 3, 4}, setup, std::format("flags {:#x}", flags)});
        calls.push_back({"cellSpurs_7025A5EC", {kSpurs}, setup, std::format("flags {:#x}", flags)});
    }
    for (const u32 sp : {0u, kSpurs + 0x40}) {
        calls.push_back({"cellSpurs_BAC24478", {sp, 1, 2, 3, 4}, nullptr, "bad spurs"});
        calls.push_back({"cellSpurs_7025A5EC", {sp}, nullptr, "bad spurs"});
    }
    // cellSpursInitializeForSpuSharing's refusals (a successful one would start libsre's SPURS kernel here).
    calls.push_back({"cellSpursInitializeForSpuSharing", {0, 1, 100, 1000, 0, 1, 1}, nullptr, "NULL"});
    calls.push_back({"cellSpursInitializeForSpuSharing", {kSpurs + 0x40, 1, 100, 1000, 0, 1, 1}, nullptr, "misaligned"});
    compare(t, sony, calls);
}
