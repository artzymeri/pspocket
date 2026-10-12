// cellUsbd and cellUsbPspcm against libusbd.sprx and libusbpspcm.sprx (D-193's method, D-522), on pspocket's lv2
// sys_usbd with no USB device attached: the same scripts in two fresh processes, one calling pspocket's functions and
// one Sony's modules; every answer, every byte written and the lv2 calls the script's thread makes (the libraries'
// own, with their arguments but stack addresses) the same. Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH); no
// game data.

#include <catch_amalgamated.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kOut = kData + 0x10000;
constexpr u32 kOutSize = 0x4000;
constexpr u32 kStr = kData + 0x1000;
constexpr u8 kFill = 0xA5;

// An argument read from guest memory when the call is made: the 32-bit word at the address (an allocation's).
constexpr u64 kDeref = u64{1} << 63;

// A call; with fn "=" a 32-bit store (args: address, value), with "~" the 32-bit word at the address made 1 when it is
// not 0 (an address lv2 chose, which the firmware module's own loading moves).
struct Op {
    const char* lib;
    const char* fn;
    std::vector<u64> args;
};

struct Run {
    std::vector<u32> answers;
    std::vector<u8> out;
    std::vector<std::string> calls;
};

// The lv2 calls libusbd and libusbpspcm make themselves (`sc`): threads, events, the process's SFO and sys_usbd.
bool library_call(u32 number)
{
    return number == 30 || number == 44 || number == 47 || number == 48 || (number >= 128 && number <= 138) ||
           (number >= 530 && number <= 569);
}

struct Calls {
    std::mutex mutex;
    std::thread::id thread; // the script's
    u8* base = nullptr;
    std::vector<std::string> list;
};

// The arguments of a sys_usbd call compared: the scalars and the game's pointers, not the libraries' own buffers.
std::vector<u32> compared_args(u32 number)
{
    switch (number) {
    case 531: case 552: return {0};
    case 532: case 550: return {0, 2};
    case 535: case 536: case 558: case 563: case 547: case 556: return {0, 1, 2};
    case 548: case 551: return {0, 1};
    case 549: return {0, 1, 2, 3};
    case 559: return {0, 1, 2, 3, 4, 5};
    case 565: return {0, 2, 3};
    case 569: return {0, 3};
    default: return {};
    }
}

void observe(void* user, u32 number, const u64* a)
{
    auto* c = static_cast<Calls*>(user);
    if (!library_call(number) || std::this_thread::get_id() != c->thread) return;
    // lv2 object ids differ (the firmware module's loading makes objects too): the sys_usbd calls' arguments only.
    std::string line = std::to_string(number);
    // A driver's name: as the string (the libraries' own drivers name theirs in their own memory).
    const bool named = number == 535 || number == 536 || number == 558 || number == 559 || number == 563;
    for (const u32 i : compared_args(number)) {
        if (named && i == 1) {
            line += std::format(" \"{}\"", std::string_view(reinterpret_cast<const char*>(c->base + a[1]), a[2]));
            continue;
        }
        // Memory lv2 gave (from 0x10000000): where depends on the firmware module's own loading.
        const bool lv2_memory = number == 551 && a[i] >= 0x10000000 && a[i] < 0xC0000000;
        line += lv2_memory ? std::string(" (lv2's memory)") : std::format(" {:#x}", a[i]);
    }
    std::lock_guard lock(c->mutex);
    c->list.push_back(line);
}

Run run(const std::string& flash, const std::vector<Op>& ops, bool sony)
{
    if (std::getenv("PSPOCKET_USB_TRACE")) log::set_min_level(log::Level::Trace);
    Fixture f;
    REQUIRE(f.hle->link_internal_functions({"cellUsbd", "cellUsbPspcm"}));
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100})); // threads (libusbd's callback thread) need TLS
    GuestThread t(f, 0, "ppu");
    // Sony's libusbpspcm on Sony's libusbd: one set of firmware modules with both (loaded in both runs, so memory is
    // laid out alike).
    loader::LleModules lle(flash);
    REQUIRE(lle.index());
    lle.force("cellUsbd");
    lle.force("cellUsbPspcm");
    std::string why;
    const loader::PrxModule* usbd = lle.load("cellUsbd", *f.hle, &why);
    INFO(why);
    REQUIRE(usbd);
    const loader::PrxModule* pspcm = lle.load("cellUsbPspcm", *f.hle, &why);
    INFO(why);
    REQUIRE(pspcm);
    for (const ModuleEntry& start : lle.take_starts()) run_module_entry(t.ctx, start, 0, 0);
    u8* base = f.base();
    std::memset(base + kOut, kFill, kOutSize);
    std::strcpy(reinterpret_cast<char*>(base + kStr), "testldd");
    std::strcpy(reinterpret_cast<char*>(base + kStr + 0x20), "other");
    std::strcpy(reinterpret_cast<char*>(base + kStr + 0x60), "cellUsbPspcm");
    std::memset(base + kStr + 0x80, 'a', 64); // a 64-character name, then a 63-character one
    std::memset(base + kStr + 0x100, 'b', 63);
    Calls calls;
    calls.thread = std::this_thread::get_id();
    calls.base = base;
    f.kernel->set_syscall_observer(&observe, &calls);
    Run r;
    for (const Op& op : ops) {
        const std::string_view name = op.fn;
        if (name == "=" || name == "~") {
            const u32 at = static_cast<u32>(op.args[0]);
            ps_write32(base, at, name == "=" ? static_cast<u32>(op.args[1]) : ps_read32(base, at) != 0);
            r.answers.push_back(0);
            continue;
        }
        std::array<u64, 8> a{};
        for (usize i = 0; i < op.args.size(); i++) {
            a[i] = op.args[i] & kDeref ? ps_read32(base, static_cast<u32>(op.args[i])) : op.args[i];
        }
        u32 r3 = 0;
        if (sony) {
            const loader::PrxModule* lib = std::string_view(op.lib) == "cellUsbd" ? usbd : pspcm;
            const auto it = lib->functions.find({op.lib, recomp::nid_of(op.fn)});
            REQUIRE(it != lib->functions.end());
            r3 = static_cast<u32>(call_guest(t.ctx, it->second, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
        } else {
            const Function& fn = GuestThread::find(op.fn);
            for (usize i = 0; i < a.size(); i++) t.ctx.gpr[3 + i] = a[i];
            const u64 sp = t.ctx.gpr[1];
            fn.fn(t.ctx, base);
            CHECK(t.ctx.gpr[1] == sp);
            r3 = static_cast<u32>(t.ctx.gpr[3]);
        }
        if (std::getenv("PSPOCKET_USB_PROBE")) {
            std::fprintf(stderr, "PROBE %s %s(%s) -> %#x\n", sony ? "sony" : "ours", op.fn,
                op.args.empty() ? "" : std::format("{:#x}", op.args[0]).c_str(), r3);
        }
        r.answers.push_back(r3);
        std::lock_guard lock(calls.mutex);
        calls.list.push_back(std::format("-- {}", op.fn));
    }
    f.kernel->set_syscall_observer(nullptr, nullptr);
    r.out.assign(base + kOut, base + kOut + kOutSize);
    r.calls = calls.list;
    return r;
}

void compare(const std::string& flash, const std::vector<Op>& ops, const std::string& what)
{
    const Run sonys = run(flash, ops, true);
    if (std::getenv("PSPOCKET_USB_PROBE")) {
        CHECK(false); // print the probe
        return;
    }
    const Run mine = run(flash, ops, false);
    for (usize i = 0; i < ops.size(); i++) {
        INFO(std::format("{}: step {} {} -> ours {:#x}, Sony's {:#x}", what, i, ops[i].fn, mine.answers[i],
            sonys.answers[i]));
        CHECK(mine.answers[i] == sonys.answers[i]);
    }
    usize at = 0;
    while (at < mine.out.size() && mine.out[at] == sonys.out[at]) at++;
    INFO(std::format("{}: output differs first at +{:#x}", what, at));
    CHECK(at == mine.out.size());
    const usize n = std::max(mine.calls.size(), sonys.calls.size());
    for (usize i = 0; i < n; i++) {
        const std::string m = i < mine.calls.size() ? mine.calls[i] : "(none)";
        const std::string s = i < sonys.calls.size() ? sonys.calls[i] : "(none)";
        INFO(std::format("{}: lv2 call {}: ours {}, Sony's {}", what, i, m, s));
        CHECK(m == s);
        if (m != s) break;
    }
}

std::vector<Op> usbd_script()
{
    const char* u = "cellUsbd";
    const u64 ops = kOut + 0x100;   // CellUsbdLddOps {name, probe, attach, detach}: "testldd"
    const u64 other = kOut + 0x110; // "other"
    const u64 bad = kOut + 0x120;   // a null probe
    const u64 ed = kOut + 0x130;    // an endpoint descriptor
    const u64 req = kOut + 0x140;   // a device request
    const u64 p = kOut + 0x300;     // allocations' addresses
    // A signed argument as the caller's word (the functions take int32_t; bit 63 is the script's kDeref).
    const auto arg = [](s64 v) { return u64{static_cast<u32>(v)}; };
    std::vector<Op> s;
    for (const auto& [at, name] : {std::pair{ops, kStr}, std::pair{other, kStr + 0x20}, std::pair{bad, kStr}}) {
        s.push_back({u, "=", {at, name}});
        s.push_back({u, "=", {at + 4, at == bad ? 0ull : 0x1000ull}});
        s.push_back({u, "=", {at + 8, 0x1010}});
        s.push_back({u, "=", {at + 12, 0x1020}});
    }
    s.push_back({u, "=", {ed, 0x07050102}});
    s.push_back({u, "=", {req, 0x80060100}});
    s.push_back({u, "=", {req + 4, 0x00000012}});
    const auto devices = [&]() {
        for (const u64 dev : {0ull, 1ull, 0x10001ull, 0xFFFFFFFFull}) {
            s.push_back({u, "cellUsbdOpenPipe", {dev, ed}});
            s.push_back({u, "cellUsbdOpenPipe", {dev, 0}});
            s.push_back({u, "cellUsbdClosePipe", {dev}});
            s.push_back({u, "cellUsbdGetDeviceLocation", {dev, kOut + 0x210}});
            s.push_back({u, "cellUsbdGetDeviceSpeed", {dev, kOut + 0x220}});
            s.push_back({u, "cellUsbdResetDevice", {dev}});
            s.push_back({u, "cellUsbdGetPrivateData", {dev}});
            s.push_back({u, "cellUsbdSetPrivateData", {dev, 0}});
            s.push_back({u, "cellUsbdSetPrivateData", {dev, 0x1234}});
            s.push_back({u, "cellUsbdControlTransfer", {dev, 0, kOut + 0x240, 0, 0}});
            s.push_back({u, "cellUsbdControlTransfer", {dev, req, kOut + 0x240, 0x1030, 0}});
            s.push_back({u, "cellUsbdBulkTransfer", {dev, kOut + 0x240, 16, 0x1030, 0}});
            s.push_back({u, "cellUsbdInterruptTransfer", {dev, kOut + 0x240, 16, 0x1030, 0}});
            s.push_back({u, "cellUsbdIsochronousTransfer", {dev, kOut + 0x250, 0x1030, 0}});
            s.push_back({u, "cellUsbdHSIsochronousTransfer", {dev, kOut + 0x250, 0x1030, 0}});
            s.push_back({u, "cellUsbdScanStaticDescriptor", {dev, 0, 0}});
            s.push_back({u, "cellUsbdScanStaticDescriptor", {dev, ed, 5}});
        }
    };

    // Before cellUsbdInit: every call refused.
    for (const s64 type : {0, 1, 2}) s.push_back({u, "cellUsbdGetThreadPriority", {arg(type)}});
    s.push_back({u, "cellUsbdSetThreadPriority", {0, 400}});
    s.push_back({u, "cellUsbdSetThreadPriority2", {100, 200, 300}});
    s.push_back({u, "cellUsbdRegisterLdd", {ops}});
    s.push_back({u, "cellUsbdRegisterExtraLdd", {ops, 0x54C, 0x1CB}});
    s.push_back({u, "cellUsbdRegisterExtraLdd2", {ops, 0x54C, 0x100, 0x200}});
    s.push_back({u, "cellUsbdRegisterCompositeLdd", {ops}});
    s.push_back({u, "cellUsbdUnregisterLdd", {ops}});
    s.push_back({u, "cellUsbdUnregisterExtraLdd", {ops}});
    s.push_back({u, "cellUsbdUnregisterCompositeLdd", {ops}});
    devices();
    s.push_back({u, "cellUsbdAllocateMemory", {p, 0x100}});
    s.push_back({u, "cellUsbdAllocateMemoryFromContainer", {p, 0x100, 0}});
    s.push_back({u, "cellUsbdAllocateSharedMemory", {p + 4, p, 0x100}});
    s.push_back({u, "cellUsbdFreeMemory", {0}});
    s.push_back({u, "cellUsbdEnd", {}});

    s.push_back({u, "cellUsbdInit", {}});
    s.push_back({u, "cellUsbdInit", {}});

    // Thread priorities: 0 the drivers' (kept), 1 the usbd thread's, 2 the callback thread's.
    for (const s64 type : {0, 1, 2, 3, -1}) s.push_back({u, "cellUsbdGetThreadPriority", {arg(type)}});
    for (const auto& [type, prio] : {std::pair{0, 400}, std::pair{1, 600}, std::pair{2, 700}, std::pair{1, 5000},
             std::pair{3, 1}, std::pair{0, -5}}) {
        s.push_back({u, "cellUsbdSetThreadPriority", {arg(type), arg(prio)}});
        s.push_back({u, "cellUsbdGetThreadPriority", {arg(type)}});
    }
    for (const auto& [a, b, c] : {std::tuple{100, 200, 300}, std::tuple{500, 400, 600}, std::tuple{1, 2, 2},
             std::tuple{2, 2, 3}, std::tuple{1, 2, 5000}, std::tuple{-1, 0, 1}}) {
        s.push_back({u, "cellUsbdSetThreadPriority2", {arg(a), arg(b), arg(c)}});
        for (const u64 type : {0, 1, 2}) s.push_back({u, "cellUsbdGetThreadPriority", {type}});
    }
    s.push_back({u, "cellUsbdSetThreadPriority2", {600, 700, 800}});

    // Drivers: by name for registering, by the four pointers for unregistering.
    s.push_back({u, "cellUsbdRegisterLdd", {ops}});
    s.push_back({u, "cellUsbdRegisterLdd", {ops}});
    s.push_back({u, "cellUsbdRegisterLdd", {0}});
    s.push_back({u, "cellUsbdRegisterLdd", {bad}});
    s.push_back({u, "cellUsbdRegisterLdd", {other}});
    s.push_back({u, "cellUsbdRegisterExtraLdd", {ops, 0x54C, 0x1CB}});
    s.push_back({u, "cellUsbdRegisterExtraLdd2", {ops, 0x54C, 0x100, 0x200}});
    s.push_back({u, "cellUsbdRegisterCompositeLdd", {ops}});
    s.push_back({u, "cellUsbdRegisterExtraLdd", {bad, 0x54C, 0x1CB}});
    s.push_back({u, "cellUsbdRegisterCompositeLdd", {0}});
    s.push_back({u, "cellUsbdUnregisterLdd", {ops}});
    s.push_back({u, "cellUsbdUnregisterLdd", {ops}});
    s.push_back({u, "cellUsbdUnregisterExtraLdd", {ops}});
    s.push_back({u, "cellUsbdUnregisterCompositeLdd", {ops}});
    s.push_back({u, "cellUsbdUnregisterLdd", {bad}});
    s.push_back({u, "cellUsbdUnregisterLdd", {0}});
    s.push_back({u, "cellUsbdUnregisterCompositeLdd", {other}});
    s.push_back({u, "cellUsbdRegisterExtraLdd", {ops, 0x54C, 0x1CB}});
    s.push_back({u, "cellUsbdRegisterLdd", {ops}});
    s.push_back({u, "cellUsbdUnregisterExtraLdd", {ops}});
    s.push_back({u, "cellUsbdRegisterExtraLdd2", {ops, 0x1BAD, 0x100, 0x200}});
    s.push_back({u, "cellUsbdUnregisterLdd", {ops}});
    // (cellUsbdUnregisterLdd of a composite driver compares what libusbd's allocator left in the entry: not tried.)
    s.push_back({u, "cellUsbdRegisterCompositeLdd", {ops}});
    s.push_back({u, "cellUsbdRegisterLdd", {ops}});
    s.push_back({u, "cellUsbdUnregisterCompositeLdd", {ops}});
    s.push_back({u, "cellUsbdUnregisterCompositeLdd", {ops}});
    s.push_back({u, "=", {other + 4, 0x1004}}); // the pointers changed since: not that driver
    s.push_back({u, "cellUsbdUnregisterLdd", {other}});
    s.push_back({u, "=", {other + 4, 0x1000}});
    s.push_back({u, "cellUsbdUnregisterLdd", {other}});
    s.push_back({u, "cellUsbdRegisterLdd", {ops}});
    s.push_back({u, "=", {kStr + 0x40, 0x74657374}}); // "test": another name now at ops' name
    s.push_back({u, "=", {ops, kStr + 0x40}});
    s.push_back({u, "cellUsbdRegisterLdd", {ops}});
    s.push_back({u, "=", {ops, kStr}});

    devices();

    // Memory for transfers.
    s.push_back({u, "cellUsbdAllocateMemory", {p, 0x100}});
    s.push_back({u, "cellUsbdFreeMemory", {p | kDeref}});
    s.push_back({u, "cellUsbdFreeMemory", {p | kDeref}});
    s.push_back({u, "~", {p}});
    s.push_back({u, "cellUsbdAllocateMemory", {p + 4, 0}});
    s.push_back({u, "cellUsbdAllocateMemory", {0, 0x100}});
    s.push_back({u, "cellUsbdAllocateMemory", {p + 8, 0x20000}});
    s.push_back({u, "~", {p + 8}});
    // lv2's call 565 answers CELL_OK and writes nothing, so libusbd reads its stack's leftovers as the address: only
    // the refusals before it are compared.
    s.push_back({u, "cellUsbdAllocateMemoryFromContainer", {p + 12, 0, 0x12345, 0}});
    s.push_back({u, "cellUsbdAllocateMemoryFromContainer", {0, 0x100, 0x12345, 0}});
    s.push_back({u, "cellUsbdAllocateSharedMemory", {0, p + 24, 0x100}});
    s.push_back({u, "cellUsbdAllocateSharedMemory", {p + 28, 0, 0x100}});
    s.push_back({u, "cellUsbdAllocateSharedMemory", {p + 28, p + 24, 0}});
    s.push_back({u, "cellUsbdFreeMemory", {0}});
    s.push_back({u, "cellUsbdFreeMemory", {0x10000}});

    s.push_back({u, "cellUsbdEnd", {}});
    s.push_back({u, "cellUsbdEnd", {}});
    s.push_back({u, "cellUsbdRegisterLdd", {ops}});
    // A second session: drivers and priorities start over.
    s.push_back({u, "cellUsbdInit", {}});
    for (const u64 type : {0, 1, 2}) s.push_back({u, "cellUsbdGetThreadPriority", {type}});
    s.push_back({u, "cellUsbdRegisterLdd", {ops}});
    s.push_back({u, "cellUsbdEnd", {}});
    return s;
}

std::vector<Op> pspcm_script()
{
    const char* u = "cellUsbd";
    const char* p = "cellUsbPspcm";
    const u64 param = kOut + 0x400;   // CellUsbPspcmParam
    const u64 ops = kOut + 0x100;     // a CellUsbdLddOps with libusbpspcm's driver's name
    const u64 buf = kOut + 0x800;     // a transfer buffer
    const u64 pool = kData + 0x40000; // the library's own memory: not compared
    const u32 pool_size = 8268 * 2 * 3 + 128 * 2 + 144 * 3;
    const auto arg = [](s64 v) { return u64{static_cast<u32>(v)}; };
    std::vector<Op> s;
    const auto field = [&](u32 offset, u64 value) { s.push_back({p, "=", {param + offset, value}}); };
    for (const auto& [offset, value] : {std::pair<u32, u64>{0u, 32}, std::pair<u32, u64>{4u, 2},
             std::pair<u32, u64>{8u, 3}, std::pair<u32, u64>{12u, pool}, std::pair<u32, u64>{16u, pool_size},
             std::pair<u32, u64>{20u, 1000}, std::pair<u32, u64>{24u, 0x4000}, std::pair<u32, u64>{28u, 0}}) {
        field(offset, value);
    }
    s.push_back({u, "=", {ops, kStr + 0x60}});
    for (const u32 i : {1u, 2u, 3u}) s.push_back({u, "=", {ops + 4 * i, 0x1000 + 0x10 * i}});

    // The pool's size, and the parameters it checks.
    s.push_back({p, "cellUsbPspcmCalcPoolSize", {0}});
    s.push_back({p, "cellUsbPspcmCalcPoolSize", {param}});
    using Row = std::tuple<u32, u64, u64>;
    for (const auto& [offset, value, back] : {Row{0u, 31, 32}, Row{4u, 0, 2}, Row{4u, 17, 2}, Row{4u, 16, 2},
             Row{4u, arg(-1), 2}, Row{8u, 0, 3}, Row{8u, arg(-3), 3}, Row{8u, 0x100000, 3}}) {
        field(offset, value);
        s.push_back({p, "cellUsbPspcmCalcPoolSize", {param}});
        s.push_back({p, "cellUsbPspcmInit", {param}});
        field(offset, back);
    }

    // Everything before cellUsbPspcmInit.
    const auto all = [&](u64 dev, u64 id) {
        s.push_back({p, "cellUsbPspcmGetAddr", {}});
        for (const char* fn : {"cellUsbPspcmBind", "cellUsbPspcmBindAsync", "cellUsbPspcmPollBindAsync",
                 "cellUsbPspcmWaitBindAsync", "cellUsbPspcmCancelBind"}) {
            s.push_back({p, fn, {dev}});
        }
        s.push_back({p, "cellUsbPspcmClose", {id}});
        for (const char* fn : {"cellUsbPspcmSend", "cellUsbPspcmSendAsync", "cellUsbPspcmRecv", "cellUsbPspcmRecvAsync"}) {
            s.push_back({p, fn, {id, buf, 16, 0}});
            s.push_back({p, fn, {id, 0, 16, 0}});
            s.push_back({p, fn, {id, 0, 0, 1}});
            s.push_back({p, fn, {id, buf, 16, 2}});
        }
        for (const char* fn : {"cellUsbPspcmReset", "cellUsbPspcmResetAsync", "cellUsbPspcmWaitData",
                 "cellUsbPspcmPollData", "cellUsbPspcmPollSendAsync", "cellUsbPspcmWaitSendAsync",
                 "cellUsbPspcmPollRecvAsync", "cellUsbPspcmWaitRecvAsync", "cellUsbPspcmPollResetAsync",
                 "cellUsbPspcmWaitResetAsync", "cellUsbPspcmCancelWaitData"}) {
            s.push_back({p, fn, {id}});
        }
    };
    all(0, 0);
    s.push_back({p, "cellUsbPspcmRegister", {0x1040}});
    s.push_back({p, "cellUsbPspcmUnregister", {0x1040}});
    s.push_back({p, "cellUsbPspcmEnd", {}});

    // Before cellUsbdInit: the driver is refused (cellUsbd's error), after the thread was made and ended.
    s.push_back({p, "cellUsbPspcmInit", {param}});
    s.push_back({u, "cellUsbdInit", {}});
    s.push_back({p, "cellUsbPspcmInit", {0}});
    field(28, kStr + 0x80); // a name too long
    s.push_back({p, "cellUsbPspcmInit", {param}});
    field(28, kStr + 0x100);
    field(16, pool_size - 1);
    s.push_back({p, "cellUsbPspcmInit", {param}});
    field(16, pool_size);
    field(20, 5000); // a priority lv2 refuses for the thread
    s.push_back({p, "cellUsbPspcmInit", {param}});
    field(20, 1000);
    s.push_back({p, "cellUsbPspcmInit", {param}});
    s.push_back({p, "cellUsbPspcmInit", {param}});
    s.push_back({u, "cellUsbdRegisterLdd", {ops}}); // libusbpspcm's driver is registered

    s.push_back({p, "cellUsbPspcmRegister", {0}});
    s.push_back({p, "cellUsbPspcmUnregister", {0x1040}});
    s.push_back({p, "cellUsbPspcmRegister", {0x1040}});
    s.push_back({p, "cellUsbPspcmRegister", {0x1050}});
    s.push_back({p, "cellUsbPspcmRegister", {0x1040}});
    s.push_back({p, "cellUsbPspcmUnregister", {0x1050}});
    s.push_back({p, "cellUsbPspcmUnregister", {0}});
    s.push_back({p, "cellUsbPspcmUnregister", {0x1040}});
    s.push_back({p, "cellUsbPspcmUnregister", {0x1040}});
    s.push_back({p, "cellUsbPspcmRegister", {0x1050}});
    const std::array<std::pair<s64, s64>, 6> args = {
        {{-1, -1}, {0, 0}, {1, 2}, {2, 3}, {255, 4}, {256, 0x7FFFFFFF}}};
    for (const auto& [dev, id] : args) all(arg(dev), arg(id));

    s.push_back({p, "cellUsbPspcmEnd", {}});
    s.push_back({p, "cellUsbPspcmEnd", {}});
    s.push_back({u, "cellUsbdRegisterLdd", {ops}}); // the driver is gone
    s.push_back({u, "cellUsbdUnregisterLdd", {ops}});
    s.push_back({p, "cellUsbPspcmGetAddr", {}});

    // Again with one device slot and one connection.
    field(4, 1);
    field(8, 1);
    s.push_back({p, "cellUsbPspcmInit", {param}});
    all(1, 1);
    all(0, 0);
    s.push_back({p, "cellUsbPspcmEnd", {}});
    s.push_back({u, "cellUsbdEnd", {}});
    return s;
}

} // namespace

TEST_CASE("cellUsbd against libusbd", "[hle][lle][usb]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, usbd_script(), "usbd");
}

TEST_CASE("cellUsbPspcm against libusbpspcm", "[hle][lle][usb]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, pspcm_script(), "usbpspcm");
}

// pspocket's own, without the firmware: a session of each library on lv2 with no USB device.
TEST_CASE("cellUsbd and cellUsbPspcm with no USB device", "[hle][usb]")
{
    Fixture f;
    REQUIRE(f.hle->link_internal_functions({"cellUsbd", "cellUsbPspcm"}));
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread t(f, 0, "ppu");
    u8* base = f.base();
    const auto r = [&](auto... a) { return static_cast<u32>(t.call(a...)); };
    const u32 ops = kOut + 0x100, param = kOut + 0x200;
    std::strcpy(reinterpret_cast<char*>(base + kStr), "testldd");
    for (u32 i = 0; i < 4; i++) ps_write32(base, ops + 4 * i, i ? 0x1000 + i : kStr);

    CHECK(r("cellUsbdRegisterLdd", ops) == 0x80110001);
    REQUIRE(r("cellUsbdInit") == 0);
    CHECK(r("cellUsbdInit") == 0x80110002);
    CHECK(r("cellUsbdGetThreadPriority", 1) == 500);
    CHECK(r("cellUsbdSetThreadPriority2", 400, 450, 600) == 0);
    CHECK(r("cellUsbdGetThreadPriority", 2) == 600);
    CHECK(r("cellUsbdRegisterLdd", ops) == 0);
    CHECK(r("cellUsbdRegisterLdd", ops) == 0x80110006);
    CHECK(r("cellUsbdOpenPipe", 0, 0) == 0x8011000A);
    CHECK(r("cellUsbdBulkTransfer", 0, kOut, 16, 0, 0) == 0x8011000D);
    REQUIRE(r("cellUsbdAllocateMemory", kOut, 0x100) == 0);
    CHECK(r("cellUsbdFreeMemory", ps_read32(base, kOut)) == 0);
    CHECK(r("cellUsbdUnregisterLdd", ops) == 0);

    // CellUsbPspcmParam {32, 2 devices, 3 connections, pool, its size, priority, stack, name}.
    const u32 pool_size = 8268 * 6 + 128 * 2 + 144 * 3;
    const u32 p[8] = {32, 2, 3, kData + 0x40000, pool_size, 1000, 0x4000, 0};
    for (u32 i = 0; i < 8; i++) ps_write32(base, param + 4 * i, p[i]);
    CHECK(r("cellUsbPspcmCalcPoolSize", param) == pool_size);
    REQUIRE(r("cellUsbPspcmInit", param) == 0);
    CHECK(r("cellUsbPspcmInit", param) == 0x80110402);
    CHECK(r("cellUsbPspcmBind", 1) == 0x8011040B);
    CHECK(r("cellUsbPspcmBind", 2) == 0x80110403);
    CHECK(r("cellUsbPspcmSend", 0, kOut, 16, 0) == 0x8011040B);
    CHECK(r("cellUsbPspcmWaitRecvAsync", 0) == 0x80110403);
    CHECK(r("cellUsbPspcmClose", 2) == 0x80110402);
    CHECK(r("cellUsbPspcmEnd") == 0);
    CHECK(r("cellUsbdEnd") == 0);
    CHECK(r("cellUsbdEnd") == 0x80110001);
}
