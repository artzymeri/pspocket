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
// The keys (key_vault.h SDAT_KEY, EDAT_KEY_0/1, EDAT_HASH_0/1, EDAT_IV) come from the build like the SELF keys
// (scripts/self-keys.py; never committed).

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pspocket/core/types.hpp"
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
    u32 flags = 0;
    u32 block_size = 0;
    u64 file_size = 0; // the plain size
};

class NpdFile {
public:
    // Reads exactly `size` bytes at `offset` of the encrypted file; false on an I/O error.
    using ReadAt = std::function<bool(u64 offset, void* dst, usize size)>;

    // An SDAT file of `source_size` bytes, opened with its own key. EDATA needs the game's licence key, which the
    // runtime does not keep yet: refused, as is anything that is not an NPD file or whose first block does not
    // decrypt. Errors name what failed.
    static Result<std::unique_ptr<NpdFile>> open(ReadAt source, u64 source_size, const NpdKeys& keys);

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
