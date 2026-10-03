// SPU disassembler. Operand order and mnemonics follow the SPU assembly language (and RPCS3's SPUDisAsm.h);
// immediates print as signed decimal or hex, branch and LS targets as absolute addresses.

#include "pspocket/spu/disasm.hpp"

#include <format>

#include "pspocket/spu/decoder.hpp"

namespace pspocket::spu {

std::string_view channel_name(u32 channel)
{
    switch (channel) {
    case 0: return "SPU_RdEventStat";
    case 1: return "SPU_WrEventMask";
    case 2: return "SPU_WrEventAck";
    case 3: return "SPU_RdSigNotify1";
    case 4: return "SPU_RdSigNotify2";
    case 7: return "SPU_WrDec";
    case 8: return "SPU_RdDec";
    case 9: return "MFC_WrMSSyncReq";
    case 11: return "SPU_RdEventMask";
    case 12: return "MFC_RdTagMask";
    case 13: return "SPU_RdMachStat";
    case 14: return "SPU_WrSRR0";
    case 15: return "SPU_RdSRR0";
    case 16: return "MFC_LSA";
    case 17: return "MFC_EAH";
    case 18: return "MFC_EAL";
    case 19: return "MFC_Size";
    case 20: return "MFC_TagID";
    case 21: return "MFC_Cmd";
    case 22: return "MFC_WrTagMask";
    case 23: return "MFC_WrTagUpdate";
    case 24: return "MFC_RdTagStat";
    case 25: return "MFC_RdListStallStat";
    case 26: return "MFC_WrListStallAck";
    case 27: return "MFC_RdAtomicStat";
    case 28: return "SPU_WrOutMbox";
    case 29: return "SPU_RdInMbox";
    case 30: return "SPU_WrOutIntrMbox";
    case 69: return "SPU_Set_Bkmk_Tag";
    case 70: return "SPU_PM_Start_Ev";
    case 71: return "SPU_PM_Stop_Ev";
    default: return {};
    }
}

std::string reg_name(u32 r)
{
    if (r == 0) return "$lr";
    if (r == 1) return "$sp";
    return std::format("${}", r);
}

namespace {

std::string ch_text(u32 ch)
{
    const std::string_view n = channel_name(ch);
    return n.empty() ? std::format("$ch{}", ch) : std::string(n);
}

std::string imm(s32 v) { return v < 0 ? std::format("-0x{:x}", -static_cast<s64>(v)) : std::format("0x{:x}", v); }

std::string branch_suffix(const Inst& inst)
{
    std::string s;
    if (inst.e()) s += 'e';
    if (inst.d()) s += 'd';
    return s;
}

} // namespace

std::string disassemble(u32 insn, u32 pc)
{
    const Inst inst = decode(insn);
    if (inst.op == Op::UNKNOWN) return std::format(".long 0x{:08x}", insn);
    const OpInfo& info = inst.info();
    const std::string mn = info.mnemonic;
    const auto r = [](u32 n) { return reg_name(n); };

    switch (info.fmt) {
    case Fmt::NONE:
        if (inst.op == Op::SYNC && inst.c()) return "syncc";
        return mn;
    case Fmt::RR: return std::format("{} {},{},{}", mn, r(inst.rt()), r(inst.ra()), r(inst.rb()));
    case Fmt::RR_RT_RA: return std::format("{} {},{}", mn, r(inst.rt()), r(inst.ra()));
    case Fmt::RR_RT: return std::format("{} {}", mn, r(inst.rt()));
    case Fmt::RR_RA: return std::format("{} {}", mn, r(inst.ra()));
    case Fmt::RRR:
        return std::format("{} {},{},{},{}", mn, r(inst.rt4()), r(inst.ra()), r(inst.rb()), r(inst.rc()));
    case Fmt::RI7: return std::format("{} {},{},{}", mn, r(inst.rt()), r(inst.ra()), inst.si7());
    case Fmt::RI8: return std::format("{} {},{},{}", mn, r(inst.rt()), r(inst.ra()), inst.i8());
    case Fmt::RI10:
        if (inst.op == Op::ORBI || inst.op == Op::ANDBI || inst.op == Op::XORBI || inst.op == Op::CGTBI ||
            inst.op == Op::CLGTBI || inst.op == Op::CEQBI)
            return std::format("{} {},{},0x{:x}", mn, r(inst.rt()), r(inst.ra()), inst.i10() & 0xFF);
        return std::format("{} {},{},{}", mn, r(inst.rt()), r(inst.ra()), inst.si10());
    case Fmt::RI10_LS: return std::format("{} {},{}({})", mn, r(inst.rt()), imm(inst.si10() * 16), r(inst.ra()));
    case Fmt::RI16:
        if (inst.op == Op::IL) return std::format("{} {},{}", mn, r(inst.rt()), inst.si16());
        return std::format("{} {},0x{:x}", mn, r(inst.rt()), inst.i16());
    case Fmt::RI16_LS: return std::format("{} {},0x{:05x}", mn, r(inst.rt()), ls_target(inst, pc));
    case Fmt::RI16_BR:
        if (inst.op == Op::BR || inst.op == Op::BRA) return std::format("{} 0x{:05x}", mn, branch_target(inst, pc));
        return std::format("{} {},0x{:05x}", mn, r(inst.rt()), branch_target(inst, pc));
    case Fmt::RI18: return std::format("{} {},0x{:x}", mn, r(inst.rt()), inst.i18());
    case Fmt::BI:
        if (inst.op == Op::BI || inst.op == Op::IRET) return std::format("{}{} {}", mn, branch_suffix(inst), r(inst.ra()));
        return std::format("{}{} {},{}", mn, branch_suffix(inst), r(inst.rt()), r(inst.ra()));
    case Fmt::HBR:
        return std::format("{} 0x{:05x},{}", inst.c() ? "hbrp" : "hbr", (pc + static_cast<u32>(inst.hint_offset())) & 0x3FFFC,
            r(inst.ra()));
    case Fmt::HBRI:
        return std::format("{} 0x{:05x},0x{:05x}", mn, (pc + static_cast<u32>(inst.hint_offset())) & 0x3FFFC,
            branch_target(inst, pc));
    case Fmt::STOP: return std::format("{} 0x{:x}", mn, inst.stop_signal());
    case Fmt::CH_READ: return std::format("{} {},{}", mn, r(inst.rt()), ch_text(inst.channel()));
    case Fmt::CH_WRITE: return std::format("{} {},{}", mn, ch_text(inst.channel()), r(inst.rt()));
    case Fmt::SPR_READ: return std::format("{} {},$sp{}", mn, r(inst.rt()), inst.ra());
    case Fmt::SPR_WRITE: return std::format("{} $sp{},{}", mn, inst.ra(), r(inst.rt()));
    }
    return mn;
}

} // namespace pspocket::spu
