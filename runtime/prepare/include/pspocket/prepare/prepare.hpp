#pragma once

// Preparing a game (route B, stage 4, D-154): from the game's decrypted executable to its game library, on the phone
// ("Preparing game…") and on the Mac (pspocket-prepare), with the same code. The steps:
//   1. translate: every PPU function of the executable to C (recomp/ppu, as ppu-lift --elf), and the SPU programs runs
//      saved (recomp/spu, as scripts/lift-spu.sh)
//   2. compile: every file with the device's compiler, the slowest first, objects kept (codegen::build_library)
//   3. link, then sign the library (the user's development identity on the phone, ad hoc on the Mac)
//   4. a manifest beside it: what it was made from and by, so a later launch knows whether it is still current
// Progress and the time left cover all of it.

#include <atomic>
#include <functional>
#include <array>
#include <string>
#include <vector>

#include "pspocket/codesign/codesign.hpp"

namespace pspocket::prepare {

// Where the device's compiler finds what it compiles against (the app's codegen folder, or the source tree and the
// LLVM build on the Mac), and what it compiles for.
struct Toolchain {
    std::string resource_dir;              // clang's headers
    std::string sysroot;                   // runtime/codegen/sysroot
    std::vector<std::string> include_dirs; // the runtime's ABI headers (pspocket/ppu, spu, core, game)
    std::string entry_source;              // runtime/game/library/library_entry.c
    std::string target = "arm64-apple-ios17.0", cpu = "apple-a12", platform = "ios", min_os = "17.0";
};

struct Progress {
    enum class Phase { Translating, Compiling, Linking, Signing, Done, Failed };
    Phase phase = Phase::Translating;
    double fraction = 0;      // of the whole preparation, by time: elapsed / (elapsed + time left)
    double seconds = 0;       // since it started
    double seconds_left = -1; // -1: not known yet
    size_t files_done = 0, files_total = 0;
    std::string detail;       // what it is doing, in a few words
};

struct Options {
    std::string executable;   // the decrypted executable (ELF)
    std::string title_id;     // "BLES01807"
    std::string game_dir;     // the game's folder: game.dylib, manifest.json, and the work (ppu/, objects/)
    std::string ppu_dir;      // translated PPU code to use instead of translating (Mac experiments), or empty
    std::string spu_dir;      // lifted SPU programs (scripts/lift-spu.sh's output) to include, or empty
    // SPU images runs saved (pspocket-boot --spu-dump, the app while the game runs): lifted into game_dir/spu, which
    // keeps their entries as seeds for the next time (D-155), and included. Used when spu_dir is empty.
    std::string spu_images_dir;
    std::vector<std::string> spu_logs; // runs' logs: where execution left lifted SPU code, more seeds
    // Folders of SPU ELFs from the disc (disc::save_disc_modules, D-239): translated as the executable's are (D-226).
    std::vector<std::string> spu_elf_dirs;
    bool discover_spu = true;          // also lift SPU code only registers reach (recomp::spu discover, D-155)
    // Where the PPU interpreter ran code in earlier sessions (ppu::save_landings: pspocket-boot --ppu-landings, the app
    // while the game runs): translated as continuations (D-255), so a fiber switch's landing goes on translated.
    std::string ppu_landings;
    Toolchain toolchain;
    unsigned workers = 0;     // 0: one per core
    double memory_mb = 0;     // compiles start while their predicted peaks fit; 0: no limit
    unsigned qos_class = 0;   // the compiling threads' QoS class (codegen::CompileOptions::qos_class); 0: the default
    const codesign::SigningKey* key = nullptr; // nullptr: ad hoc
    std::string identifier;   // the signature's identifier (the app's bundle ID on the phone)
    // This device's speed against the cost model's, per kind of file (PPU code, SPU code, tables): the last
    // preparation's (Result::rates, the manifest's).
    double prior_rates[3] = {1.0, 1.0, 1.0};
    int thermal = 0; // the device's heat when it starts (0 nominal .. 3 critical): kept in the manifest beside the rates
    bool keep_sources = false; // keep the translated C after a successful build (it is about 0.8 GB for GTA V)
    int system_generation = 0; // the system library's (kSystemGeneration, system.hpp), in its manifest; 0 for a game
    std::vector<std::string> extra_ppu_args, extra_spu_args, llvm_options; // added to translated_code_settings()
    std::function<void(const Progress&)> progress;
    // Every file's start (seconds -1) and end, for recording a build (pspocket-prepare --events).
    std::function<void(const std::string& file, int kind, double predicted, double seconds, double at)> on_file;
    const std::atomic<bool>* cancel = nullptr;
};

struct Result {
    bool ok = false;
    std::string error;
    std::string library;        // game_dir/game.dylib
    double seconds = 0;         // all of it
    double translate_seconds = 0, spu_seconds = 0, compile_seconds = 0, link_seconds = 0, sign_seconds = 0;
    size_t spu_programs = 0;
    size_t files_compiled = 0, files_kept = 0;
    double rates[3] = {1.0, 1.0, 1.0}; // real compile seconds per predicted second here, per kind: the next priors
};

Result prepare_game(const Options& options);

// The lifters' generation: raised when a change fixes how translated code behaves (not for every change, which only
// the translator hash follows): a library of an older generation is prepared again (library_is_current). 2: D-161
// (a longjmp or fiber switch in translated code). 3: D-203 (jump tables a bounds check branches into) and D-205 (SPU
// code words without guessed code, seeds carried over as used).
inline constexpr int kTranslatorGeneration = 3;

// What a game's preparation feeds the lifters: raised when a change adds inputs (not covered by the translator hash,
// which follows the lifters only). A game library of an older inputs generation is prepared again; the system library
// is not (it takes its inputs from the firmware). 1: D-226 (the SPU programs the executable carries).
inline constexpr int kGameInputsGeneration = 1;

// What a game library was made from and by (game_dir/manifest.json).
struct Manifest {
    std::string title_id, executable_sha256, made_by, compiler, cpu;
    std::string abi_hash, translator_hash; // hex
    int translator_generation = 1;         // kTranslatorGeneration it was made with (1: before the field)
    int system_generation = 0;             // a system library's kSystemGeneration; 0 for a game, or from before D-190
    int inputs_generation = 0;             // a game library's kGameInputsGeneration (0: before the field)
    long long made_at = 0;                 // seconds since 1970
    double seconds = 0;
    size_t files = 0;
    double rates[3] = {1.0, 1.0, 1.0};
    int thermal = 0; // the heat the rates were measured at
};

// How much slower a device compiles at a heat level than at nominal (D-154): iOS slows the chips as they heat. The
// iPhone 17 Pro Max prepared Assassin's Creed at 0.86 of the cost model's speed, cool, and GTA V right after, warm
// ("fair"), at 1.05: about 1.2 per level, refined as more preparations are recorded.
double thermal_slowdown(int thermal);
bool read_manifest(const std::string& game_dir, Manifest* out);

// The time left's starting speed per kind of file (D-164), each brought from the heat it was measured at to
// `thermal_now`: this game's own last preparation when there is one (the same code on the same device); else the
// device's speed from the last game's PPU code, for PPU and SPU code alike, and the last game's table rate. SPU code's
// speed against the cost model depends on the game (Assassin's Creed's compiled 6 times faster than GTA V's, run 099),
// so it is not carried from one game to another. {1, 1, 1} with neither.
std::array<double, 3> prior_rates(const Manifest* own, const Manifest* last, int thermal_now);

// Whether game_dir holds a library this build can load for this executable: made for this runtime (ABI hash), for
// this CPU, from an executable with this SHA-256. A different translator hash leaves it usable (`*why` says a newer
// translation exists).
// With spu_images_dir, also whether the game saved SPU images since the library was made (programs to add to it); with
// ppu_landings, whether new places the PPU interpreter ran code were saved since (continuations to add, D-255).
bool library_is_current(const std::string& game_dir, const std::string& executable_sha256, std::string* why,
    const std::string& spu_images_dir = {}, const std::string& ppu_landings = {});

// The SHA-256 of a file, in hex ("" if it cannot be read).
std::string file_sha256(const std::string& path);

} // namespace pspocket::prepare
