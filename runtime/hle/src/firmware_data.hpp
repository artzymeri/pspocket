#pragma once

// Data a firmware module carries that pspocket's own library needs and does not copy into its source (D-526, D-913):
// read at run time from the user's installed system software. The module (/dev_flash/sys/external/<file>) is decrypted
// with the build's SELF keys, as the loader's modules are, and accepted only when it is the module whose layout the
// caller knows (its identity, recomp::prx_identity); the bytes at a virtual address of its image are then given back.
// Nothing is kept but what was read; a module missing, unreadable or of another firmware gives nothing (the caller
// falls back and says so).

#include <optional>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle {

class HleProcess;

// `size` bytes at virtual address `vaddr` of the module `file`'s image, or nothing. Read once per process and module.
std::optional<std::vector<u8>> firmware_module_data(HleProcess& process, std::string_view file,
    std::string_view identity, u64 vaddr, u64 size);

} // namespace pspocket::hle
