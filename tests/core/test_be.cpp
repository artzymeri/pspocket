#include <catch_amalgamated.hpp>

#include <array>

#include "pspocket/core/be.h"
#include "pspocket/core/types.hpp"

using namespace pspocket;

TEST_CASE("big-endian loads read the most significant byte first")
{
    const std::array<u8, 8> bytes{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    CHECK(ps_ld8(bytes.data()) == 0x01);
    CHECK(ps_ld16(bytes.data()) == 0x0123);
    CHECK(ps_ld32(bytes.data()) == 0x01234567u);
    CHECK(ps_ld64(bytes.data()) == 0x0123456789ABCDEFull);
    CHECK(ps_ld32(bytes.data() + 1) == 0x23456789u); // unaligned
}

TEST_CASE("big-endian stores round-trip")
{
    std::array<u8, 8> bytes{};
    ps_st64(bytes.data(), 0x0123456789ABCDEFull);
    CHECK(bytes == std::array<u8, 8>{0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF});
    ps_st16(bytes.data() + 3, 0xBEEF);
    CHECK(bytes[3] == 0xBE);
    CHECK(bytes[4] == 0xEF);
}

TEST_CASE("float accessors keep the IEEE bit pattern")
{
    std::array<u8, 8> bytes{};
    ps_stf32(bytes.data(), 1.0f);
    CHECK(ps_ld32(bytes.data()) == 0x3F800000u);
    CHECK(ps_ldf32(bytes.data()) == 1.0f);
    ps_stf64(bytes.data(), -2.5);
    CHECK(ps_ld64(bytes.data()) == 0xC004000000000000ull);
    CHECK(ps_ldf64(bytes.data()) == -2.5);
}

TEST_CASE("128-bit accessors use the full-reverse lane order (D-004)")
{
    // Guest vector of four words {0x00010203, 0x04050607, 0x08090A0B, 0x0C0D0E0F} in big-endian memory.
    std::array<u8, 16> guest{};
    for (int i = 0; i < 16; i++) guest[i] = static_cast<u8>(i);

    alignas(16) u32 host[4];
    ps_ld128_rev(host, guest.data());
    // Guest element i lives in host lane 3 - i, in host byte order.
    CHECK(host[3] == 0x00010203u);
    CHECK(host[2] == 0x04050607u);
    CHECK(host[1] == 0x08090A0Bu);
    CHECK(host[0] == 0x0C0D0E0Fu);

    std::array<u8, 16> back{};
    ps_st128_rev(back.data(), host);
    CHECK(back == guest);
}
