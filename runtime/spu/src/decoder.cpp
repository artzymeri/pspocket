// SPU decoder tables, built at compile time from ops.inc. The table is indexed by the top 11 bits of the word, as
// RPCS3's decoder (rpcs3/Emu/Cell/SPUOpcodes.h, spu_decoder): an op with an n-bit opcode fills 2^(11-n) entries.

#include "pspocket/spu/decoder.hpp"

namespace pspocket::spu {
namespace {

constexpr OpInfo kOps[] = {
    {Op::UNKNOWN, "?", Fmt::NONE, 0, 0, 0},
#define PS_SPU_OP(id, mn, fmt, bits, opc, fl) {Op::id, mn, Fmt::fmt, bits, opc, fl},
#include "pspocket/spu/ops.inc"
#undef PS_SPU_OP
};
static_assert(std::size(kOps) == kOpCount, "ops.inc and Op enum out of sync");

struct Table {
    std::array<Op, 2048> ops{};
    bool collision = false;
};

constexpr Table build_table()
{
    Table t{};
    for (const OpInfo& o : kOps) {
        if (o.op == Op::UNKNOWN) continue;
        const u32 shift = 11u - o.opbits;
        for (u32 i = 0; i < (1u << shift); i++) {
            Op& slot = t.ops[(u32{o.opcode} << shift) | i];
            if (slot != Op::UNKNOWN) t.collision = true;
            slot = o.op;
        }
    }
    return t;
}

constexpr Table kTable = build_table();
static_assert(!kTable.collision, "two SPU ops share an opcode");

} // namespace

const OpInfo& op_info(Op op)
{
    const auto i = static_cast<usize>(op);
    return i < kOpCount ? kOps[i] : kOps[0];
}

std::span<const OpInfo> all_ops() { return std::span<const OpInfo>(kOps).subspan(1); }

Inst decode(u32 raw) { return Inst{raw, kTable.ops[raw >> 21]}; }

std::string_view unsupported_reason(const Inst& inst)
{
    if (inst.op == Op::UNKNOWN) return "unknown opcode";
    const OpInfo& info = inst.info();
    if (info.flags & OPF_NOSUPPORT) return "SPU ISA 1.2 double-precision compare: not implemented by the Cell BE";
    if (inst.op == Op::STOP && inst.stop_reserved() != 0) return "stop with non-zero bits 11-17";
    if (info.fmt == Fmt::BI && inst.d() && inst.e()) return "indirect branch with both D and E set";
    return {};
}

bool is_supported(const Inst& inst) { return unsupported_reason(inst).empty(); }

RegUse reg_use(const Inst& inst)
{
    RegUse u;
    if (!is_supported(inst)) return u;
    const OpInfo& info = inst.info();
    const u32 fl = info.flags;
    if (fl & OPF_READS_RT) u.read.set(inst.rt());

    switch (info.fmt) {
    case Fmt::NONE: break;
    case Fmt::RR:
        if (inst.op == Op::STOPD) {
            u.runtime = true;
            break;
        }
        u.read.set(inst.ra());
        u.read.set(inst.rb());
        if (fl & OPF_HALT) {
            u.runtime = true;
        } else if (fl & OPF_STORE) {
            u.ls_write = true;
        } else {
            u.write.set(inst.rt());
            if (fl & OPF_LOAD) u.ls_read = true;
        }
        break;
    case Fmt::RR_RT_RA:
    case Fmt::RI7:
    case Fmt::RI8:
        u.read.set(inst.ra());
        u.write.set(inst.rt());
        break;
    case Fmt::RR_RT:
        if (inst.op == Op::FSCRRD) {
            u.write.set(inst.rt());
            u.fpscr_read = true;
        }
        break;
    case Fmt::RR_RA:
        u.read.set(inst.ra());
        u.fpscr_write = true;
        break;
    case Fmt::RRR:
        u.read.set(inst.ra());
        u.read.set(inst.rb());
        u.read.set(inst.rc());
        u.write.set(inst.rt4());
        break;
    case Fmt::RI10:
        u.read.set(inst.ra());
        if (fl & OPF_HALT)
            u.runtime = true;
        else
            u.write.set(inst.rt());
        break;
    case Fmt::RI10_LS:
        u.read.set(inst.ra());
        if (fl & OPF_STORE) {
            u.ls_write = true;
        } else {
            u.write.set(inst.rt());
            u.ls_read = true;
        }
        break;
    case Fmt::RI16:
    case Fmt::RI18: u.write.set(inst.rt()); break;
    case Fmt::RI16_LS:
        if (fl & OPF_STORE) {
            u.ls_write = true;
        } else {
            u.write.set(inst.rt());
            u.ls_read = true;
        }
        break;
    case Fmt::RI16_BR:
        if (fl & OPF_CALL) u.write.set(inst.rt());
        break;
    case Fmt::BI:
        if (inst.op == Op::IRET)
            u.srr0_read = true;
        else
            u.read.set(inst.ra());
        if (fl & OPF_CALL) u.write.set(inst.rt());
        if (inst.op == Op::BISLED || inst.d() || inst.e()) u.runtime = true;
        break;
    case Fmt::HBR:
    case Fmt::HBRI: break;
    case Fmt::STOP: u.runtime = true; break;
    case Fmt::CH_READ:
        u.write.set(inst.rt());
        u.runtime = true;
        break;
    case Fmt::CH_WRITE:
        u.read.set(inst.rt());
        u.runtime = true;
        break;
    case Fmt::SPR_READ: u.write.set(inst.rt()); break;
    case Fmt::SPR_WRITE: u.read.set(inst.rt()); break;
    }
    return u;
}

} // namespace pspocket::spu
