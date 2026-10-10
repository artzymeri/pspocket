// sys_libc part 6 (runtime/hle/src/sys_libc_spu.cpp, D-904): an SPU thread's and a raw SPU's local store read and
// written from the PPU, Sony's descriptor slots, _Getmem and the names nothing describes. An SPU program built here.
// No game data (test_libc_spu_game.cpp compares with a game's copy).

#include <catch_amalgamated.hpp>

#include <bit>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "hle_test_util.hpp"
#include "spu_program.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/recomp/spu/asm.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
using pspocket::test::SpuProgram;
namespace as = pspocket::recomp::spu::as;

namespace {

constexpr u32 kBuf = kData + 0x1000, kScratch = kData + 0x20000;

s32 i32(u64 r3) { return static_cast<s32>(static_cast<u32>(r3)); }

// The local store both tests start from: at 0x1000 the bytes 80 81 7F 01, the word 0x12345678, the doubleword
// 0x0123456789ABCDEF, the float 1.5 (and a pad word), the double -2.25.
std::vector<u8> sample()
{
    std::vector<u8> d = {0x80, 0x81, 0x7F, 0x01, 0x12, 0x34, 0x56, 0x78, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    const u32 f = std::bit_cast<u32>(1.5f);
    const u64 x = std::bit_cast<u64>(-2.25);
    for (u32 i = 0; i < 4; i++) d.push_back(static_cast<u8>(f >> (24 - 8 * i)));
    for (u32 i = 0; i < 4; i++) d.push_back(0);
    for (u32 i = 0; i < 8; i++) d.push_back(static_cast<u8>(x >> (56 - 8 * i)));
    return d;
}

// Every accessor of one family (`prefix` "spu_thread_" or "raw_spu_") on the local store of `id`, which holds
// sample() at 0x1000; `bad` is what a misaligned word access answers.
void exercise(GuestThread& g, u8* b, const std::string& prefix, u32 id, s32 bad)
{
    const auto call = [&](const char* name, auto... args) { return g.call(prefix + name, id, args...); };
    CHECK(i32(call("read_char", 0x1000)) == -128);
    CHECK(i32(call("read_char", 0x1001)) == static_cast<s8>(0x81));
    CHECK(call("read_uchar", 0x1001) == 0x81);
    CHECK(i32(call("read_short", 0x1000)) == static_cast<s16>(0x8081));
    CHECK(i32(call("read_short", 0x1001)) == static_cast<s16>(0x8081)); // the aligned halfword
    CHECK(i32(call("read_short", 0x1002)) == 0x7F01);
    CHECK(call("read_ushort", 0x1003) == 0x7F01);
    CHECK(i32(call("read_int", 0x1004)) == 0x12345678);
    CHECK(i32(call("read_long", 0x1004)) == 0x12345678);
    CHECK(call("read_uint", 0x1004) == 0x12345678);
    CHECK(call("read_ulong", 0x1004) == 0x12345678);
    CHECK(call("read_ptr", 0x1004) == 0x12345678);
    CHECK(call("read_llong", 0x1008) == 0x0123456789ABCDEFull);
    CHECK(call("read_ullong", 0x1008) == 0x0123456789ABCDEFull);
    call("read_float", 0x1010);
    CHECK(g.ctx.fpr[1] == 1.5);
    call("read_double", 0x1018);
    CHECK(g.ctx.fpr[1] == -2.25);
    call("read_ldouble", 0x1018);
    CHECK(g.ctx.fpr[1] == -2.25);
    std::memset(b + kBuf, 0xEE, 16);
    call("read_mem", 0x1001, kBuf, 6);
    CHECK(std::vector<u8>(b + kBuf, b + kBuf + 7) == std::vector<u8>{0x81, 0x7F, 0x01, 0x12, 0x34, 0x56, 0xEE});

    CHECK(i32(call("write_char", 0x1101, 0xAB)) == 0);
    CHECK(call("read_uint", 0x1100) == 0x00AB0000);
    CHECK(i32(call("write_uchar", 0x1103, 0x1CD)) == 0);
    CHECK(call("read_uint", 0x1100) == 0x00AB00CD);
    CHECK(i32(call("write_short", 0x1105, 0xBEEF)) == 0); // the aligned halfword
    CHECK(call("read_uint", 0x1104) == 0xBEEF0000);
    CHECK(i32(call("write_ushort", 0x1106, 0x1234)) == 0);
    CHECK(call("read_uint", 0x1104) == 0xBEEF1234);
    CHECK(i32(call("write_int", 0x1108, 0xCAFEBABE)) == 0);
    CHECK(i32(call("write_long", 0x110C, 0xFFFFFFFF80000001ull)) == 0);
    CHECK(call("read_ullong", 0x1108) == 0xCAFEBABE80000001ull);
    CHECK(i32(call("write_uint", 0x1110, 0x11223344)) == 0);
    CHECK(i32(call("write_ulong", 0x1114, 0x55667788)) == 0);
    CHECK(i32(call("write_ptr", 0x1118, 0x99AABBCC)) == 0);
    CHECK(i32(call("write_llong", 0x1120, 0x0102030405060708ull)) == 0);
    CHECK(i32(call("write_ullong", 0x1128, 0xF1F2F3F4F5F6F7F8ull)) == 0);
    CHECK(call("read_llong", 0x1110) == 0x1122334455667788ull);
    CHECK(call("read_uint", 0x1118) == 0x99AABBCC);
    CHECK(call("read_llong", 0x1120) == 0x0102030405060708ull);
    CHECK(call("read_llong", 0x1128) == 0xF1F2F3F4F5F6F7F8ull);
    g.ctx.fpr[1] = 3.5;
    CHECK(i32(call("write_float", 0x1130)) == 0);
    CHECK(call("read_uint", 0x1130) == std::bit_cast<u32>(3.5f));
    g.ctx.fpr[1] = -0.125;
    CHECK(i32(call("write_double", 0x1138)) == 0);
    CHECK(call("read_llong", 0x1138) == std::bit_cast<u64>(-0.125));
    g.ctx.fpr[1] = 1e300;
    CHECK(i32(call("write_ldouble", 0x1140)) == 0);
    CHECK(call("read_llong", 0x1140) == std::bit_cast<u64>(1e300));
    // write_mem: two whole words, then the last three bytes merged into the word there (its fourth byte kept).
    call("write_llong", 0x1200, 0xA0A1A2A3A4A5A6A7ull);
    call("write_int", 0x1208, 0xB0B1B2B3);
    const u8 src[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    std::memcpy(b + kBuf, src, sizeof src);
    CHECK(i32(call("write_mem", 0x1200, kBuf, 11)) == 0);
    CHECK(call("read_llong", 0x1200) == 0x0102030405060708ull);
    CHECK(call("read_uint", 0x1208) == 0x090A0BB3);
    // A misaligned word: lv2 refuses it (an SPU thread) and the read gives 0; a raw SPU's store is plain memory.
    if (bad != 0) {
        CHECK(i32(call("write_int", 0x1301, 1)) == bad);
        CHECK(call("read_uint", 0x1005) == 0);
    }
}

} // namespace

TEST_CASE("sys_libc: an SPU thread's local store from the PPU, a word at a time (D-904)", "[hle][libc][spu-threads]")
{
    Fixture f;
    GuestThread g(f, 0, "ppu");
    lv2::PpuThread& t = *g.t;
    u8* b = f.base();
    SpuProgram p;
    p.emit(as::br(0)); // spin while the PPU reads and writes its store
    p.data(0x1000, sample());
    p.data(0x1400, {0});
    const auto group = pspocket::test::make_spu_group(t, b, kScratch, p);
    REQUIRE(group.thread);
    // Before the group runs lv2 refuses the store (CELL_ESTAT): reads give 0, writes answer the error.
    CHECK(g.call("spu_thread_read_uint", group.thread, 0x1004) == 0);
    CHECK(i32(g.call("spu_thread_write_int", group.thread, 0x1004, 1)) == static_cast<s32>(lv2::CELL_ESTAT));
    REQUIRE(lv2::sys_spu_thread_group_start(t, group.group) == lv2::CELL_OK);
    exercise(g, b, "spu_thread_", group.thread, static_cast<s32>(lv2::CELL_EINVAL));
    // Out of the store: lv2's EINVAL.
    CHECK(i32(g.call("spu_thread_write_int", group.thread, 0x40000, 1)) == static_cast<s32>(lv2::CELL_EINVAL));
    // read_mem leaves r3 as the id when there is nothing to read.
    CHECK(g.call("spu_thread_read_mem", group.thread, 0x1000, kBuf, 0) == group.thread);
    REQUIRE(lv2::sys_spu_thread_group_terminate(t, group.group, 0) == lv2::CELL_OK);
}

TEST_CASE("sys_libc: a raw SPU's local store from the PPU (D-904)", "[hle][libc][spu-threads]")
{
    Fixture f;
    GuestThread g(f, 0, "ppu");
    lv2::PpuThread& t = *g.t;
    u8* b = f.base();
    REQUIRE(lv2::sys_spu_initialize(t, 6, 1) == lv2::CELL_OK);
    if (lv2::sys_raw_spu_create(t, kScratch, 0) != lv2::CELL_OK) SKIP("raw SPUs need fault emulation (Apple AArch64)");
    const u32 id = ps_read32(b, kScratch);
    const u32 ls = 0xE0000000u + 0x100000u * id;
    const std::vector<u8> s = sample();
    std::memcpy(b + ls + 0x1000, s.data(), s.size());
    exercise(g, b, "raw_spu_", id, 0);
    // Beyond the store, in the problem-state registers' range without host pages: nothing read or written.
    CHECK(i32(g.call("raw_spu_write_int", id, 0x7FFF0, 1)) == static_cast<s32>(lv2::CELL_EFAULT));
    CHECK(g.call("raw_spu_read_uint", id, 0x7FFF0) == 0);
    REQUIRE(lv2::sys_raw_spu_destroy(t, id) == lv2::CELL_OK);
}

TEST_CASE("sys_libc: Sony's descriptor slots, _Getmem, the names nothing describes (D-904)", "[hle][libc]")
{
    Fixture f;
    GuestThread g(f, 0, "ppu");
    // Slots 0..2 hold 0..2: the first free one is 3.
    CHECK(i32(g.call("_get_fd", 10, 7)) == 3);
    CHECK(i32(g.call("_get_fd", 11, 0)) == 4);
    g.call("_put_fd", 3);
    CHECK(i32(g.call("_get_fd", 12, 0)) == 3);
    g.call("_put_fd", 0);
    CHECK(i32(g.call("_get_fd", 13, 0)) == 0);
    g.call("_put_fd", 256); // beyond the table: nothing
    s32 last = 0, taken = 0;
    while ((last = i32(g.call("_get_fd", 99, 0))) != -1) taken++;
    CHECK(taken == 256 - 5);
    // _Getmem: distinct aligned blocks of main memory, none for 0 bytes.
    std::set<u32> blocks;
    for (u32 n : {1u, 16u, 100u, 0x10000u}) {
        const u32 a = static_cast<u32>(g.call("_Getmem", n));
        CHECK(a != 0);
        CHECK(a % 16 == 0);
        blocks.insert(a);
    }
    CHECK(blocks.size() == 4);
    CHECK(g.call("_Getmem", 0) == 0);
    for (const char* name : {"_Atrealloc", "_Once_ctor", "_Once_dtor", "_d2rv", "_rv2d", "_Fd2rv", "_Frv2d", "_Ld2rv",
             "_Lrv2d"})
        CHECK(g.call(name, 1, 2, 3) == ~u64{0});
}
