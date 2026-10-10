#pragma once

// Decrypting a SELF (a disc game's EBOOT.BIN) into the ELF it carries, on the Mac and on the iPhone (D-121), as RPCS3's
// SELFDecrypter does (rpcs3/Crypto/unself.cpp: LoadHeaders, LoadMetadata, DecryptData, MakeElf): the metadata info
// opens with the AES-256-CBC key of the executable's type and key revision, the metadata with the AES-128-CTR key and
// counter it holds, and each section with its own; zlib sections inflate; the ELF is laid out from the SELF's copies
// of its headers. Keys come from the build (scripts/self-keys.py reads RPCS3's key_vault.cpp; never committed).
// Executables from PSN (an NPDRM control header: a PSN game's EBOOT.BIN and SPRX modules) have one more layer over the
// metadata info, opened with their licence (D-582, RPCS3 SELFDecrypter::DecryptNPDRM): given an NpdrmLicence they
// decrypt, without one they are refused.

#include <array>
#include <span>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/npdrm.hpp"
#include "pspocket/recomp/result.hpp"
#include "pspocket/recomp/sce.hpp"

namespace pspocket::recomp {

// One SELF key (RPCS3 key_vault.h SELF_KEY): what it opens, and the metadata info's AES-256-CBC key and IV.
struct SelfKey {
    u32 type = 0;             // SelfAppInfo::self_type: 1 LV0, 2 LV1, 3 LV2, 4 APP, 5 ISO, 6 LDR, 7 UNK7, 8 NPDRM
    u64 version_start = 0;    // the app info versions it covers (LV1, LV2, UNK7, ISO)
    u64 version_end = 0;
    u32 revision = 0;         // the SCE header's key revision (APP, ISO, NPDRM)
    std::array<u8, 32> erk{};
    std::array<u8, 16> riv{};
};

// The keys this build carries: scripts/self-keys.py's table, empty when the build had no RPCS3 source.
std::span<const SelfKey> builtin_self_keys();

// The first key for an executable of this type, key revision and version (RPCS3 KeyVault::FindSelfKey), or nullptr.
const SelfKey* find_self_key(std::span<const SelfKey> keys, u32 type, u32 revision, u64 version);

// The ELF inside a SELF. A PSN executable needs `licence`: its content's key comes from it (npdrm_content_key: the
// klicensee for free content, the RIF key of the player's RAP for its content ID otherwise). Errors name what failed:
// not a SELF, a PSN executable without a licence or whose licence is missing, no key, a key that does not open it
// (the build has no keys, the licence is for other content, or the file is damaged), a section that does not inflate.
Result<std::vector<u8>> decrypt_self(std::span<const u8> self, std::span<const SelfKey> keys,
    const NpdrmLicence* licence = nullptr);

// RPCS3 SELFDecrypter::DecryptNPDRM: the key of the AES-128-CBC layer (a zero IV) over a PSN executable's metadata
// info, the content key of its NPD header (npdrm_content_key for a SELF) decrypted with NP_KLIC_KEY (AES-128-ECB).
Result<Key128> npdrm_metadata_key(const NpdrmInfo& npd, const NpdrmLicence& licence);

// The key of the firmware update packages (RPCS3 key_vault.h SCEPKG_ERK, SCEPKG_RIV), from the build like the SELF
// keys; `present` is false in a build without them.
struct PackageKey {
    std::array<u8, 32> erk{};
    std::array<u8, 16> riv{};
    bool present = false;
};
const PackageKey& builtin_package_key();

// The sections of an SCE package (a firmware update's dev_flash packages, D-156), as RPCS3's SCEDecrypter makes them
// (LoadHeaders, LoadMetadata with the package key, DecryptData, MakeFile): each decrypted, and inflated when zlib.
Result<std::vector<std::vector<u8>>> decrypt_sce_package(std::span<const u8> package, const PackageKey& key);

} // namespace pspocket::recomp
