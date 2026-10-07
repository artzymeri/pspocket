#pragma once

// What prepare_game and the SPU libraries of a session (session.cpp, D-236) share: files whole or not at all, and a
// translated C file as a build job.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/codegen/build.hpp"

namespace pspocket::prepare::detail {

bool read_bytes(const std::filesystem::path& p, std::string& out);
// Beside the destination, then renamed: a file is there whole or not at all.
bool write_bytes(const std::filesystem::path& p, const void* data, size_t size);
uint64_t fnv(std::string_view text);
// The headers beside a file (the game's prototypes): part of its object's name, so a unit is compiled again when they
// change though its own text did not.
std::string headers_define(const std::filesystem::path& dir);
codegen::BuildJob job_for(const std::filesystem::path& source, bool spu, const std::string& headers,
    const std::vector<std::string>& ppu_args, const std::vector<std::string>& spu_args);

} // namespace pspocket::prepare::detail
