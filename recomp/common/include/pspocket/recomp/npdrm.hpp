#pragma once

// The PSN licence layer, NPDRM (D-580): what opens a PSN game's executables (NPDRM SELFs, D-582) and its EDATA files
// (D-581). Such content carries an NPD header naming its content ID ("UP0001-NPUB30000_00-0000000000000001") and its
// licence type: network (1) and local (2) content needs the player's licence for that content ID, and free content
// (3) opens with a klicensee, the key the game itself holds and shows the system through sceNpDrmIsAvailable
// (NP_KLIC_FREE when it shows none).
//
// The player's licence is a RAP file: 16 bytes, named <CONTENT_ID>.rap (what the PS3 keeps as a .rif and act.dat,
// as the dumping tools give it). rap_to_rif turns it into the RIF key, the content's key. pspocket looks for it where
// RPCS3 does, in /dev_hdd0/home/00000001/exdata (RPCS3 looks in every user's folder; pspocket has the one user,
// 00000001); the iOS app is to offer to put a player's .rap files there (ps_psn_install_rap, pspocket/disc/psn.h).
//
// As RPCS3: rpcs3/Crypto/key_vault.cpp rap_to_rif; Crypto/unself.cpp SELFDecrypter::DecryptNPDRM and GetKeyFromRap
// (which key a licence type takes); Crypto/unedat.cpp EDATADecrypter::ReadHeader; Emu/system_utils.cpp
// is_valid_content_id and get_rap_file_path. The keys (key_vault.h RAP_KEY, RAP_PBOX, RAP_E1, RAP_E2, NP_KLIC_FREE,
// NP_KLIC_KEY, NP_OMAC_KEY_2, NP_OMAC_KEY_3) come from the build like the SELF keys (scripts/self-keys.py; never
// committed).

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/result.hpp"

namespace pspocket::recomp {

using Key128 = std::array<u8, 16>;

struct NpdrmKeys {
    Key128 rap_key{};   // RAP_KEY: the RAP's AES-128 layer
    Key128 rap_pbox{};  // RAP_PBOX, RAP_E1, RAP_E2: rap_to_rif's five rounds
    Key128 rap_e1{};
    Key128 rap_e2{};
    Key128 klic_free{}; // NP_KLIC_FREE: the klicensee of free content that names none
    Key128 klic_key{};  // NP_KLIC_KEY: an NPDRM SELF's content key is decrypted with it
    Key128 omac_key2{}; // NP_OMAC_KEY_2: an EDAT's dev hash, keyed with the klicensee
    Key128 omac_key3{}; // NP_OMAC_KEY_3: an EDAT's title hash, over its content ID and file name
    bool present = false;
};

// The build's NPDRM keys; `present` is false in a build without them (no RPCS3 source when it was configured).
const NpdrmKeys& builtin_npdrm_keys();

// Licence types, the NPD header's `license`.
inline constexpr s32 kNpdrmLicenseNetwork = 1;
inline constexpr s32 kNpdrmLicenseLocal = 2;
inline constexpr s32 kNpdrmLicenseFree = 3;

// RPCS3 rap_to_rif: the RIF key of a RAP licence (an AES-128-CBC decryption with RAP_KEY and a zero IV, then five
// rounds of RAP_PBOX's permutation with RAP_E1 and RAP_E2).
Key128 rap_to_rif(const Key128& rap, const NpdrmKeys& keys);

// RPCS3 is_valid_content_id: 1 to 0x30 characters of A-Z, a-z, 0-9, '-' and '_'.
bool valid_content_id(std::string_view id);

// Where the player's licences are, as a guest path, and a content ID's RAP there ("" for an invalid content ID).
inline constexpr std::string_view kLicenceDir = "/dev_hdd0/home/00000001/exdata";
std::string rap_guest_path(std::string_view content_id);
// The same RAP under a host folder that is /dev_hdd0 (pspocket-boot's --hdd0, the app's Documents/hdd0).
std::string rap_host_path(std::string_view hdd0_dir, std::string_view content_id);

// A RAP licence's 16 bytes from a host file. RPCS3 GetKeyFromRap's two failures, as errors: no file ("no licence
// ..."), and one shorter than 16 bytes. A longer file gives its first 16 bytes, as RPCS3 reads it.
Result<Key128> read_rap_file(const std::string& host_path);

// How a decrypter finds the key of a piece of NPDRM content.
struct NpdrmLicence {
    // Free content's klicensee; none: NP_KLIC_FREE (RPCS3 KeyVault's default).
    std::optional<Key128> klicensee;
    // The RAP licence of a content ID (16 bytes), or an error that says where it was looked for. Unset: no licences.
    std::function<Result<Key128>(std::string_view content_id)> rap;
    // The keys; nullptr: the build's.
    const NpdrmKeys* keys = nullptr;

    const NpdrmKeys& npdrm_keys() const { return keys ? *keys : builtin_npdrm_keys(); }
};

// A licence whose RAPs are read from a host folder that is /dev_hdd0 (rap_host_path), with this klicensee.
NpdrmLicence hdd0_licence(std::string hdd0_dir, std::optional<Key128> klicensee = std::nullopt);

// What a content key is for: a SELF and an EDAT take their key from the licence type a little differently.
enum class NpdrmContent { self, edat };

// The key that opens NPDRM content of this licence type and content ID: the RIF key of the player's RAP, or the
// klicensee. A SELF (RPCS3 DecryptNPDRM, before its NP_KLIC_KEY layer): types 1 and 2 take the RAP, 3 the klicensee,
// any other type is refused. An EDAT (EDATADecrypter::ReadHeader): a type whose low two bits are 3 takes the
// klicensee, every other the RAP. Errors say what is missing: the build's keys, the RAP (and where it was looked for).
Result<Key128> npdrm_content_key(NpdrmContent kind, s32 license, std::string_view content_id,
    const NpdrmLicence& licence);

} // namespace pspocket::recomp
