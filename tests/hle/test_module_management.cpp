// The module-management entries of the modules pspocket serves itself (D-918, part 2: runtime/hle module_management.cpp):
// - a firmware module loaded by path whose libraries are the host's gets them: sys_prx_get_module_info reports
//   module_start and module_stop, sys_prx_start_module runs module_prologue with module_start (SYS_PRX_RESIDENT),
//   sys_prx_stop_module module_epilogue with module_stop;
// - module_prologue and module_epilogue call the entry they are given with the arguments' low words and answer its
//   result, 0 for an entry of all ones: compared with Sony's (librudp's, the SDK's start-up code) on a fresh module
//   (the user's installed system software; skipped without it).
// No game data.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <string>

#include "compare_util.hpp"
#include "pspocket/loader/lle.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
namespace as = pspocket::recomp::ppu::as;

namespace {

constexpr u32 kNidStart = 0xBC9A0086, kNidStop = 0xAB779874, kNidPrologue = 0x0D10FD3F, kNidEpilogue = 0x330F7005,
              kNidExit = 0x3AB9A95E, kNidInfo = 0xD7F43016;
constexpr u32 kCode = kData + 0x2000, kEntry = kData + 0x2400, kRecord = kData + 0x2500;

// An entry that records its r3 and r4 (as 64-bit words) at kRecord, counts its calls at kRecord + 16 and answers
// 0x12345678 | its count.
void write_entry(u8* b)
{
    as::Assembler e(kCode);
    e.emit(as::lis(9, static_cast<s32>(kRecord >> 16)));
    e.emit(as::ori(9, 9, kRecord & 0xFFFF));
    e.emit(as::std_(3, 0, 9));
    e.emit(as::std_(4, 8, 9));
    e.emit(as::lwz(10, 16, 9));
    e.emit(as::addi(10, 10, 1));
    e.emit(as::stw(10, 16, 9));
    e.emit(as::lis(3, 0x1234));
    e.emit(as::ori(3, 3, 0x5600));
    e.emit(as::or_(3, 3, 10));
    e.emit(as::blr());
    const std::vector<u8> code = e.finish();
    std::memcpy(b + kCode, code.data(), code.size());
    ps_write32(b, kEntry, kCode);
    ps_write32(b, kEntry + 4, 0);
    std::memset(b + kRecord, 0, 32);
}

std::string record(u8* b)
{
    return std::format("r3 {:#x} r4 {:#x} calls {}", ps_read64(b, kRecord), ps_read64(b, kRecord + 8),
        ps_read32(b, kRecord + 16));
}

} // namespace

TEST_CASE("The module-management entries are pspocket's own, under the nameless library", "[hle][prx]")
{
    const hle::Registry& r = hle::registry();
    for (const auto& [nid, name] : {std::pair{kNidStart, "module_start"}, {kNidStop, "module_stop"},
             {kNidPrologue, "module_prologue"}, {kNidEpilogue, "module_epilogue"}, {kNidExit, "module_exit"}}) {
        const hle::Function* f = r.find("", nid);
        REQUIRE(f != nullptr);
        CHECK(f->name == name);
        CHECK(recomp::NidDb::builtin().find("", nid) == f->name);
    }
    const hle::Variable* info = r.find_variable("", kNidInfo);
    REQUIRE(info != nullptr);
    CHECK(info->name == "module_info");
    CHECK(info->size == 0x34);
}

TEST_CASE("A firmware module the host serves, loaded by path: the host's module_start and module_stop, through the "
          "prologue and epilogue",
    "[hle][prx]")
{
    Fixture f;
    loader::RuntimeModules modules(*f.hle, nullptr, nullptr);
    f.hle->set_module_loader(&modules);
    GuestThread t(f, 0, "main");
    u8* b = f.base();
    const u32 path = kData, result = kData + 0x300, info = kData + 0x400;
    const std::string p = "/dev_flash/sys/external/libsail_rec.sprx";
    std::memcpy(b + path, p.c_str(), p.size() + 1);
    const u64 id = t.call("sys_prx_load_module", path, 0, 0);
    REQUIRE(static_cast<s32>(id) > 0);
    // sys_prx_get_module_info: the host's entries.
    std::memset(b + info, 0, 0x58);
    ps_write64(b, info, 0x48);
    CHECK(t.call("sys_prx_get_module_info", id, 0, info) == lv2::CELL_OK);
    const u32 start = f.hle->link_function("", kNidStart, "module_start"),
              stop = f.hle->link_function("", kNidStop, "module_stop");
    CHECK(ps_read32(b, info + 0x2C) == start);
    CHECK(ps_read32(b, info + 0x30) == stop);
    CHECK(ps_read32(b, info + 0x34) == 0); // no segments
    // Started through the prologue: module_start's SYS_PRX_RESIDENT; a second start refused by the loader.
    ps_write32(b, result, 0xEEEEEEEE);
    CHECK(t.call("sys_prx_start_module", id, 4, kData + 0x500, result, 0, 0) == lv2::CELL_OK);
    CHECK(ps_read32(b, result) == 0);
    CHECK(static_cast<u32>(t.call("sys_prx_start_module", id, 0, 0, result, 0, 0)) == 0x80011133); // already started
    ps_write32(b, result, 0xEEEEEEEE);
    CHECK(t.call("sys_prx_stop_module", id, 0, 0, result, 0, 0) == lv2::CELL_OK);
    CHECK(ps_read32(b, result) == 0);
    CHECK(t.call("sys_prx_unload_module", id, 0, 0) == lv2::CELL_OK);
    f.hle->set_module_loader(nullptr);
    // module_info as a variable: the host's description.
    const u32 mi = f.hle->link_variable("", kNidInfo);
    REQUIRE(mi);
    CHECK(ps_read16(b, mi) == 0);
    CHECK(ps_read8(b, mi + 2) == 1);
    CHECK(ps_read8(b, mi + 3) == 1);
    CHECK(std::string(reinterpret_cast<const char*>(b + mi + 4)) == "pspocket");
    CHECK(t.call("module_exit") == 0);
}

namespace {

// module_prologue and module_epilogue with an entry recording its arguments: the answers and the records.
Observed entries_half(const Setup& s, bool sony)
{
    Half<> h(s, sony, "cellRudp", /*start=*/false); // librudp unstarted: its prologue is fresh
    write_entry(h.b);
    u32 prologue = 0, epilogue = 0;
    if (h.side.sony()) {
        prologue = h.side.firmware()->module->prologue;
        epilogue = h.side.firmware()->module->epilogue;
    } else {
        prologue = h.f.hle->link_function("", kNidPrologue, "module_prologue");
        epilogue = h.f.hle->link_function("", kNidEpilogue, "module_epilogue");
    }
    REQUIRE(prologue);
    REQUIRE(epilogue);
    const u64 args = 0xABCD000000000005ull, argp = 0x1111111122223333ull;
    h.o.line("module_prologue(entry, args, argp)", answer(hle::call_guest(h.t.ctx, prologue, {kEntry, args, argp})));
    h.o.line("module_prologue: the entry got", record(h.b));
    h.o.line("module_epilogue(entry, args, argp)", answer(hle::call_guest(h.t.ctx, epilogue, {kEntry, argp, args})));
    h.o.line("module_epilogue: the entry got", record(h.b));
    return std::move(h.o);
}

Observed none_half(const Setup& s, bool sony)
{
    Half<> h(s, sony, "cellRudp", false);
    write_entry(h.b);
    const u32 prologue = h.side.sony() ? h.side.firmware()->module->prologue
                                       : h.f.hle->link_function("", kNidPrologue, "module_prologue");
    const u32 epilogue = h.side.sony() ? h.side.firmware()->module->epilogue
                                       : h.f.hle->link_function("", kNidEpilogue, "module_epilogue");
    h.o.line("module_prologue without an entry", answer(hle::call_guest(h.t.ctx, prologue, {~0ull, 1, 2})));
    h.o.line("module_epilogue without an entry", answer(hle::call_guest(h.t.ctx, epilogue, {~0ull, 1, 2})));
    h.o.line("no entry called", record(h.b));
    return std::move(h.o);
}

} // namespace

TEST_CASE("module_prologue and module_epilogue against the SDK's (librudp's): the entry, its arguments, its answer",
    "[hle][lle][compare][prx]")
{
    const Setup s = setup();
    run(s, "the module-management entries against librudp's", [&](bool sony) { return entries_half(s, sony); },
        [](const Observed& o) {
            CHECK(o.value("module_prologue(entry, args, argp)") == "0x12345601");
            CHECK(o.value("module_prologue: the entry got") == "r3 0x5 r4 0x22223333 calls 1");
            CHECK(o.value("module_epilogue: the entry got") == "r3 0x22223333 r4 0x5 calls 2");
        });
    run(s, "the module-management entries without an entry, against librudp's",
        [&](bool sony) { return none_half(s, sony); }, [](const Observed& o) {
            CHECK(o.value("module_prologue without an entry") == "0x0");
            CHECK(o.value("no entry called") == "r3 0x0 r4 0x0 calls 0");
        });
}
