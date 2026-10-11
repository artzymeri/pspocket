// AES, AES-CMAC and HMAC-SHA1 (D-580). See pspocket/recomp/crypto.hpp.
//
// The portable AES is PolarSSL's table-driven AES (aes_gen_tables, aes_setkey_enc, aes_setkey_dec, aes_crypt_ecb with
// its AES_FROUND/AES_RROUND rounds), as RPCS3 carries it in rpcs3/Crypto/aes.cpp (PolarSSL, GPLv2 or later): the same
// little-endian column words and tables, here generated at compile time. CBC, CTR and CMAC follow aes_crypt_cbc,
// aes_crypt_ctr and aes_cmac there.

#include "pspocket/recomp/crypto.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/recomp/hash.hpp"

#if defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#endif

namespace pspocket::recomp::crypto {

namespace {

struct Tables {
    std::array<u8, 256> fsb{}, rsb{};
    std::array<u32, 256> ft0{}, ft1{}, ft2{}, ft3{};
    std::array<u32, 256> rt0{}, rt1{}, rt2{}, rt3{};
    std::array<u32, 10> rcon{};
};

constexpr u32 rotl8(u32 x) { return (x << 8) | (x >> 24); }
constexpr int xtime(int x) { return ((x << 1) ^ ((x & 0x80) ? 0x1B : 0x00)) & 0xFF; }

// PolarSSL aes_gen_tables: GF(2^8) powers and logarithms of the generator 3, the S-boxes from the multiplicative
// inverses and the affine map, and the round tables (MixColumns, and InvMixColumns over the inverse S-box).
consteval Tables make_tables()
{
    Tables t;
    std::array<int, 256> power{}, logarithm{};
    for (int i = 0, x = 1; i < 256; i++) {
        power[i] = x;
        logarithm[x] = i;
        x = (x ^ xtime(x)) & 0xFF;
    }
    for (int i = 0, x = 1; i < 10; i++) {
        t.rcon[i] = static_cast<u32>(x);
        x = xtime(x);
    }
    t.fsb[0x00] = 0x63;
    t.rsb[0x63] = 0x00;
    for (int i = 1; i < 256; i++) {
        int x = power[255 - logarithm[i]];
        int y = x;
        y = ((y << 1) | (y >> 7)) & 0xFF;
        x ^= y;
        y = ((y << 1) | (y >> 7)) & 0xFF;
        x ^= y;
        y = ((y << 1) | (y >> 7)) & 0xFF;
        x ^= y;
        y = ((y << 1) | (y >> 7)) & 0xFF;
        x ^= y ^ 0x63;
        t.fsb[i] = static_cast<u8>(x);
        t.rsb[x] = static_cast<u8>(i);
    }
    const auto mul = [&](int a, int b) { return (a && b) ? power[(logarithm[a] + logarithm[b]) % 255] : 0; };
    for (int i = 0; i < 256; i++) {
        const int x = t.fsb[i];
        const int y = xtime(x);
        const int z = (y ^ x) & 0xFF;
        t.ft0[i] = static_cast<u32>(y) ^ (static_cast<u32>(x) << 8) ^ (static_cast<u32>(x) << 16) ^
            (static_cast<u32>(z) << 24);
        t.ft1[i] = rotl8(t.ft0[i]);
        t.ft2[i] = rotl8(t.ft1[i]);
        t.ft3[i] = rotl8(t.ft2[i]);
        const int r = t.rsb[i];
        t.rt0[i] = static_cast<u32>(mul(0x0E, r)) ^ (static_cast<u32>(mul(0x09, r)) << 8) ^
            (static_cast<u32>(mul(0x0D, r)) << 16) ^ (static_cast<u32>(mul(0x0B, r)) << 24);
        t.rt1[i] = rotl8(t.rt0[i]);
        t.rt2[i] = rotl8(t.rt1[i]);
        t.rt3[i] = rotl8(t.rt2[i]);
    }
    return t;
}

constexpr Tables kT = make_tables();

u32 load_le(const u8* p)
{
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
        (static_cast<u32>(p[3]) << 24);
}

void store_le(u8* p, u32 v)
{
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
    p[2] = static_cast<u8>(v >> 16);
    p[3] = static_cast<u8>(v >> 24);
}

u32 sub_word(u32 w)
{
    return static_cast<u32>(kT.fsb[w & 0xFF]) | (static_cast<u32>(kT.fsb[(w >> 8) & 0xFF]) << 8) |
        (static_cast<u32>(kT.fsb[(w >> 16) & 0xFF]) << 16) | (static_cast<u32>(kT.fsb[(w >> 24) & 0xFF]) << 24);
}

bool key_ok(std::span<const u8> key) { return key.size() == 16 || key.size() == 24 || key.size() == 32; }

} // namespace

// ------------------------------------------------------------------------------------------------- Aes

Aes::Aes(std::span<const u8> key)
{
    if (!key_ok(key)) return;
    const int nk = static_cast<int>(key.size() / 4);
    rounds_ = nk + 6;
    const int words = 4 * (rounds_ + 1);
    // FIPS-197 KeyExpansion in PolarSSL's little-endian words (aes_setkey_enc): RotWord is a right rotation by a byte
    // there, and the round constant sits in the low byte.
    for (int i = 0; i < nk; i++) enc_[i] = load_le(key.data() + 4 * i);
    for (int i = nk; i < words; i++) {
        u32 t = enc_[i - 1];
        if (i % nk == 0) t = sub_word((t >> 8) | (t << 24)) ^ kT.rcon[i / nk - 1];
        else if (nk > 6 && i % nk == 4) t = sub_word(t);
        enc_[i] = enc_[i - nk] ^ t;
    }
    // aes_setkey_dec: the rounds in reverse, the inner ones through InvMixColumns.
    for (int w = 0; w < 4; w++) dec_[w] = enc_[4 * rounds_ + w];
    for (int r = 1; r < rounds_; r++) {
        for (int w = 0; w < 4; w++) {
            const u32 v = enc_[4 * (rounds_ - r) + w];
            dec_[4 * r + w] = kT.rt0[kT.fsb[v & 0xFF]] ^ kT.rt1[kT.fsb[(v >> 8) & 0xFF]] ^
                kT.rt2[kT.fsb[(v >> 16) & 0xFF]] ^ kT.rt3[kT.fsb[(v >> 24) & 0xFF]];
        }
    }
    for (int w = 0; w < 4; w++) dec_[4 * rounds_ + w] = enc_[w];
}

void Aes::encrypt_block(const u8* in, u8* out) const
{
    const u32* rk = enc_.data();
    u32 x0 = load_le(in) ^ *rk++, x1 = load_le(in + 4) ^ *rk++, x2 = load_le(in + 8) ^ *rk++,
        x3 = load_le(in + 12) ^ *rk++;
    u32 y0 = 0, y1 = 0, y2 = 0, y3 = 0;
    const auto round = [&rk](u32& o0, u32& o1, u32& o2, u32& o3, u32 i0, u32 i1, u32 i2, u32 i3) {
        o0 = *rk++ ^ kT.ft0[i0 & 0xFF] ^ kT.ft1[(i1 >> 8) & 0xFF] ^ kT.ft2[(i2 >> 16) & 0xFF] ^ kT.ft3[(i3 >> 24) & 0xFF];
        o1 = *rk++ ^ kT.ft0[i1 & 0xFF] ^ kT.ft1[(i2 >> 8) & 0xFF] ^ kT.ft2[(i3 >> 16) & 0xFF] ^ kT.ft3[(i0 >> 24) & 0xFF];
        o2 = *rk++ ^ kT.ft0[i2 & 0xFF] ^ kT.ft1[(i3 >> 8) & 0xFF] ^ kT.ft2[(i0 >> 16) & 0xFF] ^ kT.ft3[(i1 >> 24) & 0xFF];
        o3 = *rk++ ^ kT.ft0[i3 & 0xFF] ^ kT.ft1[(i0 >> 8) & 0xFF] ^ kT.ft2[(i1 >> 16) & 0xFF] ^ kT.ft3[(i2 >> 24) & 0xFF];
    };
    for (int i = (rounds_ >> 1) - 1; i > 0; i--) {
        round(y0, y1, y2, y3, x0, x1, x2, x3);
        round(x0, x1, x2, x3, y0, y1, y2, y3);
    }
    round(y0, y1, y2, y3, x0, x1, x2, x3);
    const auto last = [&rk](u32 i0, u32 i1, u32 i2, u32 i3) {
        return *rk++ ^ static_cast<u32>(kT.fsb[i0 & 0xFF]) ^ (static_cast<u32>(kT.fsb[(i1 >> 8) & 0xFF]) << 8) ^
            (static_cast<u32>(kT.fsb[(i2 >> 16) & 0xFF]) << 16) ^ (static_cast<u32>(kT.fsb[(i3 >> 24) & 0xFF]) << 24);
    };
    x0 = last(y0, y1, y2, y3);
    x1 = last(y1, y2, y3, y0);
    x2 = last(y2, y3, y0, y1);
    x3 = last(y3, y0, y1, y2);
    store_le(out, x0);
    store_le(out + 4, x1);
    store_le(out + 8, x2);
    store_le(out + 12, x3);
}

void Aes::decrypt_block(const u8* in, u8* out) const
{
    const u32* rk = dec_.data();
    u32 x0 = load_le(in) ^ *rk++, x1 = load_le(in + 4) ^ *rk++, x2 = load_le(in + 8) ^ *rk++,
        x3 = load_le(in + 12) ^ *rk++;
    u32 y0 = 0, y1 = 0, y2 = 0, y3 = 0;
    const auto round = [&rk](u32& o0, u32& o1, u32& o2, u32& o3, u32 i0, u32 i1, u32 i2, u32 i3) {
        o0 = *rk++ ^ kT.rt0[i0 & 0xFF] ^ kT.rt1[(i3 >> 8) & 0xFF] ^ kT.rt2[(i2 >> 16) & 0xFF] ^ kT.rt3[(i1 >> 24) & 0xFF];
        o1 = *rk++ ^ kT.rt0[i1 & 0xFF] ^ kT.rt1[(i0 >> 8) & 0xFF] ^ kT.rt2[(i3 >> 16) & 0xFF] ^ kT.rt3[(i2 >> 24) & 0xFF];
        o2 = *rk++ ^ kT.rt0[i2 & 0xFF] ^ kT.rt1[(i1 >> 8) & 0xFF] ^ kT.rt2[(i0 >> 16) & 0xFF] ^ kT.rt3[(i3 >> 24) & 0xFF];
        o3 = *rk++ ^ kT.rt0[i3 & 0xFF] ^ kT.rt1[(i2 >> 8) & 0xFF] ^ kT.rt2[(i1 >> 16) & 0xFF] ^ kT.rt3[(i0 >> 24) & 0xFF];
    };
    for (int i = (rounds_ >> 1) - 1; i > 0; i--) {
        round(y0, y1, y2, y3, x0, x1, x2, x3);
        round(x0, x1, x2, x3, y0, y1, y2, y3);
    }
    round(y0, y1, y2, y3, x0, x1, x2, x3);
    const auto last = [&rk](u32 i0, u32 i1, u32 i2, u32 i3) {
        return *rk++ ^ static_cast<u32>(kT.rsb[i0 & 0xFF]) ^ (static_cast<u32>(kT.rsb[(i1 >> 8) & 0xFF]) << 8) ^
            (static_cast<u32>(kT.rsb[(i2 >> 16) & 0xFF]) << 16) ^ (static_cast<u32>(kT.rsb[(i3 >> 24) & 0xFF]) << 24);
    };
    x0 = last(y0, y3, y2, y1);
    x1 = last(y1, y0, y3, y2);
    x2 = last(y2, y1, y0, y3);
    x3 = last(y3, y2, y1, y0);
    store_le(out, x0);
    store_le(out + 4, x1);
    store_le(out + 8, x2);
    store_le(out + 12, x3);
}

// ------------------------------------------------------------------------------------------------- portable modes

namespace soft {

bool aes_ecb_encrypt(std::span<const u8> key, const u8* in, usize size, u8* out)
{
    const Aes aes(key);
    if (!aes.valid() || size % 16) return false;
    for (usize at = 0; at < size; at += 16) aes.encrypt_block(in + at, out + at);
    return true;
}

bool aes_ecb_decrypt(std::span<const u8> key, const u8* in, usize size, u8* out)
{
    const Aes aes(key);
    if (!aes.valid() || size % 16) return false;
    for (usize at = 0; at < size; at += 16) aes.decrypt_block(in + at, out + at);
    return true;
}

// PolarSSL aes_crypt_cbc.
bool aes_cbc_encrypt(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out)
{
    const Aes aes(key);
    if (!aes.valid() || size % 16) return false;
    u8 chain[16];
    std::memcpy(chain, iv, 16);
    for (usize at = 0; at < size; at += 16) {
        u8 x[16];
        for (int i = 0; i < 16; i++) x[i] = static_cast<u8>(in[at + i] ^ chain[i]);
        aes.encrypt_block(x, out + at);
        std::memcpy(chain, out + at, 16);
    }
    return true;
}

bool aes_cbc_decrypt(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out)
{
    const Aes aes(key);
    if (!aes.valid() || size % 16) return false;
    u8 chain[16];
    std::memcpy(chain, iv, 16);
    for (usize at = 0; at < size; at += 16) {
        u8 cipher[16], plain[16];
        std::memcpy(cipher, in + at, 16); // `out` may be `in`
        aes.decrypt_block(cipher, plain);
        for (int i = 0; i < 16; i++) out[at + i] = static_cast<u8>(plain[i] ^ chain[i]);
        std::memcpy(chain, cipher, 16);
    }
    return true;
}

// PolarSSL aes_crypt_ctr: the counter block counts big-endian over all 128 bits.
bool aes_ctr(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out)
{
    const Aes aes(key);
    if (!aes.valid()) return false;
    u8 counter[16], stream[16];
    std::memcpy(counter, iv, 16);
    for (usize at = 0; at < size; at += 16) {
        aes.encrypt_block(counter, stream);
        const usize n = std::min<usize>(16, size - at);
        for (usize i = 0; i < n; i++) out[at + i] = static_cast<u8>(in[at + i] ^ stream[i]);
        for (int i = 15; i >= 0; i--)
            if (++counter[i] != 0) break;
    }
    return true;
}

} // namespace soft

// ------------------------------------------------------------------------------------------------- the platform's

#if defined(__APPLE__)

namespace {

bool cc_crypt(CCOperation op, CCOptions options, std::span<const u8> key, const u8* iv, const u8* in, usize size,
    u8* out)
{
    if (!key_ok(key) || size % 16) return false;
    if (!size) return true;
    usize moved = 0;
    return CCCrypt(op, kCCAlgorithmAES, options, key.data(), key.size(), iv, in, size, out, size, &moved) ==
               kCCSuccess &&
        moved == size;
}

} // namespace

bool aes_ecb_encrypt(std::span<const u8> key, const u8* in, usize size, u8* out)
{
    return cc_crypt(kCCEncrypt, kCCOptionECBMode, key, nullptr, in, size, out);
}

bool aes_ecb_decrypt(std::span<const u8> key, const u8* in, usize size, u8* out)
{
    return cc_crypt(kCCDecrypt, kCCOptionECBMode, key, nullptr, in, size, out);
}

bool aes_cbc_encrypt(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out)
{
    return cc_crypt(kCCEncrypt, 0, key, iv, in, size, out);
}

bool aes_cbc_decrypt(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out)
{
    return cc_crypt(kCCDecrypt, 0, key, iv, in, size, out);
}

// CommonCrypto's big-endian counter counts in its low 64 bits only and wraps there without a carry (D-908), where
// PolarSSL's (and so RPCS3's and the PS3 formats') counts over all 128: the data is cut where the low half would wrap,
// and the next part starts from the counter with its high half incremented and its low half 0.
bool aes_ctr(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out)
{
    if (!key_ok(key)) return false;
    u8 counter[16];
    std::memcpy(counter, iv, 16);
    while (size) {
        const u64 low = ps_ld64(counter + 8);
        const u64 room = ~low + 1; // blocks before the low half wraps (0: all 2^64 of them)
        const usize n = room != 0 && room <= size / 16 ? static_cast<usize>(room) * 16 : size;
        CCCryptorRef c = nullptr;
        if (CCCryptorCreateWithMode(kCCEncrypt, kCCModeCTR, kCCAlgorithmAES, ccNoPadding, counter, key.data(), key.size(),
                nullptr, 0, 0, kCCModeOptionCTR_BE, &c) != kCCSuccess) {
            return false;
        }
        usize moved = 0;
        const bool ok = CCCryptorUpdate(c, in, n, out, n, &moved) == kCCSuccess && moved == n;
        CCCryptorRelease(c);
        if (!ok) return false;
        in += n;
        out += n;
        size -= n;
        ps_st64(counter, ps_ld64(counter) + 1);
        ps_st64(counter + 8, 0);
    }
    return true;
}

const char* aes_library() { return "CommonCrypto"; }

#else

bool aes_ecb_encrypt(std::span<const u8> key, const u8* in, usize size, u8* out)
{
    return soft::aes_ecb_encrypt(key, in, size, out);
}

bool aes_ecb_decrypt(std::span<const u8> key, const u8* in, usize size, u8* out)
{
    return soft::aes_ecb_decrypt(key, in, size, out);
}

bool aes_cbc_encrypt(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out)
{
    return soft::aes_cbc_encrypt(key, iv, in, size, out);
}

bool aes_cbc_decrypt(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out)
{
    return soft::aes_cbc_decrypt(key, iv, in, size, out);
}

bool aes_ctr(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out)
{
    return soft::aes_ctr(key, iv, in, size, out);
}

const char* aes_library() { return "portable"; }

#endif

// ------------------------------------------------------------------------------------------------- MACs

// RFC 4493 (RPCS3 aes.cpp aes_cmac): the subkeys from E(0), the chain over all but the last block as one CBC
// encryption, the last block (padded, or whole) XORed with its subkey and encrypted after it.
bool aes_cmac(std::span<const u8> key, const u8* in, usize size, u8* out16)
{
    if (key.size() != 16) return false;
    u8 l[16]{}, k1[16], k2[16];
    const u8 zero[16]{};
    if (!aes_ecb_encrypt(key, zero, 16, l)) return false;
    const auto shift = [](const u8* from, u8* to) {
        for (int i = 0; i < 16; i++) to[i] = static_cast<u8>((from[i] << 1) | (i < 15 ? from[i + 1] >> 7 : 0));
        if (from[0] & 0x80) to[15] ^= 0x87;
    };
    shift(l, k1);
    shift(k1, k2);
    usize n = (size + 15) / 16;
    bool whole = false;
    if (n == 0) n = 1;
    else whole = size % 16 == 0;
    u8 last[16]{};
    const u8* tail = in + 16 * (n - 1);
    if (whole) {
        for (int i = 0; i < 16; i++) last[i] = static_cast<u8>(tail[i] ^ k1[i]);
    } else {
        const usize rest = size % 16;
        for (usize i = 0; i < 16; i++) last[i] = static_cast<u8>((i < rest ? tail[i] : i == rest ? 0x80 : 0) ^ k2[i]);
    }
    u8 x[16]{};
    if (n > 1) {
        std::vector<u8> chain(16 * (n - 1));
        if (!aes_cbc_encrypt(key, zero, in, chain.size(), chain.data())) return false;
        std::memcpy(x, chain.data() + chain.size() - 16, 16);
    }
    for (int i = 0; i < 16; i++) x[i] ^= last[i];
    return aes_ecb_encrypt(key, x, 16, out16);
}

// RFC 2104 over SHA-1 (RPCS3 sha1.cpp sha1_hmac): a key longer than the block is hashed first.
void hmac_sha1(std::span<const u8> key, const u8* in, usize size, u8* out20)
{
    std::array<u8, 64> k{};
    if (key.size() > k.size()) {
        const Sha1Digest d = Sha1::hash(key);
        std::copy(d.begin(), d.end(), k.begin());
    } else {
        std::copy(key.begin(), key.end(), k.begin());
    }
    std::array<u8, 64> ipad{}, opad{};
    for (usize i = 0; i < k.size(); i++) {
        ipad[i] = static_cast<u8>(k[i] ^ 0x36);
        opad[i] = static_cast<u8>(k[i] ^ 0x5C);
    }
    Sha1 inner;
    inner.update(ipad);
    inner.update(std::span<const u8>(in, size));
    const Sha1Digest inner_digest = inner.finish();
    Sha1 outer;
    outer.update(opad);
    outer.update(inner_digest);
    const Sha1Digest digest = outer.finish();
    std::memcpy(out20, digest.data(), digest.size());
}

} // namespace pspocket::recomp::crypto
