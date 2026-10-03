#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/recomp/result.hpp"

// The per-game manifest (plan section 3, "Game manifest"; games/<title>/manifest.toml): which input files the
// build expects (by hash), function overrides, patches (data, never code: CLAUDE.md rule 6) and the SPU program
// table. Parsed from the TOML subset in toml.hpp and validated strictly: unknown keys, wrong types and malformed
// values are errors, so a typo can't silently drop a patch.
//
//   title_id = "BLES00229"            # 4 letters + 5 digits
//   title = "..."
//   app_version = "01.08"             # NN.NN
//   iso_sha256 = ""                   # 64 hex digits, or empty until known
//   [[files]]        path, sha256
//   [[overrides]]    addr (u32, 4-aligned), name (C identifier)
//   [[patches]]      addr (u32), bytes ("hex", spaces allowed), why (required, non-empty)
//   [[spu_programs]] sha1 (40 hex digits), name, action = "lift" | "replace" | "interpret"
namespace pspocket::recomp {

struct ManifestFile {
    std::string path;
    std::string sha256; // lowercase hex, or empty
};

struct FunctionOverride {
    u32 addr = 0;
    std::string name;
};

struct ManifestPatch {
    u32 addr = 0;
    std::vector<u8> bytes;
    std::string why;
};

enum class SpuAction : u8 { Lift, Replace, Interpret };

std::string_view spu_action_name(SpuAction action);

struct SpuProgramEntry {
    std::string sha1; // lowercase hex
    std::string name;
    SpuAction action = SpuAction::Lift;
};

struct GameManifest {
    std::string title_id;
    std::string title;
    std::string app_version;
    std::string iso_sha256;
    std::vector<ManifestFile> files;
    std::vector<FunctionOverride> overrides;
    std::vector<ManifestPatch> patches;
    std::vector<SpuProgramEntry> spu_programs;
};

struct ManifestOptions {
    // Build-time mode: every hash (iso_sha256, files[].sha256) must be filled in. Off for drafts.
    bool require_hashes = false;
};

// Problems with an already-built manifest; empty when valid.
std::vector<std::string> validate_manifest(const GameManifest& manifest, const ManifestOptions& options = {});

// Parses and validates. The error lists every problem found, one per line.
Result<GameManifest> parse_manifest(std::string_view toml_text, const ManifestOptions& options = {});
Result<GameManifest> load_manifest(const std::filesystem::path& path, const ManifestOptions& options = {});

} // namespace pspocket::recomp
