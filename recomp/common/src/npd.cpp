#include "pspocket/recomp/npd.hpp"

#include <algorithm>
#include <cstring>

#include "pspocket/core/be.h"
#include "pspocket/recomp/crypto.hpp"

namespace pspocket::recomp {

namespace {

constexpr u64 kHeaderSize = 0x100;   // NPD header, EDAT header, the header's own hashes
constexpr u64 kMetadataOffset = 0x100;

std::span<const u8> key16(const u8* key) { return std::span<const u8>(key, 16); }

// RPCS3 generate_key / generate_hash: the mode's top nibble says whether the block's key (or hash key) is used as
// it is (0), decrypted with EDAT_KEY and EDAT_IV (1), or replaced by EDAT_KEY/EDAT_IV (EDAT_HASH) (2).
bool final_key(const NpdKeys& k, u32 mode, bool v4, const u8* key, const u8* iv, u8* key_out, u8* iv_out)
{
    const u8* edat_key = v4 ? k.edat_key1.data() : k.edat_key0.data();
    switch (mode & 0xF0000000u) {
    case 0x10000000u:
        if (!crypto::aes_cbc_decrypt(key16(edat_key), k.edat_iv.data(), key, 16, key_out)) return false;
        std::memcpy(iv_out, iv, 16);
        return true;
    case 0x20000000u:
        std::memcpy(key_out, edat_key, 16);
        std::memcpy(iv_out, k.edat_iv.data(), 16);
        return true;
    default:
        std::memcpy(key_out, key, 16);
        std::memcpy(iv_out, iv, 16);
        return true;
    }
}

bool final_hash(const NpdKeys& k, u32 mode, bool v4, const u8* hash, u8* out)
{
    switch (mode & 0xF0000000u) {
    case 0x10000000u:
        return crypto::aes_cbc_decrypt(key16(v4 ? k.edat_key1.data() : k.edat_key0.data()), k.edat_iv.data(), hash,
            16, out);
    case 0x20000000u: std::memcpy(out, v4 ? k.edat_hash1.data() : k.edat_hash0.data(), 16); return true;
    default: std::memcpy(out, hash, 16); return true;
    }
}

// RPCS3 decrypt: the block decrypted (crypto mode 1: stored plain, 2: AES-128-CBC) and its encrypted bytes' hash
// checked against `expected` (hash mode 1: HMAC-SHA1, 0x14 bytes; 2: AES-CMAC; 4: HMAC-SHA1's first 0x10 bytes).
bool decrypt(const NpdKeys& k, u32 hash_mode, u32 crypto_mode, bool v4, const u8* in, u8* out, usize size,
    const u8* key, const u8* iv, const u8* hash, const u8* expected, std::string& error)
{
    u8 key_final[16]{}, iv_final[16]{}, hash_final[0x14]{};
    if (!final_key(k, crypto_mode, v4, key, iv, key_final, iv_final) || !final_hash(k, hash_mode, v4, hash, hash_final)) {
        error = "decrypting the block's key failed";
        return false;
    }
    switch (crypto_mode & 0xFF) {
    case 1: std::memcpy(out, in, size); break;
    case 2:
        if (!crypto::aes_cbc_decrypt(key16(key_final), iv_final, in, size, out)) {
            error = "AES-128-CBC failed";
            return false;
        }
        break;
    default: error = std::format("unknown crypto mode {:#x}", crypto_mode); return false;
    }
    switch (hash_mode & 0xFF) {
    case 1: {
        u8 mac[20];
        crypto::hmac_sha1(std::span<const u8>(hash_final, 0x14), in, size, mac);
        if (std::memcmp(mac, expected, 0x14) == 0) return true;
        break;
    }
    case 2: {
        u8 mac[16];
        if (!crypto::aes_cmac(key16(hash_final), in, size, mac)) {
            error = "AES-CMAC failed";
            return false;
        }
        if (std::memcmp(mac, expected, 16) == 0) return true;
        break;
    }
    case 4: {
        u8 mac[20];
        crypto::hmac_sha1(std::span<const u8>(hash_final, 0x10), in, size, mac);
        if (std::memcmp(mac, expected, 0x10) == 0) return true;
        break;
    }
    default: error = std::format("unknown hash mode {:#x}", hash_mode); return false;
    }
    error = "the block's hash does not match (wrong keys, or a damaged file)";
    return false;
}

// RPCS3 dec_section: a compressed block's metadata (NPD version 2 and later) hides its offset, length and
// compression end by XOR.
void dec_section(const u8* m, u64& offset, u64& length, s32& compression_end)
{
    u8 d[16];
    for (int i = 0; i < 4; i++) {
        d[i] = static_cast<u8>(m[0xC + i] ^ m[0x8 + i] ^ m[0x10 + i]);
        d[4 + i] = static_cast<u8>(m[0x4 + i] ^ m[0x8 + i] ^ m[0x14 + i]);
        d[8 + i] = static_cast<u8>(m[0xC + i] ^ m[0x0 + i] ^ m[0x18 + i]);
        d[12 + i] = static_cast<u8>(m[0x4 + i] ^ m[0x0 + i] ^ m[0x1C + i]);
    }
    offset = ps_ld64(d);
    length = static_cast<u64>(static_cast<s64>(static_cast<s32>(ps_ld32(d + 8))));
    compression_end = static_cast<s32>(ps_ld32(d + 12));
}

// ---- RPCS3 lz.cpp, with the input's end checked (bytes past it read as zero).

struct LzInput {
    const u8* p;
    const u8* end;
    u8 at(usize i) const { return p + i < end ? p[i] : 0; }
};

void decode_range(u32* range, u32* code, LzInput& src)
{
    if (!((*range) >> 24)) {
        (*range) <<= 8;
        *code = ((*code) << 8) + src.at(5);
        src.p++;
    }
}

template <class Index>
int decode_bit(u32* range, u32* code, Index* index, LzInput& src, u8* c)
{
    decode_range(range, code, src);
    const u32 val = ((*range) >> 8) * (*c);
    *c = static_cast<u8>(*c - ((*c) >> 3));
    if (index) *index = static_cast<Index>(static_cast<u32>(*index) << 1);
    if (*code < val) {
        *range = val;
        *c = static_cast<u8>(*c + 31);
        if (index) (*index)++;
        return 1;
    }
    *code -= val;
    *range -= val;
    return 0;
}

// The number's bits accumulate unsigned: damaged input can shift past 31 bits, which must not be undefined.
u32 decode_number(u8* ptr, int index, int* bit_flag, u32* range, u32* code, LzInput& src)
{
    u32 i = 1;
    if (index >= 3) {
        decode_bit(range, code, &i, src, ptr + 0x18);
        if (index >= 4) {
            decode_bit(range, code, &i, src, ptr + 0x18);
            if (index >= 5) {
                decode_range(range, code, src);
                for (; index >= 5; index--) {
                    i <<= 1;
                    (*range) >>= 1;
                    if (*code < *range) i++;
                    else (*code) -= *range;
                }
            }
        }
    }
    *bit_flag = decode_bit(range, code, &i, src, ptr);
    if (index >= 1) {
        decode_bit(range, code, &i, src, ptr + 0x8);
        if (index >= 2) decode_bit(range, code, &i, src, ptr + 0x10);
    }
    return i;
}

u32 decode_word(u8* ptr, int index, int* bit_flag, u32* range, u32* code, LzInput& src)
{
    u32 i = 1;
    index /= 8;
    if (index >= 3) {
        decode_bit(range, code, &i, src, ptr + 4);
        if (index >= 4) {
            decode_bit(range, code, &i, src, ptr + 4);
            if (index >= 5) {
                decode_range(range, code, src);
                for (; index >= 5; index--) {
                    i <<= 1;
                    (*range) >>= 1;
                    if (*code < *range) i++;
                    else (*code) -= *range;
                }
            }
        }
    }
    *bit_flag = decode_bit(range, code, &i, src, ptr);
    if (index >= 1) {
        decode_bit(range, code, &i, src, ptr + 1);
        if (index >= 2) decode_bit(range, code, &i, src, ptr + 2);
    }
    return i;
}

} // namespace

bool npd_aes_cmac(const u8* key, const u8* in, usize size, u8* out)
{
    return crypto::aes_cmac(key16(key), in, size, out);
}

int npd_decompress(u8* out, const u8* in_bytes, usize in_size, u32 size)
{
    if (in_size < 5) return -1;
    LzInput in{in_bytes, in_bytes + in_size};
    u8* start = out;
    const u8* end = out + size;
    const u8 head = in_bytes[0];
    u32 range = 0xFFFFFFFFu;
    u32 code = ps_ld32(in_bytes + 1);

    if (head > 0x80) {
        // The data is not compressed: the length, then the bytes.
        if (code > size || code > in_size - 5) return -1;
        std::memcpy(out, in_bytes + 5, code);
        return static_cast<int>(code);
    }

    int offset = 0;
    int bit_flag = 0;
    u8 prev = 0;
    std::vector<u8> window(0xCC8); // the sliding window's statistics
    u8* tmp = window.data();
    std::memset(tmp, 0x80, 0xCA8);
    while (true) {
        u8* sect1 = tmp + offset + 0xB68;
        if (!decode_bit(&range, &code, static_cast<int*>(nullptr), in, sect1)) { // a plain byte
            if (offset > 0) offset--;
            if (start == end) return static_cast<int>(start - out);
            const u32 context = (((static_cast<u32>(start - out) & 7) << 8) + prev);
            const int sect = static_cast<int>(((head < 32 ? context >> head : 0) & 7) * 0xFF) - 1;
            sect1 = tmp + sect;
            int index = 1;
            do {
                decode_bit(&range, &code, &index, in, sect1 + index);
            } while ((index >> 8) == 0);
            *start++ = static_cast<u8>(index);
        } else { // a copy of earlier bytes
            int index = -1;
            do {
                sect1 += 8;
                bit_flag = decode_bit(&range, &code, static_cast<int*>(nullptr), in, sect1);
                index += bit_flag;
            } while (bit_flag != 0 && index < 6);

            int b_size = 0x160;
            u8* sect2 = tmp + index + 0x7F1;
            int data_length = 0;
            if (index >= 0 || bit_flag != 0) {
                const int sect = (index << 5) | (((static_cast<int>(start - out) << index) & 3) << 3) | (offset & 7);
                const u32 n = decode_number(tmp + 0xBA8 + sect, index, &bit_flag, &range, &code, in);
                if (n == 0xFF) return static_cast<int>(start - out); // the end of the stream
                if (n > size) return -1;
                data_length = static_cast<int>(n);
            } else {
                data_length = 1;
            }
            if (data_length <= 2) {
                sect2 += 0xF8;
                b_size = 0x40;
            }

            int diff = 0;
            int shift = 1;
            do {
                diff = (shift << 4) - b_size;
                bit_flag = decode_bit(&range, &code, &shift, in, sect2 + (shift << 3));
            } while (diff < 0);

            int data_offset = 0;
            if (diff > 0 || bit_flag != 0) {
                if (bit_flag == 0) diff -= 8;
                const u32 n = decode_word(tmp + 0x928 + diff, diff, &bit_flag, &range, &code, in);
                if (n > size) return -1;
                data_offset = static_cast<int>(n);
            } else {
                data_offset = 1;
            }

            if (data_offset <= 0 || data_offset > start - out) return -1; // before the start
            if (data_length + 1 > end - start) return -1;              // past the end
            const u8* from = start - data_offset;
            const u8* buf_end = start + data_length + 1;
            offset = ((static_cast<int>(buf_end - out) + 1) & 1) + 6;
            do {
                *start++ = *from++;
            } while (start < buf_end);
        }
        prev = *(start - 1);
    }
}

std::optional<NpdHeader> parse_npd_header(std::span<const u8> bytes)
{
    if (bytes.size() < kNpdHeaderBytes || std::memcmp(bytes.data(), "NPD\0", 4) != 0) return std::nullopt;
    const u8* h = bytes.data();
    NpdHeader n;
    n.version = static_cast<s32>(ps_ld32(h + 4));
    n.license = static_cast<s32>(ps_ld32(h + 8));
    n.type = static_cast<s32>(ps_ld32(h + 12));
    n.content_id.assign(reinterpret_cast<const char*>(h + 0x10), strnlen(reinterpret_cast<const char*>(h + 0x10), 0x30));
    std::memcpy(n.digest.data(), h + 0x40, 16);
    std::memcpy(n.title_hash.data(), h + 0x50, 16);
    std::memcpy(n.dev_hash.data(), h + 0x60, 16);
    n.activate_time = static_cast<s64>(ps_ld64(h + 0x70));
    n.expire_time = static_cast<s64>(ps_ld64(h + 0x78));
    n.flags = ps_ld32(h + 0x80);
    n.block_size = ps_ld32(h + 0x84);
    n.file_size = ps_ld64(h + 0x88);
    return n;
}

bool npd_klicensee_matches(std::span<const u8> bytes, const Key128& klicensee, const NpdrmKeys& keys)
{
    // RPCS3 validate_dev_klic: the header's first 0x60 bytes as the file has them (its integers big-endian).
    if (bytes.size() < 0x70) return false;
    if ((static_cast<s32>(ps_ld32(bytes.data() + 8)) & 3) != 3) return true;
    Key128 key{};
    for (usize i = 0; i < key.size(); i++) key[i] = static_cast<u8>(klicensee[i] ^ keys.omac_key2[i]);
    u8 mac[16]{};
    return crypto::aes_cmac(key, bytes.data(), 0x60, mac) && std::memcmp(mac, bytes.data() + 0x60, 16) == 0;
}

bool npd_hashes_valid(std::span<const u8> bytes, std::string_view file_name, const Key128& klicensee,
    const NpdrmKeys& keys)
{
    if (bytes.size() < kNpdHeaderBytes) return false;
    if (ps_ld32(bytes.data() + 0x80) & kNpdDebugDataFlag) return true; // RPCS3: no header checks for debug data
    if (!npd_klicensee_matches(bytes, klicensee, keys)) return false;
    if (file_name.empty()) return true;
    // The content ID's 0x30 bytes and the name; the extension (from the name's last '.', or the content ID's last
    // byte when there is none, as RPCS3's search stops there) lowered and raised for the two other tries.
    std::vector<u8> as_is(0x30 + file_name.size());
    std::memcpy(as_is.data(), bytes.data() + 0x10, 0x30);
    std::memcpy(as_is.data() + 0x30, file_name.data(), file_name.size());
    usize from = 0x2F;
    for (usize i = as_is.size(); i > 0x30; i--) {
        if (as_is[i - 1] == '.') {
            from = i - 1;
            break;
        }
    }
    std::vector<u8> lower = as_is, upper = as_is;
    for (usize i = from; i < as_is.size(); i++) {
        const u8 c = as_is[i];
        lower[i] = c >= 'A' && c <= 'Z' ? static_cast<u8>(c + 32) : c;
        upper[i] = c >= 'a' && c <= 'z' ? static_cast<u8>(c - 32) : c;
    }
    const auto title_hash_of = [&](const std::vector<u8>& v) {
        u8 mac[16]{};
        return crypto::aes_cmac(keys.omac_key3, v.data(), v.size(), mac) &&
            std::memcmp(mac, bytes.data() + 0x50, 16) == 0;
    };
    return title_hash_of(as_is) || title_hash_of(lower) || title_hash_of(upper);
}

Result<std::unique_ptr<NpdFile>> NpdFile::open(ReadAt source, u64 source_size, const NpdKeys& keys,
    const std::optional<Key128>& edata_key, const NpdrmKeys& npdrm)
{
    u8 h[kNpdHeaderBytes]{};
    if (source_size < kHeaderSize) return make_error("{} bytes: too small for an NPD file", source_size);
    if (!source(0, h, sizeof(h))) return make_error("reading the header failed");
    const std::optional<NpdHeader> header = parse_npd_header(h);
    if (!header) return make_error("not an NPD file");

    std::unique_ptr<NpdFile> f(new NpdFile);
    NpdHeader& n = f->header_;
    n = *header;
    const bool sdat = n.flags & kNpdSdatFlag;
    if (!sdat && !edata_key) {
        return make_error("an EDATA file (content {}, licence {}): it opens with the game's licence key, and none was "
                          "given", n.content_id, n.license);
    }
    if (!keys.present) {
        return make_error("an {} file, and this build has no NPD keys (built without RPCS3's source)",
            sdat ? "SDATA" : "EDATA");
    }
    if (!sdat && !(n.flags & kNpdDebugDataFlag) && (n.license & 3) == 3) {
        // RPCS3 ReadHeader: a klicensee that does not match the header decrypts nothing.
        if (!npdrm.present) return make_error("a free EDATA file, and this build has no NPDRM keys to check its klicensee");
        if (!npd_klicensee_matches(h, *edata_key, npdrm))
            return make_error("the klicensee is not the one this EDATA file (content {}) was made with", n.content_id);
    }
    if (n.file_size && (n.block_size == 0 || n.block_size > (16u << 20)))
        return make_error("block size {:#x} for {} bytes", n.block_size, n.file_size);

    f->source_ = std::move(source);
    f->source_size_ = source_size;
    f->keys_ = keys;
    if (sdat) {
        for (int i = 0; i < 16; i++) f->key_[i] = static_cast<u8>(n.dev_hash[i] ^ keys.sdat[i]); // RPCS3 ReadHeader
    } else {
        f->key_ = *edata_key;
    }
    const u64 blocks = n.block_size ? (n.file_size + n.block_size - 1) / n.block_size : 0;
    if (blocks > 0xFFFFFFFFull) return make_error("{} blocks", blocks);
    f->total_blocks_ = static_cast<u32>(blocks);
    f->cached_.resize(n.block_size + 16);

    // As RPCS3: the first block must decrypt.
    std::string error;
    u8 sample = 0;
    if (n.file_size && !f->read(0, &sample, 1, &error)) return make_error("the first block: {}", error);
    return f;
}

s64 NpdFile::decrypt_block(u32 block, std::vector<u8>& out, std::string& error) const
{
    const NpdHeader& n = header_;
    const bool compressed = n.flags & kNpdCompressedFlag;
    const bool flag20 = n.flags & kNpdFlag20;
    const u64 meta_size = (compressed || flag20) ? 0x20 : 0x10;
    const bool last = block == total_blocks_ - 1;
    const u64 rest = n.file_size % n.block_size;

    u8 expected[0x14]{};
    u64 offset = 0, length = 0;
    s32 compression_end = 0;
    u8 m[0x20]{};
    if (compressed) {
        if (!source_(kMetadataOffset + u64{block} * meta_size, m, 0x20)) {
            error = "reading the block's metadata failed";
            return -1;
        }
        // NPD version 1 keeps the metadata plain.
        if (n.version <= 1) {
            offset = ps_ld64(m + 0x10);
            length = static_cast<u64>(static_cast<s64>(static_cast<s32>(ps_ld32(m + 0x18))));
            compression_end = static_cast<s32>(ps_ld32(m + 0x1C));
        } else {
            dec_section(m, offset, length, compression_end);
        }
        std::memcpy(expected, m, 0x10);
    } else if (flag20) {
        // The metadata right before each block; the hash is its two halves XORed.
        const u64 at = kMetadataOffset + u64{block} * (meta_size + n.block_size);
        if (!source_(at, m, 0x20)) {
            error = "reading the block's metadata failed";
            return -1;
        }
        std::memcpy(expected, m, 0x14);
        for (int j = 0; j < 0x10; j++) expected[j] = static_cast<u8>(m[j] ^ m[j + 0x10]);
        offset = at + 0x20;
        length = last && rest ? rest : n.block_size;
    } else {
        if (!source_(kMetadataOffset + u64{block} * meta_size, expected, 0x10)) {
            error = "reading the block's metadata failed";
            return -1;
        }
        offset = kMetadataOffset + u64{block} * n.block_size + u64{total_blocks_} * meta_size;
        length = last && rest ? rest : n.block_size;
    }
    if (length > 2 * u64{n.block_size} + 0x40) { // a compressed block holding plain bytes is a little larger
        error = std::format("block {}: length {:#x} in a file of {:#x}-byte blocks", block, length, n.block_size);
        return -1;
    }

    const usize pad_length = static_cast<usize>(length);
    const usize aligned = (pad_length + 15) & ~usize{15};
    std::vector<u8> enc(aligned), dec(aligned);
    // The last block's padding may be missing from the file: what is there is read, the rest stays zero.
    const u64 available = offset < source_size_ ? std::min<u64>(aligned, source_size_ - offset) : 0;
    if (available < pad_length || !source_(offset, enc.data(), static_cast<usize>(available))) {
        error = std::format("block {} at {:#x} ({:#x} bytes) is past the file's end", block, offset, pad_length);
        return -1;
    }

    // The block's key (RPCS3 get_block_key): the dev hash's first 12 bytes (zero for NPD version 0 and 1) and the
    // block number, encrypted with the file's key.
    u8 block_key[16]{};
    if (n.version > 1) std::memcpy(block_key, n.dev_hash.data(), 12);
    ps_st32(block_key + 12, block);
    u8 key_result[16]{}, hash_key[16]{};
    if (!crypto::aes_ecb_encrypt(key_, block_key, 16, key_result) ||
        ((n.flags & kNpdFlag10) && !crypto::aes_ecb_encrypt(key_, key_result, 16, hash_key))) {
        error = "AES-128-ECB failed";
        return -1;
    }
    if (!(n.flags & kNpdFlag10)) std::memcpy(hash_key, key_result, 16);

    u32 crypto_mode = (n.flags & kNpdFlag02) == 0 ? 0x2 : 0x1;
    u32 hash_mode = (n.flags & kNpdFlag10) == 0 ? 0x02 : (n.flags & kNpdFlag20) == 0 ? 0x04 : 0x01;
    if (n.flags & kNpdEncryptedKeyFlag) {
        crypto_mode |= 0x10000000;
        hash_mode |= 0x10000000;
    }
    const bool should_decompress = compressed && compression_end;
    if (n.flags & kNpdDebugDataFlag) {
        dec = enc; // stored plain, unchecked
    } else {
        const u8 zero_iv[16]{};
        const u8* iv = n.version <= 1 ? zero_iv : n.digest.data();
        if (!decrypt(keys_, hash_mode, crypto_mode, n.version == 4, enc.data(), dec.data(), aligned, key_result, iv,
                hash_key, expected, error)) {
            error = std::format("block {} at {:#x}: {}", block, offset, error);
            return -1;
        }
    }

    if (should_decompress) {
        const int res = npd_decompress(out.data(), dec.data(), dec.size(), n.block_size);
        if (res < 0) {
            error = std::format("block {} at {:#x} does not decompress", block, offset);
            return -1;
        }
        return res;
    }
    // A compressed file's block that is not compressed after all (its compression end 0) may say it is longer than a
    // block: RPCS3 stops there (its ensure that the output holds it); pspocket refuses the block (D-605).
    if (pad_length > out.size()) {
        error = std::format("block {} at {:#x}: {:#x} bytes, more than a block", block, offset, pad_length);
        return -1;
    }
    if (pad_length) std::memcpy(out.data(), dec.data(), pad_length);
    return static_cast<s64>(pad_length);
}

// RPCS3 EDATADecrypter::ReadData, asked for whole ranges.
bool NpdFile::read(u64 offset, void* dst, usize size, std::string* error) const
{
    std::string why;
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (!size) return true;
    const NpdHeader& n = header_;
    if (offset > n.file_size || size > n.file_size - offset)
        return fail(std::format("{:#x} bytes at {:#x} in a file of {:#x}", size, offset, n.file_size));

    std::lock_guard lock(mutex_);
    u8* to = static_cast<u8*>(dst);
    usize written = 0;
    const u64 start_offset = offset % n.block_size;
    const u64 num_blocks = (start_offset + size + n.block_size - 1) / n.block_size;
    const u32 first = static_cast<u32>(offset / n.block_size);
    const u32 end_block = static_cast<u32>(std::min<u64>(first + num_blocks, total_blocks_));
    for (u32 i = first; i < end_block; i++) {
        if (cached_block_ != i) {
            cached_block_ = ~0u;
            cached_size_ = decrypt_block(i, cached_, why);
            if (cached_size_ < 0) return fail(why);
            cached_block_ = i;
        }
        const usize res = static_cast<usize>(cached_size_);
        const usize skip = i == first ? static_cast<usize>(start_offset) : 0;
        if (skip >= res) break;
        const usize end_pos = i != total_blocks_ - 1 ? n.block_size : static_cast<usize>((n.file_size - 1) % n.block_size + 1);
        const usize read_end = std::min<usize>(res, i == end_block - 1
                ? std::min<usize>(end_pos, static_cast<usize>((start_offset + size - 1) % n.block_size + 1))
                : end_pos);
        std::memcpy(to + written, cached_.data() + skip, read_end - skip);
        written += read_end - skip;
    }
    if (written != size) return fail(std::format("{:#x} of {:#x} bytes at {:#x} came out", written, size, offset));
    return true;
}

} // namespace pspocket::recomp
