#pragma once

// libsysmodule's key for the export RPCS3 registers as cellSysmodule_0x59521326 (D-639): the console's OpenPSID, its
// halves swapped, is decrypted with it (AES-128-CBC). It comes from RPCS3's source at build time (scripts/self-keys.py
// reads KEY_UNK in rpcs3/Emu/Cell/Modules/cellSysmodule.cpp) into the build directory only, never into the repository
// (CLAUDE.md rule 1), like the SELF keys.

#include <array>

#include "pspocket/core/types.hpp"

namespace pspocket::recomp {

struct SysmoduleKey {
    std::array<u8, 16> key{};
    bool present = false; // false in a build without RPCS3's source: the export is then left unimplemented
};
const SysmoduleKey& builtin_sysmodule_key();

} // namespace pspocket::recomp
