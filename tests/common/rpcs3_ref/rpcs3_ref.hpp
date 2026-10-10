#pragma once

// RPCS3's own NPDRM and package code, for differential tests (D-580, D-583): the build compiles RPCS3's PolarSSL AES
// and SHA-1 (rpcs3/Crypto/aes.cpp, aesni.cpp, sha1.cpp, lz.cpp) as they are, and the functions
// scripts/rpcs3-extract.py copies out of key_vault.cpp, utils.cpp, unedat.cpp, unself.cpp and unpkg.cpp, with RPCS3's
// keys from its key_vault.h, into the test-only library pspocket_rpcs3_ref, when PSPOCKET_RPCS3_DIR names RPCS3's
// source (PSPOCKET_HAVE_RPCS3_REF is then defined). Nothing of it is in git: the library is built from the RPCS3 tree
// in external/.

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/npd.hpp"
#include "pspocket/recomp/npdrm.hpp"
#include "pspocket/recomp/pkg.hpp"

namespace pspocket::test::rpcs3_ref {

using Key = std::array<u8, 16>;

// Whether the build's keys (scripts/self-keys.py) are key_vault.h's.
bool same_keys(const recomp::NpdrmKeys& npdrm, const recomp::NpdKeys& npd);

// key_vault.cpp rap_to_rif.
Key rap_to_rif(const Key& rap);

// unedat.cpp decrypt_block on block `block` of the NPD file `file` (its headers read by read_npd_edat_header) with
// the file's key (EDATADecrypter's dec_key): the block's plain bytes, or nullopt where RPCS3 answers -1.
std::optional<std::vector<u8>> decrypt_block(std::span<const u8> file, u32 block, const Key& key);

// unedat.cpp validate_dev_klic and validate_npd_hashes over the file's headers.
bool validate_dev_klic(std::span<const u8> file, const Key& klicensee);
bool validate_npd_hashes(std::span<const u8> file, std::string_view file_name, const Key& klicensee);

// unself.cpp SELFDecrypter::DecryptNPDRM on a SELF's metadata info (`size` bytes, decrypted in place) for an NPD
// header of this licence type and content ID, with KeyVault's klicensee and the player's RAPs (GetKeyFromRap reads
// them; nullopt: no file). False where RPCS3 fails.
bool decrypt_npdrm(u8* info, u32 size, s32 license, std::string_view content_id, const Key& klicensee,
    const std::function<std::optional<Key>(std::string_view)>& rap);

// PolarSSL's AES, AES-CMAC and HMAC-SHA1 as RPCS3 carries them.
void aes_ecb(bool encrypt, std::span<const u8> key, const u8* in, u8* out);
void aes_cbc(bool encrypt, std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out);
void aes_ctr(std::span<const u8> key, const u8* iv, const u8* in, usize size, u8* out);
void aes_cmac(const Key& key, const u8* in, usize size, u8* out);
void hmac_sha1(std::span<const u8> key, const u8* in, usize size, u8* out);

// Whether the build's package keys are key_vault.h's PKG_AES_KEY and PKG_AES_KEY_IDU.
bool same_pkg_keys(const recomp::PkgKeys& keys);

// unpkg.cpp package_reader::decrypt over a whole package in memory (its header read as RPCS3's PKGHeader): `size`
// bytes of its data at `offset` (on a 16-byte boundary, as RPCS3 reads them), decrypted with `key` (a debug package
// needs none); nullopt where RPCS3 gives fewer bytes than asked.
std::optional<std::vector<u8>> pkg_decrypt(std::span<const u8> package, u64 offset, u64 size, const Key& key);

// package_reader::read_entries with `key`: whether RPCS3 finds the package's table sound (set_decryption_key tries
// PKG_AES_KEY, then PKG_AES_KEY_IDU, by it).
bool pkg_table_sound(std::span<const u8> package, const Key& key);

// unpkg.cpp leaves_directory: whether an entry's name would leave the install folder.
bool pkg_leaves_directory(std::string_view name);

} // namespace pspocket::test::rpcs3_ref
