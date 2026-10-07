#pragma once

// SPU programs translated while the game runs (D-236, docs/plan-background-spu.md). A game's first session meets SPU
// code no preparation could know: the jobs and modules its tasks copy in from data files (Assassin's Creed interprets
// 21.6 billion SPU instructions in its first 150 s, run 207). The images the session saves (hle::set_spu_dump_dir)
// are lifted, compiled and signed into small libraries of SPU programs only, on one thread in the background, and
// loaded as they are ready: the SPUs running them switch at their next taken branch (D-233, D-234). The next launch
// prepares them into the game library as before (D-155).

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pspocket/codesign/codesign.hpp"
#include "pspocket/prepare/prepare.hpp"

namespace pspocket::prepare {

// LLVM's options for translated code (codegen::translated_code_settings, then `extra`), set once in the process
// before its first compile: a preparation and the session's translator share them. A later call asking for other
// options fails (they cannot change once compiles ran).
bool ensure_llvm_options(const std::vector<std::string>& extra, std::string* error);

// Held while a build compiles and links: one at a time in a process (a preparation, the session's translator).
std::mutex& compiler_mutex();

// A library of SPU programs only (PS_GAME_HAS_PPU 0): the lifted programs in spu_dir (a folder each and game_spu.c,
// as lift_spu_images writes them) compiled in work_dir/objects, linked and signed as work_dir/<name>.dylib.
struct SpuLibraryOptions {
    Toolchain toolchain;
    const codesign::SigningKey* key = nullptr; // nullptr: ad hoc
    std::string identifier = "com.pspocket.spu";
    std::string title_id;
    unsigned workers = 1;
    double memory_mb = 0;
    unsigned qos_class = 0; // codegen::CompileOptions::qos_class
    std::vector<std::string> extra_spu_args, llvm_options;
    const std::atomic<bool>* cancel = nullptr;
};
struct SpuLibraryResult {
    bool ok = false;
    bool compile_failed = false; // a file did not compile (the program's code), rather than the link or the signature
    std::string error, library;
    size_t programs = 0, files = 0;
    double compile_seconds = 0, link_seconds = 0, sign_seconds = 0;
};
SpuLibraryResult build_spu_library(const std::string& spu_dir, const std::string& work_dir, const std::string& name,
    const SpuLibraryOptions& options);

struct SessionOptions {
    std::string images_dir; // where the session saves the SPU images it meets without a program
    std::string work_dir;   // the translator's own folder, emptied at start: batch-N/ (images, lifted C, objects)
    SpuLibraryOptions library;
    double settle_seconds = 0.5; // an image's description must be this old (written whole)
    double poll_seconds = 1.0;   // how often images_dir is looked at
    // A program predicted to compile longer than this (codegen::estimate_cost, scaled by how fast this session's
    // compiles ran against it) waits for the next launch's preparation; 0: none waits.
    double max_program_seconds = 0;
    // Asked before each batch: whether compiling now is all right (memory, heat, the app in front). False: later.
    std::function<bool()> may_compile;
    // Loads a built library into the process (game::load_library, under a spu::RegistryBatch). False: it failed, and
    // the translator stops for the session.
    std::function<bool(const std::string& library, std::string* error)> load;
};

struct SessionStats {
    size_t images = 0;     // saved images looked at
    size_t known = 0;      // of those, with a program registered already
    size_t lifted = 0;     // programs lifted
    size_t deferred = 0;   // programs left to the next launch (too slow to compile now)
    size_t loaded = 0;     // programs in libraries loaded
    size_t failed = 0;     // programs in batches that failed
    size_t batches = 0;    // libraries loaded
    double compile_seconds = 0, link_seconds = 0, sign_seconds = 0;
    double first_seen_to_loaded_max = 0; // seconds from an image's description to its library loaded, the most
    bool stopped_on_failure = false;
};

class SessionTranslator {
public:
    explicit SessionTranslator(SessionOptions options);
    ~SessionTranslator();
    SessionTranslator(const SessionTranslator&) = delete;
    SessionTranslator& operator=(const SessionTranslator&) = delete;

    // Empties work_dir and starts the thread.
    bool start(std::string* error);
    // Stops it: a compile under way is cancelled. Idempotent.
    void stop();
    SessionStats stats() const;

private:
    void run();
    bool batch(); // false: stop
    SessionOptions o_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    SessionStats stats_;
    std::set<std::string> handled_; // image names looked at already (the translator's thread)
    int batch_number_ = 0, library_number_ = 0;
    double predicted_done_ = 0, compiled_seconds_ = 0; // the cost model against this session's compiles
};

} // namespace pspocket::prepare
