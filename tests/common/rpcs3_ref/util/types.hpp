#pragma once

// A stand-in for RPCS3's rpcs3/util/types.hpp, with only what its Crypto headers (key_vault.h, utils.h) and the
// functions scripts/rpcs3-extract.py copies out need: the integer names. pspocket's own; holds no RPCS3 code (D-580).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using usz = std::size_t;
using u128 = unsigned __int128;
