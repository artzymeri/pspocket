// sys_libc's process side (runtime/hle/src/sys_libc_process.cpp, D-516): exit and atexit, assertions, the library's
// locks and _Once, _Printf and _Scanf through guest callbacks, and SPU printf with an SPU program built here that sends
// its printf as the SDK's spu_printf does. No game data.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "guest.hpp"
#include "hle_test_util.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
namespace as = pspocket::recomp::spu::as;

namespace {

constexpr u32 kA = kData + 0x1000, kStr = kData + 0x1100, kFmt = kData + 0x1200, kBuf = kData + 0x2000,
              kOut = kData + 0x3000, kCounter = kData + 0x3800, kCode = kData + 0x4000, kElf = kData + 0x10000;

void put(u8* b, u32 at, std::string_view s)
{
    std::memcpy(b + at, s.data(), s.size());
    b[at + s.size()] = 0;
}

std::string cstr(u8* b, u32 at)
{
    std::string s;
    while (b[at + s.size()]) s += static_cast<char>(b[at + s.size()]);
    return s;
}

s32 i32(u64 r3) { return static_cast<s32>(static_cast<u32>(r3)); }

// Guest functions at kCode, each with its descriptor right after its code; their descriptors' addresses returned.
struct Code {
    u8* b;
    u32 at = kCode;
    u32 add(std::initializer_list<u32> words)
    {
        const u32 entry = at;
        for (const u32 w : words) {
            ps_write32(b, at, w);
            at += 4;
        }
        const u32 opd = at;
        ps_write32(b, opd, entry);
        ps_write32(b, opd + 4, 0);
        at += 16;
        return opd;
    }
};

// counter = counter * 10 + rN (r9 for a constant function, r3 for one that takes its argument).
u32 recorder(Code& c, s32 value)
{
    return c.add({0x3CE00000u | (kCounter >> 16), 0x60E70000u | (kCounter & 0xFFFF), 0x81070000u, 0x1D08000Au,
        value < 0 ? 0x7D081A14u : (0x39200000u | static_cast<u32>(value)), value < 0 ? 0x60000000u : 0x7D084A14u,
        0x91070000u, 0x4E800020u});
}

// The TTY's text written while it lives.
struct Tty {
    std::mutex mutex;
    std::string text;
    Tty()
    {
        log::set_sink(
            [](void* user, log::Level, std::string_view channel, std::string_view message) {
                if (channel != "tty") return;
                auto* self = static_cast<Tty*>(user);
                std::lock_guard lock(self->mutex);
                self->text += message;
            },
            this);
    }
    ~Tty() { log::set_sink(nullptr, nullptr); }
    Tty(const Tty&) = delete;
    Tty& operator=(const Tty&) = delete;
    std::string get()
    {
        std::lock_guard lock(mutex);
        return text;
    }
};

} // namespace

TEST_CASE("sys_libc: exit runs the atexit functions last-first, then liblv2's exit", "[hle][libc]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    const u32 one = recorder(c, 1), two = recorder(c, 2), with_arg = recorder(c, -1);
    ps_write32(b, kCounter, 0);
    CHECK(t.call("atexit", one) == 0);
    CHECK(t.call("__cxa_atexit", with_arg, 7, 0) == 0);
    CHECK(t.call("atexit", two) == 0);
    CHECK(i32(t.call("atexit", 0)) == -1);
    CHECK_THROWS_AS(t.call("exit", 3), lv2::ThreadExit);
    CHECK(ps_read32(b, kCounter) == 271); // two, then with_arg(7), then one
    CHECK(f.kernel->exited());
}

TEST_CASE("sys_libc: assertions, environ, _Once and the library's locks", "[hle][libc]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    // _Nonfatal_Assert prints Dinkumware's message and returns.
    {
        Tty tty;
        put(b, kStr, "x == 1");
        put(b, kA, "main.c:12");
        t.call("_Nonfatal_Assert", kStr, kA);
        CHECK(tty.get().find("x == 1 main.c:12 -- assertion failed") != std::string::npos);
    }
    // environ: an empty list.
    const u32 env = HleProcess::current()->link_variable("sys_libc", recomp::nid_of("environ"));
    REQUIRE(env != 0);
    CHECK(ps_read32(b, ps_read32(b, env)) == 0);
    // _Once: the function once, the flag 2 after.
    const u32 one = recorder(c, 1);
    ps_write32(b, kCounter, 0);
    ps_write32(b, kA, 0);
    t.call("_Once", kA, one);
    t.call("_Once", kA, one);
    CHECK(ps_read32(b, kCounter) == 1);
    CHECK(ps_read32(b, kA) == 2);
    // _Mtxlock is recursive; another thread waits for the last _Mtxunlock.
    t.call("_Mtxinit", kOut);
    t.call("_Mtxlock", kOut);
    t.call("_Mtxlock", kOut);
    std::atomic<bool> got{false};
    std::thread other([&] {
        GuestThread o(f, 1, "other");
        o.call("_Mtxlock", kOut);
        got = true;
        o.call("_Mtxunlock", kOut);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    t.call("_Mtxunlock", kOut);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!got);
    t.call("_Mtxunlock", kOut);
    other.join();
    CHECK(got);
    t.call("_Mtxdst", kOut);
    t.call("_Locksyslock", 2);
    t.call("_Unlocksyslock", 2);
    // _Assert ends the program.
    put(b, kStr, "never");
    CHECK_THROWS_AS(t.call("_Assert", kStr, 0), lv2::ThreadExit);
}

TEST_CASE("sys_libc: Dinkumware's thread-local storage and its Win32 names (D-570)", "[hle][libc]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    const u32 destructor = recorder(c, -1); // counter = counter * 10 + its argument
    ps_write32(b, kCounter, 0);
    REQUIRE(i32(t.call("_Tlsalloc", kA, destructor)) == 0);
    const u32 k1 = ps_read32(b, kA);
    REQUIRE(i32(t.call("_Tlsalloc", kA, 0)) == 0);
    const u32 k2 = ps_read32(b, kA);
    CHECK(k1 != k2);
    CHECK(t.call("_Tlsget", k1) == 0); // 0 until set
    CHECK(i32(t.call("_Tlsset", k1, 3)) == 0);
    CHECK(t.call("TlsSetValue", k2, 0x1234) == 1);
    CHECK(t.call("_Tlsget", k1) == 3);
    CHECK(t.call("TlsGetValue", k2) == 0x1234);

    // Another thread has values of its own; its _Destroytls gives its value to the destructor.
    u64 before = 1, after = 1, set = 1, destroyed = 1;
    std::thread other([&] {
        GuestThread o(f, 1, "other");
        before = o.call("_Tlsget", k1);
        set = o.call("_Tlsset", k1, 4);
        after = o.call("_Tlsget", k1);
        o.call("_Destroytls");
        destroyed = o.call("_Tlsget", k1);
    });
    other.join();
    CHECK(before == 0);
    CHECK(i32(set) == 0);
    CHECK(after == 4);
    CHECK(destroyed == 0);
    CHECK(ps_read32(b, kCounter) == 4);
    CHECK(t.call("_Tlsget", k1) == 3);
    t.call("_Destroytls");
    CHECK(ps_read32(b, kCounter) == 43); // key 2 has no destructor
    CHECK(t.call("_Tlsget", k1) == 0);
    CHECK(t.call("TlsGetValue", k2) == 0);

    // A freed key is refused; 64 keys in all.
    CHECK(i32(t.call("_Tlsset", k2, 9)) == 0);
    CHECK(i32(t.call("_Tlsfree", k2)) == 0);
    CHECK(i32(t.call("_Tlsfree", k2)) == -1);
    CHECK(i32(t.call("_Tlsset", k2, 9)) == -1);
    CHECK(t.call("TlsSetValue", k2, 9) == 0);
    CHECK(t.call("_Tlsget", 999) == 0);
    u32 made = 0;
    while (i32(t.call("_Tlsalloc", kA, 0)) == 0 && made < 100) made++;
    CHECK(made == 63);
}

TEST_CASE("sys_libc: _Printf and _Scanf through the caller's functions", "[hle][libc]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    Code c{b};
    // pfn(dst, text, n): copies n bytes to dst, returns dst + n.
    const u32 copy = c.add({0x7CA903A6u, 0x2C050000u, 0x41820018u, 0x88C40000u, 0x98C30000u, 0x38840001u,
        0x38630001u, 0x4200FFF0u, 0x4E800020u});
    put(b, kFmt, "%d-%s|%5.1f");
    put(b, kStr, "abc");
    ps_write64(b, kA, static_cast<u64>(-42));
    ps_write64(b, kA + 8, kStr);
    ps_write64(b, kA + 16, std::bit_cast<u64>(2.25));
    std::memset(b + kBuf, 0, 64);
    CHECK(i32(t.call("_Printf", copy, kBuf, kFmt, kA)) == 13);
    CHECK(cstr(b, kBuf) == "-42-abc|  2.2");
    // pfn(cursor, c, get): the next byte of a string (EOF at its NUL), or c pushed back.
    const u32 getc = c.add({0x80C30000u, 0x2C050000u, 0x41820028u, 0x88E60000u, 0x2C070000u, 0x41820014u,
        0x38C60001u, 0x90C30000u, 0x7CE33B78u, 0x4E800020u, 0x3860FFFFu, 0x4E800020u, 0x38C6FFFFu, 0x90C30000u,
        0x7C832378u, 0x4E800020u});
    put(b, kStr, "  123 word!");
    ps_write32(b, kCounter, kStr);
    put(b, kFmt, "%d %[a-z]");
    ps_write64(b, kA, kOut);
    ps_write64(b, kA + 8, kOut + 8);
    CHECK(i32(t.call("_Scanf", getc, kCounter, kFmt, kA)) == 2);
    CHECK(ps_read32(b, kOut) == 123);
    CHECK(cstr(b, kOut + 8) == "word");
    CHECK(ps_read32(b, kCounter) == kStr + 10); // the '!' pushed back
}

namespace {

// An SPU program at LS 0 with data after it, as a 32-bit big-endian SPU ELF (one PT_LOAD at LS 0, a PT_NOTE).
struct SpuProgram {
    std::vector<u32> words;
    void emit(u32 insn) { words.push_back(insn); }
    void load32(u32 rt, u32 v)
    {
        emit(as::ilhu(rt, static_cast<s32>(v >> 16)));
        emit(as::iohl(rt, static_cast<s32>(v & 0xFFFF)));
    }
    // Bytes at LS `lsa` (the program grows to reach it).
    void data(u32 lsa, const std::vector<u8>& bytes)
    {
        const usize need = (lsa + bytes.size() + 3) / 4;
        if (words.size() < need) words.resize(need, 0);
        for (usize i = 0; i < bytes.size(); i++) {
            u32& w = words[(lsa + i) / 4];
            const u32 shift = 8 * (3 - ((lsa + static_cast<u32>(i)) & 3));
            w = (w & ~(0xFFu << shift)) | (u32{bytes[i]} << shift);
        }
    }
    void write_elf(u8* b, u32 ea) const
    {
        const u32 size = static_cast<u32>(words.size() * 4);
        std::memset(b + ea, 0, 0x200 + size);
        static constexpr u8 kIdent[16] = {0x7F, 'E', 'L', 'F', 1, 2, 1, 0};
        std::memcpy(b + ea, kIdent, 16);
        ps_write16(b, ea + 0x10, 2);
        ps_write16(b, ea + 0x12, 0x17);
        ps_write32(b, ea + 0x14, 1);
        ps_write32(b, ea + 0x18, 0);
        ps_write32(b, ea + 0x1C, 0x34);
        ps_write16(b, ea + 0x28, 0x34);
        ps_write16(b, ea + 0x2A, 0x20);
        ps_write16(b, ea + 0x2C, 2);
        const u32 ph = ea + 0x34;
        ps_write32(b, ph + 0x00, 1);
        ps_write32(b, ph + 0x04, 0x100);
        ps_write32(b, ph + 0x10, size);
        ps_write32(b, ph + 0x14, size + 0x100);
        ps_write32(b, ph + 0x18, 7);
        ps_write32(b, ph + 0x20, 4);
        ps_write32(b, ph + 0x24, 0x80);
        ps_write32(b, ph + 0x30, 0x20);
        for (usize i = 0; i < words.size(); i++) ps_write32(b, ea + 0x100 + 4 * static_cast<u32>(i), words[i]);
    }
};

std::vector<u8> bytes_of(std::string_view s)
{
    std::vector<u8> v(s.begin(), s.end());
    v.push_back(0);
    return v;
}

std::vector<u8> quad32(u32 v) { return {u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; }

std::vector<u8> quad64(u64 v)
{
    std::vector<u8> q(16, 0);
    for (u32 i = 0; i < 8; i++) q[i] = static_cast<u8>(v >> (56 - 8 * i));
    return q;
}

} // namespace

TEST_CASE("sys_libc: an SPU's spu_printf through the server, and spu_thread_snprintf", "[hle][libc][spu-threads]")
{
    Fixture f;
    GuestThread g(f, 0, "ppu");
    lv2::PpuThread& t = *g.t;
    u8* b = f.base();
    REQUIRE(f.hle->link_internal_functions({"sys_libc"}));
    Tty tty;
    // The SPU: the arguments' LS address to the outbound mailbox, a user event on port 1, the send status and then
    // printf's result from the inbound mailbox; the result stored at kOut; then it spins for the snprintf below.
    SpuProgram p;
    constexpr u32 kArgs = 0x2000, kFormat = 0x1000, kWord = 0x1100;
    p.load32(3, kArgs);
    p.emit(as::wrch(PS_SPU_WrOutMbox, 3));
    p.emit(as::ilhu(4, 0x0100));
    p.emit(as::wrch(PS_SPU_WrOutIntrMbox, 4));
    p.emit(as::rdch(5, PS_SPU_RdInMbox)); // the send's status
    p.emit(as::rdch(6, PS_SPU_RdInMbox)); // printf's result
    // DMA $6's quadword to kOut.
    p.emit(as::il(71, 0x3000));
    p.emit(as::stqd(6, 0, 71));
    p.emit(as::wrch(PS_MFC_LSA, 71));
    p.emit(as::il(70, 0));
    p.emit(as::wrch(PS_MFC_EAH, 70));
    p.load32(72, kOut);
    p.emit(as::wrch(PS_MFC_EAL, 72));
    p.emit(as::il(70, 16));
    p.emit(as::wrch(PS_MFC_Size, 70));
    p.emit(as::il(70, 5));
    p.emit(as::wrch(PS_MFC_TagID, 70));
    p.emit(as::il(70, static_cast<s32>(PS_MFC_PUT)));
    p.emit(as::wrch(PS_MFC_Cmd, 70));
    p.emit(as::il(70, 1 << 5));
    p.emit(as::wrch(PS_MFC_WrTagMask, 70));
    p.emit(as::il(70, PS_MFC_TAG_UPDATE_ALL));
    p.emit(as::wrch(PS_MFC_WrTagUpdate, 70));
    p.emit(as::rdch(70, PS_MFC_RdTagStat));
    p.emit(as::br(0)); // spin
    p.data(kFormat, bytes_of("spu %d:%s:%llx:%.1f\n"));
    p.data(kWord, bytes_of("word"));
    std::vector<u8> args;
    for (const auto& q : {quad32(kFormat), quad32(static_cast<u32>(-5)), quad32(kWord), quad64(0x123456789Aull),
             quad64(std::bit_cast<u64>(0.5))})
        args.insert(args.end(), q.begin(), q.end());
    p.data(kArgs, args);
    p.write_elf(b, kElf);
    constexpr u32 kImg = kData + 0x8000, kGroup = kData + 0x8100, kThread = kData + 0x8108, kAttr = kData + 0x8200,
                  kThreadAttr = kData + 0x8240, kThreadArgs = kData + 0x8280, kName = kData + 0x8300;
    REQUIRE(lv2::_sys_spu_image_import(t, kImg, kElf, 0x100 + static_cast<u32>(p.words.size() * 4), 0) ==
            lv2::CELL_OK);
    std::memcpy(b + kName, "test", 5);
    ps_write32(b, kAttr, 5);
    ps_write32(b, kAttr + 4, kName);
    ps_write32(b, kAttr + 8, 0);
    REQUIRE(lv2::sys_spu_thread_group_create(t, kGroup, 1, 100, kAttr) == lv2::CELL_OK);
    const u32 group = ps_read32(b, kGroup);
    ps_write32(b, kThreadAttr, kName);
    ps_write32(b, kThreadAttr + 4, 5);
    ps_write32(b, kThreadAttr + 8, 0);
    std::memset(b + kThreadArgs, 0, 32);
    REQUIRE(lv2::sys_spu_thread_initialize(t, kThread, group, 0, kImg, kThreadAttr, kThreadArgs) == lv2::CELL_OK);
    const u32 spu = ps_read32(b, kThread);
    // The server, then the thread attached to it (port 1 of its events to the server's queue).
    CHECK(i32(g.call("spu_printf_attach_thread", spu)) == static_cast<s32>(lv2::CELL_ESTAT)); // no server yet
    REQUIRE(g.call("spu_printf_initialize", 1000, 0) == 0);
    REQUIRE(g.call("spu_printf_attach_thread", spu) == 0);
    ps_write32(b, kOut, 0xFFFFFFFF);
    REQUIRE(lv2::sys_spu_thread_group_start(t, group) == lv2::CELL_OK);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    // The SPU's PUT stores atomically, with release order: the test reads the word the same way (D-571).
    while (hle::atomic_load_be32(b, kOut) == 0xFFFFFFFF && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const std::string text = "spu -5:word:123456789a:0.5\n";
    CHECK(hle::atomic_load_be32(b, kOut) == text.size());
    CHECK(tty.get().find(text.substr(0, text.size() - 1)) != std::string::npos);
    // spu_thread_snprintf reads the same arguments from the spinning thread's local store.
    CHECK(i32(g.call("spu_thread_snprintf", kBuf, 9, spu, kArgs)) == static_cast<s32>(text.size()));
    CHECK(cstr(b, kBuf) == "spu -5:w");
    REQUIRE(lv2::sys_spu_thread_group_terminate(t, group, 0) == lv2::CELL_OK);
    CHECK(g.call("spu_printf_detach_thread", spu) == 0);
    CHECK(g.call("spu_printf_finalize") == 0);
    CHECK(i32(g.call("spu_printf_finalize")) == static_cast<s32>(lv2::CELL_ESTAT));
}
