#pragma once

// AES and the MACs built on it, for the PS3's encrypted formats: SELF executables and SCE packages (D-121, D-156),
// NPD files (D-165), the NPDRM licence layer and PKG files (D-580 to D-583). AES with 128-, 192- or 256-bit keys
// (FIPS-197) in the modes those formats use: ECB, CBC (no padding), CTR with a big-endian 128-bit counter (mbedtls
// aes_crypt_ctr, as RPCS3 uses it), AES-CMAC (RFC 4493); and HMAC-SHA1 (RFC 2104) over recomp::Sha1.
//
// On Apple platforms CommonCrypto does the AES (the system's, with the CPU's AES instructions), as it did before this
// layer (D-121). Everywhere else the portable AES below does it (D-580): a Linux host decrypts, and tests the
// decrypters, the same way. The portable AES is built on every platform (crypto::soft), so a test on the Mac can set
// it against CommonCrypto.
//
// Sizes for ECB and CBC must be whole blocks (16 bytes); CTR takes any size. `in` and `out` may be the same buffer.
// Every function returns false for a key that is not 16, 24 or 32 bytes (CMAC: 16), a size that is not whole blocks,
// or a failure of the platform's library; nothing else fails.

#include <array>
#include <span>

#include "pspocket/core/types.hpp"

namespace pspocket::recomp::crypto {

// One AES key's schedules, encryption and decryption (FIPS-197; the table-driven form of PolarSSL's aes.c, which
// RPCS3 carries as rpcs3/Crypto/aes.cpp).
class Aes {
public:
    explicit Aes(std::span<const u8> key);
    bool valid() const { return rounds_ != 0; }
    void encrypt_block(const u8* in, u8* out) const;
    void decrypt_block(const u8* in, u8* out) const;

private:
    int rounds_ = 0; // 10, 12 or 14
    std::array<u32, 60> enc_{};
    std::array<u32, 60> dec_{};
};

bool aes_ecb_encrypt(std::span<const u8> key, const u8* in, usize size, u8* out);
bool aes_ecb_decrypt(std::span<const u8> key, const u8* in, usize size, u8* out);
bool aes_cbc_encrypt(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out);
bool aes_cbc_decrypt(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out);
bool aes_ctr(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out);

// AES-CMAC (RFC 4493) of `size` bytes with a 128-bit key, as RPCS3 aes.cpp aes_cmac.
bool aes_cmac(std::span<const u8> key, const u8* in, usize size, u8* out16);

// HMAC-SHA1 (RFC 2104), as RPCS3 sha1.cpp sha1_hmac: 20 bytes.
void hmac_sha1(std::span<const u8> key, const u8* in, usize size, u8* out20);

// The portable implementations, on every platform (the functions above use them where CommonCrypto is not there).
namespace soft {
bool aes_ecb_encrypt(std::span<const u8> key, const u8* in, usize size, u8* out);
bool aes_ecb_decrypt(std::span<const u8> key, const u8* in, usize size, u8* out);
bool aes_cbc_encrypt(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out);
bool aes_cbc_decrypt(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out);
bool aes_ctr(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out);
} // namespace soft

// Which library does the AES here: "CommonCrypto" or "portable".
const char* aes_library();

} // namespace pspocket::recomp::crypto
