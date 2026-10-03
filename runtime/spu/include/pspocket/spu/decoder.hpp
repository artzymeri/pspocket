#pragma once

// SPU instruction decoder: 32-bit instruction word -> Op + field accessors. Covers the whole SPU ISA v1.2 as the
// Cell BE implements it (the DP compares of ISA 1.2 decode but are not supported: the PS3 does not have them).

#include <array>
#include <span>
#include <string>
#include <string_view>

#include "pspocket/core/types.hpp"

namespace pspocket::spu {

// Operand formats. They drive decoding, disassembly, register usage and the generic encoder
// (recomp/spu/include/pspocket/recomp/spu/asm.hpp). Field positions use IBM bit numbering (bit 0 = MSB).
enum class Fmt : u8 {
    NONE,      // lnop sync dsync
    RR,        // rt, ra, rb         op 0-10, rb 11-17, ra 18-24, rt 25-31
    RR_RT_RA,  // rt, ra
    RR_RT,     // rt                 fscrrd nop
    RR_RA,     // ra                 fscrwr
    RRR,       // rt, ra, rb, rc     op 0-3, rt 4-10, rb 11-17, ra 18-24, rc 25-31
    RI7,       // rt, ra, I7         I7 in 11-17 (signed)
    RI8,       // rt, ra, I8         op 0-9, I8 10-17 (conversion scale)
    RI10,      // rt, ra, I10        op 0-7, I10 8-17 (signed)
    RI10_LS,   // rt, I10*16(ra)     lqd stqd
    RI16,      // rt, I16            op 0-8, I16 9-24
    RI16_LS,   // rt, I16*4          lqa stqa (absolute), lqr stqr (pc-relative)
    RI16_BR,   // [rt,] I16*4        br bra brsl brasl brz brnz brhz brhnz
    RI18,      // rt, I18            op 0-6, I18 7-24 (ila)
    BI,        // [rt,] ra           indirect branches; D (bit 12) and E (bit 13) disable/enable interrupts
    HBR,       // brinst, ra         hbr (P bit 11)
    HBRI,      // brinst, target     hbra hbrr: ROH 7-8, I16 9-24, ROL 25-31
    STOP,      // signal             stop: 14-bit signal type in 18-31, bits 11-17 zero
    CH_READ,   // rt, channel        rdch rchcnt (channel in the ra field)
    CH_WRITE,  // channel, rt        wrch
    SPR_READ,  // rt, spr            mfspr
    SPR_WRITE, // spr, rt            mtspr
};

// Op flags.
inline constexpr u32 OPF_BRANCH = 1u << 0;    // changes control flow (ends a basic block)
inline constexpr u32 OPF_COND = 1u << 1;      // conditional branch (bisled: taken when an event is pending)
inline constexpr u32 OPF_INDIRECT = 1u << 2;  // target from a register (or SRR0 for iret)
inline constexpr u32 OPF_CALL = 1u << 3;      // writes the return address to rt
inline constexpr u32 OPF_ABS = 1u << 4;       // absolute LS address (else pc-relative)
inline constexpr u32 OPF_LOAD = 1u << 5;
inline constexpr u32 OPF_STORE = 1u << 6;
inline constexpr u32 OPF_CHANNEL = 1u << 7;
inline constexpr u32 OPF_STOP = 1u << 8;      // stop / stopd
inline constexpr u32 OPF_HALT = 1u << 9;      // heq hgt hlgt (+ immediate forms)
inline constexpr u32 OPF_HINT = 1u << 10;     // branch hints: architecturally no-ops
inline constexpr u32 OPF_SYNC = 1u << 11;     // sync dsync
inline constexpr u32 OPF_FP = 1u << 12;       // single-precision floating point
inline constexpr u32 OPF_DP = 1u << 13;       // double-precision floating point
inline constexpr u32 OPF_XFLOAT = 1u << 14;   // result depends on the single-precision model (fp.h)
inline constexpr u32 OPF_READS_RT = 1u << 15; // rt is also a source operand
inline constexpr u32 OPF_NOSUPPORT = 1u << 16; // decoded, not supported (not implemented by the Cell BE)

enum class Op : u16 {
    UNKNOWN = 0,
#define PS_SPU_OP(id, mn, fmt, bits, opc, fl) id,
#include "pspocket/spu/ops.inc"
#undef PS_SPU_OP
    COUNT
};

inline constexpr usize kOpCount = static_cast<usize>(Op::COUNT);

struct OpInfo {
    Op op;
    const char* mnemonic;
    Fmt fmt;
    u8 opbits;  // width of the opcode field
    u16 opcode; // value of the opcode field
    u32 flags;
};

const OpInfo& op_info(Op op);
std::span<const OpInfo> all_ops(); // every op except UNKNOWN, in ops.inc order

// A decoded instruction. Accessors use the RPCS3 spu_opcode_t field positions (Emu/Cell/SPUOpcodes.h).
struct Inst {
    u32 raw = 0;
    Op op = Op::UNKNOWN;

    const OpInfo& info() const { return op_info(op); }
    u32 flags() const { return info().flags; }

    u32 rt() const { return raw & 0x7F; }          // bits 25-31 (also rc of RRR)
    u32 ra() const { return (raw >> 7) & 0x7F; }   // bits 18-24 (also channel / SPR number)
    u32 rb() const { return (raw >> 14) & 0x7F; }  // bits 11-17
    u32 rc() const { return raw & 0x7F; }          // RRR: bits 25-31
    u32 rt4() const { return (raw >> 21) & 0x7F; } // RRR: bits 4-10 (the target)
    u32 dest() const { return info().fmt == Fmt::RRR ? rt4() : rt(); }

    u32 i7() const { return (raw >> 14) & 0x7F; }
    s32 si7() const { return static_cast<s32>(i7() << 25) >> 25; }
    u32 i8() const { return (raw >> 14) & 0xFF; }
    u32 i10() const { return (raw >> 14) & 0x3FF; }
    s32 si10() const { return static_cast<s32>(i10() << 22) >> 22; }
    u32 i16() const { return (raw >> 7) & 0xFFFF; }
    s32 si16() const { return static_cast<s16>(i16()); }
    u32 i18() const { return (raw >> 7) & 0x3FFFF; }

    u32 e() const { return (raw >> 18) & 1; } // bit 13: enable interrupts
    u32 d() const { return (raw >> 19) & 1; } // bit 12: disable interrupts
    u32 c() const { return (raw >> 20) & 1; } // bit 11: sync C bit, hbr P bit
    u32 stop_signal() const { return raw & 0x3FFF; }
    u32 stop_reserved() const { return (raw >> 14) & 0x7F; } // bits 11-17 of stop
    u32 channel() const { return ra(); }

    // hbr / hbra / hbrr: address of the hinted branch (the instruction the hint refers to).
    s32 hint_offset() const
    {
        if (op == Op::HBR) return static_cast<s32>(((static_cast<s32>((raw >> 14) & 3) << 30) >> 23) | rt()) * 4;
        return static_cast<s32>(((static_cast<s32>((raw >> 23) & 3) << 30) >> 23) | rt()) * 4;
    }
};

Inst decode(u32 raw);

// True when the interpreter executes and the lifter lifts the instruction (they agree on this list). Unsupported:
// unknown words, the ISA 1.2 DP compares (not on the Cell BE), stop with non-zero bits 11-17, indirect branches
// with both the D and E bits set.
bool is_supported(const Inst& inst);
std::string_view unsupported_reason(const Inst& inst);

// ---- branch helpers ----

// Target of a direct branch (RI16_BR) at pc: (pc or 0) + I16 * 4, wrapped to the LS (RPCS3 spu_branch_target).
inline u32 branch_target(const Inst& inst, u32 pc)
{
    const u32 base = (inst.flags() & OPF_ABS) ? 0u : pc;
    return (base + (inst.i16() << 2)) & 0x3FFFCu;
}

// LS address of lqa/stqa/lqr/stqr at pc (RPCS3 spu_ls_target).
inline u32 ls_target(const Inst& inst, u32 pc)
{
    const u32 base = (inst.flags() & OPF_ABS) ? 0u : pc;
    return (base + (inst.i16() << 2)) & 0x3FFF0u;
}

// ---- register usage ----

// One bit per GPR.
struct RegSet {
    std::array<u64, 2> bits{};

    void set(u32 r) { bits[r >> 6] |= 1ull << (r & 63); }
    bool test(u32 r) const { return (bits[r >> 6] >> (r & 63)) & 1; }
    bool empty() const { return bits[0] == 0 && bits[1] == 0; }
    RegSet& operator|=(const RegSet& o)
    {
        bits[0] |= o.bits[0];
        bits[1] |= o.bits[1];
        return *this;
    }
    bool operator==(const RegSet&) const = default;
};

struct RegUse {
    RegSet read;
    RegSet write;
    bool fpscr_read = false, fpscr_write = false;
    bool srr0_read = false;
    bool ls_read = false, ls_write = false;
    bool runtime = false; // calls the runtime (channels, stop, halt, sync with side effects)
};

// Register usage of a supported instruction (exact for every op: SPU instructions have fixed operands).
RegUse reg_use(const Inst& inst);

} // namespace pspocket::spu
