#pragma once

// NPD files: game data Sony encrypted in blocks (D-165). A disc game keeps some of its files as SDATA ("NPD\0" with
// the SDAT flag; opened with cellFsSdataOpen, or sys_fs_open's 8-byte type argument 0x18000000010); content from PSN
// uses EDATA, which needs the game's licence key. As RPCS3's EDATADecrypter (rpcs3/Crypto/unedat.cpp: ReadHeader,
// ReadData, decrypt_block, decrypt, generate_key, generate_hash, dec_section, get_block_key) with its LZ decoder
// (rpcs3/Crypto/lz.cpp, Hykem, GPLv2 or later):
// - The file is a 0x100-byte header (NPD header at 0, the EDAT header's flags, block size and plain size at 0x80),
//   then per block a hash (0x10 bytes; 0x20 with offsets when compressed or flag 0x20) and the encrypted blocks.
// - An SDAT file's key is its header's dev hash XOR SDAT_KEY. Each block's key is that key's AES-128 encryption of
//   the dev hash's first 12 bytes and the block number; the block is AES-128-CBC with the header's digest as IV, and
//   carries an AES-CMAC or HMAC-SHA1 of its encrypted bytes, which is checked.
// - An EDATA file's key is its content key (D-581): the klicensee of free content, the RIF key of the player's RAP
//   otherwise (recomp::npdrm_content_key). Its header carries two hashes RPCS3 checks (validate_npd_hashes): the dev
//   hash, keyed with the klicensee, for free content, and the title hash over its content ID and file name.
// The keys (key_vault.h SDAT_KEY, EDAT_KEY_0/1, EDAT_HASH_0/1, EDAT_IV) come from the build like the SELF keys
// (scripts/self-keys.py; never committed).

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/npdrm.hpp"
#include "pspocket/recomp/result.hpp"

namespace pspocket::recomp {

struct NpdKeys {
    std::array<u8, 16> sdat{};
    std::array<u8, 16> edat_key0{};
    std::array<u8, 16> edat_key1{};
    std::array<u8, 16> edat_hash0{};
    std::array<u8, 16> edat_hash1{};
    std::array<u8, 16> edat_iv{};
    bool present = false;
};

// The build's NPD keys; `present` is false in a build without them (no RPCS3 source when it was configured).
const NpdKeys& builtin_npd_keys();

// RPCS3 unedat.h flags (the EDAT header's).
inline constexpr u32 kNpdSdatFlag = 0x01000000;
inline constexpr u32 kNpdCompressedFlag = 0x00000001;
inline constexpr u32 kNpdFlag02 = 0x00000002;       // blocks are not encrypted
inline constexpr u32 kNpdEncryptedKeyFlag = 0x00000008;
inline constexpr u32 kNpdFlag10 = 0x00000010;       // HMAC-SHA1 instead of AES-CMAC, the hash key encrypted again
inline constexpr u32 kNpdFlag20 = 0x00000020;       // each block's hash right before it
inline constexpr u32 kNpdDebugDataFlag = 0x80000000; // blocks stored plain

// The NPD and EDAT headers (RPCS3 NPD_HEADER and EDAT_HEADER), big-endian at 0 and 0x80.
struct NpdHeader {
    s32 version = 0;
    s32 license = 0;
    s32 type = 0;
    std::string content_id;
    std::array<u8, 16> digest{};
    std::array<u8, 16> title_hash{};
    std::array<u8, 16> dev_hash{};
    s64 activate_time = 0; // milliseconds since 1970; 0: no limit
    s64 expire_time = 0;
    u32 flags = 0;
    u32 block_size = 0;
    u64 file_size = 0; // the plain size
};

inline constexpr usize kNpdHeaderBytes = 0x90; // the NPD header and the EDAT header

// The headers of an NPD file's first kNpdHeaderBytes (RPCS3 read_npd_edat_header); nullopt when fewer bytes are given
// or they do not start "NPD\0".
std::optional<NpdHeader> parse_npd_header(std::span<const u8> bytes);

// RPCS3 validate_dev_klic: whether `klicensee` is the one a free NPD file (licence type 3 in its low two bits) was
// made with, i.e. whether its dev hash (0x60) is the AES-CMAC of its first 0x60 bytes keyed with the klicensee XOR
// NP_OMAC_KEY_2. Any other licence type: true, there is nothing to check. False for fewer than 0x70 bytes.
bool npd_klicensee_matches(std::span<const u8> bytes, const Key128& klicensee, const NpdrmKeys& keys);

// RPCS3 validate_npd_hashes, for an EDATA file: true for debug data; else npd_klicensee_matches and, unless
// `file_name` is empty, the title hash (0x50): the AES-CMAC with NP_OMAC_KEY_3 of the content ID's 0x30 bytes and the
// name the file was made with (no folders), as it is, or with its extension (from its last '.', or the whole name
// when it has none) in lower or in upper case. False for fewer than kNpdHeaderBytes.
bool npd_hashes_valid(std::span<const u8> bytes, std::string_view file_name, const Key128& klicensee,
    const NpdrmKeys& keys);

class NpdFile {
public:
    // Reads exactly `size` bytes at `offset` of the encrypted file; false on an I/O error.
    using ReadAt = std::function<bool(u64 offset, void* dst, usize size)>;

    // An NPD file of `source_size` bytes. An SDAT file opens with its own key. An EDATA file opens with `edata_key`,
    // its content key, as RPCS3's EDATADecrypter given a final key (sys_fs's open, ReadHeader): unless it is debug
    // data, a free file's dev hash must match the klicensee (npd_klicensee_matches, with `npdrm`'s NP_OMAC_KEY_2);
    // without a key it is refused. Either kind is refused when its first block does not decrypt. Errors name what
    // failed.
    static Result<std::unique_ptr<NpdFile>> open(ReadAt source, u64 source_size, const NpdKeys& keys,
        const std::optional<Key128>& edata_key = std::nullopt, const NpdrmKeys& npdrm = builtin_npdrm_keys());

    const NpdHeader& header() const { return header_; }
    u64 size() const { return header_.file_size; }

    // Exactly `size` plain bytes at `offset` (offset + size <= size()). False when the source cannot be read or a
    // block fails its hash or does not decompress; `error` (if given) then says which.
    bool read(u64 offset, void* dst, usize size, std::string* error = nullptr) const;

private:
    NpdFile() = default;
    // One block's plain bytes into `out` (block_size + 16 bytes): their number, or -1 (RPCS3 decrypt_block).
    s64 decrypt_block(u32 block, std::vector<u8>& out, std::string& error) const;

    ReadAt source_;
    u64 source_size_ = 0;
    NpdKeys keys_;
    NpdHeader header_;
    std::array<u8, 16> key_{};
    u32 total_blocks_ = 0;

    // The last block decrypted: games read these files in small pieces.
    mutable std::mutex mutex_;
    mutable u32 cached_block_ = ~0u;
    mutable s64 cached_size_ = 0;
    mutable std::vector<u8> cached_;
};

// AES-CMAC (RFC 4493), as RPCS3 aes.cpp aes_cmac: the hash of the blocks of files without flag 0x10
// (crypto::aes_cmac with a 16-byte key).
bool npd_aes_cmac(const u8* key, const u8* in, usize size, u8* out);

// RPCS3 lz.cpp decompress: one compressed block (`in`, `in_size` bytes) into at most `size` bytes of `out`. The
// number of bytes written, or -1. A block whose first byte is above 0x80 holds its bytes plain (a 32-bit big-endian
// length, then the bytes); RPCS3 copies them and returns 0, here their number.
int npd_decompress(u8* out, const u8* in, usize in_size, u32 size);

} // namespace pspocket::recomp
