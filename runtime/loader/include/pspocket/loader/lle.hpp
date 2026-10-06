#pragma once

// Firmware modules run as they are (LLE, route B stage 5, D-157): a game's import library is served by the PS3's own
// module from /dev_flash (installed from the user's PS3UPDAT.PUP, D-156) when the game needs it: decrypted, placed in
// main memory, relocated, its own imports linked (to pspocket's host modules, or to other firmware modules), its
// exports given to the game, and its module_start run on the main thread before the game's entry, as RPCS3 does
// (rpcs3/Emu/Cell/PPUModule.cpp: ppu_load_prx, ppu_load_exports, ppu_load_imports; Emu/Cell/lv2/sys_prx.cpp g_prx_list).
// Firmware code runs under the interpreter.
//
// Which modules run as they are is decided per game, per module file, as a PS3 links a game to the system's libraries
// (D-175, decide()): the host's module when it implements every function the game imports from it; else the firmware's
// own module, its imports linked to other firmware modules chosen with it or to the host, as RPCS3 links a module
// (what nothing implements fails when called, a variable no module exports stays unlinked, both logged). Modules in
// sys/internal (codec cores, services) are indexed too, keyed "internal/NAME" (D-177). Some modules
// always stay on the host, as pspocket must own what they do (the process runtime, module loading, SPURS (D-037), the
// RSX, audio, input, files, network, sysutil and its dialogs, devices). Before decide() (tests, runtime loads with no
// decision), a module file goes LLE only when the host implements none of its libraries (D-157).

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"
#include "pspocket/hle/process.hpp"

namespace pspocket::recomp {
struct PpuModule;
}

namespace pspocket::loader {

struct PrxModule {
    std::string name; // the module info's ("cellSync")
    std::string file; // its file in /dev_flash/sys/external ("libsync2.sprx"); empty for load_prx
    u32 toc = 0;
    std::vector<std::pair<u32, u32>> segments;             // where its segments went: (address, size)
    std::vector<u64> segment_filesz;                       // each placed segment's file size, as `segments`
    u16 attributes = 0;                                    // the module info's (sys_prx_get_module_info)
    std::array<u8, 2> version{};
    std::pair<u32, u32> exports{}, imports{};               // the module info's ranges (start, end)
    std::map<std::pair<std::string, u32>, u32> functions; // exported (library, NID) -> function descriptor
    std::map<std::pair<std::string, u32>, u32> variables; // exported (library, NID) -> variable address
    u32 start = 0, stop = 0;                              // module_start, module_stop descriptors (0: none)
    u32 relocations = 0;                                  // applied
    u32 relocation_errors = 0;                            // out of their segments, or of a type RPCS3 lacks too
    u32 variables_linked = 0;                             // its imported variables found in other modules
};

class LleModules {
public:
    explicit LleModules(std::string dev_flash_dir);
    ~LleModules();

    // The libraries the firmware's modules export (sys/external/*.sprx), read once and kept beside /dev_flash
    // (lle-index.txt). False when there is no firmware or nothing can be read.
    bool index(std::string* error = nullptr);
    std::optional<std::string> file_for(std::string_view library) const;
    size_t libraries() const { return library_file_.size(); }

    // Whether a game's import library goes to its firmware module: after decide(), when that module was chosen for
    // the game; before, when the host implements none of the module's libraries. `forced` libraries (PSPOCKET_LLE,
    // tests) go to their firmware module whatever the host has.
    bool chooses(std::string_view library) const;
    void force(std::string library) { forced_.push_back(std::move(library)); }

    // What a game (or a module it loads) imports from one library.
    struct LibraryNeeds {
        std::string library;
        std::vector<u32> functions; // NIDs
        std::vector<u32> variables; // NIDs
    };
    static std::vector<LibraryNeeds> needs_of(const recomp::PpuModule& module);
    // The per-game choice (see the top of the file) for the executable's imports. `syscall_bound` says whether lv2
    // answers a syscall number. Logs the modules chosen and, for the libraries left on the host, the imported
    // functions it lacks.
    void decide(const std::vector<LibraryNeeds>& imports, const std::function<bool(u32)>& syscall_bound);
    // The same choice for a module the game loads while it runs (`who`: its path): its imports, and with `file` a
    // firmware module the game loads by its path. What was chosen stays chosen (it is linked); more may join. Nothing
    // before decide().
    void extend(std::string_view who, const std::vector<LibraryNeeds>& imports, std::string_view file,
        const std::function<bool(u32)>& syscall_bound);
    bool decided() const { return decided_; }
    bool chosen(std::string_view file) const { return chosen_.contains(file); }
    // The firmware module files decide() chose, and those it refused with the reason (for reports and tests).
    const std::set<std::string, std::less<>>& chosen_files() const { return chosen_; }
    const std::map<std::string, std::string, std::less<>>& refused_files() const { return refused_; }
    // For each chosen file, what it needs that nothing implements (functions and syscalls: they fail when called).
    const std::map<std::string, std::vector<std::string>, std::less<>>& gaps() const { return gaps_; }
    // Module files that stay on the host whatever a game imports (D-175).
    static bool always_native(std::string_view file);

    // The module_starts of the modules loaded since the last call that have not run (dependencies first), marked as
    // run: the boot runs them before the game's entry, sys_prx_start_module before the module that needed them. A
    // module started so and loaded again by its path (sys_prx_load_module) does not start twice (D-175).
    std::vector<u32> take_starts()
    {
        std::vector<u32> out;
        for (const u32 s : starts_)
            if (started_.insert(s).second) out.push_back(s);
        return out;
    }
    bool started(u32 start) const { return started_.contains(start); }
    void mark_started(u32 start) { started_.insert(start); }

    // The firmware's modules have fixed addresses in the PRX region, the same for every game (D-178): `file`'s, per
    // PT_LOAD (empty when it is not indexed or the layout did not fit).
    std::span<const u32> fixed_addresses(std::string_view file) const;
    // What a system library is made from (D-178): one line per module in key order (its key, its file's SHA-256, its
    // fixed addresses). A system library holds this text's SHA-256.
    std::string identity_text() const;
    // Called for each module as it loads, placed and linked, before its module_start runs (the system library checks
    // the module's code against its translation, D-178).
    void set_on_load(std::function<void(const PrxModule&)> hook) { on_load_ = std::move(hook); }

    // Loads the module exporting `library` (once), with the modules its own imports need that the host lacks. nullptr
    // with the reason in *error. Before guest threads run, or under RuntimeModules' lock.
    const PrxModule* load(std::string_view library, hle::HleProcess& hle, std::string* error);
    // The module in sys/external file `file` (once), as load(): for a game that loads one by its path (D-159).
    const PrxModule* load_file(const std::string& file, hle::HleProcess& hle, std::string* error);
    // Whether the host implements none of `file`'s libraries: the module can run as it is.
    bool runs_whole(std::string_view file) const;
    const std::vector<std::unique_ptr<PrxModule>>& loaded() const { return loaded_; }
    // module_start descriptors in the order they run: a module after the modules it imports from.
    const std::vector<u32>& starts() const { return starts_; }

private:
    // What the index knows of a module file: its libraries' exports, its imports and the syscalls its code makes.
    struct FileInfo {
        std::map<std::string, std::set<u32>, std::less<>> export_functions, export_variables;
        std::map<std::string, std::set<u32>, std::less<>> import_functions, import_variables;
        std::set<u32> syscalls;
        std::vector<u32> load_sizes; // each PT_LOAD's memsz (D-178)
        std::string sha256;          // the module file's (hex)
        std::vector<u32> addrs;      // its fixed addresses, per PT_LOAD (empty: placed in main memory)
    };
    void lay_out();
    bool forced(std::string_view library) const;
    void choose(std::string_view who, const std::vector<LibraryNeeds>& imports, std::string_view file,
        const std::function<bool(u32)>& syscall_bound);

    std::string dev_flash_;
    std::map<std::string, std::string, std::less<>> library_file_;               // library -> file
    std::map<std::string, std::vector<std::string>, std::less<>> file_libraries_; // file -> its libraries
    std::map<std::string, FileInfo, std::less<>> files_;                          // file -> what it exports and needs
    std::vector<std::string> forced_;
    bool decided_ = false;
    std::set<std::string, std::less<>> chosen_;                         // decide(), extend(): files that run as they are
    std::map<std::string, std::string, std::less<>> refused_;           // file -> why it stays on the host
    std::map<std::string, std::vector<std::string>, std::less<>> gaps_; // chosen file -> what it lacks
    std::set<std::string, std::less<>> host_linked_;                    // libraries something linked to the host
    std::set<u32> started_;
    std::vector<std::unique_ptr<PrxModule>> loaded_;
    std::vector<u32> starts_;
    std::function<void(const PrxModule&)> on_load_;
};

class Executable;

// Modules the game loads while it runs (sys_prx_load_module, D-159; RPCS3 lv2/sys_prx.cpp prx_load_module and
// _sys_prx_start_module): a module of the game's own, read through its file system (/dev_bdvd, /app_home, ...), a disc
// game's SELF decrypted with the built-in keys, placed and linked as a firmware module is, the firmware modules its
// imports need chosen as for the game's (LleModules::extend, D-175); a firmware module by its /dev_flash path through
// `lle` when it was chosen or the host replaces none of it, else an empty module (the host's functions serve its
// libraries, as RPCS3's "ignored" HLE modules). Starting one points the executable's imports of its libraries at its
// exports; firmware modules loaded with it start first. Install with HleProcess::set_module_loader before guest
// threads run.
class RuntimeModules final : public hle::ModuleLoader {
public:
    RuntimeModules(hle::HleProcess& hle, std::shared_ptr<const Executable> exe, LleModules* lle);
    ~RuntimeModules() override;

    u32 load(std::string_view path, u32* id) override;
    u32 load_at(std::string_view path, u64 offset, u32* id) override;
    u32 id_by_address(u32 addr) override;
    std::vector<u32> list() override;
    bool info(u32 id, hle::ModuleInfo& out) override;
    u32 start(u32 id, u32* entry) override;
    void started(u32 id, s32 result) override;
    u32 stop(u32 id, u32* entry) override;
    void stopped(u32 id, s32 result) override;
    u32 unload(u32 id) override;
    u32 id_by_name(std::string_view name) override;
    std::vector<u32> take_dependency_starts() override;

    // The module behind `id` (null for an empty one or an unknown ID).
    const PrxModule* module(u32 id);

private:
    enum class State { Loaded, Started, Stopped, Unloaded };
    struct Entry {
        u32 id = 0;
        std::string path;
        const PrxModule* module = nullptr; // the game's (owned) or a firmware module (lle's); null: empty
        std::unique_ptr<PrxModule> owned;
        State state = State::Loaded;
    };
    Entry* find(u32 id);
    u32 link_function(std::string_view library, u32 nid, std::string_view who);
    u32 link_variable(std::string_view library, u32 nid, std::string_view who);

    hle::HleProcess& hle_;
    std::shared_ptr<const Executable> exe_;
    LleModules* lle_;
    std::recursive_mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_;
    u32 next_id_ = 0x23000001; // RPCS3's lv2_prx IDs start at 0x23000000
};

// Places a decrypted PRX (an ELF) in main memory and links it: segments, relocations (the SCE_PPURELA program
// header's entries, RPCS3's types 1, 4, 5, 6, 10, 11, 38, 44, 57), exports, and its imports through `link` (library,
// NID -> descriptor; 0 leaves the slot as it is, with an error logged) and `link_variable` (library, NID -> the
// variable's address, whose references are then patched; none: variables stay unlinked, logged).
bool load_prx(std::span<const u8> elf, hle::HleProcess& hle,
    const std::function<u32(std::string_view library, u32 nid)>& link, PrxModule* out, std::string* error,
    const std::function<u32(std::string_view library, u32 nid)>& link_variable = {});

} // namespace pspocket::loader
