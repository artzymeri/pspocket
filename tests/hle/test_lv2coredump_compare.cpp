// liblv2coredump (sys_lv2coredump_D725F320, the crash dump writer) against Sony's module (D-943). A process that
// declares SYS_PROCESS_CRASH_DUMP_ENABLED (the executable's crash_dump_param_addr, which lv2's
// _sys_process_get_paramsfo reports) has the module started as libsysmodule starts it (Sony's module_start in Sony's
// half, pspocket's lv2coredump_start in pspocket's); then a file opened on /dev_hdd0 and D725F320 writing the core file
// into it. Compared: what start leaves (the thread and queue the core lists), the answer, and the file byte for byte,
// note by note. A process without the declaration: start does nothing and D725F320 fails as Sony's does.
//
// No game data: the process is made here.

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "../../runtime/hle/src/sys_lv2coredump.hpp"
#include "compare_util.hpp"
#include "pspocket/core/log.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;

namespace {

constexpr u32 kDecl = kData + 0x1000; // SYS_PROCESS_CRASH_DUMP_ENABLED's declaration
constexpr u32 kPath = kData + 0x2000, kFd = kData + 0x2100;
constexpr u32 kNid = 0xD725F320;

bool g_sony = false;
bool g_declared = true;

std::filesystem::path half_dir(bool sony)
{
    return std::filesystem::temp_directory_path() / (sony ? "pspocket-lv2coredump-sony" : "pspocket-lv2coredump-ours");
}

lv2::Config core_config()
{
    lv2::Config c;
    c.title_id = "PSPC00001";
    c.crash_dump_param_addr = g_declared ? kDecl : 0;
    return c;
}

// The process: the declaration in its memory, /dev_hdd0 on a directory of the half's own.
struct CoreFixture : Fixture {
    CoreFixture() : Fixture({}, core_config())
    {
        REQUIRE(hle->initialize_tls({0, 0, 0x100}));
        u8* b = base();
        std::memset(b + kDecl, 0, 0x20);
        ps_write32(b, kDecl, 0x20);
        ps_write32(b, kDecl + 4, 0x64756D70); // "dump"
        ps_write32(b, kDecl + 8, 1);
        const std::filesystem::path dir = half_dir(g_sony);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "tmp");
        REQUIRE(kernel->vfs_mount("/dev_hdd0", dir.string(), false));
    }
};

std::vector<u8> read_host(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

u32 be32(const std::vector<u8>& v, usize at)
{
    return at + 4 <= v.size() ? (u32{v[at]} << 24) | (u32{v[at + 1]} << 16) | (u32{v[at + 2]} << 8) | v[at + 3] : 0;
}
u64 be64(const std::vector<u8>& v, usize at) { return (u64{be32(v, at)} << 32) | be32(v, at + 4); }

// The core file's notes, one line each (its program header, its name, type and bytes).
void describe(Observed& o, const std::vector<u8>& core)
{
    if (core.size() < 0x40) return;
    const u32 phnum = (u32{core[0x38]} << 8) | core[0x39];
    o.line("the core: ELF header", hex(core.data(), 0x40));
    for (u32 i = 0; i < phnum; i++) {
        const usize ph = 0x40 + usize{i} * 0x38;
        if (ph + 0x38 > core.size()) break;
        const u64 off = be64(core, ph + 8), size = be64(core, ph + 0x20);
        o.line(std::format("the core: program header {}", i), hex(core.data() + ph, 0x38));
        if (off + size > core.size() || size < 0x30) continue;
        std::string name(reinterpret_cast<const char*>(core.data() + off + 12), 16);
        name = name.c_str();
        o.line(std::format("the core: note {} {} (type {:#x})", i, name, be32(core, off + 8)),
            hex(core.data() + off, static_cast<usize>(size)));
    }
}

Observed half_core(const Setup& s, bool sony_half, bool declared)
{
    g_declared = declared;
    g_sony = sony_half;
    Half<CoreFixture> h(s, sony_half, "sys_lv2coredump");
    u8* b = h.b;
    if (!h.side.sony()) h.o.line("start", answer(static_cast<u64>(hle::lv2coredump_start(h.t.ctx, 0, 0))));
    else h.o.line("start", answer(0));
    const char* path = "/dev_hdd0/tmp/core.elf";
    std::memcpy(b + kPath, path, std::strlen(path) + 1);
    REQUIRE(h.t.call("cellFsOpen", kPath, 0x242, kFd, 0, 0) == 0);
    const u32 fd = ps_read32(b, kFd);
    const std::optional<u64> r = h.side.call_nid(kNid, {fd});
    REQUIRE(r);
    h.o.line("D725F320(fd)", answer(*r));
    h.t.call("cellFsClose", fd);
    const std::vector<u8> core = read_host(half_dir(sony_half) / "tmp" / "core.elf");
    h.o.line("the core file's size", std::format("{:#x}", core.size()));
    describe(h.o, core);
    h.o.bytes("the core file", core.data(), core.size());
    if (const char* dir = std::getenv("PSPOCKET_LV2COREDUMP_KEEP")) {
        std::ofstream out(std::filesystem::path(dir) / (sony_half ? "sony.core" : "ours.core"), std::ios::binary);
        out.write(reinterpret_cast<const char*>(core.data()), static_cast<std::streamsize>(core.size()));
    }
    return std::move(h.o);
}

} // namespace

TEST_CASE("liblv2coredump against Sony's: a game's declaration, D725F320's core file", "[hle][lle][compare][lv2coredump]")
{
    const Setup s = setup();
    run(s, "D725F320 against Sony's liblv2coredump: a game's declaration",
        [&](bool sony) { return half_core(s, sony, true); },
        [](const Observed& o) {
            CHECK(o.value("D725F320(fd)") == "0x0");
            CHECK(o.value("the core: note 0 SYSTEM_INFO (type 0x1000)").size() > 0);
        });
}

TEST_CASE("liblv2coredump against Sony's: no declaration, D725F320 fails", "[hle][lle][compare][lv2coredump]")
{
    const Setup s = setup();
    run(s, "D725F320 against Sony's liblv2coredump: no declaration",
        [&](bool sony) { return half_core(s, sony, false); },
        [](const Observed& o) { CHECK(o.value("D725F320(fd)") == "0x80010023"); });
}

// Without the firmware: pspocket's start registers the handler's queue and thread only for a process with the
// declaration, and D725F320 writes a retail console's core file (the four notes) or, without a start, fails.
TEST_CASE("liblv2coredump: start with a declaration, a retail core file; none without one", "[hle][lv2coredump]")
{
    constexpr u64 kNotRegistered = 0xFFFFFFFF8001042Bull; // CELL_LV2DBG_ERROR_DEHANDLENOTREGISTERED
    for (const bool declared : {true, false}) {
        INFO("declared " << declared);
        g_declared = declared;
        g_sony = false;
        CoreFixture f;
        GuestThread t(f, 0, "game");
        u8* b = f.base();
        CHECK(hle::lv2coredump_start(t.ctx, 0, 0) == 0);
        // The handler's queue ("_l2cDBG") hears a game's event only when start registered it.
        const u64 sent = hle::lv2_syscall(t.ctx, 959, {1, 0, 0, 0});
        CHECK(sent == (declared ? 0 : kNotRegistered));
        const char* path = "/dev_hdd0/tmp/core.elf";
        std::memcpy(b + kPath, path, std::strlen(path) + 1);
        REQUIRE(t.call("cellFsOpen", kPath, 0x242, kFd, 0, 0) == 0);
        const u32 fd = ps_read32(b, kFd);
        const u64 r = t.call("sys_lv2coredump_D725F320", fd);
        t.call("cellFsClose", fd);
        const std::vector<u8> core = read_host(half_dir(false) / "tmp" / "core.elf");
        if (!declared) {
            CHECK(r == 0xFFFFFFFF80010023ull); // CELL_ENOSPC: no configuration, no device's free space read
            CHECK(core.empty());
            continue;
        }
        REQUIRE(r == 0);
        REQUIRE(core.size() == 0x7BC);
        CHECK(be32(core, 0) == 0x7F454C46);
        CHECK(((u32{core[0x38]} << 8) | core[0x39]) == 4); // e_phnum, written as each note ended
        const char* names[] = {"SYSTEM_INFO", "DUMP_CAUSE_INFO", "PROCESS_INFO", "GAME_APP_INFO"};
        for (u32 i = 0; i < 4; i++) {
            const usize ph = 0x40 + usize{i} * 0x38;
            CHECK(be32(core, ph) == 4); // PT_NOTE
            const u64 off = be64(core, ph + 8);
            CHECK(std::string(reinterpret_cast<const char*>(core.data() + off + 12)) == names[i]);
        }
        // PROCESS_INFO counts the game's thread and the handler's.
        const u64 process = be64(core, 0x40 + 2 * 0x38 + 8);
        CHECK(be32(core, process + 0x30 + 4) == 2);
    }
}
