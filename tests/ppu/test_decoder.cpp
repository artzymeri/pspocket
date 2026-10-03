// Decoder, disassembler, assembler and register-usage tests.

#include <catch_amalgamated.hpp>

#include "ppu_test_util.hpp"
#include "pspocket/ppu/decoder.hpp"
#include "pspocket/ppu/disasm.hpp"
#include "pspocket/ppu/interpreter.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

using namespace pspocket;
using namespace pspocket::ppu;
namespace as = pspocket::recomp::ppu::as;

TEST_CASE("every op round-trips through the generic encoder and the decoder")
{
    test::Rng rng(1);
    for (const OpInfo& info : all_ops()) {
        for (int k = 0; k < 64; k++) {
            as::Fields f;
            f.d = rng.below(32);
            f.a = rng.below(32);
            f.b = rng.below(32);
            f.c = rng.below(32);
            f.e = rng.below(32);
            f.imm = static_cast<s32>(rng.next());
            f.sh = rng.below(64);
            f.mb = rng.below(64);
            f.crfd = rng.below(8);
            f.crfs = rng.below(8);
            f.l = rng.below(2);
            f.spr = rng.below(1024);
            f.fxm = 0x80u >> rng.below(8);
            f.flm = rng.below(256);
            f.u4 = rng.below(16);
            f.rc = rng.chance(50);
            f.oe = rng.chance(50);
            f.lk = rng.chance(50);
            f.aa = rng.chance(50);
            const u32 raw = as::encode(info.op, f);
            const Inst in = decode(raw);
            INFO(info.mnemonic << " " << std::hex << raw);
            REQUIRE(in.op == info.op);
            CHECK(form_variant(in) == ((f.rc && (info.flags & OPF_RC) && info.op != Op::STWCX_ ? kVarRc : 0) |
                                          (f.oe && (info.flags & OPF_OE) ? kVarOe : 0) |
                                          (f.lk && (info.flags & OPF_LK) ? kVarLk : 0) |
                                          (f.aa && (info.flags & OPF_AA) ? kVarAa : 0)));
            CHECK_FALSE(disassemble(in, 0x10000).empty());
        }
    }
}

TEST_CASE("decoder field extraction")
{
    const Inst rldicl = decode(as::rldicl(3, 5, 37, 41));
    CHECK(rldicl.op == Op::RLDICL);
    CHECK(rldicl.sh64() == 37);
    CHECK(rldicl.mb64() == 41);
    CHECK(rldicl.ra() == 3);
    CHECK(rldicl.rs() == 5);
    const Inst sradi = decode(as::sradi(4, 6, 33));
    CHECK(sradi.op == Op::SRADI);
    CHECK(sradi.sh64() == 33);
    const Inst mf = decode(as::mfspr(7, 287));
    CHECK(mf.spr() == 287);
    CHECK(decode(as::bc_rel(16, 0, -8)).bd() == -8);
    CHECK(decode(as::b_rel(-0x1000000)).li() == -0x1000000);
    CHECK(decode(as::ld(3, -8, 1)).ds() == -8);
    CHECK(decode(0x7C0802A6).op == Op::MFSPR);
}

TEST_CASE("disassembly uses the standard and extended mnemonics")
{
    struct Row {
        u32 raw;
        u32 pc;
        const char* text;
    };
    const Row rows[] = {
        {0x7C0802A6, 0, "mflr r0"},
        {0x7C0803A6, 0, "mtlr r0"},
        {0x4E800020, 0, "blr"},
        {0x4E800420, 0, "bctr"},
        {0x4E800421, 0, "bctrl"},
        {0x38210050, 0, "addi r1,r1,80"},
        {0x3860FFFF, 0, "li r3,-1"},
        {0x3C60FFFF, 0, "lis r3,-1"},
        {0x60000000, 0, "nop"},
        {0x7C7F1B78, 0, "mr r31,r3"},
        {0xF821FFB1, 0, "stdu r1,-80(r1)"},
        {0xE8010010, 0, "ld r0,16(r1)"},
        {0x2C030002, 0, "cmpwi r3,2"},
        {0x2F830000, 0, "cmpwi cr7,r3,0"},
        {0x28030005, 0, "cmplwi r3,5"},
        {0x4182002C, 0x10038, "beq 0x10064"},
        {0x4200FFE4, 0x100B8, "bdnz 0x1009c"},
        {0x409E0010, 0x1000, "bne cr7,0x1010"},
        {0x4D820020, 0, "beqlr"},
        {0x7F7B1A14, 0, "add r27,r27,r3"},
        {0x7C632E14, 0, "addo r3,r3,r5"},
        {0x7C632E15, 0, "addo. r3,r3,r5"},
        {0x54A5283E, 0, "rotlwi r5,r5,5"},
        {0x78A30020, 0, "clrldi r3,r5,32"},
        {0x7B7B3800, 0, "rotldi r27,r27,7"},
        {0x44000002, 0, "sc"},
        {0x7FE00008, 0, "trap"},
        {0x7C2004AC, 0, "lwsync"},
        {0x7C0004AC, 0, "sync"},
        {0xFC20F890, 0, "fmr f1,f31"},
        {0x7C6802A6, 0, "mflr r3"},
        {0x7C6902A6, 0, "mfctr r3"},
        {0x7C6C42E6, 0, "mftb r3"},
        {0x00000000, 0, ".long 0x00000000"},
    };
    for (const Row& r : rows) CHECK(disassemble(r.raw, r.pc) == r.text);
    CHECK(disassemble(as::encode(Op::VOR, as::Fields{.d = 0, .a = 1, .b = 2}), 0) == "vor v0,v1,v2");
    CHECK(disassemble(as::lwzu(3, 8, 4), 0) == "lwzu r3,8(r4)");
    CHECK(disassemble(as::fmadd(1, 2, 3, 4, true), 0) == "fmadd. f1,f2,f3,f4");
    CHECK(disassemble(as::slwi(3, 4, 2), 0) == "slwi r3,r4,2");
    CHECK(disassemble(as::srwi(3, 4, 2), 0) == "srwi r3,r4,2");
    CHECK(disassemble(as::sldi(3, 4, 3), 0) == "sldi r3,r4,3");
    CHECK(disassemble(as::srdi(3, 4, 3), 0) == "srdi r3,r4,3");
    CHECK(disassemble(as::crlog(Op::CRXOR, 6, 6, 6), 0) == "crclr 6");
    CHECK(disassemble(as::mtcrf(0xFF, 3), 0) == "mtcr r3");
    CHECK(disassemble(as::mfocrf(3, 0x02), 0) == "mfocrf r3,cr6");
}

TEST_CASE("support classification: VMX and exotic forms are recognised but unsupported")
{
    const Inst vor = decode(as::encode(Op::VOR, as::Fields{.d = 0, .a = 1, .b = 2}));
    CHECK(vor.op == Op::VOR);
    CHECK_FALSE(is_supported(vor));
    CHECK(unsupported_reason(vor).find("VMX") != std::string_view::npos);
    CHECK_FALSE(is_supported(decode(as::encode(Op::LVX, as::Fields{.d = 1, .a = 3, .b = 4}))));
    CHECK_FALSE(is_supported(decode(as::encode(Op::VPERM, as::Fields{.d = 1, .a = 2, .b = 3, .c = 4}))));
    CHECK_FALSE(is_supported(decode(as::encode(Op::VCMPEQUW, as::Fields{.d = 1, .a = 2, .b = 3, .rc = true}))));
    CHECK_FALSE(is_supported(decode(as::encode(Op::ECIWX, as::Fields{.d = 1, .a = 2, .b = 3}))));
    CHECK_FALSE(is_supported(decode(0x44000022))); // sc 1 (hypervisor)
    CHECK_FALSE(is_supported(decode(as::mfspr(3, 287)))); // PVR
    CHECK_FALSE(is_supported(decode(as::mtspr(kSprTbl, 3))));
    CHECK_FALSE(is_supported(decode(as::mfocrf(3, 0x03))));
    CHECK(is_supported(decode(as::mfocrf(3, 0x40))));
    CHECK(is_supported(decode(0x44000002)));
    CHECK(decode(0x00000000).op == Op::UNKNOWN);
    CHECK_FALSE(is_supported(decode(0x00000000)));
}

TEST_CASE("forms: variants and names")
{
    const auto add = op_variants(Op::ADD);
    CHECK(std::vector<u32>(add.begin(), add.end()) == std::vector<u32>{0, kVarRc, kVarOe, kVarOe | kVarRc});
    CHECK(form_name(Op::ADD, kVarOe | kVarRc) == "addo.");
    CHECK(form_name(Op::B, kVarLk | kVarAa) == "bla");
    CHECK(form_name(Op::BCCTR, kVarLk) == "bcctrl");
    CHECK(op_variants(Op::LWZ).size() == 1);
    const auto forms = supported_forms();
    CHECK(forms.size() > 300);
    for (const Form& f : forms) CHECK((op_info(f.op).flags & (OPF_VMX | OPF_NOSUPPORT)) == 0);
}

TEST_CASE("register usage")
{
    RegUse u = reg_use(decode(as::add(3, 4, 5, true, true)));
    CHECK(u.gpr_read == ((1u << 4) | (1u << 5)));
    CHECK(u.gpr_write == (1u << 3));
    CHECK(u.cr_write == 0xF);
    CHECK(u.spr_write == (SPR_XER_OV | SPR_XER_SO));

    u = reg_use(decode(as::lwzu(3, 8, 4)));
    CHECK(u.gpr_write == ((1u << 3) | (1u << 4)));
    CHECK(u.mem_read);
    u = reg_use(decode(as::lwz(3, 8, 0))); // (rA|0): r0 is not read
    CHECK(u.gpr_read == 0);
    u = reg_use(decode(as::stmw(29, 0, 1)));
    CHECK(u.gpr_read == ((1u << 29) | (1u << 30) | (1u << 31) | (1u << 1)));
    u = reg_use(decode(as::b_rel(8, true)));
    CHECK(u.spr_write == SPR_LR);
    u = reg_use(decode(as::bc_rel(16, 0, 8))); // bdnz
    CHECK(u.spr_read == SPR_CTR);
    CHECK(u.spr_write == SPR_CTR);
    CHECK(u.cr_read == 0);
    u = reg_use(decode(as::bc_rel(12, 6, 8))); // beq cr1
    CHECK(u.cr_read == (1u << 6));
    u = reg_use(decode(as::fadd(1, 2, 3, true)));
    CHECK(u.fpr_read == ((1u << 2) | (1u << 3)));
    CHECK(u.cr_write == 0xF0);
    CHECK(u.spr_read == SPR_FPSCR);
    u = reg_use(decode(as::lswx(5, 3, 4)));
    CHECK(u.gpr_write == 0xFFFFFFFFu);
    CHECK(u.spr_read == SPR_XER_BC);
    u = reg_use(decode(as::mtcrf(0x81, 3)));
    CHECK(u.cr_write == 0xF000000Fu);
    u = reg_use(decode(as::lswi(30, 3, 9))); // 9 bytes: r30, r31, r0
    CHECK(u.gpr_write == ((1u << 30) | (1u << 31) | 1u));
    CHECK(lswi_reg_mask(0, 0) == 0xFFu);
}
