#pragma once

// Native replacements for known builds of SPU middleware (plan section 6, "Middleware replacement"; D-965). The code
// is in runtime/spu/replace/ (its README says what is there and how each piece was checked).
//
// A known build is a lifted program recognised by its key (ps_spu_hash64 of the image it was lifted from, its code
// start and size): a table of builds is data, not a title check. Each lists functions (their entries, in the
// program's own addresses) a native implementation may take over:
//
// - The lifter makes every call into a listed function go through the program's table (the run loop) instead of a
//   direct C call (listed()), so a table can take it over.
// - When such a program is registered (ps_spu_register_program), the runtime registers before it a program derived
//   from it: the same code, its listed entries going to a dispatcher that runs the native implementation, or the
//   lifted function when the native one declines its inputs or is switched off. ps_spu_find_program returns the first
//   program registered for a key, so contexts take the derived one.
//
// Switches (read once, at the first registration):
//   PSPOCKET_SPU_NATIVE=0      no derived programs: the lifted code alone, as before.
//   PSPOCKET_SPU_NATIVE=ab     native in half A and lifted in half B of PSPOCKET_SPU_AB's period (D-950): the SPURS
//                              time lines name their half, so one walk measures both.
//   PSPOCKET_SPU_NATIVE_ONLY=ADDR[,ADDR...]  native for these functions only (hex, the program's own addresses).
//   PSPOCKET_SPU_NATIVE_SCRAMBLE=1  after each call of a listed function (native, or lifted when it returned), the
//                              registers outside its contract (below) hold garbage: the walk must not change.
//   PSPOCKET_SPU_CAPTURE=HASH:ADDR[,ADDR...]  records the state at entry and at exit of the lifted function at each
//                              ADDR of the program with key HASH (16 hex digits) into PSPOCKET_SPU_CAPTURE_DIR (default
//                              "spu-capture"), at most PSPOCKET_SPU_CAPTURE_MAX per function (default 500), every
//                              PSPOCKET_SPU_CAPTURE_EVERY-th call (default 1): the input of spu-native-check. Captures
//                              hold the game's data: they stay in the gitignored inventory/ (CLAUDE.md rule 1).
//   PSPOCKET_SPU_TRACE_ENTRIES=HASH  records how each entry of the program is entered (write_trace below).

#include <cstdint>
#include <string>
#include <vector>

#include "pspocket/spu/runtime.h"

namespace pspocket::spu::replace {

// The lifter's question: is the function at `addr` of the program with key `hash` lifted at `code_start` listed for
// replacement? Calls into it are then lifted as calls through the program's table.
bool listed(uint64_t hash, uint32_t code_start, uint32_t addr);
// Every address listed for that program (functions and loop heads and exits): the lifter's boundaries.
std::vector<uint32_t> listed_addresses(uint64_t hash, uint32_t code_start);

// A native implementation. Entered as the lifted function would be at its entry (ctx->pc the entry plus ctx->reloc,
// ctx->program the program, the SPU FP environment), it leaves what the lifted function leaves in every local-store
// byte, the pc, the stop and channel state, and the registers of exact_registers(), and returns 1; or it returns 0
// having changed nothing, and the lifted function runs instead (inputs it does not handle).
//
// The contract (D-965): the registers the SPU ABI keeps — $0, $1 and the callee-saved $80-$127 — are exact; of the
// others ($2-$79) those its callers may read after it returns (the return value $3 of a function that has one,
// whatever a caller relies on beyond the ABI), found by spu-live over the program and listed with the function. The
// rest are left unspecified. A native implementation is an implementation of what the
// function computes, not a transcription of its compiled code (CLAUDE.md rule 1): the temporaries of the compiled
// loops are not reproduced.
using Impl = int (*)(SPUContext* ctx, uint8_t* ls);

// A set of registers: bit r of bits[r / 64].
struct Registers {
    uint64_t bits[2];
    bool test(uint32_t r) const { return (bits[r >> 6] >> (r & 63)) & 1; }
};
// $0, $1 and $80-$127.
constexpr Registers abi_registers() { return {{0x3ull, 0xFFFFFFFFFFFF0000ull}}; }

// The native implementation for the function at `addr` of the program with key `hash` (code start and size as the
// program's), or nullptr.
// Where a native implementation may leave local store other than bit for bit (D-974), worked out from the state at
// entry: the bar for decoded audio is equivalent output, not the same bits (the host's float operations in another
// order than the SPU's), and some bytes are never read again by anything (a variable the program only ever stores,
// the stack below the caller's stack pointer). Everything outside these regions stays exact.
struct Region {
    enum class Kind : uint8_t {
        dead, // not compared: shown never to be read (said why where the regions are listed)
        f32,  // big-endian floats: the largest difference, against the region's largest magnitude, at most `tolerance`
        s16,  // big-endian 16-bit samples: the largest difference at most `tolerance`
    };
    Kind kind;
    uint32_t start, size; // local store, no wrap
    double tolerance;
};
using RegionsFn = void (*)(const SPUContext& entry, const uint8_t* ls_entry, std::vector<Region>& out);

// A listed function of a known build: its entry (the program's own address), its native implementation (nullptr:
// listed only — calls into it go through the table, it can be captured — no native code yet), the registers of
// $2-$79 its callers may read after it, and (none: all of local store exact) its regions.
struct KnownFunction {
    uint32_t addr;
    Impl impl;
    Registers live;
    RegionsFn regions = nullptr;
};
// Adds a known build beside the built-in table (tests; builds described elsewhere): kept for the process's life, used
// by programs registered (and lifted) from then on.
void add_build(const char* name, uint64_t hash, uint32_t code_start, uint32_t code_size, std::vector<KnownFunction> functions);

Impl native(uint64_t hash, uint32_t code_start, uint32_t code_size, uint32_t addr);
// The registers a native implementation of that function must leave exact: abi_registers() and those of $2-$79 its
// callers may read after it.
Registers exact_registers(uint64_t hash, uint32_t code_start, uint32_t code_size, uint32_t addr);
// The regions of local store that function may leave equivalent or not at all (Region), for a call entered with `entry`
// and `ls_entry`; empty: all of local store is exact.
std::vector<Region> regions(uint64_t hash, uint32_t code_start, uint32_t code_size, uint32_t addr, const SPUContext& entry,
    const uint8_t* ls_entry);

enum class Mode { off, on, ab };
// Overrides PSPOCKET_SPU_NATIVE (tools and tests; before the first registration).
void set_mode(Mode mode);
Mode mode();

// Called by ps_spu_register_program for each program it registers: the program derived from it when it is a known
// build (made once, kept for the life of the process), else nullptr. Never derives from a derived program.
ps_spu_program* derive(const ps_spu_program* program);
// The program a derived program was made from (nullptr: `program` is not derived).
const ps_spu_program* original_of(const ps_spu_program* program);
// The program derived from `program` so far, or nullptr (ps_spu_unregister_program takes it out with its original).
ps_spu_program* derived_of(const ps_spu_program* program);

// Calls into the listed functions of the derived programs so far: run natively, declined by the native code (the
// lifted function ran), and run lifted by choice (switched off, half B, no native implementation yet).
struct FunctionStats {
    std::string build;
    uint32_t addr = 0;
    uint64_t native = 0, declined = 0, lifted = 0, captured = 0;
};
std::vector<FunctionStats> stats();

// PSPOCKET_SPU_TRACE_ENTRIES=HASH (16 hex digits): every entry of that program is entered through the derived program,
// which records each (entry, link register) pair; write_trace() writes them to PSPOCKET_SPU_TRACE_FILE (default
// "spu-entries.txt"), one "entry link" line each (hex, the program's own addresses). A call through a register is the
// pair whose link is its return address: spu-live's --call targets (D-965). Debug only: it slows every entry.
void write_trace();

// The capture file format (spu-native-check): a Header, the State at entry, the local store at entry (256 KB), the
// State at exit, then the 16-byte lines of local store that differ at exit: a count (u32), and for each its LS address
// (u32) and its 16 bytes. Little-endian, host layout.
struct CaptureHeader {
    char magic[8]; // "PSNCAP01"
    uint64_t hash;
    uint32_t code_start;
    uint32_t code_size;
    uint32_t addr;  // the function, in the program's own addresses
    uint32_t reloc; // the context's
    uint64_t seq;   // the call's number among this function's calls
};
struct CaptureState {
    ps_spu_v128 gpr[128];
    ps_spu_v128 fpscr;
    uint32_t pc, srr0, stop_reason, stop_code, exception;
    uint8_t interrupts_enabled, xfloat, pad[2];
    ps_spu_channels ch;
};
void save_state(const SPUContext& ctx, CaptureState& out);
void load_state(const CaptureState& in, SPUContext& ctx);

} // namespace pspocket::spu::replace
