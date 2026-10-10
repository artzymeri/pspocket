#include "pspocket/recomp/hash.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/recomp/bytes.hpp"

// FIPS 180-4, "Secure Hash Standard": section 6.1 (SHA-1) and 6.2 (SHA-256).
namespace pspocket::recomp {

namespace {

u32 load_be32(const u8* p) { return ps_ld32(p); }

// Shared Merkle-Damgard buffering: feeds whole 64-byte blocks to `block`, keeps the tail.
template <class Hasher>
void feed(const Hasher& h, std::array<u8, 64>& buffer, usize& buffered, u64& total, std::span<const u8> data)
{
    if (data.empty()) return; // its data() may be null, which memcpy may not be given even for no bytes
    total += data.size();
    const u8* p = data.data();
    usize n = data.size();
    if (buffered != 0) {
        const usize take = std::min(n, buffer.size() - buffered);
        std::memcpy(buffer.data() + buffered, p, take);
        buffered += take;
        p += take;
        n -= take;
        if (buffered == buffer.size()) {
            h(buffer.data());
            buffered = 0;
        }
    }
    while (n >= 64) {
        h(p);
        p += 64;
        n -= 64;
    }
    if (n != 0) {
        std::memcpy(buffer.data(), p, n);
        buffered = n;
    }
}

// Appends 0x80, zero padding and the 64-bit big-endian bit length, then flushes.
template <class Hasher>
void pad(const Hasher& h, std::array<u8, 64>& buffer, usize buffered, u64 total)
{
    const u64 bits = total * 8;
    buffer[buffered++] = 0x80;
    if (buffered > 56) {
        std::memset(buffer.data() + buffered, 0, 64 - buffered);
        h(buffer.data());
        buffered = 0;
    }
    std::memset(buffer.data() + buffered, 0, 56 - buffered);
    ps_st64(buffer.data() + 56, bits);
    h(buffer.data());
}

constexpr std::array<u32, 64> kSha256K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

} // namespace

// ---- SHA-1 ----

void Sha1::reset()
{
    h_ = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    buffered_ = 0;
    total_ = 0;
    finished_ = false;
}

void Sha1::block(const u8* p)
{
    std::array<u32, 80> w;
    for (int t = 0; t < 16; t++) w[t] = load_be32(p + 4 * t);
    for (int t = 16; t < 80; t++) w[t] = std::rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);

    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int t = 0; t < 80; t++) {
        u32 f;
        u32 k;
        if (t < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const u32 temp = std::rotl(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = std::rotl(b, 30);
        b = a;
        a = temp;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::update(std::span<const u8> data)
{
    PS_CHECK(!finished_);
    feed([this](const u8* p) { block(p); }, buffer_, buffered_, total_, data);
}

void Sha1::update(std::string_view text)
{
    update(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

Sha1Digest Sha1::finish()
{
    PS_CHECK(!finished_);
    pad([this](const u8* p) { block(p); }, buffer_, buffered_, total_);
    finished_ = true;
    Sha1Digest out;
    for (usize i = 0; i < h_.size(); i++) ps_st32(out.data() + 4 * i, h_[i]);
    return out;
}

Sha1Digest Sha1::hash(std::span<const u8> data)
{
    Sha1 h;
    h.update(data);
    return h.finish();
}

Sha1Digest Sha1::hash(std::string_view text)
{
    Sha1 h;
    h.update(text);
    return h.finish();
}

// ---- SHA-256 ----

void Sha256::reset()
{
    h_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    buffered_ = 0;
    total_ = 0;
    finished_ = false;
}

void Sha256::block(const u8* p)
{
    std::array<u32, 64> w;
    for (int t = 0; t < 16; t++) w[t] = load_be32(p + 4 * t);
    for (int t = 16; t < 64; t++) {
        const u32 s0 = std::rotr(w[t - 15], 7) ^ std::rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        const u32 s1 = std::rotr(w[t - 2], 17) ^ std::rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int t = 0; t < 64; t++) {
        const u32 S1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const u32 ch = (e & f) ^ (~e & g);
        const u32 temp1 = h + S1 + ch + kSha256K[t] + w[t];
        const u32 S0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const u32 maj = (a & b) ^ (a & c) ^ (b & c);
        const u32 temp2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(std::span<const u8> data)
{
    PS_CHECK(!finished_);
    feed([this](const u8* p) { block(p); }, buffer_, buffered_, total_, data);
}

void Sha256::update(std::string_view text)
{
    update(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

Sha256Digest Sha256::finish()
{
    PS_CHECK(!finished_);
    pad([this](const u8* p) { block(p); }, buffer_, buffered_, total_);
    finished_ = true;
    Sha256Digest out;
    for (usize i = 0; i < h_.size(); i++) ps_st32(out.data() + 4 * i, h_[i]);
    return out;
}

Sha256Digest Sha256::hash(std::span<const u8> data)
{
    Sha256 h;
    h.update(data);
    return h.finish();
}

Sha256Digest Sha256::hash(std::string_view text)
{
    Sha256 h;
    h.update(text);
    return h.finish();
}

std::string to_hex(const Sha1Digest& digest) { return to_hex(std::span<const u8>(digest)); }

std::string to_hex(const Sha256Digest& digest) { return to_hex(std::span<const u8>(digest)); }

} // namespace pspocket::recomp
