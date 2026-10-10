// sys_crashdump's log areas (D-607): set keeps an entry, get gives it back, an index never set reads as zeros, RPCS3's
// EINVAL past index 127 or without an entry. Synthetic: no game data.

#include "hle_test_util.hpp"

#include <array>
#include <cstring>

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kEntrySize = 24;

// sys_crash_dump_log_area_info_t: label[16], addr, size.
void write_entry(u8* b, u32 at, const char* label, u32 addr, u32 size)
{
    std::memset(b + at, 0, 16);
    std::memcpy(b + at, label, std::strlen(label));
    ps_write32(b, at + 16, addr);
    ps_write32(b, at + 20, size);
}

std::array<u8, kEntrySize> bytes(const u8* b, u32 at)
{
    std::array<u8, kEntrySize> out{};
    std::memcpy(out.data(), b + at, kEntrySize);
    return out;
}

} // namespace

TEST_CASE("sys_crashdump: a log area set is the one got back", "[hle][crashdump]")
{
    Fixture f;
    GuestThread g(f, 0, "game");
    u8* b = f.base();
    constexpr u32 kIn = kData + 0x100, kOut = kData + 0x200, kNotGuest = 0xF0000000u;
    const u64 einval = r3_of_code(lv2::CELL_EINVAL);

    // An index never set: zeros, whatever was in the caller's entry.
    std::memset(b + kOut, 0xAA, kEntrySize);
    CHECK(g.call("sys_crash_dump_get_user_log_area", 5, kOut) == 0);
    CHECK(bytes(b, kOut) == std::array<u8, kEntrySize>{});

    write_entry(b, kIn, "frame-log", 0x00120000, 0x1000);
    CHECK(g.call("sys_crash_dump_set_user_log_area", 0, kIn) == 0);
    std::memset(b + kOut, 0xAA, kEntrySize);
    CHECK(g.call("sys_crash_dump_get_user_log_area", 0, kOut) == 0);
    CHECK(bytes(b, kOut) == bytes(b, kIn));

    // The last index; a later set replaces an entry; the label's 16 bytes as given.
    write_entry(b, kIn, "0123456789abcdef", 0x00200000, 0x40);
    CHECK(g.call("sys_crash_dump_set_user_log_area", 127, kIn) == 0);
    CHECK(g.call("sys_crash_dump_get_user_log_area", 127, kOut) == 0);
    CHECK(bytes(b, kOut) == bytes(b, kIn));
    write_entry(b, kIn, "audio", 0x00130000, 0x200);
    CHECK(g.call("sys_crash_dump_set_user_log_area", 0, kIn) == 0);
    CHECK(g.call("sys_crash_dump_get_user_log_area", 0, kOut) == 0);
    CHECK(bytes(b, kOut) == bytes(b, kIn));
    CHECK(ps_read32(b, kOut + 16) == 0x00130000);
    CHECK(ps_read32(b, kOut + 20) == 0x200);

    // RPCS3's EINVAL: an index past 127, no entry; here also an entry that is not guest memory. Nothing written.
    std::memset(b + kOut, 0xAA, kEntrySize);
    CHECK(g.call("sys_crash_dump_set_user_log_area", 128, kIn) == einval);
    CHECK(g.call("sys_crash_dump_get_user_log_area", 255, kOut) == einval);
    CHECK(g.call("sys_crash_dump_set_user_log_area", 1, 0) == einval);
    CHECK(g.call("sys_crash_dump_get_user_log_area", 1, 0) == einval);
    CHECK(g.call("sys_crash_dump_set_user_log_area", 1, kNotGuest) == einval);
    CHECK(g.call("sys_crash_dump_get_user_log_area", 1, kNotGuest) == einval);
    CHECK(b[kOut] == 0xAA);

    // The index is a byte: 256 is index 0.
    CHECK(g.call("sys_crash_dump_get_user_log_area", 256, kOut) == 0);
    CHECK(bytes(b, kOut) == bytes(b, kIn));

    CHECK(registry().is_firmware_first("sys_crashdump"));
}
