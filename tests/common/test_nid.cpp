#include <catch_amalgamated.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::recomp;

TEST_CASE("nid_of matches NIDs verified against RPCS3")
{
    CHECK(nid_of("cellSysutilRegisterCallback") == 0x9d98afa0u);
    CHECK(nid_of("sys_process_exit") == 0xe6f2c1e7u);
    CHECK(nid_of("_cellGcmInitBody") == 0x15bae46bu);
    CHECK(nid_of("cellGcmSetFlipMode") == 0x4ae8d215u);
    CHECK(nid_of("cellPadInit") == 0x1cf98800u);
    CHECK(nid_of("sys_lwmutex_lock") == 0x1573dc3fu);
    CHECK(nid_of("cellFsOpen") == 0x718bf5f8u);
    CHECK(nid_of("_sys_printf") == 0x9f04f7afu);
    CHECK(nid_of("cellSpursInitialize") == 0xacfc8dbcu);
    CHECK(nid_of("sys_ppu_thread_exit") == 0xaff080a4u);
}

TEST_CASE("the embedded NID database resolves names by module")
{
    const NidDb& db = NidDb::builtin();
    CHECK(db.size() > 6000);
    CHECK(db.module_count() > 100);

    CHECK(db.find("cellGcmSys", 0x4ae8d215u) == "cellGcmSetFlipMode");
    CHECK(db.find("cellGcmSys", 0x15bae46bu) == "_cellGcmInitBody");
    CHECK(db.find("sys_fs", 0x718bf5f8u) == "cellFsOpen"); // RPCS3 registers cellFs as library "sys_fs"
    CHECK(db.find("sys_io", 0x1cf98800u) == "cellPadInit");
    CHECK(db.find("sysPrxForUser", 0x1573dc3fu) == "sys_lwmutex_lock");
    CHECK(db.find("sysPrxForUser", 0x9f04f7afu) == "_sys_printf");
    CHECK(db.find("sysPrxForUser", 0xe6f2c1e7u) == "sys_process_exit");
    CHECK(db.find("sysPrxForUser", 0xaff080a4u) == "sys_ppu_thread_exit");
    CHECK(db.find("cellSysutil", 0x9d98afa0u) == "cellSysutilRegisterCallback");
    CHECK(db.find("cellSpurs", 0xacfc8dbcu) == "cellSpursInitialize");

    // REG_FNID with an explicit NID, REG_FNID with a string, REG_VNID, and the explicit PPUFunction.cpp tables.
    CHECK(db.find("cellSysutil", 0xE1EC7B6Au) == "cellSysutil_E1EC7B6A");
    CHECK(db.find("sys_net", nid_of("accept")) == "accept");
    CHECK(db.find("cellDmuxPamf", 0x28b2b7b2u) == "g_cell_dmux_core_ops_pamf");
    CHECK(db.find("sys_libc", 0xd97b0687u) == "_Ctype");
    CHECK(db.find("sys_libc", nid_of("memcpy")) == "memcpy");
    // Nameless module-management library.
    CHECK(db.find("", 0xbc9a0086u) == "module_start");
    CHECK(db.find("", 0xab779874u) == "module_stop");

    // Locally declared modules (static ppu_static_module inside sysPrxForUser.cpp).
    CHECK(db.has_module("sysPs2Disc"));
    // RPCS3's own pseudo-libraries are not PS3 libraries.
    CHECK_FALSE(db.has_module("RPCS3_HLE_LIBRARY"));

    CHECK_FALSE(db.find("cellGcmSys", 0x718bf5f8u).has_value());
    CHECK_FALSE(db.find("noSuchModule", 0x718bf5f8u).has_value());
}

TEST_CASE("module-agnostic lookup and resolution")
{
    const NidDb& db = NidDb::builtin();
    const auto any = db.find_any(nid_of("cellFsOpen"));
    CHECK(std::find(any.begin(), any.end(), "cellFsOpen") != any.end());

    const auto exact = db.resolve("sys_fs", nid_of("cellFsOpen"));
    CHECK(exact.match == NidMatch::Exact);
    CHECK(exact.name == "cellFsOpen");

    const auto other = db.resolve("someGameLibrary", nid_of("cellFsOpen"));
    CHECK(other.match == NidMatch::OtherModule);
    CHECK(other.name == "cellFsOpen");

    const auto unknown = db.resolve("sys_fs", 0xDEADBEEFu);
    CHECK(unknown.match == NidMatch::Unknown);
    CHECK(unknown.name.empty());
}

TEST_CASE("names files: both entry forms, comments, duplicates")
{
    NidDb db;
    const auto added = db.load_text(R"(# extra names
[myLib]
my_function           # NID computed
0x12345678 my_variable

[]
0x0D10FD3F module_prologue
[myLib]
my_function
)");
    REQUIRE(added);
    CHECK(*added == 3); // the repeated my_function is a duplicate
    CHECK(db.size() == 3);
    CHECK(db.find("myLib", nid_of("my_function")) == "my_function");
    CHECK(db.find("myLib", 0x12345678u) == "my_variable");
    CHECK(db.find("", 0x0D10FD3Fu) == "module_prologue");

    // Names added later extend a copy of the built-in database.
    NidDb extended = NidDb::builtin();
    const usize before = extended.size();
    REQUIRE(extended.load_text("[synthLibExport]\nsynthetic_export_func\n"));
    CHECK(extended.size() == before + 1);
    CHECK(extended.find("synthLibExport", nid_of("synthetic_export_func")) == "synthetic_export_func");
}

TEST_CASE("malformed names files are rejected with a line number and change nothing")
{
    struct Case {
        const char* text;
        const char* message;
    };
    const Case cases[] = {
        {"orphan_entry\n", ":1: entry before any [module] header"},
        {"[lib]\n0xZZ name\n", ":2: '0xZZ' is not a NID"},
        {"[lib]\n0x123456789 name\n", ":2:"},
        {"[lib]\na b c\n", ":2: expected"},
        {"[lib\nname\n", ":1: malformed module header"},
        {"[li]b]\nname\n", ":1: malformed module header"},
    };
    for (const auto& c : cases) {
        NidDb db;
        REQUIRE(db.load_text("[ok]\nfine\n"));
        const auto r = db.load_text(c.text, "extra.txt");
        INFO(c.text);
        REQUIRE_FALSE(r);
        CHECK(r.error().message.find(c.message) != std::string::npos);
        CHECK(db.size() == 1);
    }
}

TEST_CASE("names files load from disk")
{
    const auto dir = std::filesystem::temp_directory_path() / "pspocket_test_nid";
    std::filesystem::create_directories(dir);
    const auto path = dir / "names.txt";
    {
        std::ofstream out(path);
        out << "[diskLib]\ndisk_function\n";
    }
    NidDb db;
    const auto r = db.load_file(path);
    REQUIRE(r);
    CHECK(*r == 1);
    CHECK(db.find("diskLib", nid_of("disk_function")) == "disk_function");
    CHECK_FALSE(db.load_file(dir / "missing.txt"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("lv2 syscall table spot checks (RPCS3 lv2.cpp)")
{
    const SyscallTable& t = SyscallTable::builtin();
    CHECK(t.size() > 600);
    CHECK(t.name(1) == "sys_process_getpid");
    CHECK(t.name(22) == "_sys_process_exit");
    CHECK(t.name(41) == "_sys_ppu_thread_exit");
    CHECK(t.name(100) == "sys_mutex_create");
    CHECK(t.name(102) == "sys_mutex_lock");
    CHECK(t.name(141) == "sys_timer_usleep");
    CHECK(t.name(169) == "sys_spu_initialize");
    CHECK(t.name(170) == "sys_spu_thread_group_create");
    CHECK(t.name(352) == "sys_memory_get_user_memory_size");
    CHECK(t.name(484) == "_sys_prx_register_module");
    CHECK(t.name(801) == "sys_fs_open");
    CHECK(t.name(802) == "sys_fs_read");
    CHECK(t.number("sys_fs_open") == 801u);
    CHECK_FALSE(t.name(0).has_value());    // null_func slot
    CHECK_FALSE(t.name(6).has_value());    // unused
    CHECK_FALSE(t.name(1023).has_value());
    CHECK_FALSE(t.name(5000).has_value());

    SyscallTable custom;
    REQUIRE(custom.load_text("# comment\n7 my_call\n"));
    CHECK(custom.name(7) == "my_call");
    CHECK_FALSE(custom.load_text("x y\n"));
}
