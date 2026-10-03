#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "pspocket/core/types.hpp"

// SHA-1 and SHA-256 (FIPS 180-4), written for the toolchain: NIDs, SPU program hashes, file verification.
// Incremental use: update() any number of times, then finish() once. finish() leaves the object in a finished
// state; call reset() to hash something else.
namespace pspocket::recomp {

using Sha1Digest = std::array<u8, 20>;
using Sha256Digest = std::array<u8, 32>;

class Sha1 {
public:
    Sha1() { reset(); }

    void reset();
    void update(std::span<const u8> data);
    void update(std::string_view text);
    Sha1Digest finish();

    static Sha1Digest hash(std::span<const u8> data);
    static Sha1Digest hash(std::string_view text);

private:
    void block(const u8* p);

    std::array<u32, 5> h_{};
    std::array<u8, 64> buffer_{};
    usize buffered_ = 0;
    u64 total_ = 0;
    bool finished_ = false;
};

class Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(std::span<const u8> data);
    void update(std::string_view text);
    Sha256Digest finish();

    static Sha256Digest hash(std::span<const u8> data);
    static Sha256Digest hash(std::string_view text);

private:
    void block(const u8* p);

    std::array<u32, 8> h_{};
    std::array<u8, 64> buffer_{};
    usize buffered_ = 0;
    u64 total_ = 0;
    bool finished_ = false;
};

std::string to_hex(const Sha1Digest& digest);
std::string to_hex(const Sha256Digest& digest);

} // namespace pspocket::recomp
