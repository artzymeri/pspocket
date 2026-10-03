// Decoder, encoder (assembler helper), disassembler and register-usage tests.

#include <catch_amalgamated.hpp>

#include <set>

#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/decoder.hpp"
#include "pspocket/spu/disasm.hpp"
#include "pspocket/spu/interpreter.hpp"

using namespace pspocket;
using namespace pspocket::spu;
namespace as = pspocket::recomp::spu::as;

TEST_CASE("op table: 199 ops, opcodes decode back to their op")
{
    CHECK(all_ops().size() == 199);
    std::set<std::string> mnemonics;
    for (const OpInfo& info : all_ops()) {
        mnemonics.insert(info.mnemonic);
        // The opcode alone, other fields zero, decodes as this op.
        const u32 word = static_cast<u32>(info.opcode) << (32 - info.opbits);
        INFO(info.mnemonic);
        CHECK(decode(word).op == info.op);
        // ... and so does every value of the operand fields.
        for (u32 k = 0; k < 64; k++) {
            const u32 operands = (k * 0x9E3779B9u) & (0xFFFFFFFFu >> info.opbits);
            CHECK(decode(word | operands).op == info.op);
        }
    }
    CHECK(mnemonics.size() == all_ops().size());
}

TEST_CASE("the generic encoder round-trips every field through the decoder")
{
    for (const OpInfo& info : all_ops()) {
        INFO(info.mnemonic);
        as::Fields f;
        f.rt = 5;
        f.ra = 77;
        f.rb = 126;
        f.rc = 3;
        f.imm = -3;
        const u32 w = as::encode(info.op, f);
        const Inst inst = decode(w);
        REQUIRE(inst.op == info.op);
        switch (info.fmt) {
        case Fmt::RR:
            CHECK(inst.rt() == 5);
            CHECK(inst.ra() == 77);
            CHECK(inst.rb() == 126);
            break;
        case Fmt::RRR:
            CHECK(inst.rt4() == 5);
            CHECK(inst.ra() == 77);
            CHECK(inst.rb() == 126);
            CHECK(inst.rc() == 3);
            break;
        case Fmt::RI7: CHECK(inst.si7() == -3); break;
        case Fmt::RI8: CHECK(inst.i8() == 0xFD); break;
        case Fmt::RI10:
        case Fmt::RI10_LS: CHECK(inst.si10() == -3); break;
        case Fmt::RI16:
        case Fmt::RI16_LS:
        case Fmt::RI16_BR: CHECK(inst.si16() == -3); break;
        case Fmt::RI18: CHECK(inst.i18() == 0x3FFFD); break;
        default: break;
        }
    }
}

TEST_CASE("named encoders produce the documented encodings")
{
    // Hand-assembled reference words (SPU ISA field layout).
    CHECK(as::il(3, -1) == 0x40FFFF83u);       // il $3,-1: op 0x081, I16 0xFFFF, rt 3
    CHECK(as::ila(4, 0x3FFFF) == 0x43FFFF84u); // ila $4,0x3ffff
    CHECK(as::lqd(5, 32, 1) == 0x34008085u);   // lqd $5,32($sp): I10 = 2
    CHECK(as::stqd(0, 16, 1) == 0x24004080u);  // stqd $lr,16($sp)
    CHECK(as::bi(0) == 0x35000000u);           // bi $lr
    CHECK(as::stop(0x102) == 0x00000102u);
    CHECK(as::a(3, 4, 5) == 0x18014203u);
    CHECK(as::shufb(3, 4, 5, 6) == 0xB0614206u);
    CHECK(as::nop(127) == 0x4020007Fu);
    CHECK(as::lnop() == 0x00200000u);
    CHECK(as::rdch(3, 29) == 0x01A00E83u);
    CHECK(as::wrch(28, 3) == 0x21A00E03u);
}

TEST_CASE("unsupported forms")
{
    CHECK_FALSE(is_supported(decode(0x00800000u)));         // unknown opcode (0xFFFFFFFF is a valid fms)
    CHECK_FALSE(is_supported(decode(as::dfceq(1, 2, 3))));  // ISA 1.2 DP compare
    CHECK_FALSE(is_supported(decode(as::dftsv(1, 2, 3))));
    CHECK_FALSE(is_supported(decode(0x00004000u)));         // stop with bit 17 set
    CHECK_FALSE(is_supported(decode(as::bi(5, true, true)))); // bi with both E and D
    CHECK(is_supported(decode(as::bi(5, true, false))));
    CHECK(is_supported(decode(as::stop(0x3FFF))));
    CHECK(supported_ops().size() == 194);
}

TEST_CASE("disassembly")
{
    CHECK(disassemble(as::il(3, -1), 0) == "il $3,-1");
    CHECK(disassemble(as::lqd(5, 32, 1), 0) == "lqd $5,0x20($sp)");
    CHECK(disassemble(as::stqd(0, -16, 1), 0) == "stqd $lr,-0x10($sp)");
    CHECK(disassemble(as::bi(0), 0) == "bi $lr");
    CHECK(disassemble(as::bisl(0, 3, true), 0) == "bisle $lr,$3");
    CHECK(disassemble(as::brsl(0, 0x40), 0x100) == "brsl $lr,0x00140");
    CHECK(disassemble(as::br(-8), 0x10) == "br 0x00008");
    CHECK(disassemble(as::bra(0x3FFFC), 0) == "bra 0x3fffc");
    CHECK(disassemble(as::lqr(7, -16), 0x20) == "lqr $7,0x00010");
    CHECK(disassemble(as::shufb(3, 4, 5, 6), 0) == "shufb $3,$4,$5,$6");
    CHECK(disassemble(as::rdch(3, 29), 0) == "rdch $3,SPU_RdInMbox");
    CHECK(disassemble(as::wrch(21, 3), 0) == "wrch MFC_Cmd,$3");
    CHECK(disassemble(as::stop(0x102), 0) == "stop 0x102");
    CHECK(disassemble(as::andbi(3, 4, 0xFF), 0) == "andbi $3,$4,0xff");
    CHECK(disassemble(as::cflts(3, 4, 173), 0) == "cflts $3,$4,173");
    CHECK(disassemble(0x00800000u, 0) == ".long 0x00800000");
    CHECK(disassemble(0xFFFFFFFFu, 0) == "fms $127,$127,$127,$127");
}

TEST_CASE("register usage")
{
    const RegUse a = reg_use(decode(as::a(3, 4, 5)));
    CHECK(a.read.test(4));
    CHECK(a.read.test(5));
    CHECK(a.write.test(3));
    CHECK_FALSE(a.read.test(3));

    const RegUse addx = reg_use(decode(as::addx(3, 4, 5)));
    CHECK(addx.read.test(3)); // rt is also a source

    const RegUse selb = reg_use(decode(as::selb(9, 1, 2, 3)));
    CHECK(selb.write.test(9));
    CHECK(selb.read.test(1));
    CHECK(selb.read.test(2));
    CHECK(selb.read.test(3));

    const RegUse st = reg_use(decode(as::stqd(7, 0, 1)));
    CHECK(st.read.test(7));
    CHECK(st.write.empty());
    CHECK(st.ls_write);

    const RegUse brsl = reg_use(decode(as::brsl(0, 8)));
    CHECK(brsl.write.test(0));

    const RegUse wrch = reg_use(decode(as::wrch(28, 3)));
    CHECK(wrch.read.test(3));
    CHECK(wrch.runtime);
}
