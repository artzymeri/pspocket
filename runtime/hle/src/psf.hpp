#pragma once

// PARAM.SFO (PSF 1.1, little-endian): a sorted map of keys to string, char-array or integer entries. Reading and
// writing follow RPCS3 rpcs3/Loader/PSF.cpp (load_object / save_object), so files written here read back on a PS3.

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::psf {

enum class Format : u16 {
    Array = 0x0004,   // a char array, not NUL-terminated
    String = 0x0204,  // a NUL-terminated string
    Integer = 0x0404, // a 32-bit integer
};

struct Entry {
    Format format = Format::String;
    u32 max = 0;      // the stored size (with the NUL for a string)
    std::string text; // String / Array
    u32 integer = 0;  // Integer
};

using Registry = std::map<std::string, Entry, std::less<>>;

// Nullopt for anything that isn't a well-formed PSF.
std::optional<Registry> parse(std::span<const u8> bytes);
std::vector<u8> save(const Registry& psf);

// The string (or array) value of `key`; empty if absent or an integer.
std::string get_string(const Registry& psf, std::string_view key);

inline Entry string(u32 max_size, std::string_view value) { return {Format::String, max_size, std::string(value), 0}; }

} // namespace pspocket::hle::psf
