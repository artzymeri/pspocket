// cellSail's built-in "x-cell-fs" file source (D-901) against libsail's own, from the user's installed system software
// (firmware_compare.hpp; skipped without it): the same script in two fresh processes (libsail loaded in both), one
// calling libsail's exports and one pspocket's. The source's callbacks are called through the descriptors the source
// itself holds, so one side calls libsail's code and the other pspocket's. The file read is synthetic, in a temporary
// /dev_hdd0: a PAMF header followed by 2.5 MB of a pattern. Compared: the answers, the script's memory (the player and
// its source, the game's file calls' log, the messages the source posted, the data its items delivered), what the
// player's allocator gave out (its block, the read-ahead buffers with what was read into them) and the lv2 calls each
// thread makes in order. One test runs without the firmware: pspocket's reader alone, checked against the file. No game
// data.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "firmware_compare.hpp"
#include "guest.hpp" // runtime/hle/src (internal): atomic_load_u8
#include "hle_test_util.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

namespace fsys = std::filesystem;

// The script's memory (compared), and the player's allocator's (from kHeap up, compared to where it got to).
constexpr u32 kArea = kData + 0x1000, kAreaSize = 0x8000;
constexpr u32 kMem = kArea, kHeapPtr = kArea + 0x10, kFailAlloc = kArea + 0x14, kAnswers = kArea + 0x20,
              kCalls = kArea + 0x40, kCmd = kArea + 0x80, kOut = kArea + 0x100, kMedia = kArea + 0x200,
              kUriMovie = kArea + 0x300, kUriSmall = kArea + 0x380, kUriNone = kArea + 0x400, kProto = kArea + 0x480,
              kRes = kArea + 0x500, kAttr = kArea + 0x520, kInquiry = kArea + 0x560, kP = kArea + 0x800,
              kCode = kArea + 0xC00, kRecords = kArea + 0x1400, kFreeLog = kArea + 0x1800, kSums = kArea + 0x1A00,
              kMsgLog = kArea + 0x1C00, kFileLog = kArea + 0x2C00, kBuf = kArea + 0x5000;
constexpr u32 kHeap = 0x00800000, kHeapSize = 0x01000000;
constexpr u32 kMovieSize = 0x280000, kSmallSize = 0x600;

// Guest code, one 0x80-byte slot each with its descriptor at +0x70.
constexpr u32 slot(u32 i) { return kCode + 0x80 * i; }
constexpr u32 opd(u32 i) { return slot(i) + 0x70; }
enum : u32 { kHandler = 0, kAllocSlot = 1, kFreeSlot = 2, kRecorder = 3 }; // recorders: kRecorder + id - 1, ids 1-7

u32 hi(u32 v) { return v >> 16; }
// What the game's file call `id` answers from now on (0: lv2's answer).
void answer(u8* b, u32 id, u32 v) { ps_write32(b, kAnswers + 4 * id, v); }
u32 lo(u32 v) { return v & 0xFFFF; }

void put_code(u8* b, u32 i, std::initializer_list<u32> words)
{
    u32 at = slot(i);
    for (const u32 w : words) {
        ps_write32(b, at, w);
        at += 4;
    }
    REQUIRE(at <= opd(i));
    ps_write32(b, opd(i), slot(i));
    ps_write32(b, opd(i) + 4, 0);
}

// The event handler: records (r3..r7) at kRecords (a count, then 48 bytes each from +16) and returns 0.
// The player's allocator: alloc(arg, boundary, size) bumps from kHeap (0 while kFailAlloc is set); free(arg, boundary,
// ptr) logs {boundary, ptr} at kFreeLog (a count, then 8 bytes each from +8).
// The game's file calls (player parameter 33), ids 1-7 (open, after open, close, fstat, read, lseek, cancel): each logs
// {id, r3..r7} at kFileLog (a count, then 48 bytes each from +16), then answers its word at kAnswers (4 bytes an id)
// when it is not 0, else makes lv2's system call for it (open 801, close 804, fstat 809, read 802, lseek 818; the call
// after open and cancel answer 0).
void write_code(u8* b)
{
    put_code(b, kHandler, {0x3D600000u | hi(kRecords), 0x616B0000u | lo(kRecords), 0x818B0000u, 0x1C0C0030u,
        0x7D2B0214u, 0xF8690010u, 0xF8890018u, 0xF8A90020u, 0xF8C90028u, 0xF8E90030u, 0x7C2004ACu, 0x398C0001u,
        0x918B0000u, 0x38600000u, 0x4E800020u});
    put_code(b, kAllocSlot, {0x3D600000u | hi(kFailAlloc), 0x616B0000u | lo(kFailAlloc), 0x818B0000u, 0x2C0C0000u,
        0x4182000Cu, 0x38600000u, 0x4E800020u, 0x3D600000u | hi(kHeapPtr), 0x616B0000u | lo(kHeapPtr), 0x806B0000u,
        0x3804FFFFu, 0x7C630214u, 0x7C630078u, 0x7D832A14u, 0x918B0000u, 0x4E800020u});
    put_code(b, kFreeSlot, {0x3D600000u | hi(kFreeLog), 0x616B0000u | lo(kFreeLog), 0x818B0000u, 0x55801838u,
        0x7D2B0214u, 0x90890008u, 0x90A9000Cu, 0x398C0001u, 0x918B0000u, 0x38600000u, 0x4E800020u});
    const u32 calls[8] = {0, 801, 0, 804, 809, 802, 818, 0};
    for (u32 id = 1; id <= 7; id++) {
        std::vector<u32> w = {0x3D600000u | hi(kFileLog), 0x616B0000u | lo(kFileLog), 0x818B0000u, 0x1C0C0030u,
            0x7D2B0214u, 0x38000000u | id, 0xF8090010u, 0xF8690018u, 0xF8890020u, 0xF8A90028u, 0xF8C90030u,
            0xF8E90038u, 0x398C0001u, 0x918B0000u};
        // lis/ori r11 = kAnswers; lwz r12, 4 * id(r11); cmpwi r12,0; beq +12; mr r3,r12; blr; li r8,0; li r11,N; sc;
        // blr (or lwz r3, 4 * id(r11); blr)
        w.push_back(0x3D600000u | hi(kAnswers));
        w.push_back(0x616B0000u | lo(kAnswers));
        if (calls[id]) {
            for (const u32 x : {0x818B0000u | (4 * id), 0x2C0C0000u, 0x4182000Cu, 0x7D836378u, 0x4E800020u, 0x39000000u,
                     0x39600000u | calls[id], 0x44000002u, 0x4E800020u})
                w.push_back(x);
        } else {
            w.push_back(0x806B0000u | (4 * id));
            w.push_back(0x4E800020u);
        }
        u32 at = slot(kRecorder + id - 1);
        for (const u32 x : w) {
            ps_write32(b, at, x);
            at += 4;
        }
        REQUIRE(at <= opd(kRecorder + id - 1));
        ps_write32(b, opd(kRecorder + id - 1), slot(kRecorder + id - 1));
        ps_write32(b, opd(kRecorder + id - 1) + 4, 0);
    }
    ps_write32(b, kHeapPtr, kHeap);
    ps_write32(b, kMem, opd(kAllocSlot)); // CellSailMemAllocator: alloc, free, arg
    ps_write32(b, kMem + 4, opd(kFreeSlot));
    ps_write32(b, kMem + 8, 0x4321);
    // The game's file calls: {0, open, after open, close, fstat, read, lseek, cancel, 0, 0}.
    ps_write32(b, kCalls, 0);
    for (u32 id = 1; id <= 7; id++) ps_write32(b, kCalls + 4 * id, opd(kRecorder + id - 1));
    const auto str = [&](u32 at, const char* text) { std::memcpy(b + at, text, std::strlen(text) + 1); };
    str(kUriMovie, "x-cell-fs:///dev_hdd0/sail/movie.pam");
    str(kUriSmall, "x-cell-fs:///dev_hdd0/sail/small.pam");
    str(kUriNone, "x-cell-fs:///dev_hdd0/sail/none.pam");
    str(kProto, "x-cell-fs");
    for (u32 i = 0; i < 0x40; i++) b[kMedia + i] = static_cast<u8>(0xA0 + i);
}

// The movie: a PAMF header of two 2 KB units (test_sail.cpp's write_sail_pamf) with one AVC and one ATRAC3plus stream,
// then a pattern; the small file is its first 0x600 bytes.
std::vector<u8> movie_bytes()
{
    std::vector<u8> v(kMovieSize);
    for (u32 i = 0; i < kMovieSize; i++) v[i] = static_cast<u8>((i * 2654435761u) >> 13 ^ i >> 9);
    u8* h = v.data();
    std::memset(h, 0, 0x1000);
    std::memcpy(h, "PAMF0041", 8);
    ps_write32(h, 0x08, 2);
    ps_write32(h, 0x0C, 0x10);
    ps_write32(h, 0x50, 0x94);
    ps_write32(h, 0x58, 0x1000);
    ps_write32(h, 0x5E, 0x100000);
    ps_write32(h, 0x62, 0xFF123456);
    ps_write32(h, 0x6A, 2);
    ps_write8(h, 0x6F, 1);
    ps_write32(h, 0x70, 0x74);
    ps_write32(h, 0x76, 0x1000);
    ps_write32(h, 0x7C, 0x100000);
    ps_write8(h, 0x81, 1);
    ps_write32(h, 0x82, 0x62);
    ps_write8(h, 0x87, 2);
    ps_write8(h, 0x88, 0x1b);
    ps_write8(h, 0x8C, 0xe0);
    ps_write32(h, 0x90, 0xE8);
    ps_write32(h, 0x94, 3);
    const u8 avc_info[26] = {100, 41, 0xC4, 0xff, 0, 16, 0, 9, 0, 80, 0, 45, 0, 0, 0, 0, 0, 0, 0, 8, 0xB0, 1, 1, 1,
        0xE2, 0x30};
    std::memcpy(h + 0x98, avc_info, sizeof avc_info);
    ps_write8(h, 0xB8, 0xdc);
    ps_write8(h, 0xBC, 0xbd);
    ps_write32(h, 0xC0, 0x10C);
    ps_write32(h, 0xC4, 1);
    ps_write8(h, 0xCA, 2);
    ps_write8(h, 0xCB, 1);
    const u32 eps[3][3] = {{(1u << 14) | 5, 0x1000, 0}, {(1u << 14) | 6, 0x5000, 4}, {(2u << 14) | 7, 0x9000, 8}};
    for (u32 i = 0; i < 3; i++) {
        ps_write16(h, 0xE8 + 12 * i, static_cast<u16>(eps[i][0]));
        ps_write32(h, 0xE8 + 12 * i + 4, eps[i][1]);
        ps_write32(h, 0xE8 + 12 * i + 8, eps[i][2]);
    }
    ps_write32(h, 0x10C + 4, 0x1000);
    return v;
}

// /dev_hdd0 for every run: sail/movie.pam and sail/small.pam.
struct Tree {
    fsys::path root = fsys::temp_directory_path() / std::format("pspocket-sail-source-{}", ::getpid());
    Tree()
    {
        fsys::remove_all(root);
        fsys::create_directories(root / "hdd0" / "sail");
        const std::vector<u8> movie = movie_bytes();
        std::ofstream(root / "hdd0" / "sail" / "movie.pam", std::ios::binary)
            .write(reinterpret_cast<const char*>(movie.data()), static_cast<std::streamsize>(movie.size()));
        std::ofstream(root / "hdd0" / "sail" / "small.pam", std::ios::binary)
            .write(reinterpret_cast<const char*>(movie.data()), kSmallSize);
    }
    ~Tree()
    {
        std::error_code ec;
        fsys::remove_all(root, ec);
    }
};

// The lv2 calls, each thread's in order (by lv2 thread ID: the host reuses threads), with the arguments that are the
// same in both runs. Left out: sys_process_getpid (1) and the access control engine (871), which pspocket asks at each
// module load where libsail's module_start asked once before the script; the control thread's 3 s waits for a message
// (how many depends on when the script posts).
struct Trace {
    std::mutex m;
    std::thread::id main;
    std::vector<std::string> calls;
    std::map<u32, std::vector<std::string>> others;
};

u8* g_base = nullptr;
bool g_caller_stacks_differ = false; // the file calls' stack variables are on a caller's stack that differs
void observe(void* user, u32 n, const u64* a)
{
    std::string s;
    const auto attr = [&](u64 at, u32 words) {
        std::string w;
        for (u32 i = 0; i < words; i++) w += std::format(" {:x}", ps_read32(g_base, static_cast<u32>(at) + 4 * i));
        return w;
    };
    const auto var = [](u64 at) { return g_caller_stacks_differ ? std::string("(stack)") : std::format("{:#x}", at); };
    switch (n) {
    case 1: case 871: return;
    case 100: s = std::format("mutex_create({:#x},{})", a[0], attr(a[1], 10)); break;
    case 105: s = std::format("cond_create({:#x}, {:#x},{})", a[0], a[1], attr(a[2], 6)); break;
    case 107:
        if (a[1] == 3000000) return;
        s = std::format("cond_wait({:#x}, {})", a[0], a[1]);
        break;
    case 102: s = std::format("mutex_lock({:#x}, {})", a[0], a[1]); break;
    case 101: case 104: case 106: case 108: case 109: s = std::format("{}({:#x})", n, a[0]); break;
    case 110: s = std::format("cond_signal_to({:#x}, {:#x})", a[0], a[1]); break;
    case 44: s = std::format("join({:#x})", a[0]); break; // a[1]: the caller's stack
    case 801: s = std::format("fs_open({:#x}, {:#x}, {})", a[0], a[1], var(a[2])); break;
    case 802: s = std::format("fs_read({}, {:#x}, {:#x}, {})", a[0], a[1], a[2], var(a[3])); break;
    case 804: s = std::format("fs_close({})", a[0]); break;
    case 809: s = std::format("fs_fstat({}, {})", a[0], var(a[1])); break;
    case 818: s = std::format("fs_lseek({}, {:#x}, {}, {})", a[0], a[1], a[2], var(a[3])); break;
    default: s = std::format("sc{}", n); break;
    }
    Trace& tr = *static_cast<Trace*>(user);
    const std::lock_guard lock(tr.m);
    if (std::this_thread::get_id() == tr.main) tr.calls.push_back(s);
    else if (const lv2::PpuThread* th = lv2::current_thread()) tr.others[th->id].push_back(s);
    else tr.others[0].push_back(s);
}

struct Run {
    std::vector<std::string> lines;
    std::vector<u8> mem, heap;
    std::vector<std::string> calls;
};

// What a script gets: the memory, the exports (by name, or "cellSail_NID"), the built-in source's callbacks (by their
// CellSailSourceFuncs index), and the waits.
struct Script {
    u8* b;
    std::function<u64(const char*, std::initializer_list<u64>, bool)> call;
    std::vector<std::string>* lines;
    GuestThread* t;
    std::vector<std::pair<u32, u32>> mask; // ranges of memory each side fills with its own code or stack addresses

    u32 source() const { return ps_read32(b, kP + 0x3B0); }

    enum Func : u32 {
        Makeup, Cleanup, Open, Close, Start, Stop, Cancel, Checkout, Checkin, Clear, Read, ReadSync, GetCapabilities,
        InquireCapability,
    };
    u64 src(const char* name, Func f, std::initializer_list<u64> args, bool is_void = false)
    {
        const u32 s = source();
        std::vector<u64> all = {s};
        all.insert(all.end(), args.begin(), args.end());
        u64 v = 0;
        const u32 fn = ps_read32(b, s + 0x48 + 4 * f);
        switch (all.size()) {
        case 1: v = call_guest(t->ctx, fn, {all[0]}); break;
        case 2: v = call_guest(t->ctx, fn, {all[0], all[1]}); break;
        case 3: v = call_guest(t->ctx, fn, {all[0], all[1], all[2]}); break;
        case 5: v = call_guest(t->ctx, fn, {all[0], all[1], all[2], all[3], all[4]}); break;
        case 8: v = call_guest(t->ctx, fn, {all[0], all[1], all[2], all[3], all[4], all[5], all[6], all[7]}); break;
        default: FAIL("arguments");
        }
        lines->push_back(is_void ? std::format("{} -> (void)", name) : std::format("{} -> {:#x}", name, static_cast<u32>(v)));
        return v;
    }

    // Waits until the player's queue holds `n` messages, logs them at kMsgLog (a count, then 16 bytes each from +16)
    // and empties it (no one else reads the queue of a player that has not booted), under the queue's mutex as a reader
    // of it would (lv2's own call, so the trace does not see it).
    void messages(u32 n)
    {
        const u32 mutex = ps_read32(b, kP + 0x184);
        const auto count = [&] {
            lv2::sys_mutex_lock(*t->t, mutex, 0);
            const u32 c = ps_read32(b, kP + 0x174);
            lv2::sys_mutex_unlock(*t->t, mutex);
            return c;
        };
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (count() < n && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        lv2::sys_mutex_lock(*t->t, mutex, 0);
        const u32 have = ps_read32(b, kP + 0x174);
        const u32 cap = ps_read32(b, kP + 0x170), ring = ps_read32(b, kP + 0x178), w = ps_read32(b, kP + 0x17C);
        for (u32 i = 0; i < have; i++) {
            const u32 logged = ps_read32(b, kMsgLog);
            std::memcpy(b + kMsgLog + 16 + 16 * logged, b + ring + 16 * ((w + cap - have + i) % cap), 16);
            ps_write32(b, kMsgLog, logged + 1);
        }
        ps_write32(b, kP + 0x174, 0);
        lv2::sys_mutex_unlock(*t->t, mutex);
        CHECK(have == n);
        lines->push_back(std::format("messages {}", have));
    }

    // The item a Checkout gave at `at`: {its address, size, session, a hash of its data} logged at kSums (a count,
    // then 16 bytes each from +16).
    void sum(u32 at)
    {
        const u32 item = ps_read32(b, at), buf = ps_read32(b, item), size = ps_read32(b, item + 4);
        u32 h = 2166136261u;
        for (u32 i = 0; i < size; i++) h = (h ^ b[buf + i]) * 16777619u;
        const u32 count = ps_read32(b, kSums), e = kSums + 16 + 16 * count;
        ps_write32(b, e, item);
        ps_write32(b, e + 4, size);
        ps_write32(b, e + 8, ps_read32(b, item + 8));
        ps_write32(b, e + 12, h);
        ps_write32(b, kSums, count + 1);
    }

    void until(const std::function<bool()>& done)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done() && std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(done());
    }

    void start_command(u64 flags, u64 from, u64 length)
    {
        ps_write64(b, kCmd, flags);
        ps_write64(b, kCmd + 8, from);
        ps_write64(b, kCmd + 16, length);
        ps_write64(b, kCmd + 24, 0);
        ps_write64(b, kCmd + 32, 0);
    }

    // The source's callbacks (each side's own code), and its file calls while they are libsail's.
    void mask_source(bool own_calls)
    {
        mask.push_back({source() + 0x48, 0x38});
        if (own_calls) mask.push_back({source() + 0x100, 0x38});
    }
    // A booted player: the control thread's stack (its frame, the deepest its handler went); the handler's r7 (past its
    // four arguments); the commands left in the queue's ring (their reply records and arguments on the caller's stack).
    void mask_booted()
    {
        mask.push_back({kP + 0x3C8, 16});
        for (u32 i = 0; i < ps_read32(b, kRecords); i++) mask.push_back({kRecords + 16 + 48 * i + 32, 8});
        const u32 ring = ps_read32(b, kP + 0x178);
        for (u32 i = 0; i < ps_read32(b, kP + 0x170); i++) {
            const u32 e = ring + 16 * i, c = ps_read32(b, e);
            if (c == 0x40 || c == 0x3C || c == 0x4C || c == 0x5D) mask.push_back({e + 8, 4});
            if (c != 0) mask.push_back({e + 12, 4}); // a completion's handle is the player's
        }
    }
};

void script_open(Script& sc, bool games_calls);
void script_reader(Script& sc);
void script_reader_games_calls(Script& sc);
void script_reader_failures(Script& sc);
void script_stream(Script& sc, bool games_calls);

// `flash` empty: pspocket's side alone, without libsail loaded (the test without firmware).
Run run(const std::optional<std::string>& flash, const Tree& tree, bool sony, int script)
{
    Fixture f;
    REQUIRE(f.hle->main_memory().reserve(kHeap, kHeapSize));
    REQUIRE(f.mem.commit(kHeap, kHeapSize));
    REQUIRE(f.kernel->vfs_mount("/dev_hdd0", (tree.root / "hdd0").string(), false));
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread t(f, 0, "ppu");
    std::optional<FirmwareLibrary> sail;
    if (flash) sail.emplace(f, t, *flash, "cellSail");
    u8* b = f.base();
    std::memset(b + kArea, 0, kAreaSize);
    write_code(b);
    Run r;
    Trace tr;
    g_base = b;
    tr.main = std::this_thread::get_id();
    f.kernel->set_syscall_observer(&observe, &tr);
    Script sc{b, {}, &r.lines, &t, {}};
    sc.call = [&](const char* name, std::initializer_list<u64> args, bool is_void) {
        u64 v;
        const std::string_view n(name);
        const u32 nid = n.starts_with("cellSail_") ? static_cast<u32>(std::stoul(std::string(n.substr(9)), nullptr, 16))
                                                   : recomp::nid_of(name);
        if (sony) {
            v = call_guest(t.ctx, sail->module->functions.at({"cellSail", nid}), args);
        } else {
            const Function* fn = registry().find("cellSail", nid);
            REQUIRE(fn);
            u32 i = 0;
            for (const u64 a : args) {
                if (i < 8) t.ctx.gpr[3 + i] = a;
                else ps_write64(b, static_cast<u32>(t.ctx.gpr[1]) + 0x70 + 8 * (i - 8), a);
                i++;
            }
            fn->fn(t.ctx, b);
            v = t.ctx.gpr[3];
        }
        r.lines.push_back(is_void ? std::format("{} -> (void)", name) : std::format("{} -> {:#x}", name, v));
        return v;
    };
    if (script == 1) script_open(sc, false);
    else if (script == 2) script_open(sc, true);
    else if (script == 3) script_reader(sc);
    else if (script == 4) script_reader_games_calls(sc);
    else if (script == 5) script_reader_failures(sc);
    else script_stream(sc, script == 7);
    f.kernel->set_syscall_observer(nullptr, nullptr);
    // The game's file calls' log: the registers past a call's arguments hold what the caller's code last left there.
    for (u32 i = 0; i < ps_read32(b, kFileLog); i++) {
        const u32 e = kFileLog + 16 + 48 * i;
        constexpr u32 kArgs[8] = {0, 5, 5, 1, 2, 4, 4, 1};
        const u32 id = static_cast<u32>(ps_read64(b, e));
        REQUIRE((id >= 1 && id <= 7));
        sc.mask.push_back({e + 8 + 8 * kArgs[id], 8 * (5 - kArgs[id])});
    }
    for (const auto& [at, n] : sc.mask) std::memset(b + at, 0x5A, n);
    r.mem.assign(b + kArea, b + kArea + kAreaSize);
    r.heap.assign(b + kHeap, b + ps_read32(b, kHeapPtr));
    r.calls = tr.calls;
    std::vector<std::string> others;
    for (const auto& [id, calls] : tr.others) {
        std::string all = "thread:";
        for (const std::string& c : calls) all += " " + c;
        others.push_back(all);
    }
    std::sort(others.begin(), others.end());
    r.calls.insert(r.calls.end(), others.begin(), others.end());
    return r;
}

// A player booted without streams (on the game's SPURS, never used without streams): PAMF descriptors opened through
// the built-in source (its makeup on the first, ReadSync for the header, cleanup on the last close, by the game's
// thread or the control thread), a file that does not exist, a file shorter than the header, the capabilities.
// `games_calls`: through the game's file calls (parameter 33), one of them failing the call after open.
void script_open(Script& sc, bool games_calls)
{
    u8* b = sc.b;
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return sc.call(n, a, v); };
    ps_write32(b, kRes, 0x00ABCDEF);
    ps_write32(b, kAttr + 16, 0);
    call("cellSailPlayerInitialize2", {kP, kMem, opd(kHandler), 0x33, kAttr, kRes});
    // With the game's file calls, what libsail hands them on the caller's stack is on libsail's DescriptorOpen's stack
    // on one side and pspocket's on the other.
    g_caller_stacks_differ = games_calls;
    if (games_calls) {
        call("cellSailPlayerSetParameter", {kP, 33, kCalls, 0x28});
        ps_write64(b, kOut + 0xF8, 0x28);
        call("cellSailPlayerGetParameter", {kP, 33, kOut + 0xC0, kOut + 0xF8});
    }
    call("cellSailPlayerBoot", {kP, 0x55});
    sc.until([&] { return atomic_load_u8(b, kP + 0x163) == 2; });

    // The file is not there: makeup, ReadSync's error, cleanup.
    call("cellSailPlayerCreateDescriptor", {kP, 0, kMedia, kUriNone, kOut});
    call("cellSailDescriptorOpen", {ps_read32(b, kOut)});
    // The movie: makeup again, its header read twice.
    call("cellSailPlayerCreateDescriptor", {kP, 0, kMedia, kUriMovie, kOut + 4});
    const u32 d0 = ps_read32(b, kOut + 4);
    if (games_calls) answer(b, 2, 0x80010002); // the call after open fails: the file stays open
    call("cellSailDescriptorOpen", {d0});
    answer(b, 2, 0);
    call("cellSailDescriptorOpen", {d0});
    call("cellSailDescriptorGetCapabilities", {d0, kOut + 0x10, kOut + 0x14, kOut + 0x18, kOut + 0x1C});
    for (u32 i = 0; i < 4; i++) {
        std::memset(b + kInquiry, 0, 0x28);
        const u32 q[4][3] = {{0, 1, 2}, {1, 0, 0}, {4, 1, 2}, {0, 2, 1}};
        ps_write32(b, kInquiry, q[i][0]);
        ps_write32(b, kInquiry + 4, q[i][1]);
        ps_write32(b, kInquiry + 8, q[i][2]);
        ps_write32(b, kInquiry + 16, q[i][0] == 4 ? 1 : 0);
        ps_write64(b, kInquiry + 0x18, i * 0x800);
        call("cellSailDescriptorInquireCapability", {d0, kInquiry});
    }
    // A second descriptor of it: the source shared, no makeup.
    call("cellSailPlayerCreateDescriptor", {kP, 0, kMedia, kUriMovie, kOut + 8});
    const u32 d1 = ps_read32(b, kOut + 8);
    call("cellSailDescriptorOpen", {d1});
    // Shorter than its header.
    call("cellSailPlayerCreateDescriptor", {kP, 0, 0, kUriSmall, kOut + 12});
    call("cellSailDescriptorOpen", {ps_read32(b, kOut + 12)});
    // The last close cleans up; an open after it makes the source up again; the player's end closes it on the control
    // thread.
    call("cellSailDescriptorClose", {d1});
    call("cellSailDescriptorClose", {d0});
    call("cellSailDescriptorOpen", {d0});
    call("cellSailDescriptorGetCapabilities", {d0, kOut + 0x20, kOut + 0x24, kOut + 0x28, kOut + 0x2C});
    call("cellSailPlayerFinalize", {kP});
    sc.mask_source(!games_calls);
    sc.mask_booted();
    g_caller_stacks_differ = false;
    if (games_calls) {
        // Their stack variables (the descriptor, stat buffer, read count and seek position libsail hands them).
        for (u32 i = 0; i < ps_read32(b, kFileLog); i++) {
            const u32 e = kFileLog + 16 + 48 * i;
            const u32 id = static_cast<u32>(ps_read64(b, e));
            if (id == 1) sc.mask.push_back({e + 24, 8});
            if (id == 4) sc.mask.push_back({e + 16, 8});
            if (id == 5 || id == 6) sc.mask.push_back({e + 32, 8});
        }
    }
}

// The reader called directly on a player that has not booted (its notifications queue up in the player's ring): the
// synchronous calls, then Makeup, Read kept open for Open, Start, items checked out and in across 1 MB chunks to the
// file's end, Stop, Clear, a range, reads in the open state, refusals, Close, the closed state's answers, a file that
// is not there, the buffer not given, Cleanup while started, Makeup again (the descriptor Cleanup left), Close.
void script_reader(Script& sc)
{
    u8* b = sc.b;
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return sc.call(n, a, v); };
    call("cellSailPlayerInitialize", {kP, 0, kMem, opd(kHandler), 0x11, 0, 0, 0, 0});
    const u32 s = sc.source();
    ps_write32(b, s + 0x40, kP); // the player using the source, as the control thread sets it before Makeup

    // At once, on the caller's thread.
    sc.src("GetCapabilities", Script::GetCapabilities, {0, kMedia, kUriMovie, kOut});
    sc.src("GetCapabilities", Script::GetCapabilities, {2, 0, 0, kOut + 8});
    for (const u64 flags : {1, 0, 0x10, 2}) {
        sc.start_command(flags, 0, 0);
        sc.src("InquireCapability", Script::InquireCapability, {0, 0, kUriMovie, kCmd});
    }
    sc.src("ReadSync", Script::ReadSync, {0, kMedia, kUriMovie, 0, kBuf, 0x800, kOut + 0x10});
    sc.src("ReadSync", Script::ReadSync, {0, 0, kUriMovie, 0x1000, kBuf + 0x800, 0x100, 0});
    sc.src("ReadSync", Script::ReadSync, {0, 0, kUriSmall, 0, kBuf + 0x900, 0x800, kOut + 0x18});
    sc.src("ReadSync", Script::ReadSync, {0, 0, kUriNone, 0, kBuf + 0x1100, 0x10, kOut + 0x20});
    sc.src("ReadSync", Script::ReadSync, {0, 0, kUriMovie, 0x300000, kBuf + 0x1100, 0x40, kOut + 0x28});

    sc.src("Makeup", Script::Makeup, {kProto});
    sc.src("Checkout", Script::Checkout, {kOut + 0x30});
    // Read in the closed state: the file stays open, its position taken as the size read (0x80, not 0x90).
    sc.src("Read", Script::Read, {0, kMedia, kUriMovie, 0x10, kBuf + 0x1200, 0x80, kOut + 0x38});
    sc.messages(1);
    sc.src("Open", Script::Open, {0, kMedia, kUriMovie, 0}, true); // the same path: that file
    sc.messages(1);
    // From 0x80, which the reader takes for where it is: the items start at the file's 0x90.
    sc.start_command(1, 0x80, ~u64{0});
    sc.src("Start", Script::Start, {kCmd, 0x55}, true);
    sc.messages(3);
    for (const u32 at : {kOut + 0x40, kOut + 0x44, kOut + 0x48}) {
        sc.src("Checkout", Script::Checkout, {at});
        sc.sum(at);
    }
    sc.src("Checkin", Script::Checkin, {ps_read32(b, kOut + 0x40)});
    sc.messages(1);
    sc.src("Checkout", Script::Checkout, {kOut + 0x4C});
    sc.sum(kOut + 0x4C);
    sc.src("Checkin", Script::Checkin, {ps_read32(b, kOut + 0x44)}); // the file's end
    sc.messages(1);
    sc.src("Stop", Script::Stop, {}, true); // open: answered 1
    sc.messages(1);
    sc.src("Clear", Script::Clear, {});
    sc.start_command(1, 0x1000, 0x1800); // a range: one item, then its end
    sc.src("Start", Script::Start, {kCmd, 0x66}, true);
    sc.messages(3);
    sc.src("Checkout", Script::Checkout, {kOut + 0x50});
    sc.sum(kOut + 0x50);
    sc.src("Read", Script::Read, {0, 0, kUriSmall, 0, kBuf + 0x1300, 0x800, kOut + 0x58});
    sc.messages(1);
    sc.src("Read", Script::Read, {0, 0, kUriNone, 0, kBuf + 0x1B00, 0x10, 0});
    sc.messages(1);
    sc.start_command(2, 0, 0);
    sc.src("Start", Script::Start, {kCmd, 0x67}, true); // not from a byte position
    sc.messages(1);
    sc.src("Clear", Script::Clear, {});
    sc.start_command(1, 0, ~u64{0});
    sc.src("Start", Script::Start, {kCmd, 0x77}, true);
    sc.messages(3);
    sc.start_command(1, 0x100, ~u64{0});
    sc.src("Start", Script::Start, {kCmd, 0x78}, true); // started: a seek, no read (both items are ready)
    sc.messages(1);
    sc.src("Read", Script::Read, {0, kMedia, kUriMovie, 0x30, kBuf + 0x1C00, 0x40, kOut + 0x60}); // started: not kept
    sc.messages(1);
    sc.src("Stop", Script::Stop, {}, true); // started: back to open
    sc.messages(1);
    sc.src("Cancel", Script::Cancel, {}, true);
    sc.src("Close", Script::Close, {}, true);
    sc.messages(1);
    sc.src("Close", Script::Close, {}, true); // closed: answered 1
    sc.messages(1);
    sc.src("Stop", Script::Stop, {}, true); // closed: no answer
    sc.start_command(1, 0, 0);
    sc.src("Start", Script::Start, {kCmd, 0x88}, true); // closed: no answer
    sc.src("Close", Script::Close, {}, true);
    sc.messages(1);
    sc.src("Open", Script::Open, {0, 0, kUriNone, 0}, true);
    sc.messages(1);
    sc.src("Close", Script::Close, {}, true);
    sc.messages(1);
    ps_write32(b, kFailAlloc, 1); // no buffer: the path cleared, the reader does nothing
    sc.src("Open", Script::Open, {0, 0, kUriMovie, 0}, true);
    sc.messages(1);
    ps_write32(b, kFailAlloc, 0);
    sc.src("Open", Script::Open, {0, 0, kUriMovie, 0}, true);
    sc.messages(1);
    sc.src("Clear", Script::Clear, {});
    sc.start_command(1, 0, ~u64{0});
    sc.src("Start", Script::Start, {kCmd, 0x99}, true);
    sc.messages(3);
    sc.src("Cleanup", Script::Cleanup, {});
    // Made up again: the descriptor of the file the last reader closed is still in the source.
    sc.src("Makeup", Script::Makeup, {kProto});
    sc.src("Open", Script::Open, {0, 0, kUriMovie, 0}, true);
    sc.messages(1);
    sc.src("Close", Script::Close, {}, true);
    sc.messages(1);
    sc.src("Cleanup", Script::Cleanup, {});
    call("cellSailPlayerFinalize", {kP});
    sc.mask_source(true);
}

// The reader through the game's file calls: ReadSync (with the call after open failing once), Open, Start, an item
// checked in, Stop, Close, Read kept open, a failing call after open (the file left open), Cleanup; libsail's calls
// back.
void script_reader_games_calls(Script& sc)
{
    u8* b = sc.b;
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return sc.call(n, a, v); };
    call("cellSailPlayerInitialize", {kP, 0, kMem, opd(kHandler), 0x11, 0, 0, 0, 0});
    call("cellSailPlayerSetParameter", {kP, 33, kCalls, 0x28});
    ps_write64(b, kOut + 0xF8, 0x28);
    call("cellSailPlayerGetParameter", {kP, 33, kOut + 0xC0, kOut + 0xF8});
    const u32 s = sc.source();
    ps_write32(b, s + 0x40, kP);

    sc.src("ReadSync", Script::ReadSync, {0, kMedia, kUriMovie, 0x20, kBuf, 0x100, kOut});
    answer(b, 2, 0x80010002);
    sc.src("ReadSync", Script::ReadSync, {0, kMedia, kUriMovie, 0, kBuf + 0x100, 0x10, 0});
    answer(b, 2, 0);
    sc.src("ReadSync", Script::ReadSync, {0, kMedia, kUriNone, 0, kBuf + 0x200, 0x10, kOut + 8});

    sc.src("Makeup", Script::Makeup, {kProto});
    sc.src("Open", Script::Open, {0, kMedia, kUriMovie, 0}, true);
    sc.messages(1);
    sc.start_command(1, 0, ~u64{0});
    sc.src("Start", Script::Start, {kCmd, 5}, true);
    sc.messages(3);
    sc.src("Checkout", Script::Checkout, {kOut + 0x10});
    sc.sum(kOut + 0x10);
    sc.src("Checkin", Script::Checkin, {ps_read32(b, kOut + 0x10)});
    sc.messages(1);
    sc.src("Stop", Script::Stop, {}, true);
    sc.messages(1);
    sc.src("Close", Script::Close, {}, true);
    sc.messages(1);
    sc.src("Read", Script::Read, {0, kMedia, kUriMovie, 0x40, kBuf + 0x300, 0x20, kOut + 0x18});
    sc.messages(1);
    answer(b, 2, 0x80010002);
    sc.src("Read", Script::Read, {0, kMedia, kUriMovie, 0x40, kBuf + 0x400, 0x20, kOut + 0x20});
    sc.messages(1);
    answer(b, 2, 0);
    sc.src("Cleanup", Script::Cleanup, {});
    call("cellSailPlayerSetParameter", {kP, 33, 0, 0});
    call("cellSailPlayerGetParameter", {kP, 33, kOut + 0x80, kOut + 0xF8});
    call("cellSailPlayerFinalize", {kP});
    sc.mask_source(true);
    sc.mask.push_back({kOut + 0x80, 0x28}); // parameter 33's copy of libsail's own calls
}

// The reader's failures through the game's file calls (their answers set by the script): ReadSync's seek, stat and read
// failing; the open file polled after a second and its medium found gone (fstat's EBADF), a Start without a file and
// the file opened again five seconds on; Start's seek failing while started and while open; a read's EIO with the
// medium gone; another read error, retried half a second on. Each wait for a timeout is a wait for the messages that
// follow it.
void script_reader_failures(Script& sc)
{
    u8* b = sc.b;
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return sc.call(n, a, v); };
    constexpr u32 kEinval = 0x80010002, kEio = 0x8001002B, kEbadf = 0x8001002A;
    call("cellSailPlayerInitialize", {kP, 0, kMem, opd(kHandler), 0x11, 0, 0, 0, 0});
    call("cellSailPlayerSetParameter", {kP, 33, kCalls, 0x28});
    const u32 s = sc.source();
    ps_write32(b, s + 0x40, kP);

    answer(b, 6, kEinval);
    sc.src("ReadSync", Script::ReadSync, {0, 0, kUriMovie, 0x20, kBuf, 0x10, kOut});
    answer(b, 6, 0);
    answer(b, 4, kEio);
    ps_write64(b, kOut + 8, 0x1111);
    sc.src("ReadSync", Script::ReadSync, {0, 0, kUriMovie, 0, kBuf + 0x10, 0x10, kOut + 8});
    answer(b, 4, 0);
    answer(b, 5, kEio);
    sc.src("ReadSync", Script::ReadSync, {0, 0, kUriMovie, 0, kBuf + 0x20, 0x10, kOut + 0x10});
    answer(b, 5, 0);

    sc.src("Makeup", Script::Makeup, {kProto});
    sc.src("Open", Script::Open, {0, kMedia, kUriMovie, 0}, true);
    sc.messages(1);
    answer(b, 4, kEbadf); // a second on, the poll: the medium is gone (media state 2), the file closed
    sc.messages(1);
    answer(b, 4, 0);
    sc.start_command(1, 0, ~u64{0});
    sc.src("Start", Script::Start, {kCmd, 0x11}, true); // no file: started, opened again five seconds on
    sc.messages(1);
    sc.messages(3); // media state 0, both items read
    sc.start_command(1, 0x100, ~u64{0});
    sc.src("Start", Script::Start, {kCmd, 0x12}, true);
    sc.messages(1);
    answer(b, 6, kEinval);
    sc.start_command(1, 0x200, ~u64{0});
    sc.src("Start", Script::Start, {kCmd, 0x13}, true); // started: the seek's error answered
    sc.messages(1);
    answer(b, 6, 0);
    answer(b, 5, kEio);
    answer(b, 4, kEbadf);
    sc.src("Checkin", Script::Checkin, {0}); // the read's EIO, the medium gone: media state 2, a session error
    sc.messages(2);
    answer(b, 5, 0);
    answer(b, 4, 0);
    sc.src("Close", Script::Close, {}, true); // started without a file
    sc.messages(1);
    sc.src("Open", Script::Open, {0, kMedia, kUriMovie, 0}, true);
    sc.messages(1);
    sc.src("Clear", Script::Clear, {});
    answer(b, 5, kEinval);
    sc.start_command(1, 0, ~u64{0});
    sc.src("Start", Script::Start, {kCmd, 0x14}, true); // the read fails: media state 1, a session error
    sc.messages(3);
    answer(b, 5, 0);
    sc.messages(2); // half a second on, read again: both items
    sc.src("Stop", Script::Stop, {}, true);
    sc.messages(1);
    answer(b, 6, kEinval);
    sc.start_command(1, 0x40, ~u64{0});
    sc.src("Start", Script::Start, {kCmd, 0x15}, true); // open: the seek fails, a session error, started anyway
    sc.messages(2);
    answer(b, 6, 0);
    sc.src("Close", Script::Close, {}, true);
    sc.messages(1);
    sc.src("Cleanup", Script::Cleanup, {});
    call("cellSailPlayerFinalize", {kP});
    sc.mask_source(false);
}

// OpenStream and CloseStream (D-562 part 13) of a player without streams through the built-in source: the control
// thread tells it to open (the reader opens the file and answers, message 0x29 completing the step) and to close.
// `games_calls`: through the game's file calls, whose open fails once the descriptor is open, so the stream does not
// open (CANCEL).
void script_stream(Script& sc, bool games_calls)
{
    u8* b = sc.b;
    const auto call = [&](const char* n, std::initializer_list<u64> a, bool v = false) { return sc.call(n, a, v); };
    const auto until_state = [&](u8 st) {
        sc.until([&] { return atomic_load_u8(b, kP + 0x163) == st; });
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the call's completion event after it
    };
    // cellDmux keeps the pipeline's callback (each side's own code) at +0x1C of its memory, the first the open takes.
    const auto mask_dmux_callback = [&] { sc.mask.push_back({((ps_read32(b, kHeapPtr) + 0x7F) & ~0x7Fu) + 0x1C, 4}); };
    g_caller_stacks_differ = games_calls;
    call("cellSailPlayerInitialize", {kP, 0, kMem, opd(kHandler), 0x22, 0, 0, 0, 0});
    if (games_calls) call("cellSailPlayerSetParameter", {kP, 33, kCalls, 0x28});
    call("cellSailPlayerBoot", {kP, 0});
    until_state(2);
    call("cellSailPlayerCreateDescriptor", {kP, 0, kMedia, kUriMovie, kOut});
    const u32 d0 = ps_read32(b, kOut);
    call("cellSailDescriptorOpen", {d0});
    call("cellSailPlayerAddDescriptor", {kP, d0});
    if (games_calls) answer(b, 1, 0x80010006); // the file gone by the time the stream opens
    mask_dmux_callback();
    call("cellSailPlayerOpenStream", {kP, 0x123456789ull});
    if (games_calls) {
        until_state(2);
        answer(b, 1, 0);
    } else {
        until_state(4);
        call("cellSailPlayerCloseStream", {kP, 0x987654321ull});
        until_state(2);
        // Again: the reader keeps the file it closed closed, opens it again.
        call("cellSailPlayerCreateDescriptor", {kP, 0, kMedia, kUriMovie, kOut + 4});
        const u32 d1 = ps_read32(b, kOut + 4);
        call("cellSailDescriptorOpen", {d1});
        call("cellSailPlayerAddDescriptor", {kP, d1});
        mask_dmux_callback();
        call("cellSailPlayerOpenStream", {kP, 0x55});
        until_state(4);
        call("cellSailPlayerCloseStream", {kP, 0x66});
        until_state(2);
    }
    call("cellSailPlayerGetDescriptorCount", {kP});
    call("cellSailPlayerFinalize", {kP});
    g_caller_stacks_differ = false;
    sc.mask_source(!games_calls);
    sc.mask_booted();
    if (games_calls) {
        for (u32 i = 0; i < ps_read32(b, kFileLog); i++) {
            const u32 e = kFileLog + 16 + 48 * i;
            const u32 id = static_cast<u32>(ps_read64(b, e));
            if (id == 1) sc.mask.push_back({e + 24, 8});
            if (id == 4) sc.mask.push_back({e + 16, 8});
            if (id == 5 || id == 6) sc.mask.push_back({e + 32, 8});
        }
    }
}

void compare_lines(const char* what, const std::vector<std::string>& mine, const std::vector<std::string>& sony)
{
    usize differ = 0;
    for (usize i = 0; i < std::max(mine.size(), sony.size()); i++) {
        const std::string o = i < mine.size() ? mine[i] : "(none)", s = i < sony.size() ? sony[i] : "(none)";
        if (o == s) continue;
        if (differ++ < 20) UNSCOPED_INFO(std::format("{} {}: pspocket {} | libsail {}", what, i, o, s));
    }
    CHECK(differ == 0);
}

void compare_runs(int script)
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    const Tree tree;
    const Run mine = run(flash, tree, false, script);
    const Run sony = run(flash, tree, true, script);
    compare_lines("answer", mine.lines, sony.lines);
    compare_lines("lv2 call", mine.calls, sony.calls);
    if (std::getenv("SAIL_DIFF")) {
        const auto dump = [](const char* what, u32 base, const std::vector<u8>& a, const std::vector<u8>& c) {
            usize n = 0;
            for (usize i = 0; i < std::min(a.size(), c.size()) && n < 200; i++)
                if (a[i] != c[i] && ++n) std::fprintf(stderr, "%s %#zx: pspocket %02x libsail %02x\n", what, base + i, a[i], c[i]);
        };
        dump("area", kArea, mine.mem, sony.mem);
        dump("heap", kHeap, mine.heap, sony.heap);
        for (const std::string& l : mine.lines) std::fprintf(stderr, "ANSWER %s\n", l.c_str());
        for (const auto* r : {&mine, &sony})
            for (const std::string& c : r->calls)
                std::fprintf(stderr, "%s %s\n", r == &mine ? "CALLS-MINE" : "CALLS-SONY", c.c_str());
    }
    if (std::getenv("SAIL_DUMP")) {
        const auto w = [&](const std::vector<u8>& m, u32 at) { return ps_read32(const_cast<u8*>(m.data()), at - kArea); };
        for (const auto* r : {&mine, &sony}) {
            std::fprintf(stderr, "== %s\n", r == &mine ? "mine" : "sony");
            for (u32 i = 0; i < w(r->mem, kMsgLog); i++)
                std::fprintf(stderr, "msg %08x %08x %08x %08x\n", w(r->mem, kMsgLog + 16 + 16 * i),
                    w(r->mem, kMsgLog + 20 + 16 * i), w(r->mem, kMsgLog + 24 + 16 * i), w(r->mem, kMsgLog + 28 + 16 * i));
            for (u32 i = 0; i < w(r->mem, kSums); i++)
                std::fprintf(stderr, "sum %08x %08x %08x %08x\n", w(r->mem, kSums + 16 + 16 * i),
                    w(r->mem, kSums + 20 + 16 * i), w(r->mem, kSums + 24 + 16 * i), w(r->mem, kSums + 28 + 16 * i));
            for (u32 i = 0; i < w(r->mem, kFreeLog); i++)
                std::fprintf(stderr, "free %08x %08x\n", w(r->mem, kFreeLog + 8 + 8 * i), w(r->mem, kFreeLog + 12 + 8 * i));
            for (u32 i = 0; i < w(r->mem, kFileLog); i++) {
                const u32 e = kFileLog + 16 + 48 * i;
                std::fprintf(stderr, "file %u: %08x %08x %08x %08x %08x\n", w(r->mem, e + 4), w(r->mem, e + 12),
                    w(r->mem, e + 20), w(r->mem, e + 28), w(r->mem, e + 36), w(r->mem, e + 44));
            }
        }
    }
    usize first = 0;
    while (first < mine.mem.size() && mine.mem[first] == sony.mem[first]) first++;
    INFO(std::format("memory differs first at {:#x} (pspocket {:#04x}, libsail {:#04x})", kArea + first,
        first < mine.mem.size() ? mine.mem[first] : 0, first < sony.mem.size() ? sony.mem[first] : 0));
    CHECK(first == mine.mem.size());
    CHECK(mine.heap.size() == sony.heap.size());
    usize h = 0;
    while (h < mine.heap.size() && h < sony.heap.size() && mine.heap[h] == sony.heap[h]) h++;
    INFO(std::format("the allocator's memory differs first at {:#x} (pspocket {:#04x}, libsail {:#04x})", kHeap + h,
        h < mine.heap.size() ? mine.heap[h] : 0, h < sony.heap.size() ? sony.heap[h] : 0));
    CHECK(h == std::min(mine.heap.size(), sony.heap.size()));
}

} // namespace

// Without the firmware: the reader called directly (script 3) on pspocket's side alone, its messages and the data its
// items delivered checked against the file itself (libsail's answers, which the comparison found).
TEST_CASE("cellSail's file source: the reader called directly, without the firmware", "[hle][sail]")
{
    const Tree tree;
    const Run r = run(std::nullopt, tree, false, 3);
    const auto w = [&](u32 at) { return ps_read32(const_cast<u8*>(r.mem.data()), at - kArea); };
    const auto w64 = [&](u32 at) { return ps_read64(const_cast<u8*>(r.mem.data()), at - kArea); };
    const auto answer_of = [&](usize i) { return i < r.lines.size() ? r.lines[i] : std::string(); };
    CHECK(answer_of(3) == "InquireCapability -> 0x80610701"); // start flags 1, the only start Start takes
    CHECK(answer_of(4) == "InquireCapability -> 0x0");
    CHECK(answer_of(10) == "ReadSync -> 0x80010006");
    CHECK(answer_of(13) == "Checkout -> 0x806107ff");
    CHECK(w(kOut) == 0);
    CHECK(w64(kOut) == 0x71);
    CHECK(w64(kOut + 0x10) == kMovieSize);
    CHECK(w64(kOut + 0x18) == kSmallSize);
    const std::vector<u8> movie = movie_bytes();
    CHECK(std::memcmp(r.mem.data() + (kBuf - kArea), movie.data(), 0x800) == 0);
    CHECK(std::memcmp(r.mem.data() + (kBuf + 0x800 - kArea), movie.data() + 0x1000, 0x100) == 0);
    CHECK(std::memcmp(r.mem.data() + (kBuf + 0x1200 - kArea), movie.data() + 0x10, 0x80) == 0);
    // The messages: {code, argument} (0x29 call completed, 0x01 stream out, 0x17 input EOS).
    const std::vector<std::pair<u32, u32>> expected = {{0x29, 0}, {0x29, 0}, {0x29, 0}, {1, 1}, {1, 1}, {1, 1},
        {0x17, 0}, {0x29, 1}, {0x29, 0}, {1, 1}, {0x17, 0}, {0x29, 0}, {0x29, 0x80010006}, {0x29, 0x80610701},
        {0x29, 0}, {1, 1}, {1, 1}, {0x29, 0}, {0x29, 0}, {0x29, 0}, {0x29, 0}, {0x29, 1}, {0x29, 1},
        {0x29, 0x80010006}, {0x29, 1}, {0x29, 0x806107F0}, {0x29, 0}, {0x29, 0}, {1, 1}, {1, 1}, {0x29, 0}, {0x29, 0}};
    REQUIRE(w(kMsgLog) == expected.size());
    for (u32 i = 0; i < expected.size(); i++) {
        INFO(std::format("message {}", i));
        CHECK(w(kMsgLog + 16 + 16 * i) == expected[i].first);
        CHECK(w(kMsgLog + 28 + 16 * i) == expected[i].second);
    }
    // The items checked out: from 0x90 (Start from 0x80 after a Read of 0x80 bytes at 0x10, which the reader takes
    // for its position), the second megabyte, the first again, the second again, then the range at 0x1000.
    const auto fnv = [&](u32 from, u32 n) {
        u32 h = 2166136261u;
        for (u32 i = 0; i < n; i++) h = (h ^ movie[from + i]) * 16777619u;
        return h;
    };
    const u32 sums[5][3] = {{0x100000, 0x55, fnv(0x90, 0x100000)}, {0x100000, 0x55, fnv(0x100090, 0x100000)},
        {0x100000, 0x55, fnv(0x90, 0x100000)}, {0x100000, 0x55, fnv(0x100090, 0x100000)},
        {0x1800, 0x66, fnv(0x1000, 0x1800)}};
    REQUIRE(w(kSums) == 5);
    for (u32 i = 0; i < 5; i++) {
        INFO(std::format("item {}", i));
        CHECK(w(kSums + 20 + 16 * i) == sums[i][0]);
        CHECK(w(kSums + 24 + 16 * i) == sums[i][1]);
        CHECK(w(kSums + 28 + 16 * i) == sums[i][2]);
    }
    // The buffers freed: four read-ahead buffers (Close, Close, Cleanup, Cleanup), then the player's block.
    CHECK(w(kFreeLog) == 5);
}

TEST_CASE("cellSail's file source against libsail: descriptors opened through it", "[hle][lle][sail]")
{
    compare_runs(1);
}

TEST_CASE("cellSail's file source against libsail: descriptors opened through the game's file calls", "[hle][lle][sail]")
{
    compare_runs(2);
}

TEST_CASE("cellSail's file source against libsail: the reader called directly", "[hle][lle][sail]") { compare_runs(3); }

TEST_CASE("cellSail's file source against libsail: the reader through the game's file calls", "[hle][lle][sail]")
{
    compare_runs(4);
}

TEST_CASE("cellSail's file source against libsail: the reader's failures and timeouts", "[hle][lle][sail]")
{
    compare_runs(5);
}

TEST_CASE("cellSail's file source against libsail: OpenStream and CloseStream through it", "[hle][lle][sail]")
{
    compare_runs(6);
}

TEST_CASE("cellSail's file source against libsail: OpenStream through the game's file calls, the file gone",
    "[hle][lle][sail]")
{
    compare_runs(7);
}
