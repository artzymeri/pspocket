// Synthetic PPU programs (see synthetic.hpp). The jump-table functions reproduce the PPC64 GCC switch patterns
// the analyzer recognises.

#include "pspocket/recomp/ppu/synthetic.hpp"

#include <bit>

#include "pspocket/recomp/ppu/asm.hpp"

namespace pspocket::recomp::ppu::synthetic {
namespace {

using namespace as;
using Label = Assembler::Label;

class Builder {
public:
    explicit Builder(u32 base) : a(base) {}

    Label L(const std::string& name)
    {
        auto it = labels_.find(name);
        if (it == labels_.end()) it = labels_.emplace(name, a.new_label()).first;
        return it->second;
    }

    // Starts a function (16-byte aligned, like GCC output). seed = false keeps it out of the seed list.
    void func(const std::string& name, bool seed = true)
    {
        a.align(16);
        a.bind(L(name));
        functions_.push_back({name, seed});
    }

    void data(const std::string& name) { a.bind(L(name)); }

    // A place a non-local jump lands (a setjmp's return), given to the lifter as a continuation (D-255).
    void continuation(const std::string& name)
    {
        a.bind(L(name));
        continuations_.push_back(name);
    }

    Program finish(u32 toc)
    {
        Program p;
        p.base = a.base();
        p.toc = toc;
        p.bytes = a.finish();
        for (const auto& [name, label] : labels_)
            if (a.is_bound(label)) p.symbols[name] = a.address(label);
        for (const std::string& name : continuations_) p.continuations.push_back(p.symbols.at(name));
        for (const auto& [name, seed] : functions_) {
            if (!seed) continue;
            Seed s;
            s.addr = p.symbols.at(name);
            if (toc) s.toc = toc;
            s.name = name;
            p.seeds.push_back(s);
        }
        return p;
    }

    Assembler a;

private:
    std::map<std::string, Label> labels_;
    std::vector<std::pair<std::string, bool>> functions_;
    std::vector<std::string> continuations_;
};

// Standard frame helpers (PPC64 ELF ABI: LR saved at 16(r1) of the caller's frame).
void prologue(Assembler& a, s32 frame)
{
    a.emit(mflr(0));
    a.emit(std_(0, 16, 1));
    a.emit(stdu(1, -frame, 1));
}

void epilogue(Assembler& a, s32 frame)
{
    a.emit(addi(1, 1, frame));
    a.emit(ld(0, 16, 1));
    a.emit(mtlr(0));
    a.emit(blr());
}

void emit_fib(Builder& B)
{
    Assembler& a = B.a;
    B.func("fib");
    const Label ret = a.new_label();
    a.emit(cmpwi(0, 3, 2));
    a.blt(0, ret);
    a.emit(mflr(0));
    a.emit(std_(0, 16, 1));
    a.emit(stdu(1, -48, 1));
    a.emit(std_(31, 40, 1));
    a.emit(std_(30, 32, 1));
    a.emit(mr(31, 3));
    a.emit(addi(3, 3, -1));
    a.bl(B.L("fib"));
    a.emit(mr(30, 3));
    a.emit(addi(3, 31, -2));
    a.bl(B.L("fib"));
    a.emit(add(3, 3, 30));
    a.emit(ld(30, 32, 1));
    a.emit(ld(31, 40, 1));
    a.emit(addi(1, 1, 48));
    a.emit(ld(0, 16, 1));
    a.emit(mtlr(0));
    a.bind(ret);
    a.emit(blr());
}

} // namespace

Program build_test_programs(u32 base)
{
    Builder B(base);
    Assembler& a = B.a;

    // ---- sum_to(n)
    B.func("sum_to");
    {
        const Label loop = a.new_label(), done = a.new_label();
        a.emit(li(4, 0));
        a.emit(cmpwi(0, 3, 0));
        a.beq(0, done);
        a.emit(mtctr(3));
        a.emit(li(5, 1));
        a.bind(loop);
        a.emit(add(4, 4, 5));
        a.emit(addi(5, 5, 1));
        a.bdnz(loop);
        a.bind(done);
        a.emit(mr(3, 4));
        a.emit(blr());
    }

    emit_fib(B);

    // ---- memcpy_bytes(d, s, n)
    B.func("memcpy_bytes");
    {
        const Label loop = a.new_label();
        a.emit(cmpwi(0, 5, 0));
        a.emit(bclr(BO_TRUE, crbit(0, CR_EQ))); // beqlr
        a.emit(mtctr(5));
        a.emit(addi(4, 4, -1));
        a.emit(addi(6, 3, -1));
        a.bind(loop);
        a.emit(lbzu(7, 1, 4));
        a.emit(stbu(7, 1, 6));
        a.bdnz(loop);
        a.emit(blr());
    }

    // ---- memset_bytes(d, c, n)
    B.func("memset_bytes");
    {
        const Label loop = a.new_label(), done = a.new_label();
        a.emit(li(6, 0));
        a.bind(loop);
        a.emit(cmplw(0, 6, 5));
        a.bge(0, done);
        a.emit(stbx(4, 3, 6));
        a.emit(addi(6, 6, 1));
        a.b(loop);
        a.bind(done);
        a.emit(blr());
    }

    // ---- copy_dwords(d, s, n)
    B.func("copy_dwords");
    {
        const Label loop = a.new_label(), done = a.new_label();
        a.emit(cmpwi(0, 5, 0));
        a.beq(0, done);
        a.emit(mtctr(5));
        a.emit(li(6, 0));
        a.bind(loop);
        a.emit(ldx(7, 4, 6));
        a.emit(stdx(7, 3, 6));
        a.emit(addi(6, 6, 8));
        a.bdnz(loop);
        a.bind(done);
        a.emit(blr());
    }

    // ---- bubble_sort(a, n), int32
    B.func("bubble_sort");
    {
        const Label outer = a.new_label(), inner = a.new_label(), noswap = a.new_label(), inner_done = a.new_label(),
                    done = a.new_label();
        a.emit(cmpwi(0, 4, 1));
        a.emit(bclr(BO_FALSE, crbit(0, CR_GT))); // blelr
        a.emit(addi(5, 4, -1));
        a.bind(outer);
        a.emit(li(6, 0));
        a.emit(li(9, 0));
        a.bind(inner);
        a.emit(cmpw(0, 6, 5));
        a.bge(0, inner_done);
        a.emit(slwi(7, 6, 2));
        a.emit(add(8, 3, 7));
        a.emit(lwz(10, 0, 8));
        a.emit(lwz(11, 4, 8));
        a.emit(cmpw(1, 10, 11));
        a.ble(1, noswap);
        a.emit(stw(11, 0, 8));
        a.emit(stw(10, 4, 8));
        a.emit(li(9, 1));
        a.bind(noswap);
        a.emit(addi(6, 6, 1));
        a.b(inner);
        a.bind(inner_done);
        a.emit(cmpwi(0, 9, 0));
        a.beq(0, done);
        a.emit(addi(5, 5, -1));
        a.emit(cmpwi(0, 5, 0));
        a.bgt(0, outer);
        a.bind(done);
        a.emit(blr());
    }

    // ---- sw_rel(x): GCC PPC64 switch, relative entries right after the bctr
    B.func("sw_rel");
    {
        const Label table = a.new_label(), def = a.new_label();
        Label c[7];
        for (Label& l : c) l = a.new_label();
        a.emit(cmplwi(7, 3, 6));
        a.bgt(7, def);
        a.load_address_ha(9, table);
        a.emit(slwi(10, 3, 2));
        a.emit(lwax(8, 9, 10));
        a.emit(add(8, 8, 9));
        a.emit(mtctr(8));
        a.emit(bctr());
        a.bind(table);
        const int order[7] = {0, 1, 2, 3, 2, 5, 6}; // case 4 shares case 2's code
        for (int i : order) a.label_offset_word(c[i], table);
        a.bind(c[0]);
        a.emit(li(3, 100));
        a.emit(blr());
        a.bind(c[1]);
        a.emit(li(3, 111));
        a.emit(blr());
        a.bind(c[2]);
        a.emit(li(3, 122));
        a.emit(blr());
        a.bind(c[3]);
        a.emit(addi(3, 3, 1000));
        a.emit(blr());
        a.bind(c[4]); // unused label
        a.bind(c[5]);
        a.emit(li(3, 155));
        a.emit(blr());
        a.bind(c[6]);
        a.emit(li(3, -66));
        a.emit(blr());
        a.bind(def);
        a.emit(li(3, -1));
        a.emit(blr());
    }

    // ---- sw_toc(x): table address from the TOC, clrlsldi scaling, table in the data area
    B.func("sw_toc");
    {
        const Label def = a.new_label();
        a.emit(cmplwi(0, 3, 4));
        a.bgt(0, def);
        a.emit(ld(9, 16, 2));
        a.emit(rldic(10, 3, 2, 30));
        a.emit(lwax(8, 10, 9));
        a.emit(add(8, 9, 8));
        a.emit(mtctr(8));
        a.emit(bctr());
        a.bind(def);
        a.emit(li(3, 0));
        a.emit(blr());
        a.bind(B.L("sw_toc.0"));
        a.emit(li(3, 10));
        a.emit(blr());
        a.bind(B.L("sw_toc.1"));
        a.emit(li(3, 20));
        a.emit(blr());
        a.bind(B.L("sw_toc.2"));
        a.emit(mulli(3, 3, 7));
        a.emit(blr());
        a.bind(B.L("sw_toc.3"));
        a.emit(li(3, 40));
        a.emit(blr());
        a.bind(B.L("sw_toc.4"));
        a.emit(neg(3, 3));
        a.emit(blr());
    }

    // ---- sw_snc(x): the SNC switch: the index masked to a byte by both the bounds check (clrlwi; cmplwi) and the
    // scaling (rlwinm r9, r9, 2, 22, 29), the table address loaded from the TOC as a word, the entries sign-extended
    // by extsw after lwzx, and unrelated instructions between the compare and its branch.
    B.func("sw_snc");
    {
        const Label def = a.new_label();
        a.emit(addi(9, 3, -37));
        a.emit(clrlwi(0, 9, 24));
        a.emit(cmplwi(7, 0, 3));
        a.emit(mr(7, 3));
        a.emit(li(8, 0));
        a.bgt(7, def);
        a.emit(lwz(11, 24, 2));
        a.emit(rlwinm(9, 9, 2, 22, 29));
        a.emit(lwzx(0, 9, 11));
        a.emit(extsw(0, 0));
        a.emit(add(0, 0, 11));
        a.emit(mtctr(0));
        a.emit(bctr());
        a.bind(def);
        a.emit(li(3, -1));
        a.emit(blr());
        a.bind(B.L("sw_snc.0"));
        a.emit(li(3, 70));
        a.emit(blr());
        a.bind(B.L("sw_snc.1"));
        a.emit(addi(3, 7, 1));
        a.emit(blr());
        a.bind(B.L("sw_snc.2"));
        a.emit(li(3, 72));
        a.emit(blr());
        a.bind(B.L("sw_snc.3"));
        a.emit(li(3, 73));
        a.emit(blr());
    }

    // ---- entry_above(x): x + 5, by setting r7 and branching back into a body below the entry (not a function of its
    // own: never called, never seeded), like SNC's wrappers around a shared body.
    B.func("entry_above.body", false);
    a.emit(add(3, 3, 7));
    a.emit(blr());
    B.func("entry_above");
    a.emit(li(7, 5));
    a.b(B.L("entry_above.body"));

    // ---- vm(code): a bytecode interpreter in SNC's shape. The dispatch table's address is set once in the prologue (lis
    // and addic into r19, which only the exit helper restores), a call sits between it and the first dispatch (the
    // matcher's backward slice stops there), and the opcode is a loaded byte with no bounds check. Opcodes: 0 return
    // acc, 1 acc += 1, 2 acc *= 2, 3 acc += 10, 4 acc = add_one(acc). Handlers 1, 3 and 4 dispatch again themselves (4
    // across a call); 2 branches back.
    B.func("vm");
    {
        const Label dispatch = a.new_label();
        auto dispatch_here = [&] {
            a.emit(lbz(5, 0, 31));
            a.emit(addi(31, 31, 1));
            a.emit(slwi(5, 5, 2));
            a.emit(lwzx(5, 19, 5));
            a.emit(mtctr(5));
            a.emit(bctr());
        };
        prologue(a, 112);
        a.emit(std_(19, 64, 1));
        a.emit(std_(30, 72, 1));
        a.emit(std_(31, 80, 1));
        a.emit(std_(26, 88, 1));
        a.load_address_hac(19, B.L("vm.table"));
        a.emit(mr(31, 3));
        a.emit(li(3, -1));
        a.bl(B.L("add_one"));
        a.emit(mr(30, 3)); // acc = 0
        a.bind(dispatch);
        dispatch_here();
        a.bind(B.L("vm.0"));
        a.emit(mr(3, 30));
        a.b(B.L("vm_exit"));
        a.bind(B.L("vm.1"));
        a.emit(addi(30, 30, 1));
        dispatch_here();
        a.bind(B.L("vm.2"));
        a.emit(add(30, 30, 30));
        a.b(dispatch);
        a.bind(B.L("vm.3"));
        a.emit(addi(30, 30, 10));
        dispatch_here();
        // 4: acc = add_one(acc), the next handler's address loaded before the call into r26 (which the call keeps):
        // the matcher's slice stops at the call, so this dispatch is not a recognised table.
        a.bind(B.L("vm.4"));
        a.emit(lbz(5, 0, 31));
        a.emit(addi(31, 31, 1));
        a.emit(slwi(5, 5, 2));
        a.emit(lwzx(26, 19, 5));
        a.emit(mr(3, 30));
        a.bl(B.L("add_one"));
        a.emit(mr(30, 3));
        a.emit(mtctr(26));
        a.emit(bctr());
    }
    // Restores vm's registers and returns from it, reached by a branch (SNC's restgpr helpers): part of vm, so vm writes
    // r19 there too, on its way out.
    B.func("vm_exit", false);
    a.emit(ld(19, 64, 1));
    a.emit(ld(30, 72, 1));
    a.emit(ld(31, 80, 1));
    a.emit(ld(26, 88, 1));
    epilogue(a, 112);

    // ---- sw_abs(x): lis/ori table address, absolute entries
    B.func("sw_abs");
    {
        const Label tab = a.new_label(), def = a.new_label();
        Label c[4];
        for (Label& l : c) l = a.new_label();
        a.emit(cmplwi(0, 3, 3));
        a.bgt(0, def);
        a.load_address(9, tab);
        a.emit(slwi(10, 3, 2));
        a.emit(lwzx(11, 9, 10));
        a.emit(mtctr(11));
        a.emit(bctr());
        a.bind(tab);
        for (const Label& l : c) a.label_word(l);
        a.bind(c[0]);
        a.emit(li(3, 1000));
        a.emit(blr());
        a.bind(c[1]);
        a.emit(li(3, 2000));
        a.emit(blr());
        a.bind(c[2]);
        a.emit(subfic(3, 3, 50));
        a.emit(blr());
        a.bind(c[3]);
        a.emit(li(3, 4000));
        a.emit(blr());
        a.bind(def);
        a.emit(li(3, 9));
        a.emit(blr());
    }

    // ---- function-pointer targets and callers
    B.func("add_one");
    a.emit(addi(3, 3, 1));
    a.emit(blr());
    B.func("times_three");
    a.emit(mulli(3, 3, 3));
    a.emit(blr());
    B.func("secret_fn", false); // not seeded: reached only through a pointer -> dispatch miss
    a.emit(xori(3, 3, 0x5a5a));
    a.emit(blr());

    B.func("apply");
    prologue(a, 32);
    a.emit(mtctr(3));
    a.emit(mr(3, 4));
    a.emit(bctrl());
    epilogue(a, 32);

    B.func("apply_table");
    prologue(a, 32);
    a.load_address(9, B.L("fntab"));
    a.emit(slwi(10, 3, 2));
    a.emit(lwzx(11, 9, 10));
    a.emit(mtctr(11));
    a.emit(mr(3, 4));
    a.emit(bctrl());
    epilogue(a, 32);

    // ---- floating point
    B.func("dot_sqrt"); // (a, b, n) -> f1
    {
        const Label loop = a.new_label(), done = a.new_label();
        a.emit(li(7, 0));
        a.emit(std_(7, -8, 1)); // red zone
        a.emit(lfd(1, -8, 1));  // f1 = +0.0
        a.emit(cmpwi(0, 5, 0));
        a.beq(0, done);
        a.emit(mtctr(5));
        a.emit(li(6, 0));
        a.bind(loop);
        a.emit(lfdx(2, 3, 6));
        a.emit(lfdx(3, 4, 6));
        a.emit(fmadd(1, 2, 3, 1));
        a.emit(addi(6, 6, 8));
        a.bdnz(loop);
        a.bind(done);
        a.emit(fsqrt(1, 1));
        a.emit(blr());
    }
    B.func("sum_floats_to_int"); // (a, n) -> r3 = (int)sum
    {
        const Label loop = a.new_label(), done = a.new_label();
        a.emit(li(7, 0));
        a.emit(stw(7, -16, 1));
        a.emit(lfs(1, -16, 1));
        a.emit(cmpwi(0, 4, 0));
        a.beq(0, done);
        a.emit(mtctr(4));
        a.emit(li(6, 0));
        a.bind(loop);
        a.emit(lfsx(2, 3, 6));
        a.emit(fadds(1, 1, 2));
        a.emit(addi(6, 6, 4));
        a.bdnz(loop);
        a.bind(done);
        a.emit(fctiwz(0, 1));
        a.emit(addi(8, 1, -8));
        a.emit(stfiwx(0, 0, 8));
        a.emit(lwz(3, -8, 1));
        a.emit(extsw(3, 3));
        a.emit(blr());
    }
    B.func("fmax"); // (f1, f2) -> f1
    {
        const Label ret = a.new_label();
        a.emit(fcmpu(0, 1, 2));
        a.bge(0, ret);
        a.emit(fmr(1, 2));
        a.bind(ret);
        a.emit(blr());
    }

    // ---- multi-word arithmetic
    B.func("add256"); // (d, a, b) -> carry
    for (s32 i = 0; i < 4; i++) {
        a.emit(ld(6, 8 * i, 4));
        a.emit(ld(7, 8 * i, 5));
        a.emit(i == 0 ? addc(8, 6, 7) : adde(8, 6, 7));
        a.emit(std_(8, 8 * i, 3));
    }
    a.emit(li(3, 0));
    a.emit(addze(3, 3));
    a.emit(blr());

    B.func("sub256"); // (d, a, b) -> 0 or -1 (borrow)
    for (s32 i = 0; i < 4; i++) {
        a.emit(ld(6, 8 * i, 4));
        a.emit(ld(7, 8 * i, 5));
        a.emit(i == 0 ? subfc(8, 7, 6) : subfe(8, 7, 6));
        a.emit(std_(8, 8 * i, 3));
    }
    a.emit(subfe(3, 3, 3));
    a.emit(blr());

    // ---- atomics, system
    B.func("atomic_add"); // (p, v) -> new value
    {
        const Label retry = a.here();
        a.emit(lwarx(5, 0, 3));
        a.emit(add(5, 5, 4));
        a.emit(stwcx_(5, 0, 3));
        a.bne(0, retry);
        a.emit(mr(3, 5));
        a.emit(blr());
    }
    B.func("syscall_test");
    a.emit(li(11, 77));
    a.emit(sc());
    a.emit(blr());
    B.func("trap_test");
    a.emit(twi(4, 3, 13)); // trap if x == 13
    a.emit(addi(3, 3, 1));
    a.emit(blr());

    B.func("pc_rel");
    a.emit(mflr(0));
    a.emit(bc_rel(BO_ALWAYS, 31, 4, true)); // bcl 20,31,$+4
    a.emit(mflr(9));
    a.emit(mtlr(0));
    a.emit(lwz(3, 16, 9));
    a.emit(blr());
    a.word(0xC0FFEE11);

    B.func("str_copy13"); // (d, s)
    a.emit(lswi(5, 4, 13));
    a.emit(stswi(5, 3, 13));
    a.emit(blr());

    B.func("cr_games"); // (x, y)
    a.emit(cmpw(0, 3, 4));
    a.emit(cmpwi(1, 3, 0));
    a.emit(cmplw(7, 4, 3));
    a.emit(crlog(Op::CROR, 2, 0, 5));     // cr0.eq = cr0.lt | cr1.gt
    a.emit(crlog(Op::CRXOR, 6, 6, 6));    // crclr cr1.eq
    a.emit(mcrf(5, 7));
    a.emit(crlog(Op::CREQV, 20, 20, 20)); // crset cr5.lt
    a.emit(crlog(Op::CRNAND, 9, 28, 1));  // cr2.gt = !(cr7.lt & cr0.gt)
    a.emit(mfcr(3));
    a.emit(li(5, 0x5A));
    a.emit(mtcrf(0x0F, 5));
    a.emit(mfocrf(4, 0x02));
    a.emit(add(3, 3, 4));
    a.emit(blr());

    // ---- control-flow shapes
    B.func("gcd"); // (a, b), unsigned; tail recursion as a branch to its own entry
    a.emit(cmpwi(0, 4, 0));
    a.emit(bclr(BO_TRUE, crbit(0, CR_EQ)));
    a.emit(divwu(5, 3, 4));
    a.emit(mullw(6, 5, 4));
    a.emit(subf(5, 6, 3));
    a.emit(mr(3, 4));
    a.emit(mr(4, 5));
    a.b(B.L("gcd"));

    // ---- setjmp/longjmp (D-161): a longjmp's blr leaves r1 elsewhere than at its entry; translated code must go on
    // at the setjmp's return in the frame the jump restored, as the interpreter does.
    B.func("jmp_set"); // (buf): r1, r2, r31 and LR saved; returns 0
    a.emit(std_(1, 0, 3));
    a.emit(std_(2, 8, 3));
    a.emit(std_(31, 16, 3));
    a.emit(mflr(0));
    a.emit(std_(0, 24, 3));
    a.emit(li(3, 0));
    a.emit(blr());
    B.func("jmp_long"); // (buf, val): back to the jmp_set's return with val
    a.emit(ld(1, 0, 3));
    a.emit(ld(2, 8, 3));
    a.emit(ld(31, 16, 3));
    a.emit(ld(0, 24, 3));
    a.emit(mtlr(0));
    a.emit(mr(3, 4));
    a.emit(blr());
    B.func("jmp_deep"); // (buf, depth): depth frames down, then jmp_long(buf, 7)
    {
        const Label bottom = a.new_label();
        a.emit(mflr(0));
        a.emit(std_(0, 16, 1));
        a.emit(stdu(1, -112, 1));
        a.emit(cmpwi(0, 4, 0));
        a.beq(0, bottom);
        a.emit(addi(4, 4, -1));
        a.emit(li(31, 0x55)); // clobbered here: the jump must bring back the jmp_set's r31
        a.bl(B.L("jmp_deep"));
        a.bind(bottom);
        a.emit(li(4, 7));
        a.bl(B.L("jmp_long"));
        a.emit(addi(1, 1, 112)); // never reached
        a.emit(ld(0, 16, 1));
        a.emit(mtlr(0));
        a.emit(blr());
    }
    B.func("jmp_test"); // (x): r = jmp_set(buf); r == 0 ? jmp_deep(buf, 3) : x + r
    {
        const Label after = a.new_label();
        a.emit(mflr(0));
        a.emit(std_(0, 16, 1));
        a.emit(std_(31, -8, 1));
        a.emit(stdu(1, -160, 1));
        a.emit(mr(31, 3));
        a.emit(addi(3, 1, 112));
        a.bl(B.L("jmp_set"));
        B.continuation("jmp_test.set_returned"); // where jmp_long lands: translated as a continuation (D-255)
        a.emit(cmpwi(0, 3, 0));
        a.bne(0, after);
        a.emit(addi(3, 1, 112));
        a.emit(li(4, 3));
        a.bl(B.L("jmp_deep"));
        a.bind(after);
        a.emit(add(3, 3, 31));
        a.emit(addi(1, 1, 160));
        a.emit(ld(0, 16, 1));
        a.emit(ld(31, -8, 1));
        a.emit(mtlr(0));
        a.emit(blr());
    }

    // A landing without a continuation (D-255): the interpreter runs it and the frame above, and notes the return it
    // makes into jmp_wrap (no translated code there either) for the next preparation.
    B.func("jmp_mid"); // (x): as jmp_test
    {
        const Label after = a.new_label();
        a.emit(mflr(0));
        a.emit(std_(0, 16, 1));
        a.emit(std_(31, -8, 1));
        a.emit(stdu(1, -160, 1));
        a.emit(mr(31, 3));
        a.emit(addi(3, 1, 112));
        a.bl(B.L("jmp_set"));
        B.data("jmp_mid.set_returned");
        a.emit(cmpwi(0, 3, 0));
        a.bne(0, after);
        a.emit(addi(3, 1, 112));
        a.emit(li(4, 3));
        a.bl(B.L("jmp_deep"));
        a.bind(after);
        a.emit(add(3, 3, 31));
        a.emit(addi(1, 1, 160));
        a.emit(ld(0, 16, 1));
        a.emit(ld(31, -8, 1));
        a.emit(mtlr(0));
        a.emit(blr());
    }
    B.func("jmp_wrap"); // (x): jmp_mid(x) + 1
    a.emit(mflr(0));
    a.emit(std_(0, 16, 1));
    a.emit(stdu(1, -112, 1));
    a.bl(B.L("jmp_mid"));
    B.data("jmp_wrap.mid_returned");
    a.emit(addi(3, 3, 1));
    a.emit(addi(1, 1, 112));
    a.emit(ld(0, 16, 1));
    a.emit(mtlr(0));
    a.emit(blr());

    B.func("tail_calls"); // x + 5 == 10 ? add_one(x + 5) : times_three(x + 5)
    a.emit(addi(3, 3, 5));
    a.emit(cmpwi(0, 3, 10));
    a.beq(0, B.L("add_one"));
    a.b(B.L("times_three"));

    B.func("driver"); // sum_to(n) + fib(n) + sw_rel(n & 7)
    prologue(a, 64);
    a.emit(std_(31, 56, 1));
    a.emit(std_(30, 48, 1));
    a.emit(mr(31, 3));
    a.bl(B.L("sum_to"));
    a.emit(mr(30, 3));
    a.emit(mr(3, 31));
    a.bl(B.L("fib"));
    a.emit(add(30, 30, 3));
    a.emit(andi_(3, 31, 7));
    a.bl(B.L("sw_rel"));
    a.emit(add(3, 30, 3));
    a.emit(ld(30, 48, 1));
    a.emit(ld(31, 56, 1));
    epilogue(a, 64);

    // ---- rarer control flow: conditional bcctr, blrl, conditional bcl, bdzlr, trap resume, fall-through
    B.func("cond_ctr"); // (x, fn): x != 0 ? fn(x) as a tail jump : -1
    a.emit(mtctr(4));
    a.emit(cmpwi(0, 3, 0));
    a.emit(bcctr(BO_FALSE, crbit(0, CR_EQ))); // bnectr
    a.emit(li(3, -1));
    a.emit(blr());

    B.func("blrl_call"); // (fn, x): fn(x) through LR
    prologue(a, 32);
    a.emit(mtlr(3));
    a.emit(mr(3, 4));
    a.emit(blrl());
    epilogue(a, 32);

    B.func("cond_call"); // x == 5 ? add_one(x) : x, via beql
    prologue(a, 32);
    a.emit(cmpwi(0, 3, 5));
    a.bc(BO_TRUE, crbit(0, CR_EQ), B.L("add_one"), true);
    epilogue(a, 32);

    B.func("count_bdzlr"); // n >= 1: returns n, leaving through bdzlr
    {
        const Label loop = a.new_label();
        a.emit(mtctr(3));
        a.emit(li(3, 0));
        a.bind(loop);
        a.emit(addi(3, 3, 1));
        a.emit(bclr(BO_DZ, 0)); // bdzlr
        a.b(loop);
    }

    B.func("trap_resume"); // trap; the handler sets r3 = 1000; then r3 += 7
    a.emit(trap());
    a.emit(addi(3, 3, 7));
    a.emit(blr());

    B.func("fall_a"); // (x + 3) * 2, by falling through into fall_b (16 bytes, so fall_b follows without padding)
    a.emit(addi(3, 3, 3));
    a.emit(nop());
    a.emit(nop());
    a.emit(nop());
    B.func("fall_b");
    a.emit(mulli(3, 3, 2));
    a.emit(blr());

    // ---- data
    a.align(16);
    B.data("fntab");
    a.label_word(B.L("add_one"));
    a.label_word(B.L("times_three"));
    a.label_word(B.L("secret_fn"));
    a.align(16);
    B.data("sw_toc.table");
    for (int i = 0; i < 5; i++) a.label_offset_word(B.L("sw_toc." + std::to_string(i)), B.L("sw_toc.table"));
    a.align(16);
    a.align(16);
    B.data("vm.table");
    for (int i = 0; i < 5; i++) a.label_word(B.L("vm." + std::to_string(i)));
    a.word(0); // not code: the end of the table
    a.align(16);
    B.data("sw_snc.table"); // in the data area, like SNC's .rodata
    for (int i = 0; i < 4; i++) a.label_offset_word(B.L("sw_snc." + std::to_string(i)), B.L("sw_snc.table"));
    a.align(16);
    B.data("toc");
    a.word(0);
    a.word(0);
    a.word(0);
    a.word(0);
    a.label_dword(B.L("sw_toc.table")); // toc + 16
    a.label_word(B.L("sw_snc.table"));  // toc + 24

    const u32 toc = a.address(B.L("toc"));
    return B.finish(toc);
}

Program build_selftest_bench(u32 base)
{
    Builder B(base);
    Assembler& a = B.a;

    B.func("bench"); // (buf, words, iterations, fib_n) -> acc
    {
        const Label loop = a.new_label(), done = a.new_label();
        prologue(a, 80);
        for (u32 r = 27; r <= 31; r++) a.emit(std_(r, static_cast<s32>(40 + 8 * (r - 27)), 1));
        a.emit(mr(31, 3));
        a.emit(mr(30, 4));
        a.emit(mr(29, 5));
        a.emit(mr(28, 6));
        a.emit(li(27, 0));
        a.bind(loop);
        a.emit(cmpwi(0, 29, 0));
        a.beq(0, done);
        a.emit(mr(3, 31));
        a.emit(mr(4, 30));
        a.bl(B.L("checksum"));
        a.emit(add(27, 27, 3));
        a.emit(mr(3, 28));
        a.bl(B.L("fib"));
        a.emit(xor_(27, 27, 3));
        a.emit(rotldi(27, 27, 7));
        a.emit(addi(29, 29, -1));
        a.b(loop);
        a.bind(done);
        a.emit(mr(3, 27));
        for (u32 r = 27; r <= 31; r++) a.emit(ld(r, static_cast<s32>(40 + 8 * (r - 27)), 1));
        epilogue(a, 80);
    }

    B.func("checksum"); // (buf, words) -> 32-bit checksum; rewrites the buffer
    {
        const Label loop = a.new_label();
        a.emit(li(5, 0x1234));
        a.emit(mtctr(4));
        a.emit(li(6, 0));
        a.bind(loop);
        a.emit(lwzx(7, 3, 6));
        a.emit(rotlwi(5, 5, 5));
        a.emit(xor_(5, 5, 7));
        a.emit(add(5, 5, 6));
        a.emit(add(8, 7, 5));
        a.emit(stwx(8, 3, 6));
        a.emit(addi(6, 6, 4));
        a.bdnz(loop);
        a.emit(clrldi(3, 5, 32));
        a.emit(blr());
    }

    emit_fib(B);
    return B.finish(0);
}

u64 fib_reference(u32 n) { return n < 2 ? n : fib_reference(n - 1) + fib_reference(n - 2); }

u64 bench_reference(std::span<u32> buffer, u32 iterations, u32 fib_n)
{
    const u64 f = fib_reference(fib_n);
    u64 acc = 0;
    for (u32 it = 0; it < iterations; it++) {
        u64 r5 = 0x1234;
        for (usize i = 0; i < buffer.size(); i++) {
            const u64 r6 = 4 * i;
            const u64 r7 = buffer[i];
            r5 = std::rotl(static_cast<u32>(r5), 5); // rotlwi: ROTL32 masked to the low word
            r5 ^= r7;
            r5 += r6;
            buffer[i] = static_cast<u32>(r7 + r5);
        }
        acc += r5 & 0xFFFFFFFFull;
        acc ^= f;
        acc = std::rotl(acc, 7);
    }
    return acc;
}

} // namespace pspocket::recomp::ppu::synthetic
