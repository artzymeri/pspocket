// RPCS3's own NPDRM functions, compiled for differential tests (D-580). See rpcs3_ref.hpp.
//
// The functions scripts/rpcs3-extract.py copies out of RPCS3 (rpcs3_ref_extracted.inc, in the build directory) are
// compiled in namespace rpcs3x around stand-ins for the RPCS3 utilities they use: be_t, read_from_ptr, utils::align,
// ensure, the log channels, fs::file (over bytes in memory), and a SELFDecrypter with only what DecryptNPDRM touches.
// Two of RPCS3's helpers are written here instead of copied: mbedtls_zeroize (sha1.cpp links to the global one) and
// hmac_hash_compare, whose RPCS3 form allocates key_len bytes for sha1_hmac's 20 (a 16-byte key overruns it).

#include "rpcs3_ref.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "util/types.hpp" // tests/common/rpcs3_ref's stand-in

// RPCS3's own headers (rpcs3/Crypto).
#include "aes.h"
#include "key_vault.h"
#include "lz.h"
#include "sha1.h"

void mbedtls_zeroize(void* v, size_t n)
{
    static void* (*const volatile unop_memset)(void*, int, size_t) = &memset;
    (void)unop_memset(v, 0, n);
}

namespace rpcs3x {

template <typename T>
T swap_bytes(T v)
{
    auto bytes = std::bit_cast<std::array<u8, sizeof(T)>>(v);
    std::reverse(bytes.begin(), bytes.end());
    return std::bit_cast<T>(bytes);
}

// A value kept big-endian (RPCS3 util/endian.hpp be_t), as far as the functions use it: stored as read from memory,
// read as a host value.
template <typename T>
struct be_t {
    T raw;
    operator T() const { return swap_bytes(raw); }
};

template <typename To, typename From>
To read_from_ptr(const From& src, usz pos = 0)
{
    To v;
    std::memcpy(&v, reinterpret_cast<const u8*>(std::data(src)) + pos, sizeof(To));
    return v;
}

namespace utils {
template <typename T, typename U>
constexpr T align(T value, U alignment)
{
    return static_cast<T>((value + (alignment - 1)) & ~static_cast<T>(alignment - 1));
}
inline int toupper(u8 c) { return std::toupper(c); }
inline int tolower(u8 c) { return std::tolower(c); }
} // namespace utils

inline void ensure(bool ok)
{
    if (!ok) std::abort();
}

struct LogChannel {
    template <typename... A> void error(A&&...) const {}
    template <typename... A> void warning(A&&...) const {}
    template <typename... A> void notice(A&&...) const {}
    template <typename... A> void trace(A&&...) const {}
    template <typename... A> void success(A&&...) const {}
    template <typename... A> void fatal(A&&...) const {}
};
inline const LogChannel edat_log{}, self_log{};

namespace fs {
struct file {
    std::span<const u8> bytes;
    u64 position = 0;
    u64 pos() const { return position; }
    u64 read_at(u64 offset, void* dst, u64 size) const
    {
        if (offset >= bytes.size()) return 0;
        const u64 n = std::min<u64>(size, bytes.size() - offset);
        std::memcpy(dst, bytes.data() + offset, static_cast<usz>(n));
        return n;
    }
};
} // namespace fs

struct NPD_HEADER;

struct KeyVaultStandIn {
    u8 klicensee_key[16]{};
    const u8* GetKlicenseeKey() const { return klicensee_key; }
};

class SELFDecrypter {
public:
    const NPD_HEADER* npd = nullptr;
    KeyVaultStandIn key_v;
    const std::function<std::optional<pspocket::test::rpcs3_ref::Key>(std::string_view)>* rap = nullptr;

    const NPD_HEADER* GetNPDHeader() const { return npd; }
    bool GetKeyFromRap(std::string_view content_id, u8* npdrm_key);
    bool DecryptNPDRM(u8* metadata, u32 metadata_size);
};

bool hmac_hash_compare(const unsigned char* key, int key_len, const unsigned char* in, usz in_len,
    const unsigned char* hash, usz hash_len)
{
    unsigned char out[20];
    sha1_hmac(key, static_cast<size_t>(key_len), in, in_len, out);
    return std::memcmp(out, hash, hash_len) == 0;
}

#include "rpcs3_ref_extracted.inc"

// RPCS3 GetKeyFromRap reads exdata/<content ID>.rap (16 bytes) and turns it with rap_to_rif; the test hands over the
// file's bytes.
bool SELFDecrypter::GetKeyFromRap(std::string_view content_id, u8* npdrm_key)
{
    const std::optional<pspocket::test::rpcs3_ref::Key> bytes = (*rap)(content_id);
    if (!bytes) return false;
    rap_to_rif(bytes->data(), npdrm_key);
    return true;
}

} // namespace rpcs3x

namespace pspocket::test::rpcs3_ref {

namespace {

rpcs3x::NPD_HEADER headers(std::span<const u8> file, rpcs3x::EDAT_HEADER& edat)
{
    const rpcs3x::fs::file f{file, 0};
    rpcs3x::NPD_HEADER npd{};
    rpcs3x::read_npd_edat_header(&f, npd, edat);
    return npd;
}

bool same(const Key& a, const u8* b) { return std::memcmp(a.data(), b, 16) == 0; }

} // namespace

bool same_keys(const recomp::NpdrmKeys& k, const recomp::NpdKeys& n)
{
    return k.present && n.present && same(k.rap_key, RAP_KEY) && same(k.rap_pbox, RAP_PBOX) && same(k.rap_e1, RAP_E1) &&
        same(k.rap_e2, RAP_E2) && same(k.klic_free, NP_KLIC_FREE) && same(k.klic_key, NP_KLIC_KEY) &&
        same(k.omac_key2, NP_OMAC_KEY_2) && same(k.omac_key3, NP_OMAC_KEY_3) && same(n.sdat, SDAT_KEY) &&
        same(n.edat_key0, EDAT_KEY_0) && same(n.edat_key1, EDAT_KEY_1) && same(n.edat_hash0, EDAT_HASH_0) &&
        same(n.edat_hash1, EDAT_HASH_1) && same(n.edat_iv, EDAT_IV);
}

Key rap_to_rif(const Key& rap)
{
    Key rif{};
    rpcs3x::rap_to_rif(rap.data(), rif.data());
    return rif;
}

std::optional<std::vector<u8>> decrypt_block(std::span<const u8> file, u32 block, const Key& key)
{
    rpcs3x::EDAT_HEADER edat{};
    const rpcs3x::NPD_HEADER npd = headers(file, edat);
    if (!edat.block_size) return std::nullopt;
    const u32 total = static_cast<u32>((edat.file_size + edat.block_size - 1) / edat.block_size);
    // As EDATADecrypter::ReadData: a block-size buffer with 16 bytes to spare, the encrypted bytes read into it.
    std::vector<u8> out(edat.block_size + 16);
    const rpcs3x::fs::file f{file, 0};
    const s64 n = rpcs3x::decrypt_block(f, out, edat, npd, key.data(), block, total, edat.file_size, true, false);
    if (n < 0) return std::nullopt;
    out.resize(static_cast<usz>(n));
    return out;
}

bool validate_dev_klic(std::span<const u8> file, const Key& klicensee)
{
    rpcs3x::EDAT_HEADER edat{};
    return rpcs3x::validate_dev_klic(klicensee.data(), headers(file, edat));
}

bool validate_npd_hashes(std::span<const u8> file, std::string_view file_name, const Key& klicensee)
{
    rpcs3x::EDAT_HEADER edat{};
    const rpcs3x::NPD_HEADER npd = headers(file, edat);
    return rpcs3x::validate_npd_hashes(file_name, klicensee.data(), npd, edat, false);
}

bool decrypt_npdrm(u8* info, u32 size, s32 license, std::string_view content_id, const Key& klicensee,
    const std::function<std::optional<Key>(std::string_view)>& rap)
{
    rpcs3x::NPD_HEADER npd{};
    std::memcpy(&npd.magic, "NPD", 4);
    npd.license = license;
    std::memcpy(npd.content_id, content_id.data(), std::min<usz>(content_id.size(), sizeof(npd.content_id)));
    rpcs3x::SELFDecrypter self;
    self.npd = &npd;
    std::memcpy(self.key_v.klicensee_key, klicensee.data(), 16);
    self.rap = &rap;
    return self.DecryptNPDRM(info, size);
}

void aes_ecb(bool encrypt, std::span<const u8> key, const u8* in, u8* out)
{
    aes_context ctx;
    if (encrypt) aes_setkey_enc(&ctx, key.data(), static_cast<unsigned>(key.size() * 8));
    else aes_setkey_dec(&ctx, key.data(), static_cast<unsigned>(key.size() * 8));
    aes_crypt_ecb(&ctx, encrypt ? AES_ENCRYPT : AES_DECRYPT, in, out);
}

void aes_cbc(bool encrypt, std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out)
{
    aes_context ctx;
    if (encrypt) aes_setkey_enc(&ctx, key.data(), static_cast<unsigned>(key.size() * 8));
    else aes_setkey_dec(&ctx, key.data(), static_cast<unsigned>(key.size() * 8));
    u8 chain[16];
    std::memcpy(chain, iv, 16);
    aes_crypt_cbc(&ctx, encrypt ? AES_ENCRYPT : AES_DECRYPT, size, chain, in, out);
}

void aes_ctr(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out)
{
    aes_context ctx;
    aes_setkey_enc(&ctx, key.data(), static_cast<unsigned>(key.size() * 8));
    u8 counter[16], stream[16]{};
    std::memcpy(counter, iv, 16);
    size_t offset = 0;
    aes_crypt_ctr(&ctx, size, &offset, counter, stream, in, out);
}

void aes_cmac(const Key& key, const u8* in, usize size, u8* out)
{
    aes_context ctx;
    aes_setkey_enc(&ctx, key.data(), 128);
    ::aes_cmac(&ctx, size, in, out);
}

void hmac_sha1(std::span<const u8> key, const u8* in, usize size, u8* out)
{
    sha1_hmac(key.data(), key.size(), in, size, out);
}

} // namespace pspocket::test::rpcs3_ref
