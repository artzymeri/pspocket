// sys_io's sys_config_* (libio's config service agent, D-533) against libio, Sony's own module, from the user's
// installed system software (D-193's method): the same script runs in two fresh processes, one calling libio's
// exports and one pspocket's, both over pspocket's lv2. The answers, what the game's callbacks were called with (from
// the agent's event thread), the bytes written and the lv2 calls each thread made, with their arguments, must be the
// same. Events lv2 never sends on this console (I/O errors, malformed service events) are sent to the agent's queue
// through a port. Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH, firmware_compare.hpp); no game data.

#include <catch_amalgamated.hpp>

#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kOut = kData + 0x1000;     // the calls' outputs
constexpr u32 kIn = kData + 0x1800;      // listener and service data
constexpr u32 kCode = kData + 0x2000;    // the callbacks' code (0x100 apart), their descriptors at +0xF0
constexpr u32 kRecords = kData + 0x4000; // the callbacks' records: a count, then 64 bytes each from +64
constexpr u32 kRecordsSize = 0x2000;

// A callback that records (kind, r3..r7) at kRecords and returns 0: lis/ori r11, lwz r12 (count), mulli/add r9, the
// stores, lwsync, the count raised with lwarx/stwcx. (as cellAtomicIncr32 does: the test reads it through the
// reservation system, so the record and what lv2 wrote for the event before are visible with it, D-585), li r3,0, blr.
void callback_code(u8* b, u32 at, u32 kind)
{
    const u32 words[] = {0x3D600000u | (kRecords >> 16), 0x616B0000u | (kRecords & 0xFFFF), 0x818B0000u, 0x1C0C0040u,
        0x7D2B0214u, 0x38000000u | kind, 0xF8090040u, 0xF8690048u, 0xF8890050u, 0xF8A90058u, 0xF8C90060u, 0xF8E90068u,
        0x7C2004ACu, 0x398C0001u,
        0x7D405828u, 0x7D80592Du, 0x4082FFF8u, // lwarx r10, 0, r11; stwcx. r12, 0, r11; bne- back to the lwarx
        0x38600000u, 0x4E800020u};
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, at + 4 * i, words[i]);
    ps_write32(b, at + 0xF0, at); // the descriptor: entry, TOC
    ps_write32(b, at + 0xF4, 0);
}

constexpr u32 desc(u32 kind) { return kCode + 0x100 * kind + 0xF0; }

// The lv2 calls libio and pspocket make, as text, per thread (the test's, and the agent's event thread). Calls whose
// count depends on timing or on liblv2's internals (lwmutex sleeps, the heap's memory) are left out; addresses on a
// stack are left out, the heap's are kept.
struct Trace {
    std::mutex m;
    std::thread::id main;
    u8* base = nullptr;
    u32 queue_slot = 0; // where sys_event_queue_create wrote the agent's queue
    std::vector<std::string> mine, other;
};

std::string cstr(const u8* b, u32 at, u32 max)
{
    std::string s;
    for (u32 i = 0; i < max && b[at + i]; i++) s.push_back(static_cast<char>(b[at + i]));
    return s;
}

void observe(void* user, u32 n, const u64* a)
{
    auto& tr = *static_cast<Trace*>(user);
    u8* b = tr.base;
    std::string s;
    switch (n) {
    case 44: s = std::format("join"); break;
    case 52: s = std::format("thread_create(arg {:#x}, prio {}, stack {:#x}, flags {:#x}, {})", a[2], a[4], a[5], a[6],
                 cstr(b, static_cast<u32>(a[7]), 32));
             break;
    case 128:
        tr.queue_slot = static_cast<u32>(a[0]);
        s = std::format("queue_create({:#x}, protocol {}, type {}, \"{}\", key {:#x}, size {})", a[0],
            ps_read32(b, static_cast<u32>(a[1])), ps_read32(b, static_cast<u32>(a[1]) + 4),
            cstr(b, static_cast<u32>(a[1]) + 8, 8), a[2], a[3]);
        break;
    case 129: s = std::format("queue_destroy(mode {})", a[1]); break;
    case 130: s = std::format("queue_receive(timeout {})", a[2]); break;
    case 141: s = std::format("usleep({})", a[0]); break;
    case 403: s = std::format("tty \"{}\"", std::string(reinterpret_cast<const char*>(b + a[1]), a[2])); break;
    case 516: s = std::format("config_open(-> {:#x})", a[1]); break;
    case 517: s = "config_close"; break;
    case 518: s = std::format("get_service_event({:#x}, {:#x}, {})", a[1], a[2], a[3]); break;
    case 519: s = std::format("add_listener({:#x}, {}, {:#x}, {}, {}, {:#x})", a[1], a[2], a[3], a[4], a[5], a[6]); break;
    case 520: s = std::format("remove_listener({:#x})", a[1]); break;
    case 521: s = std::format("register({:#x}, {}, {}, {:#x}, {}, {:#x})", a[1], a[2], a[3], a[4], a[5], a[6]); break;
    case 522: s = std::format("unregister({:#x})", a[1]); break;
    case 523: s = std::format("get_io_event({:#x}, {:#x}, {})", a[1], a[2], a[3]); break;
    case 524: s = "register_io_error_listener"; break;
    case 525: s = "unregister_io_error_listener"; break;
    default: return;
    }
    std::lock_guard lock(tr.m);
    (std::this_thread::get_id() == tr.main ? tr.mine : tr.other).push_back(std::move(s));
}

struct Run {
    std::vector<std::string> answers;
    std::vector<u8> out, records;
    std::vector<std::string> mine, other;
};

Run run(const std::string& flash, bool sony)
{
    Fixture f;
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread t(f, 0, "ppu");
    // libio in both processes, so both have the same memory; one calls its exports, the other pspocket's.
    FirmwareLibrary io(f, t, flash, "sys_io");
    u8* b = f.base();
    std::memset(b + kOut, 0xEE, 0x800);
    std::memset(b + kIn, 0, 0x800);
    std::memset(b + kRecords, 0, kRecordsSize);
    for (u32 k = 1; k <= 3; k++) callback_code(b, kCode + 0x100 * k, k);
    std::memcpy(b + kIn, "hello", 5);
    b[kIn + 0x10] = 1; // a pad manager listener's data: the first byte 1
    ps_write32(b, kIn + 0x20, desc(1));   // callbacks: registered, unregistered
    ps_write32(b, kIn + 0x24, desc(2));

    Trace tr;
    tr.main = std::this_thread::get_id();
    tr.base = b;
    f.kernel->set_syscall_observer(&observe, &tr);
    Run r;
    const auto call = [&](const char* name, std::vector<u64> args) {
        u32 v;
        if (sony) {
            std::array<u64, 8> a{};
            std::copy(args.begin(), args.end(), a.begin());
            v = io.call(t, recomp::nid_of(name), {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]});
        } else {
            const Function& fn = GuestThread::find(name);
            for (usize i = 0; i < args.size(); i++) t.ctx.gpr[3 + i] = args[i];
            fn.fn(t.ctx, b);
            v = static_cast<u32>(t.ctx.gpr[3]);
        }
        r.answers.push_back(std::format("{} -> {:#x}", name, v));
        return v;
    };
    // Waits until the callbacks have recorded `n` calls (the event thread runs them).
    const auto wait_records = [&](u32 n) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (ps_read32(b, kRecords) < n && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(ps_read32(b, kRecords) == n);
    };
    const u64 none = 0xFFFFFFFFFFFFFFFFull;

    call("sys_config_stop", {}); // before any start
    call("sys_config_unregister_io_error_handler", {});
    call("sys_config_start", {});
    call("sys_config_start", {});
    const u32 queue = ps_read32(b, tr.queue_slot);
    call("sys_config_add_service_listener", {none, 0, kIn, 0, kIn + 0x20, 1, kOut}); // no service ID with data
    call("sys_config_add_service_listener", {none, 0, 0, 4, kIn + 0x20, 1, kOut});
    call("sys_config_add_service_listener", {0x12, 0, 0, 0, kIn + 0x20, 1, kOut}); // the pad manager lv2 has
    wait_records(1);
    call("sys_config_add_service_listener", {0x11, 0, kIn + 0x10, 1, kIn + 0x20, 1, kOut + 4});
    wait_records(2);
    call("sys_config_add_service_listener", {0x100, 2, 0, 0, kIn + 0x20, 1, kOut + 8});
    call("sys_config_add_service_listener", {0x101, 0, 0, 0, kIn + 0x20, 0, kOut + 12}); // once
    call("sys_config_register_service", {0x100, 7, 3, kIn, 5, kOut + 0x10});
    wait_records(3);
    call("sys_config_register_service", {0x100, 8, 1, 0, 0, kOut + 0x14}); // below the listener's verbosity
    call("sys_config_register_service", {0x200, 9, 5, kIn, 2, kOut + 0x18}); // no listener
    call("sys_config_register_service", {0x101, 10, 0, 0, 0, kOut + 0x1C});
    wait_records(4);
    call("sys_config_register_service", {0x101, 11, 0, 0, 0, kOut + 0x20}); // the ONCE listener has heard
    call("sys_config_unregister_service", {ps_read32(b, kOut + 0x10)});
    wait_records(5);
    call("sys_config_unregister_service", {0x12345678});
    call("sys_config_remove_service_listener", {ps_read32(b, kOut + 8)});
    call("sys_config_remove_service_listener", {ps_read32(b, kOut + 8)}); // gone
    call("sys_config_remove_service_listener", {ps_read32(b, kOut + 12)});
    call("sys_config_unregister_service", {ps_read32(b, kOut + 0x14)}); // its listener gone: no event
    call("sys_config_register_io_error_handler", {desc(3)});
    call("sys_config_register_io_error_handler", {desc(1)}); // one at a time

    // Events lv2 does not send here, through a port onto the agent's queue: an I/O error (source 2), a service event
    // for another handle with no such event, an unknown source, then an I/O error again to wait for.
    {
        REQUIRE(lv2::sys_event_port_create(*t.t, kOut + 0x100, 1, 2) == lv2::CELL_OK);
        REQUIRE(lv2::sys_event_port_create(*t.t, kOut + 0x104, 1, 1) == lv2::CELL_OK);
        REQUIRE(lv2::sys_event_port_create(*t.t, kOut + 0x108, 1, 7) == lv2::CELL_OK);
        for (const u32 p : {0x100u, 0x104u, 0x108u})
            REQUIRE(lv2::sys_event_port_connect_local(*t.t, ps_read32(b, kOut + p), queue) == lv2::CELL_OK);
        REQUIRE(lv2::sys_event_port_send(*t.t, ps_read32(b, kOut + 0x100), 0, 0x55, 12) == lv2::CELL_OK);
        wait_records(6);
        REQUIRE(lv2::sys_event_port_send(*t.t, ps_read32(b, kOut + 0x104), 0x99, 0x0000000177777777ull, 0x40) ==
            lv2::CELL_OK);
        REQUIRE(lv2::sys_event_port_send(*t.t, ps_read32(b, kOut + 0x108), 0x1234, 0, 0) == lv2::CELL_OK);
        REQUIRE(lv2::sys_event_port_send(*t.t, ps_read32(b, kOut + 0x100), 0, 0x56, 4) == lv2::CELL_OK);
        wait_records(7);
        for (const u32 p : {0x100u, 0x104u, 0x108u}) {
            REQUIRE(lv2::sys_event_port_disconnect(*t.t, ps_read32(b, kOut + p)) == lv2::CELL_OK);
            REQUIRE(lv2::sys_event_port_destroy(*t.t, ps_read32(b, kOut + p)) == lv2::CELL_OK);
        }
    }
    call("sys_config_unregister_io_error_handler", {});
    call("sys_config_unregister_io_error_handler", {});
    call("sys_config_stop", {});
    call("sys_config_stop", {}); // the last: the listeners freed, the thread joined, the agent deleted
    call("sys_config_stop", {});
    // Again from the start: a new agent and thread.
    call("sys_config_start", {});
    call("sys_config_add_service_listener", {0x12, 0, 0, 0, kIn + 0x20, 1, kOut + 0x30});
    wait_records(8);
    call("sys_config_stop", {});

    f.kernel->set_syscall_observer(nullptr, nullptr);
    r.out.assign(b + kOut, b + kOut + 0x100);
    // The registers past a callback's arguments hold what the caller left there: not compared.
    for (u32 i = 0; i < ps_read32(b, kRecords) && 64 + 64 * i < kRecordsSize; i++) {
        const u32 at = kRecords + 64 + 64 * i;
        const u64 kind = ps_read64(b, at);
        const u32 args = kind == 1 ? 5 : kind == 2 ? 2 : 3;
        for (u32 k = args; k < 5; k++) ps_write64(b, at + 8 + 8 * k, 0);
    }
    r.records.assign(b + kRecords, b + kRecords + kRecordsSize);
    std::lock_guard lock(tr.m);
    r.mine = tr.mine;
    r.other = tr.other;
    return r;
}

u64 rd64(const std::vector<u8>& v, u32 at)
{
    u64 x = 0;
    for (u32 i = 0; i < 8; i++) x = x << 8 | v[at + i];
    return x;
}

void compare_lines(const char* what, const std::vector<std::string>& mine, const std::vector<std::string>& sony)
{
    usize differ = 0;
    for (usize i = 0; i < std::max(mine.size(), sony.size()); i++) {
        const std::string o = i < mine.size() ? mine[i] : "(none)", s = i < sony.size() ? sony[i] : "(none)";
        if (o == s) continue;
        if (differ++ < 12) UNSCOPED_INFO(std::format("{} {}: pspocket {} | libio {}", what, i, o, s));
    }
    CHECK(differ == 0);
}

} // namespace

TEST_CASE("sys_config against libio's config service agent", "[hle][lle][sys_io]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    const Run sony = run(*flash, true);
    const Run mine = run(*flash, false);
    compare_lines("answer", mine.answers, sony.answers);
    compare_lines("lv2 call (caller)", mine.mine, sony.mine);
    compare_lines("lv2 call (event thread)", mine.other, sony.other);
    usize out_first = 0;
    while (out_first < mine.out.size() && mine.out[out_first] == sony.out[out_first]) out_first++;
    INFO(std::format("the outputs differ first at +{:#x}", out_first));
    CHECK(out_first == mine.out.size());
    for (u32 i = 0; i < 8; i++) {
        const u32 at = 64 + 64 * i;
        UNSCOPED_INFO(std::format("record {}: kind {} ({:#x}, {:#x}, {:#x}, {:#x}, {:#x}) | libio kind {} ({:#x}, {:#x}, "
            "{:#x}, {:#x}, {:#x})", i, rd64(mine.records, at), rd64(mine.records, at + 8),
            rd64(mine.records, at + 16), rd64(mine.records, at + 24),
            rd64(mine.records, at + 32), rd64(mine.records, at + 40),
            rd64(sony.records, at), rd64(sony.records, at + 8),
            rd64(sony.records, at + 16), rd64(sony.records, at + 24),
            rd64(sony.records, at + 32), rd64(sony.records, at + 40)));
    }
    usize first = 0;
    while (first < mine.records.size() && mine.records[first] == sony.records[first]) first++;
    INFO(std::format("the records differ first at +{:#x}", first));
    CHECK(first == mine.records.size());
    CHECK(rd64(sony.records, 0) >> 32 == 8);
}

// Without the firmware: pspocket's agent alone. The pad manager lv2 registers reaches a listener through the event
// thread; a service of the game's is heard of when registered and when unregistered; stop takes it all down.
TEST_CASE("sys_config: libio's config service agent", "[hle][sys_io]")
{
    Fixture f;
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    std::memset(b + kRecords, 0, kRecordsSize);
    for (u32 k = 1; k <= 2; k++) callback_code(b, kCode + 0x100 * k, k);
    ps_write32(b, kIn + 0x20, desc(1));
    ps_write32(b, kIn + 0x24, desc(2));
    std::memcpy(b + kIn, "data", 4);
    const auto wait_records = [&](u32 n) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (line_word(b, kRecords) < n && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(line_word(b, kRecords) == n);
    };
    const auto record = [&](u32 i, u32 k) { return ps_read64(b, kRecords + 64 + 64 * i + 8 * k); };

    CHECK(static_cast<u32>(t.call("sys_config_stop")) == 0x8001000C); // not started
    CHECK(t.call("sys_config_start") == 0);
    CHECK(static_cast<u32>(t.call("sys_config_add_service_listener", ~0ull, 0, kIn, 0, kIn + 0x20, 1, kOut)) ==
        0x80010002);
    CHECK(t.call("sys_config_add_service_listener", 0x12, 0, 0, 0, kIn + 0x20, 1, kOut) == 0);
    wait_records(1);
    CHECK(record(0, 0) == 1);
    CHECK(record(0, 1) == 0x12);
    CHECK(record(0, 5) == 0x1A); // the controller's HID data
    CHECK(t.call("sys_config_add_service_listener", 0x100, 0, 0, 0, kIn + 0x20, 1, kOut + 4) == 0);
    CHECK(t.call("sys_config_register_service", 0x100, 5, 2, kIn, 4, kOut + 8) == 0);
    wait_records(2);
    CHECK(record(1, 1) == 0x100);
    CHECK(record(1, 2) == 5);
    CHECK(record(1, 3) == 2);
    CHECK(std::memcmp(b + static_cast<u32>(record(1, 4)), "data", 4) == 0);
    CHECK(record(1, 5) == 4);
    CHECK(t.call("sys_config_unregister_service", ps_read32(b, kOut + 8)) == 0);
    wait_records(3);
    CHECK(record(2, 0) == 2);
    CHECK(record(2, 1) == 0x100);
    CHECK(record(2, 2) == 5);
    CHECK(t.call("sys_config_register_io_error_handler", desc(1)) == 0);
    CHECK(static_cast<u32>(t.call("sys_config_register_io_error_handler", desc(1))) == 0x80010014);
    CHECK(t.call("sys_config_unregister_io_error_handler") == 0);
    CHECK(t.call("sys_config_remove_service_listener", ps_read32(b, kOut + 4)) == 0);
    CHECK(t.call("sys_config_stop") == 0);
    CHECK(static_cast<u32>(t.call("sys_config_stop")) == 0x8001000C);
}
