// RPCS3's own NPDRM and package functions, compiled for differential tests (D-580, D-583). See rpcs3_ref.hpp.
//
// The functions scripts/rpcs3-extract.py copies out of RPCS3 (rpcs3_ref_extracted.inc, in the build directory) are
// compiled in namespace rpcs3x around stand-ins for the RPCS3 utilities they use: be_t and le_t, read_from_ptr and
// write_to_ptr, utils::align and aligned_div, umax, ensure, the log channels, fs::file (over bytes in memory), a
// SELFDecrypter with only what DecryptNPDRM touches, and a package_reader with only what decrypt, read_entries and
// archive_read_block touch.
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
// read as a host value, made from one (package_reader::decrypt's counter block and key stream input).
template <typename T>
struct be_t {
    T raw;
    be_t() = default;
    be_t(T v) : raw(swap_bytes(v)) {}
    operator T() const { return swap_bytes(raw); }
    T value() const { return swap_bytes(raw); }
    be_t operator++(int)
    {
        const be_t old = *this;
        *this = be_t(value() + 1);
        return old;
    }
};

// A value kept little-endian (RPCS3 le_t) on a little-endian host.
template <typename T>
struct le_t {
    T raw;
    operator T() const { return raw; }
};

using uchar = unsigned char;
constexpr usz umax = ~usz{0};

template <typename T, typename To>
void write_to_ptr(To&& dst, usz pos, const T& value)
{
    std::memcpy(reinterpret_cast<u8*>(std::data(dst)) + pos, &value, sizeof(T));
}

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
template <typename T, typename U>
constexpr T aligned_div(T value, U divisor)
{
    return static_cast<T>((value + divisor - 1) / divisor);
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
inline const LogChannel edat_log{}, self_log{}, pkg_log{};

namespace fs {
struct file {
    std::span<const u8> bytes;
    u64 position = 0;
    u64 pos() const { return position; }
    u64 size() const { return bytes.size(); }
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
struct PKGHeader;
struct PKGEntry;

// RPCS3 unpkg.h package_reader, with what decrypt, read_entries and archive_read_block use: the package's bytes, its
// header, and the data's key.
class package_reader {
public:
    package_reader(const PKGHeader& header, std::span<const u8> bytes) : m_header(header), m_file{bytes, 0} {}

    bool read_entries(std::vector<PKGEntry>& entries);
    std::span<const char> archive_read_block(u64 offset, std::span<u8> dst, u64 num_bytes);
    usz decrypt(u64 offset, u64 size, const uchar* key, std::span<u8> local_buf);

    static constexpr usz BUF_SIZE = 8192 * 1024;
    static constexpr usz BUF_PADDING = 32;
    bool m_is_valid = true;
    const PKGHeader& m_header;
    fs::file m_file;
    std::array<uchar, 16> m_dec_key{};
};

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

namespace {

rpcs3x::PKGHeader pkg_header(std::span<const u8> package)
{
    static_assert(sizeof(rpcs3x::PKGHeader) == 0x80 && sizeof(rpcs3x::PKGEntry) == 0x20);
    rpcs3x::PKGHeader header{};
    std::memcpy(&header, package.data(), std::min<usz>(package.size(), sizeof header));
    return header;
}

} // namespace

bool same_pkg_keys(const recomp::PkgKeys& keys)
{
    return keys.present && same(keys.ps3, PKG_AES_KEY) && same(keys.idu, PKG_AES_KEY_IDU);
}

std::optional<std::vector<u8>> pkg_decrypt(std::span<const u8> package, u64 offset, u64 size, const Key& key)
{
    const rpcs3x::PKGHeader header = pkg_header(package);
    rpcs3x::package_reader reader(header, package);
    // As RPCS3's callers: a buffer with BUF_PADDING bytes to spare.
    std::vector<u8> out(static_cast<usz>(size) + rpcs3x::package_reader::BUF_PADDING);
    const usz n = reader.decrypt(offset, size, key.data(), out);
    if (n < size) return std::nullopt;
    out.resize(static_cast<usz>(size));
    return out;
}

bool pkg_table_sound(std::span<const u8> package, const Key& key)
{
    const rpcs3x::PKGHeader header = pkg_header(package);
    rpcs3x::package_reader reader(header, package);
    std::memcpy(reader.m_dec_key.data(), key.data(), 16);
    std::vector<rpcs3x::PKGEntry> entries;
    return reader.read_entries(entries);
}

bool pkg_leaves_directory(std::string_view name) { return rpcs3x::leaves_directory(name); }

} // namespace pspocket::test::rpcs3_ref
