#pragma once

// The host's side of a game library (route B, D-152; pspocket/game/library.h): the runtime table it gives the
// library, and loading one for the executable in guest memory.

#include <cstdint>
#include <filesystem>
#include <string>

#include "pspocket/core/types.hpp"
#include "pspocket/game/library.h"

namespace pspocket {
class GuestMemory;
}

extern "C" {
// Made by the build from the ABI headers and the lifters' sources (runtime/game/CMakeLists.txt).
extern const uint64_t ps_game_abi_hash;
extern const uint64_t ps_game_translator_hash;
}

namespace pspocket::game {

// The runtime every game library binds to: this process's functions and tables.
const ps_game_runtime& host_runtime();

struct LoadedLibrary {
    const ps_game_library* library = nullptr;
    std::string path;
    bool translator_current = false; // translated by the lifters of this build
};

struct LoadOptions {
    // The SHA-256 its executable_sha256 must hold (32 bytes), or none. The system library's (D-178): what it was made
    // from (LleModules::identity_text).
    const u8* executable_sha256 = nullptr;
    // Check its D-098 samples against guest memory. Off for the system library: its modules are not in memory yet,
    // and each is checked as it loads (loader::SystemLibrary).
    bool check_image_words = true;
};

// The copies load_library made for processes that have ended, removed from `dir` (the temporary folder): how many.
size_t remove_stale_copies(const std::filesystem::path& dir);

// Loads a game library: copies it to a new file in the temporary folder (the kernel keeps the first signature it saw of
// a file; the copies of ended processes are removed first), registers its signature, dlopen, binds it to host_runtime(), checks it was made for this runtime (layout, ABI hash) and for the
// executable now in guest memory (its D-098 samples), then registers its PPU functions and SPU programs. Never
// unloaded. On refusal returns false with the reason in *error, and nothing is registered.
bool load_library(const std::string& path, const GuestMemory& memory, LoadedLibrary* out, std::string* error,
    const LoadOptions& options = {});

} // namespace pspocket::game
