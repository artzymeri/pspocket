#pragma once

// Decrypting a SELF (a disc game's EBOOT.BIN) into the ELF it carries, on the Mac and on the iPhone (D-121), as RPCS3's
// SELFDecrypter does (rpcs3/Crypto/unself.cpp: LoadHeaders, LoadMetadata, DecryptData, MakeElf): the metadata info
// opens with the AES-256-CBC key of the executable's type and key revision, the metadata with the AES-128-CTR key and
// counter it holds, and each section with its own; zlib sections inflate; the ELF is laid out from the SELF's copies
// of its headers. Keys come from the build (scripts/self-keys.py reads RPCS3's key_vault.cpp; never committed).
// Executables from PSN (an NPDRM control header) need their licence and are refused.

#include <array>
#include <span>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/result.hpp"

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

// The ELF inside a SELF. Errors name what failed: not a SELF, a PSN executable, no key, a key that does not open it
// (the build has no keys, or the file is damaged), a section that does not inflate.
Result<std::vector<u8>> decrypt_self(std::span<const u8> self, std::span<const SelfKey> keys);

} // namespace pspocket::recomp
