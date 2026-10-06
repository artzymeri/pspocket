// sceNpTrophy (synthetic: no game data): init and term, contexts and handles with RPCS3's checks in RPCS3's order, and
// IDs as RPCS3's idm hands them out (1 to 4, in order, then the lowest free one; term doesn't rewind them). Then a
// trophy set registered from a synthetic TROPHY.TRP on a mounted disc directory: the installation, the status
// callback's direct call and RPCS3's steps through cellSysutilCheckCallback, game and trophy info, unlocks with the
// platinum trophy, and a second process that finds it all; and the checks and failures on the way.

#include "hle_test_util.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "psf.hpp" // runtime/hle/src (internal)
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/ppu/asm.hpp"
#include "trophy_test_util.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::ppu::as;
namespace fsys = std::filesystem;

namespace {

constexpr u64 kAlreadyInit = 0xFFFFFFFF80022901ull, kNotInit = 0xFFFFFFFF80022902ull,
              kNotSupported = 0xFFFFFFFF80022903ull, kNotRegistered = 0xFFFFFFFF80022904ull,
              kInvalArg = 0xFFFFFFFF80022906ull, kExceedsMax = 0xFFFFFFFF80022907ull,
              kUnknownContext = 0xFFFFFFFF8002290Aull, kInvalidContext = 0xFFFFFFFF8002290Eull,
              kProcessingAborted = 0xFFFFFFFF8002290Full, kAbort = 0xFFFFFFFF80022910ull,
              kUnknownHandle = 0xFFFFFFFF80022911ull, kHidden = 0xFFFFFFFF80022913ull,
              kCannotUnlockPlatinum = 0xFFFFFFFF80022914ull, kAlreadyUnlocked = 0xFFFFFFFF80022915ull,
              kInvalCommId = 0xFFFFFFFF80022918ull, kConfMissing = 0xFFFFFFFF8002291Bull,
              kInvalidTrophyId = 0xFFFFFFFF80022920ull, kIllegalUpdate = 0xFFFFFFFF8002292Aull;

constexpr u32 kCommId = kData + 0x100, kSign = kData + 0x200, kCtx = kData + 0x300, kHandle = kData + 0x310;
constexpr u32 kSpace = kData + 0x320, kPlatinum = kData + 0x330;
// SceNpTrophyGameDetails (1176 bytes), SceNpTrophyGameData (20), SceNpTrophyDetails (1164), SceNpTrophyData (16).
constexpr u32 kGameDetails = kData + 0x1000, kGameData = kData + 0x1500, kDetails = kData + 0x1600,
              kTrophyData = kData + 0x1C00;
constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests

// A SceNpCommunicationId (12 bytes) at kCommId: up to nine characters, the terminator, num, dummy.
void put_comm_id(u8* b, std::string_view data, u8 num, u8 term = 0)
{
    std::memset(b + kCommId, 0, 12);
    std::memcpy(b + kCommId, data.data(), data.size());
    b[kCommId + 9] = term;
    b[kCommId + 10] = num;
}

// A SceNpCommunicationSignature (160 bytes) at kSign whose header is valid: magic, version 1.00, six zero bytes. The
// rest stands for the signature proper (not checked).
void put_sign(u8* b)
{
    for (u32 i = 0; i < 160; i++) b[kSign + i] = static_cast<u8>(0x40 + i);
    ps_write32(b, kSign, 0xB9DDE13B);
    ps_write16(b, kSign + 4, 0x100);
    std::memset(b + kSign + 6, 0, 6);
}

void trophy_up(GuestThread& g) { REQUIRE(g.call("sceNpTrophyInit", 0, 0, 0xFFFFFFFFu, 0) == 0); }

// Creates a context for the communication ID and signature in place and returns its ID.
u32 create_context(GuestThread& g, u8* b, u64 options = 0)
{
    ps_write32(b, kCtx, 0xEEEEEEEE);
    REQUIRE(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, options) == 0);
    return ps_read32(b, kCtx);
}

u32 create_handle(GuestThread& g, u8* b)
{
    ps_write32(b, kHandle, 0xEEEEEEEE);
    REQUIRE(g.call("sceNpTrophyCreateHandle", kHandle) == 0);
    return ps_read32(b, kHandle);
}

// Init, a context for the set "<comm>_<num>" and a handle: IDs 1 and 1 in a new process.
void open_session(GuestThread& g, u8* b, std::string_view comm, u8 num)
{
    trophy_up(g);
    put_comm_id(b, comm, num);
    put_sign(b);
    REQUIRE(create_context(g, b) == 1);
    REQUIRE(create_handle(g, b) == 1);
}

// Guest functions without lifted code run in the interpreter while this lives (the default policy aborts).
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// A guest status callback (SceNpTrophyStatusCallback: context, status, completed, total, arg) behind the descriptor
// kCbOpd. Each call appends r3..r7 and r1 (low words) to a log at kCbLog + 32 (32 bytes an entry), counts the calls at
// kCbLog, and returns the word at kCbLog + 4. As a sysutil callback it logs (status, param, userdata) the same way.
constexpr u32 kCbCode = kData + 0x20000, kCbOpd = kData + 0x20800, kCbLog = kData + 0x21000, kToc = 0x00123400;

void write_status_callback(u8* b)
{
    const std::vector<u32> code = {
        as::lis(8, static_cast<s32>(kCbLog >> 16)),
        as::ori(8, 8, kCbLog & 0xFFFF),
        as::lwz(9, 0, 8),
        as::slwi(10, 9, 5),
        as::addi(10, 10, 32),
        as::stwx(3, 8, 10),
        as::addi(11, 10, 4),
        as::stwx(4, 8, 11),
        as::addi(11, 10, 8),
        as::stwx(5, 8, 11),
        as::addi(11, 10, 12),
        as::stwx(6, 8, 11),
        as::addi(11, 10, 16),
        as::stwx(7, 8, 11),
        as::addi(11, 10, 20),
        as::stwx(1, 8, 11),
        as::addi(9, 9, 1),
        as::stw(9, 0, 8),
        as::lwz(3, 4, 8),
        as::blr(),
    };
    for (usize i = 0; i < code.size(); i++) ps_write32(b, kCbCode + 4 * static_cast<u32>(i), code[i]);
    ps_write32(b, kCbOpd, kCbCode);
    ps_write32(b, kCbOpd + 4, kToc);
    std::memset(b + kCbLog, 0, 0x1000);
}

struct CallbackEntry {
    u32 context, status, completed, total, arg, sp;
};

u32 callback_count(u8* b) { return ps_read32(b, kCbLog); }
void set_callback_answer(u8* b, s32 answer) { ps_write32(b, kCbLog + 4, static_cast<u32>(answer)); }

CallbackEntry callback_entry(u8* b, u32 i)
{
    const u32 e = kCbLog + 32 + 32 * i;
    return {ps_read32(b, e), ps_read32(b, e + 4), ps_read32(b, e + 8), ps_read32(b, e + 12), ps_read32(b, e + 16),
        ps_read32(b, e + 20)};
}

// The GuestThread index whose guest stack holds `sp`.
u32 stack_thread(u32 sp) { return (sp - kStacks) / 0x10000; }

// RPCS3's status steps for a set of `trophies`: SETUP 0..3, PROGRESS 0..n-1, FINALIZE 0..max(n-1, 9)-5, COMPLETE 0
// (status, completed, total).
struct Step {
    u32 status;
    s32 completed, total;
};

std::vector<Step> status_steps(u32 trophies)
{
    const s32 progress = static_cast<s32>(trophies) - 1;
    std::vector<Step> steps;
    for (const auto& [status, total] :
        {std::pair<u32, s32>{5, 3}, {6, progress}, {7, std::max(progress, 9) - 5}, {8, 0}}) {
        for (s32 completed = 0; completed <= total; completed++) steps.push_back({status, completed, total});
    }
    return steps;
}

// What sceNpTrophyUnlockTrophy stamps: an RTC tick (cellRtcGetCurrentTick's clock).
u64 rtc_tick_now()
{
    s64 sec = 0, nsec = 0;
    lv2::host_current_time(sec, nsec);
    return static_cast<u64>(nsec / 1000) + static_cast<u64>(sec) * 1'000'000ull + 62135596800000000ull;
}

// The host directories the tests mount: a disc at /dev_bdvd (read only), the user's drive at /dev_hdd0.
struct TrophyDirs {
    fsys::path root, bdvd, hdd0;

    explicit TrophyDirs(std::string_view name)
        : root(fsys::temp_directory_path() / std::format("pspocket-hle-trophy-{}-{}", name, ::getpid())),
          bdvd(root / "bdvd"), hdd0(root / "hdd0")
    {
        fsys::remove_all(root);
        fsys::create_directories(bdvd / "PS3_GAME");
        fsys::create_directories(hdd0);
    }
    ~TrophyDirs()
    {
        std::error_code ec;
        fsys::remove_all(root, ec);
    }
    TrophyDirs(const TrophyDirs&) = delete;
    TrophyDirs& operator=(const TrophyDirs&) = delete;

    static void write(const fsys::path& p, const std::vector<u8>& bytes)
    {
        fsys::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    void add_disc_pack(std::string_view set, const std::vector<u8>& trp) const
    {
        write(bdvd / "PS3_GAME" / "TROPDIR" / set / "TROPHY.TRP", trp);
    }
    void add_hdd_pack(std::string_view title, std::string_view set, const std::vector<u8>& trp) const
    {
        write(hdd0 / "game" / title / "TROPDIR" / set / "TROPHY.TRP", trp);
    }
    fsys::path installed(std::string_view set) const { return hdd0 / "home" / "00000001" / "trophy" / set; }
    void mount(Fixture& f) const
    {
        REQUIRE(f.kernel->vfs_mount("/dev_bdvd", bdvd.string(), true));
        REQUIRE(f.kernel->vfs_mount("/dev_hdd0", hdd0.string(), false));
    }
};

std::string read_text(const fsys::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string guest_string(u8* b, u32 addr) { return std::string(reinterpret_cast<const char*>(b + addr)); }

// The guest bytes [addr, addr + size) are all `value`.
bool all_bytes(u8* b, u32 addr, u32 size, u8 value)
{
    return std::all_of(b + addr, b + addr + size, [&](u8 x) { return x == value; });
}

} // namespace

TEST_CASE("sceNpTrophy: the functions GTA V calls are registered", "[hle][trophy]")
{
    const std::pair<std::string_view, u32> expected[] = {
        {"sceNpTrophyInit", 0x39567781},
        {"sceNpTrophyTerm", 0xA7FABF4D},
        {"sceNpTrophyCreateContext", 0xE3BF9A28},
        {"sceNpTrophyDestroyContext", 0x3741ECC7},
        {"sceNpTrophyCreateHandle", 0x1C25470D},
        {"sceNpTrophyDestroyHandle", 0x623CD2DC},
        {"sceNpTrophyAbortHandle", 0x48BD97C7},
        {"sceNpTrophyGetRequiredDiskSpace", 0x370136FE},
        {"sceNpTrophyRegisterContext", 0x1197B52C},
        {"sceNpTrophyGetGameInfo", 0x49D18217},
        {"sceNpTrophyGetTrophyInfo", 0xFCE6D30A},
        {"sceNpTrophyUnlockTrophy", 0x8CEEDD21},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const auto& [name, nid] : expected) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("sceNpTrophy", nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
}

TEST_CASE("sceNpTrophy: init once, without options, until term", "[hle][trophy]")
{
    Fixture f;
    GuestThread g(f, 0, "trophy");
    CHECK(g.call("sceNpTrophyTerm") == kNotInit);
    CHECK(g.call("sceNpTrophyInit", 0, 0, 0xFFFFFFFFu, 1) == kNotSupported);
    CHECK(g.call("sceNpTrophyInit", 0, 0, 0xFFFFFFFFu, 0x100000000ull) == kNotSupported); // options is a u64
    trophy_up(g); // GTA V's call: no pool, no container
    CHECK(g.call("sceNpTrophyInit", 0, 0, 0xFFFFFFFFu, 0) == kAlreadyInit);
    CHECK(g.call("sceNpTrophyInit", 0, 0, 0xFFFFFFFFu, 1) == kAlreadyInit); // checked before the options
    REQUIRE(g.call("sceNpTrophyTerm") == 0); // GTA V terminates right away
    CHECK(g.call("sceNpTrophyTerm") == kNotInit);
    REQUIRE(g.call("sceNpTrophyInit", kData, 0x10000, 0x1234, 0) == 0); // the pool and container aren't used
    REQUIRE(g.call("sceNpTrophyTerm") == 0);
}

TEST_CASE("sceNpTrophy contexts: RPCS3's checks in its order, IDs 1 to 4, destroyed, cleared by term",
    "[hle][trophy]")
{
    Fixture f;
    GuestThread g(f, 0, "trophy");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    put_comm_id(b, "NPWR01234", 0);
    put_sign(b);
    ps_write32(b, kCtx, 0xEEEEEEEE);

    // The signature pointer is checked before the library's state.
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, 0, 0) == kInvalArg);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kNotInit);
    CHECK(g.call("sceNpTrophyDestroyContext", 1) == kNotInit);
    trophy_up(g);
    CHECK(g.call("sceNpTrophyDestroyContext", 1) == kUnknownContext);

    // The pointers, the options (only READ_ONLY), then the ID's number (at most 99) and the signature's header.
    CHECK(g.call("sceNpTrophyCreateContext", 0, kCommId, kSign, 0) == kInvalArg);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, 0, kSign, 0) == kInvalArg);
    CHECK(g.call("sceNpTrophyCreateContext", 0, kCommId, kSign, 2) == kInvalArg);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 2) == kNotSupported);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0x100000000ull) == kNotSupported);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kUnmapped, kSign, 0) == kInvalArg); // RPCS3 faults
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kUnmapped, 0) == kInvalArg);
    CHECK(g.call("sceNpTrophyCreateContext", kUnmapped, kCommId, kSign, 0) == kInvalArg);
    put_comm_id(b, "NPWR01234", 100);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kInvalCommId);
    put_comm_id(b, "NPWR01234", 0xFF);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kInvalCommId);
    put_comm_id(b, "NPWR01234", 99);
    ps_write32(b, kSign, 0xB9DDE13C); // magic
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kInvalCommId);
    put_sign(b);
    for (const u32 i : {6u, 11u}) { // the six zero bytes
        b[kSign + i] = 1;
        CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kInvalCommId);
        b[kSign + i] = 0;
    }
    for (const u16 version : {u16{0x0001}, u16{0x0101}, u16{0x0200}}) {
        ps_write16(b, kSign + 4, version);
        CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kInvalCommId);
    }
    put_sign(b);
    CHECK(ps_read32(b, kCtx) == 0xEEEEEEEE); // nothing created

    // IDs 1 to 4. The characters and the terminator aren't checked; read only is an option.
    CHECK(create_context(g, b) == 1);
    CHECK(create_context(g, b, 1) == 2);
    put_comm_id(b, "npwr", 7);
    CHECK(create_context(g, b) == 3);
    put_comm_id(b, "NPWR98765", 0, 'X');
    CHECK(create_context(g, b) == 4);
    ps_write32(b, kCtx, 0xEEEEEEEE);
    CHECK(g.call("sceNpTrophyCreateContext", kCtx, kCommId, kSign, 0) == kExceedsMax);
    CHECK(ps_read32(b, kCtx) == 0xEEEEEEEE);

    // Destroying: the ID must be 1 to 4, then known.
    for (const u32 id : {0u, 5u, 0xFFFFFFFFu}) CHECK(g.call("sceNpTrophyDestroyContext", id) == kInvalArg);
    REQUIRE(g.call("sceNpTrophyDestroyContext", 2) == 0);
    CHECK(g.call("sceNpTrophyDestroyContext", 2) == kUnknownContext);
    CHECK(create_context(g, b) == 2); // all four were handed out: the lowest free one
    REQUIRE(g.call("sceNpTrophyDestroyContext", 3) == 0);
    REQUIRE(g.call("sceNpTrophyDestroyContext", 1) == 0);
    CHECK(create_context(g, b) == 1);

    // Term clears every context.
    REQUIRE(g.call("sceNpTrophyTerm") == 0);
    CHECK(g.call("sceNpTrophyDestroyContext", 1) == kNotInit);
    trophy_up(g);
    for (u32 id = 1; id <= 4; id++) CHECK(g.call("sceNpTrophyDestroyContext", id) == kUnknownContext);
    CHECK(create_context(g, b) == 1);
    REQUIRE(g.call("sceNpTrophyTerm") == 0);
}

TEST_CASE("sceNpTrophy handles: IDs 1 to 4 that term doesn't rewind; abort and destroy without init",
    "[hle][trophy]")
{
    Fixture f;
    GuestThread g(f, 0, "trophy");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));

    // Creating needs the library; destroying and aborting don't (RPCS3: unknown whether a PS3 checks).
    ps_write32(b, kHandle, 0xEEEEEEEE);
    CHECK(g.call("sceNpTrophyCreateHandle", kHandle) == kNotInit);
    CHECK(g.call("sceNpTrophyCreateHandle", 0) == kNotInit);
    CHECK(ps_read32(b, kHandle) == 0xEEEEEEEE);
    CHECK(g.call("sceNpTrophyDestroyHandle", 1) == kUnknownHandle);
    CHECK(g.call("sceNpTrophyAbortHandle", 1) == kUnknownHandle);
    CHECK(g.call("sceNpTrophyDestroyHandle", 0) == kInvalArg);
    CHECK(g.call("sceNpTrophyAbortHandle", 5) == kInvalArg);

    trophy_up(g);
    CHECK(g.call("sceNpTrophyCreateHandle", 0) == kInvalArg);
    CHECK(g.call("sceNpTrophyCreateHandle", kUnmapped) == kInvalArg); // RPCS3 faults
    CHECK(create_handle(g, b) == 1);
    CHECK(create_handle(g, b) == 2);

    // Term clears the handles, but their IDs go on from where they were (idm keeps its highest index).
    REQUIRE(g.call("sceNpTrophyTerm") == 0);
    trophy_up(g);
    CHECK(g.call("sceNpTrophyDestroyHandle", 1) == kUnknownHandle);
    CHECK(g.call("sceNpTrophyAbortHandle", 2) == kUnknownHandle);
    CHECK(create_handle(g, b) == 3);
    CHECK(create_handle(g, b) == 4);
    CHECK(create_handle(g, b) == 1);
    CHECK(create_handle(g, b) == 2);
    ps_write32(b, kHandle, 0xEEEEEEEE);
    CHECK(g.call("sceNpTrophyCreateHandle", kHandle) == kExceedsMax);
    CHECK(ps_read32(b, kHandle) == 0xEEEEEEEE);

    // Contexts have IDs of their own.
    put_comm_id(b, "NPWR01234", 0);
    put_sign(b);
    CHECK(create_context(g, b) == 1);

    // An aborted handle stays until it is destroyed.
    for (const u32 id : {0u, 5u, 0xFFFFFFFFu}) {
        CHECK(g.call("sceNpTrophyAbortHandle", id) == kInvalArg);
        CHECK(g.call("sceNpTrophyDestroyHandle", id) == kInvalArg);
    }
    REQUIRE(g.call("sceNpTrophyAbortHandle", 3) == 0);
    CHECK(g.call("sceNpTrophyAbortHandle", 3) == 0);
    REQUIRE(g.call("sceNpTrophyDestroyHandle", 3) == 0);
    CHECK(g.call("sceNpTrophyAbortHandle", 3) == kUnknownHandle);
    CHECK(g.call("sceNpTrophyDestroyHandle", 3) == kUnknownHandle);
    CHECK(create_handle(g, b) == 3);

    // After term, destroying and aborting still answer (the handles are gone).
    REQUIRE(g.call("sceNpTrophyTerm") == 0);
    CHECK(g.call("sceNpTrophyDestroyHandle", 1) == kUnknownHandle);
    CHECK(g.call("sceNpTrophyAbortHandle", 4) == kUnknownHandle);
}

TEST_CASE("sceNpTrophy registration: the disc's pack installed, RPCS3's status steps, game and trophy info, unlocks; "
          "a second process finds it all",
    "[hle][trophy]")
{
    TrophyDirs dirs("register");
    const std::vector<u8> pack = make_trp(sample_trophy_files());
    dirs.add_disc_pack("NPWR12345_00", pack);
    const fsys::path installed = dirs.installed("NPWR12345_00");
    const std::vector<Step> steps = status_steps(4);
    REQUIRE(steps.size() == 14);
    constexpr u32 kArg = 0x00ABC0DE;
    u64 first_unlock = 0;
    {
        InterpretMisses interpret;
        Fixture f;
        dirs.mount(f);
        GuestThread a(f, 0, "trophy");
        u8* b = f.base();
        REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
        write_status_callback(b);
        open_session(a, b, "NPWR12345", 0);

        // Installing takes the pack's files (the pack less its header and entry table).
        CHECK(a.call("sceNpTrophyGetRequiredDiskSpace", 1, 1, kUnmapped, 0) == kInvalArg); // RPCS3 faults
        REQUIRE(a.call("sceNpTrophyGetRequiredDiskSpace", 1, 1, kSpace, 0) == 0);
        CHECK(ps_read64(b, kSpace) == pack.size() - 0x40 - 9 * 0x40);

        // Registering, while a second thread checks sysutil callbacks the whole time, as a game's main loop does.
        const hle::Function& check = GuestThread::find("cellSysutilCheckCallback");
        std::atomic<bool> stop{false};
        std::thread checker([&] {
            GuestThread c(f, 1, "checker");
            while (!stop) {
                check.fn(c.ctx, b);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        const auto start = std::chrono::steady_clock::now();
        const u64 r = a.call("sceNpTrophyRegisterContext", 1, 1, kCbOpd, kArg, 0);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        stop = true;
        checker.join();
        REQUIRE(r == 0);
        REQUIRE(a.call("cellSysutilCheckCallback") == 0); // whatever the checker didn't take
        CHECK(elapsed >= std::chrono::milliseconds(1900)); // RPCS3: at least 2 s over the steps

        // The callback: once directly on the registering thread (the set isn't installed yet), then each step through
        // a check.
        REQUIRE(callback_count(b) == 1 + steps.size());
        const CallbackEntry direct = callback_entry(b, 0);
        CHECK(direct.context == 1);
        CHECK(direct.status == 1); // SCE_NP_TROPHY_STATUS_NOT_INSTALLED
        CHECK(direct.completed == 0);
        CHECK(direct.total == 0);
        CHECK(direct.arg == kArg);
        CHECK(stack_thread(direct.sp) == 0);
        CHECK(stack_thread(callback_entry(b, 1).sp) == 1);
        for (usize i = 0; i < steps.size(); i++) {
            INFO("step " << i);
            const CallbackEntry e = callback_entry(b, static_cast<u32>(1 + i));
            CHECK(e.context == 1);
            CHECK(e.status == steps[i].status);
            CHECK(e.completed == static_cast<u32>(steps[i].completed));
            CHECK(e.total == static_cast<u32>(steps[i].total));
            CHECK(e.arg == kArg);
        }

        // Installed as RPCS3 lays it out: the English configuration as TROPCONF.SFM, every icon, TROPUSR.DAT; no other
        // configuration file.
        CHECK(read_text(installed / "TROPCONF.SFM") == kTropEnglish);
        for (const char* icon : {"ICON0.PNG", "TROP000.PNG", "TROP001.PNG", "TROP002.PNG", "TROP003.PNG"}) {
            CHECK(fsys::exists(installed / icon));
        }
        CHECK(read_text(installed / "TROP002.PNG") == "\x89PNG gold");
        usize files = 0;
        for ([[maybe_unused]] const auto& e : fsys::directory_iterator(installed)) files++;
        CHECK(files == 7);
        CHECK(fsys::file_size(installed / "TROPUSR.DAT") == 0x30 + 2 * 0x20 + 4 * 0x60 + 4 * 0x70);
        // Installed: nothing left to take.
        REQUIRE(a.call("sceNpTrophyGetRequiredDiskSpace", 1, 1, kSpace, 0) == 0);
        CHECK(ps_read64(b, kSpace) == 0);

        // The game: its title and its trophies by grade; nothing unlocked yet. Everything asked for is written.
        std::memset(b + kGameDetails, 0xAA, 1176);
        std::memset(b + kGameData, 0xAA, 20);
        REQUIRE(a.call("sceNpTrophyGetGameInfo", 1, 1, kGameDetails, kGameData) == 0);
        CHECK(ps_read32(b, kGameDetails + 0) == 4);
        for (u32 grade = 1; grade <= 4; grade++) CHECK(ps_read32(b, kGameDetails + 4 * grade) == 1);
        CHECK(guest_string(b, kGameDetails + 20) == "Synthetic & Test");
        CHECK(guest_string(b, kGameDetails + 148) == "A trophy set the test suite built");
        CHECK(all_bytes(b, kGameDetails + 20 + 16, 128 - 16, 0));
        CHECK(all_bytes(b, kGameDetails + 1172, 4, 0));
        CHECK(all_bytes(b, kGameData, 20, 0));
        std::memset(b + kGameDetails, 0xAA, 1176);
        REQUIRE(a.call("sceNpTrophyGetGameInfo", 1, 1, 0, kGameData) == 0);
        CHECK(ps_read32(b, kGameDetails) == 0xAAAAAAAAu); // not asked for
        CHECK(a.call("sceNpTrophyGetGameInfo", 1, 1, 0, 0) == kInvalArg);
        CHECK(a.call("sceNpTrophyGetGameInfo", 1, 1, kUnmapped, 0) == kInvalArg); // RPCS3 faults

        // Trophies: grade, name, description, hidden; the hidden one is refused while it is locked.
        std::memset(b + kDetails, 0xAA, 1164);
        std::memset(b + kTrophyData, 0xAA, 16);
        REQUIRE(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 0, kDetails, kTrophyData) == 0);
        CHECK(ps_read32(b, kDetails + 0) == 0);
        CHECK(ps_read32(b, kDetails + 4) == 1); // SCE_NP_TROPHY_GRADE_PLATINUM
        CHECK(guest_string(b, kDetails + 8) == "All of them");
        CHECK(guest_string(b, kDetails + 136) == "Unlock every other trophy");
        CHECK(ps_read8(b, kDetails + 1160) == 0);
        CHECK(all_bytes(b, kDetails + 1161, 3, 0));
        CHECK(ps_read64(b, kTrophyData + 0) == 0);
        CHECK(ps_read32(b, kTrophyData + 8) == 0);
        CHECK(all_bytes(b, kTrophyData + 12, 4, 0)); // locked, reserved
        REQUIRE(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 3, kDetails, 0) == 0);
        CHECK(ps_read32(b, kDetails + 4) == 3); // SCE_NP_TROPHY_GRADE_SILVER
        CHECK(guest_string(b, kDetails + 8) == "Extra <bonus>");
        CHECK(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 2, kDetails, kTrophyData) == kHidden);
        CHECK(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 4, kDetails, kTrophyData) == kInvalidTrophyId); // not in the set
        CHECK(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 128, kDetails, kTrophyData) == kInvalidTrophyId);
        CHECK(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 1, 0, 0) == kInvalArg);
        CHECK(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 1, 0, kUnmapped) == kInvalArg); // RPCS3 faults

        // Unlocking: not the platinum trophy itself; it comes with the last trophy that links to it.
        CHECK(a.call("sceNpTrophyUnlockTrophy", 1, 1, 0, kPlatinum) == kCannotUnlockPlatinum);
        CHECK(a.call("sceNpTrophyUnlockTrophy", 1, 1, 4, kPlatinum) == kInvalidTrophyId);
        CHECK(a.call("sceNpTrophyUnlockTrophy", 1, 1, -1, kPlatinum) == kInvalidTrophyId);
        CHECK(a.call("sceNpTrophyUnlockTrophy", 1, 1, 1, kUnmapped) == kInvalArg); // before anything changes
        ps_write32(b, kPlatinum, 0x5555);
        const u64 before = rtc_tick_now();
        REQUIRE(a.call("sceNpTrophyUnlockTrophy", 1, 1, 1, kPlatinum) == 0);
        CHECK(ps_read32(b, kPlatinum) == 0xFFFFFFFFu); // SCE_NP_TROPHY_INVALID_TROPHY_ID: the gold one is locked
        CHECK(a.call("sceNpTrophyUnlockTrophy", 1, 1, 1, kPlatinum) == kAlreadyUnlocked);
        REQUIRE(a.call("sceNpTrophyUnlockTrophy", 1, 1, 3, 0) == 0); // outside the platinum trophy; no pointer
        REQUIRE(a.call("sceNpTrophyUnlockTrophy", 1, 1, 2, kPlatinum) == 0);
        const u64 after = rtc_tick_now();
        CHECK(ps_read32(b, kPlatinum) == 0); // the platinum trophy
        CHECK(a.call("sceNpTrophyUnlockTrophy", 1, 1, 0, kPlatinum) == kCannotUnlockPlatinum);

        // Unlocked trophies show, the hidden one too, with their RTC ticks.
        REQUIRE(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 2, kDetails, kTrophyData) == 0);
        CHECK(guest_string(b, kDetails + 8) == "Secret");
        CHECK(ps_read8(b, kDetails + 1160) == 1);
        CHECK(ps_read8(b, kTrophyData + 12) == 1);
        const u64 gold_tick = ps_read64(b, kTrophyData);
        REQUIRE(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 0, 0, kTrophyData) == 0);
        CHECK(ps_read8(b, kTrophyData + 12) == 1);
        CHECK(ps_read64(b, kTrophyData) == gold_tick); // unlocked with it
        REQUIRE(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 1, 0, kTrophyData) == 0);
        first_unlock = ps_read64(b, kTrophyData);
        CHECK(first_unlock >= before);
        CHECK(first_unlock <= gold_tick);
        CHECK(gold_tick <= after);
        std::memset(b + kGameData, 0xAA, 20);
        REQUIRE(a.call("sceNpTrophyGetGameInfo", 1, 1, 0, kGameData) == 0);
        CHECK(ps_read32(b, kGameData) == 4);
        for (u32 grade = 1; grade <= 4; grade++) CHECK(ps_read32(b, kGameData + 4 * grade) == 1);

        // TROPUSR.DAT holds it: table 6, trophy 1: unlocked at that tick (both timestamps).
        const std::string usr = read_text(installed / "TROPUSR.DAT");
        const u8* e6 = reinterpret_cast<const u8*>(usr.data()) + 0x70 + 4 * 0x60 + 1 * 0x70;
        CHECK(ps_ld32(e6 + 0x14) == 1);
        CHECK(ps_ld64(e6 + 0x20) == first_unlock);
        CHECK(ps_ld64(e6 + 0x28) == first_unlock);
        REQUIRE(a.call("sceNpTrophyTerm") == 0);
    }

    // A new process: the set is installed, so the callback hears so, and the progress is still there. Nobody checks
    // sysutil callbacks during the registration this time: each step waits 50 ms more, then the next comes anyway,
    // and the next check delivers them all.
    {
        InterpretMisses interpret;
        Fixture f;
        dirs.mount(f);
        GuestThread a(f, 0, "trophy");
        u8* b = f.base();
        write_status_callback(b);
        open_session(a, b, "NPWR12345", 0);
        REQUIRE(a.call("sceNpTrophyGetRequiredDiskSpace", 1, 1, kSpace, 0) == 0);
        CHECK(ps_read64(b, kSpace) == 0);
        const auto start = std::chrono::steady_clock::now();
        REQUIRE(a.call("sceNpTrophyRegisterContext", 1, 1, kCbOpd, kArg, 0) == 0);
        CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(1900));
        REQUIRE(callback_count(b) == 1);
        CHECK(callback_entry(b, 0).status == 3); // SCE_NP_TROPHY_STATUS_INSTALLED
        REQUIRE(a.call("cellSysutilCheckCallback") == 0);
        REQUIRE(callback_count(b) == 1 + steps.size());
        for (usize i = 0; i < steps.size(); i++) {
            INFO("step " << i);
            const CallbackEntry e = callback_entry(b, static_cast<u32>(1 + i));
            CHECK(e.status == steps[i].status);
            CHECK(e.completed == static_cast<u32>(steps[i].completed));
            CHECK(e.total == static_cast<u32>(steps[i].total));
        }
        REQUIRE(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 1, 0, kTrophyData) == 0);
        CHECK(ps_read8(b, kTrophyData + 12) == 1);
        CHECK(ps_read64(b, kTrophyData) == first_unlock);
        REQUIRE(a.call("sceNpTrophyGetGameInfo", 1, 1, 0, kGameData) == 0);
        CHECK(ps_read32(b, kGameData) == 4);
        CHECK(a.call("sceNpTrophyUnlockTrophy", 1, 1, 3, kPlatinum) == kAlreadyUnlocked);
        CHECK(read_text(installed / "TROPCONF.SFM") == kTropEnglish);
        REQUIRE(a.call("sceNpTrophyTerm") == 0);
    }
}

TEST_CASE("sceNpTrophy registration: RPCS3's checks in its order, a callback that says no, packs that can't be "
          "installed",
    "[hle][trophy]")
{
    InterpretMisses interpret;
    TrophyDirs dirs("errors");
    dirs.add_disc_pack("NPWR12345_00", make_trp(sample_trophy_files()));
    std::vector<u8> corrupt = make_trp(sample_trophy_files());
    corrupt.back() ^= 1; // the SHA-1 no longer matches
    dirs.add_disc_pack("NPWR00001_00", corrupt);
    dirs.add_disc_pack("NPWR00002_00", make_trp({{"ICON0.PNG", "icon"}})); // no configuration
    Fixture f;
    dirs.mount(f);
    GuestThread a(f, 0, "trophy");
    u8* b = f.base();
    write_status_callback(b);
    constexpr u32 kArg = 0x1234;

    // Before init. The trophy ID comes first in GetTrophyInfo, the pointer and options in GetRequiredDiskSpace.
    CHECK(a.call("sceNpTrophyRegisterContext", 1, 1, kCbOpd, kArg, 0) == kNotInit);
    CHECK(a.call("sceNpTrophyGetGameInfo", 1, 1, kGameDetails, kGameData) == kNotInit);
    CHECK(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 0, kDetails, kTrophyData) == kNotInit);
    CHECK(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 128, kDetails, kTrophyData) == kInvalidTrophyId);
    CHECK(a.call("sceNpTrophyGetTrophyInfo", 1, 1, -1, kDetails, kTrophyData) == kInvalidTrophyId);
    CHECK(a.call("sceNpTrophyUnlockTrophy", 1, 1, 1, kPlatinum) == kNotInit);
    CHECK(a.call("sceNpTrophyGetRequiredDiskSpace", 1, 1, 0, 0) == kInvalArg);
    CHECK(a.call("sceNpTrophyGetRequiredDiskSpace", 1, 1, kSpace, 1) == kNotSupported);
    CHECK(a.call("sceNpTrophyGetRequiredDiskSpace", 1, 1, kSpace, 0) == kNotInit);

    // Context 1, the same set read only (2), the corrupt pack (3); handle 1, and handle 2 aborted. ID 4 is unknown.
    trophy_up(a);
    put_sign(b);
    put_comm_id(b, "NPWR12345", 0);
    REQUIRE(create_context(a, b) == 1);
    REQUIRE(create_context(a, b, 1) == 2);
    put_comm_id(b, "NPWR00001", 0);
    REQUIRE(create_context(a, b) == 3);
    REQUIRE(create_handle(a, b) == 1);
    REQUIRE(create_handle(a, b) == 2);
    REQUIRE(a.call("sceNpTrophyAbortHandle", 2) == 0);

    // RPCS3's get_context_ex: the context (1..4, known; writable to register or unlock), then the handle (1..4,
    // known, not aborted).
    struct Case {
        u32 context, handle;
        u64 writes, reads; // registering and unlocking; game info, trophy info and the space
    };
    const Case cases[] = {
        {0, 1, kInvalArg, kInvalArg},
        {5, 1, kInvalArg, kInvalArg},
        {4, 1, kUnknownContext, kUnknownContext},
        {2, 0, kInvalidContext, kInvalArg},
        {1, 0, kInvalArg, kInvalArg},
        {1, 5, kInvalArg, kInvalArg},
        {1, 3, kUnknownHandle, kUnknownHandle},
        {1, 2, kAbort, kAbort},
    };
    for (const Case& c : cases) {
        INFO("context " << c.context << ", handle " << c.handle);
        CHECK(a.call("sceNpTrophyRegisterContext", c.context, c.handle, kCbOpd, kArg, 0) == c.writes);
        CHECK(a.call("sceNpTrophyUnlockTrophy", c.context, c.handle, 1, kPlatinum) == c.writes);
        CHECK(a.call("sceNpTrophyGetGameInfo", c.context, c.handle, kGameDetails, kGameData) == c.reads);
        CHECK(a.call("sceNpTrophyGetTrophyInfo", c.context, c.handle, 0, kDetails, kTrophyData) == c.reads);
        CHECK(a.call("sceNpTrophyGetRequiredDiskSpace", c.context, c.handle, kSpace, 0) == c.reads);
    }
    // A read-only context can be read (it isn't registered), not registered or unlocked; its space is the pack's.
    CHECK(a.call("sceNpTrophyRegisterContext", 2, 1, kCbOpd, kArg, 0) == kInvalidContext);
    CHECK(a.call("sceNpTrophyUnlockTrophy", 2, 1, 1, kPlatinum) == kInvalidContext);
    CHECK(a.call("sceNpTrophyGetGameInfo", 2, 1, kGameDetails, kGameData) == kNotRegistered);
    REQUIRE(a.call("sceNpTrophyGetRequiredDiskSpace", 2, 1, kSpace, 0) == 0);
    CHECK(ps_read64(b, kSpace) > 0);

    // Not registered yet. GetTrophyInfo wants an output before it looks at the registration; GetGameInfo the other way
    // round (RPCS3).
    CHECK(a.call("sceNpTrophyGetGameInfo", 1, 1, kGameDetails, kGameData) == kNotRegistered);
    CHECK(a.call("sceNpTrophyGetGameInfo", 1, 1, 0, 0) == kNotRegistered);
    CHECK(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 0, kDetails, kTrophyData) == kNotRegistered);
    CHECK(a.call("sceNpTrophyGetTrophyInfo", 1, 1, 0, 0, 0) == kInvalArg);
    CHECK(a.call("sceNpTrophyUnlockTrophy", 1, 1, 1, kPlatinum) == kNotRegistered);
    CHECK(a.call("sceNpTrophyRegisterContext", 1, 1, 0, kArg, 0) == kInvalArg);
    CHECK(a.call("sceNpTrophyRegisterContext", 1, 1, kCbOpd, kArg, 2) == kNotSupported);
    CHECK(callback_count(b) == 0); // none of these got as far as the callback

    // The callback says no: nothing is installed. With SHOW_ERROR_EXIT the game is also asked to exit (there is no
    // error dialog).
    set_callback_answer(b, -1);
    CHECK(a.call("sceNpTrophyRegisterContext", 1, 1, kCbOpd, kArg, 0) == kProcessingAborted);
    REQUIRE(callback_count(b) == 1);
    CHECK(callback_entry(b, 0).status == 1); // not installed
    CHECK_FALSE(fsys::exists(dirs.installed("NPWR12345_00")));
    REQUIRE(a.call("cellSysutilRegisterCallback", 0, kCbOpd, 0x5150) == 0);
    CHECK(a.call("sceNpTrophyRegisterContext", 1, 1, kCbOpd, kArg, 1) == kProcessingAborted);
    REQUIRE(a.call("cellSysutilCheckCallback") == 0);
    REQUIRE(callback_count(b) == 3);
    CHECK(callback_entry(b, 2).context == 0x0101);   // the sysutil callback's status: CELL_SYSUTIL_REQUEST_EXITGAME
    CHECK(callback_entry(b, 2).completed == 0x5150); // its userdata
    REQUIRE(a.call("cellSysutilUnregisterCallback", 0) == 0);
    set_callback_answer(b, 0);

    // Packs that can't be installed fail after the callback's yes, and leave nothing behind. A corrupt pack's space
    // is 0 (an error log), not an error.
    CHECK(a.call("sceNpTrophyRegisterContext", 3, 1, kCbOpd, kArg, 0) == kIllegalUpdate);
    CHECK(callback_count(b) == 4);
    CHECK_FALSE(fsys::exists(dirs.installed("NPWR00001_00")));
    ps_write64(b, kSpace, 0xAAAA);
    REQUIRE(a.call("sceNpTrophyGetRequiredDiskSpace", 3, 1, kSpace, 0) == 0);
    CHECK(ps_read64(b, kSpace) == 0);
    REQUIRE(a.call("sceNpTrophyDestroyContext", 3) == 0);
    put_comm_id(b, "NPWR00002", 0);
    REQUIRE(create_context(a, b) == 4);
    CHECK(a.call("sceNpTrophyRegisterContext", 4, 1, kCbOpd, kArg, 0) == kIllegalUpdate);
    CHECK(callback_count(b) == 5);
    CHECK_FALSE(fsys::exists(dirs.installed("NPWR00002_00")));

    // No pack at all: refused before the callback.
    REQUIRE(a.call("sceNpTrophyDestroyContext", 2) == 0);
    put_comm_id(b, "NPWR77777", 0);
    REQUIRE(create_context(a, b) == 2);
    CHECK(a.call("sceNpTrophyRegisterContext", 2, 1, kCbOpd, kArg, 0) == kConfMissing);
    CHECK(a.call("sceNpTrophyGetRequiredDiskSpace", 2, 1, kSpace, 0) == kConfMissing);
    CHECK(callback_count(b) == 5);
    CHECK_FALSE(fsys::exists(dirs.installed("NPWR12345_00")));
}

TEST_CASE("sceNpTrophy: an HDD game's own pack; a game booted from its game data uses the disc's", "[hle][trophy]")
{
    TrophyDirs dirs("hdd");
    const std::vector<u8> pack = make_trp(sample_trophy_files());
    dirs.add_hdd_pack("NPEB00001", "NPWR12345_00", pack);
    dirs.add_disc_pack("NPWR99999_00", pack);
    const u64 space = pack.size() - 0x40 - 9 * 0x40;
    for (const std::string_view category : {"HG", "GD"}) {
        INFO(category);
        hle::ProcessConfig config;
        config.game.boot = hle::GameInfo::Boot::Hdd;
        config.game.title_id = "NPEB00001";
        hle::psf::Registry sfo;
        sfo["CATEGORY"] = hle::psf::string(4, category);
        config.game.param_sfo = hle::psf::save(sfo);
        Fixture f(config);
        dirs.mount(f);
        GuestThread a(f, 0, "trophy");
        u8* b = f.base();
        trophy_up(a);
        put_sign(b);
        put_comm_id(b, "NPWR12345", 0);
        REQUIRE(create_context(a, b) == 1);
        put_comm_id(b, "NPWR99999", 0); // only on the disc
        REQUIRE(create_context(a, b) == 2);
        REQUIRE(create_handle(a, b) == 1);
        REQUIRE(a.call("sceNpTrophyGetRequiredDiskSpace", 1, 1, kSpace, 0) == 0);
        CHECK(ps_read64(b, kSpace) == space);
        if (category == "GD") {
            REQUIRE(a.call("sceNpTrophyGetRequiredDiskSpace", 2, 1, kSpace, 0) == 0);
            CHECK(ps_read64(b, kSpace) == space);
        } else {
            CHECK(a.call("sceNpTrophyGetRequiredDiskSpace", 2, 1, kSpace, 0) == kConfMissing);
        }
    }
}

namespace {

constexpr u64 kLocked = 0xFFFFFFFF80022912ull, kUnknownFile = 0xFFFFFFFF80022923ull;
constexpr u32 kPercent = kData + 0x340, kCount = kData + 0x344, kSize = kData + 0x348, kFlags = kData + 0x350;
constexpr u32 kIcon = kData + 0x2000, kIconSpace = 0x100;

// A set of `count` bronze trophies and no platinum one, in the default language (TROP.SFM, installed as
// TROPCONF.SFM). Trophy 1 is hidden.
std::string many_trophies(u32 count)
{
    std::string xml = "<trophyconf><title-name>Many</title-name>";
    for (u32 id = 0; id < count; id++) {
        xml += std::format("<trophy id=\"{:03}\" hidden=\"{}\" ttype=\"B\" pid=\"-1\"><name>T{}</name></trophy>", id,
            id == 1 ? "yes" : "no", id);
    }
    return xml + "</trophyconf>";
}

// sceNpTrophyRegisterContext(context, handle) with the status callback, while a second thread checks sysutil callbacks
// as a game's main loop does; then whatever that thread didn't take is checked here.
void register_checked(Fixture& f, GuestThread& a, u8* b, u32 context, u32 handle)
{
    const hle::Function& check = GuestThread::find("cellSysutilCheckCallback");
    std::atomic<bool> stop{false};
    std::thread checker([&] {
        GuestThread c(f, 1, "checker");
        while (!stop) {
            check.fn(c.ctx, b);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const u64 r = a.call("sceNpTrophyRegisterContext", context, handle, kCbOpd, 0, 0);
    stop = true;
    checker.join();
    REQUIRE(r == 0);
    REQUIRE(a.call("cellSysutilCheckCallback") == 0);
}

std::string_view guest_bytes(u8* b, u32 addr, u32 size) { return {reinterpret_cast<const char*>(b + addr), size}; }

} // namespace

TEST_CASE("sceNpTrophy progress, unlock flags, icons and sound level: RPCS3's checks and answers", "[hle][trophy]")
{
    TrophyDirs dirs("progress");
    constexpr u32 kTrophies = 136; // more than the 128 flags hold
    std::vector<TrpFile> files = {{"TROP.SFM", many_trophies(kTrophies)}, {"ICON0.PNG", "\x89PNG many"}};
    for (const u32 id : {0u, 1u, 2u, 127u, 135u}) {
        files.emplace_back(std::format("TROP{:03}.PNG", id), std::format("\x89PNG icon {}", id));
    }
    dirs.add_disc_pack("NPWR24680_00", make_trp(files));
    const fsys::path installed = dirs.installed("NPWR24680_00");
    InterpretMisses interpret;
    Fixture f;
    dirs.mount(f);
    GuestThread a(f, 0, "trophy");
    u8* b = f.base();
    REQUIRE_FALSE(f.mem.is_committed(kUnmapped));
    write_status_callback(b);

    // Before init: the arguments each function checks first, then the library.
    CHECK(a.call("sceNpTrophyGetGameProgress", 1, 1, 0) == kInvalArg);
    CHECK(a.call("sceNpTrophyGetGameProgress", 1, 1, kPercent) == kNotInit);
    CHECK(a.call("sceNpTrophyGetTrophyUnlockState", 1, 1, kFlags, 0) == kInvalArg);
    CHECK(a.call("sceNpTrophyGetTrophyUnlockState", 1, 1, 0, kCount) == kInvalArg);
    CHECK(a.call("sceNpTrophyGetTrophyUnlockState", 1, 1, kFlags, kCount) == kNotInit);
    CHECK(a.call("sceNpTrophyGetGameIcon", 1, 1, 0, 0) == kNotInit); // the size is checked after the context
    CHECK(a.call("sceNpTrophyGetTrophyIcon", 1, 1, 0, 0, 0) == kNotInit);
    CHECK(a.call("sceNpTrophySetSoundLevel", 1, 1, 19, 0) == kInvalArg);
    CHECK(a.call("sceNpTrophySetSoundLevel", 1, 1, 101, 1) == kInvalArg); // the level before the options
    CHECK(a.call("sceNpTrophySetSoundLevel", 1, 1, 50, 1) == kNotSupported);
    CHECK(a.call("sceNpTrophySetSoundLevel", 1, 1, 50, 0) == kNotInit);

    open_session(a, b, "NPWR24680", 0); // context 1, handle 1
    CHECK(a.call("sceNpTrophyGetGameProgress", 5, 1, kPercent) == kInvalArg);
    CHECK(a.call("sceNpTrophyGetGameProgress", 2, 1, kPercent) == kUnknownContext);
    CHECK(a.call("sceNpTrophyGetTrophyUnlockState", 1, 2, kFlags, kCount) == kUnknownHandle);
    CHECK(a.call("sceNpTrophyGetGameIcon", 1, 0, 0, kSize) == kInvalArg);
    CHECK(a.call("sceNpTrophyGetTrophyIcon", 2, 1, 0, 0, kSize) == kUnknownContext);
    CHECK(a.call("sceNpTrophySetSoundLevel", 1, 2, 50, 0) == kUnknownHandle);
    for (const u32 level : {20u, 75u, 100u}) CHECK(a.call("sceNpTrophySetSoundLevel", 1, 1, level, 0) == 0);

    // Not registered yet: no progress, flags or trophy icons. The game icon is looked for in the installed set: none.
    CHECK(a.call("sceNpTrophyGetGameProgress", 1, 1, kPercent) == kNotRegistered);
    CHECK(a.call("sceNpTrophyGetTrophyUnlockState", 1, 1, kFlags, kCount) == kNotRegistered);
    CHECK(a.call("sceNpTrophyGetTrophyIcon", 1, 1, 0, 0, 0) == kInvalArg); // the size before the registration
    CHECK(a.call("sceNpTrophyGetTrophyIcon", 1, 1, 0, 0, kSize) == kNotRegistered);
    CHECK(a.call("sceNpTrophyGetGameIcon", 1, 1, 0, 0) == kInvalArg);
    CHECK(a.call("sceNpTrophyGetGameIcon", 1, 1, 0, kSize) == kUnknownFile);
    // A read-only context of the set, not installed yet: no trophies and CELL_OK; nothing is made for it.
    REQUIRE(create_context(a, b, 1) == 2);
    std::memset(b + kFlags, 0xAA, 16);
    ps_write32(b, kCount, 0xEEEEEEEE);
    REQUIRE(a.call("sceNpTrophyGetTrophyUnlockState", 2, 1, kFlags, kCount) == 0);
    CHECK(ps_read32(b, kCount) == 0);
    CHECK(all_bytes(b, kFlags, 16, 0));
    CHECK_FALSE(fsys::exists(installed));

    register_checked(f, a, b, 1, 1);
    REQUIRE(fsys::exists(installed / "TROPUSR.DAT"));

    // Nothing unlocked: 0%, no flags; the count is the set's, past the 128 flags.
    ps_write32(b, kPercent, 0xEEEEEEEE);
    REQUIRE(a.call("sceNpTrophyGetGameProgress", 1, 1, kPercent) == 0);
    CHECK(ps_read32(b, kPercent) == 0);
    CHECK(a.call("sceNpTrophyGetGameProgress", 1, 1, kUnmapped) == kInvalArg); // RPCS3 faults
    std::memset(b + kFlags, 0xAA, 16);
    REQUIRE(a.call("sceNpTrophyGetTrophyUnlockState", 1, 1, kFlags, kCount) == 0);
    CHECK(ps_read32(b, kCount) == kTrophies);
    CHECK(all_bytes(b, kFlags, 16, 0));
    CHECK(a.call("sceNpTrophyGetTrophyUnlockState", 1, 1, kUnmapped, kCount) == kInvalArg);
    CHECK(a.call("sceNpTrophyGetTrophyUnlockState", 1, 1, kFlags, kUnmapped) == kInvalArg);

    // The progress, rounded to the nearest percent as RPCS3 rounds (a half down): 1 of 136 trophies is 0.74% (1),
    // 2 are 1.47% (1), 17 are 12.5% (12).
    const auto unlock = [&](s32 id) { REQUIRE(a.call("sceNpTrophyUnlockTrophy", 1, 1, id, 0) == 0); };
    const auto progress = [&] {
        REQUIRE(a.call("sceNpTrophyGetGameProgress", 1, 1, kPercent) == 0);
        return ps_read32(b, kPercent);
    };
    unlock(0);
    CHECK(progress() == 1);
    unlock(127); // the last flag
    CHECK(progress() == 1);
    unlock(135); // past the flags
    for (s32 id = 2; id < 16; id++) unlock(id);
    CHECK(progress() == 12);

    // The flags: bit n % 32 of word n / 32, for the first 128 trophies.
    REQUIRE(a.call("sceNpTrophyGetTrophyUnlockState", 1, 1, kFlags, kCount) == 0);
    CHECK(ps_read32(b, kCount) == kTrophies);
    CHECK(ps_read32(b, kFlags + 0) == 0x0000FFFDu); // 0 and 2 to 15
    CHECK(ps_read32(b, kFlags + 4) == 0);
    CHECK(ps_read32(b, kFlags + 8) == 0);
    CHECK(ps_read32(b, kFlags + 12) == 0x80000000u); // 127

    // The game icon: its size, and the file when the buffer is large enough; the system language's icon first.
    const std::string_view game_icon = "\x89PNG many";
    const u32 game_icon_size = static_cast<u32>(game_icon.size());
    ps_write32(b, kSize, 0);
    REQUIRE(a.call("sceNpTrophyGetGameIcon", 1, 1, 0, kSize) == 0);
    CHECK(ps_read32(b, kSize) == game_icon_size);
    std::memset(b + kIcon, 0xAA, kIconSpace);
    ps_write32(b, kSize, game_icon_size - 1); // too small: only the size
    REQUIRE(a.call("sceNpTrophyGetGameIcon", 1, 1, kIcon, kSize) == 0);
    CHECK(ps_read32(b, kSize) == game_icon_size);
    CHECK(all_bytes(b, kIcon, kIconSpace, 0xAA));
    ps_write32(b, kSize, kIconSpace);
    REQUIRE(a.call("sceNpTrophyGetGameIcon", 1, 1, kIcon, kSize) == 0);
    CHECK(ps_read32(b, kSize) == game_icon_size);
    CHECK(guest_bytes(b, kIcon, game_icon_size) == game_icon);
    CHECK(all_bytes(b, kIcon + game_icon_size, kIconSpace - game_icon_size, 0xAA));
    ps_write32(b, kSize, kIconSpace);
    CHECK(a.call("sceNpTrophyGetGameIcon", 1, 1, kUnmapped, kSize) == kInvalArg); // RPCS3 faults
    CHECK(ps_read32(b, kSize) == kIconSpace);                                     // nothing written
    CHECK(a.call("sceNpTrophyGetGameIcon", 1, 1, kIcon, kUnmapped) == kInvalArg);
    TrophyDirs::write(installed / "ICON0_01.PNG", {'E', 'N'});
    REQUIRE(a.call("sceNpTrophyGetGameIcon", 1, 1, kIcon, kSize) == 0);
    CHECK(ps_read32(b, kSize) == 2);
    CHECK(guest_bytes(b, kIcon, 2) == "EN");
    ps_write32(b, kSize, 0);
    REQUIRE(a.call("sceNpTrophyGetGameIcon", 2, 1, 0, kSize) == 0); // a read-only context: no registration needed
    CHECK(ps_read32(b, kSize) == 2);

    // Trophy icons, of unlocked trophies only: a locked hidden trophy is HIDDEN, any other locked one LOCKED.
    for (const s32 id : {136, -1}) CHECK(a.call("sceNpTrophyGetTrophyIcon", 1, 1, id, 0, kSize) == kInvalidTrophyId);
    CHECK(a.call("sceNpTrophyGetTrophyIcon", 1, 1, 1, 0, kSize) == kHidden);
    CHECK(a.call("sceNpTrophyGetTrophyIcon", 1, 1, 16, 0, kSize) == kLocked);
    const std::string icon_127 = "\x89PNG icon 127";
    ps_write32(b, kSize, kIconSpace);
    REQUIRE(a.call("sceNpTrophyGetTrophyIcon", 1, 1, 127, kIcon, kSize) == 0);
    CHECK(ps_read32(b, kSize) == icon_127.size());
    CHECK(guest_bytes(b, kIcon, static_cast<u32>(icon_127.size())) == icon_127);
    CHECK(a.call("sceNpTrophyGetTrophyIcon", 1, 1, 3, 0, kSize) == kUnknownFile); // unlocked; the pack has no icon
    CHECK(a.call("sceNpTrophyGetTrophyIcon", 1, 1, 127, kUnmapped, kSize) == kInvalArg);
    unlock(1);
    REQUIRE(a.call("sceNpTrophyGetTrophyIcon", 1, 1, 1, 0, kSize) == 0); // the hidden one, now unlocked
    CHECK(ps_read32(b, kSize) == std::string("\x89PNG icon 1").size());

    // A read-only context reads the installed set afresh: what the registered one saved. It has no progress or trophy
    // icons (only a registered context does).
    std::memset(b + kFlags, 0xAA, 16);
    REQUIRE(a.call("sceNpTrophyGetTrophyUnlockState", 2, 1, kFlags, kCount) == 0);
    CHECK(ps_read32(b, kCount) == kTrophies);
    CHECK(ps_read32(b, kFlags + 0) == 0x0000FFFFu);
    CHECK(ps_read32(b, kFlags + 12) == 0x80000000u);
    CHECK(a.call("sceNpTrophyGetGameProgress", 2, 1, kPercent) == kNotRegistered);
    CHECK(a.call("sceNpTrophyGetTrophyIcon", 2, 1, 0, 0, kSize) == kNotRegistered);
    CHECK(a.call("sceNpTrophySetSoundLevel", 2, 1, 20, 0) == 0);
    REQUIRE(a.call("sceNpTrophyTerm") == 0);
}
