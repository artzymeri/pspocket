// PPU analyzer. Function discovery and the jump-table matcher follow the approach of RPCS3's analyser
// (rpcs3/Emu/Cell/PPUAnalyser.cpp: bl targets as functions, relative jump tables after bctr), reduced to what the
// lifter needs and made explicit about what it recognises.

#include "pspocket/recomp/ppu/analyzer.hpp"

#include <algorithm>
#include <format>

#include "pspocket/ppu/decoder.hpp"
#include "pspocket/ppu/disasm.hpp"

namespace pspocket::recomp::ppu {
namespace {

namespace rt = ::pspocket::ppu;
using rt::Inst;
using rt::Op;

constexpr int kSliceLimit = 48; // instructions scanned backwards by the jump-table matcher
constexpr u32 kMaxTableEntries = 4096;
constexpr u32 kMaxScannedEntries = 1024;

bool always_taken(const Inst& in)
{
    switch (in.op) {
    case Op::B: return true;
    case Op::BC:
    case Op::BCLR: return (in.bo() & 0x14) == 0x14;
    case Op::BCCTR: return (in.bo() & 0x10) != 0; // bcctr never tests CTR
    default: return false;
    }
}

// Instructions after which control never falls through to pc + 4.
bool is_terminator(const Inst& in)
{
    if (in.op == Op::UNKNOWN) return true;
    if (rt::is_unconditional_trap(in)) return true;
    if (in.flags() & rt::OPF_BRANCH) {
        if (in.lk()) return false; // calls return
        return always_taken(in);
    }
    return false;
}

bool is_call(const Inst& in)
{
    return ((in.flags() & rt::OPF_BRANCH) && in.lk()) || in.op == Op::SC;
}

struct Def {
    u32 pc;
    Inst in;
};

class Walker {
public:
    Walker(const CodeImage& image, const std::set<u32>& entries, const AnalyzerOptions& options, Program* diag)
        : image_(image), entries_(entries), options_(options), diag_(diag)
    {
    }

    Function explore(u32 entry, std::optional<u32> toc);

private:
    void branch_to(Function& f, u32 target, u32 pc, std::vector<u32>& work);
    std::optional<JumpTable> match_jump_table(u32 bctr_pc, std::optional<u32> toc);

    Inst insn(u32 pc) const { return rt::decode(image_.read32(pc).value_or(0)); }
    std::optional<Def> find_def(u32 from_pc, u32 gpr_mask, u32 spr_mask) const;
    std::optional<Def> find_gpr_def(u32 reg, u32 from_pc) const { return find_def(from_pc, 1u << reg, 0); }
    std::optional<u64> eval_const(u32 reg, u32 at_pc, std::optional<u32> toc, int depth) const;
    std::optional<u32> find_bound(u32 index_reg, u32 from_pc) const;

    void warn(u32 addr, std::string message)
    {
        if (diag_) diag_->warnings.push_back(Diagnostic{addr, image_.read32(addr).value_or(0), std::move(message)});
    }

    const CodeImage& image_;
    const std::set<u32>& entries_;
    const AnalyzerOptions& options_;
    Program* diag_;
    const std::set<u32>* visited_ = nullptr; // instructions of the function being explored
};

std::optional<Def> Walker::find_def(u32 from_pc, u32 gpr_mask, u32 spr_mask) const
{
    u32 pc = from_pc;
    for (int i = 0; i < kSliceLimit; i++) {
        if (pc < 4) return std::nullopt;
        pc -= 4;
        if (!visited_->contains(pc)) return std::nullopt;
        const Inst in = insn(pc);
        if (is_terminator(in) || is_call(in)) return std::nullopt; // not a straight-line predecessor
        const rt::RegUse u = rt::reg_use(in);
        if ((u.gpr_write & gpr_mask) || (u.spr_write & spr_mask)) return Def{pc, in};
    }
    return std::nullopt;
}

// Constant value of a GPR just before at_pc, from lis/addi/ori chains, TOC loads and the bcl/mflr PC idiom.
std::optional<u64> Walker::eval_const(u32 reg, u32 at_pc, std::optional<u32> toc, int depth) const
{
    if (depth > 8) return std::nullopt;
    const auto def = find_gpr_def(reg, at_pc);
    if (!def) {
        if (reg == 2 && toc) return *toc;
        return std::nullopt;
    }
    const Inst& in = def->in;
    auto ra_val = [&]() -> std::optional<u64> {
        if (in.ra() == 0) return 0;
        return eval_const(in.ra(), def->pc, toc, depth + 1);
    };
    switch (in.op) {
    case Op::ADDI:
        if (auto v = ra_val()) return *v + static_cast<u64>(static_cast<s64>(in.simm()));
        return std::nullopt;
    case Op::ADDIS:
        if (auto v = ra_val()) return *v + static_cast<u64>(static_cast<s64>(in.simm()) * 65536);
        return std::nullopt;
    case Op::ORI:
        if (auto v = eval_const(in.rs(), def->pc, toc, depth + 1)) return *v | in.uimm();
        return std::nullopt;
    case Op::ORIS:
        if (auto v = eval_const(in.rs(), def->pc, toc, depth + 1)) return *v | (u64{in.uimm()} << 16);
        return std::nullopt;
    case Op::OR:
        if (in.rs() == in.rb()) return eval_const(in.rs(), def->pc, toc, depth + 1);
        return std::nullopt;
    case Op::LWZ:
        if (auto v = ra_val()) {
            if (auto m = image_.read32(static_cast<u32>(*v + static_cast<u64>(static_cast<s64>(in.simm()))))) return *m;
        }
        return std::nullopt;
    case Op::LD:
        if (auto v = ra_val()) {
            if (auto m = image_.read64(static_cast<u32>(*v + static_cast<u64>(static_cast<s64>(in.ds()))))) return *m;
        }
        return std::nullopt;
    case Op::RLDICL:
        if (in.sh64() == 0)
            if (auto v = eval_const(in.rs(), def->pc, toc, depth + 1)) return *v & (~0ull >> in.mb64());
        return std::nullopt;
    case Op::EXTSW:
        if (auto v = eval_const(in.rs(), def->pc, toc, depth + 1))
            return static_cast<u64>(static_cast<s64>(static_cast<s32>(*v)));
        return std::nullopt;
    case Op::MFSPR:
        if (in.spr() == rt::kSprLr) {
            // bcl 20,31,$+4 ; mflr rX  ->  rX = address after the bcl
            const u32 prev = def->pc - 4;
            if (visited_->contains(prev)) {
                const Inst b = insn(prev);
                if ((b.op == Op::BC || b.op == Op::B) && b.lk() && rt::branch_target(b, prev) == prev + 4) return u64{prev + 4};
            }
        }
        return std::nullopt;
    default: return std::nullopt;
    }
}

// Entry count from the bounds check guarding the table: "cmplwi crN, rI, max; bgt crN, default" (count max + 1)
// or "... ; bge crN, default" (count max). Follows one register copy (mr / extsw / clrldi / clrlwi).
std::optional<u32> Walker::find_bound(u32 index_reg, u32 from_pc) const
{
    u32 regs[2] = {index_reg, 99};
    if (const auto d = find_gpr_def(index_reg, from_pc)) {
        const Inst& in = d->in;
        const bool copy = (in.op == Op::OR && in.rs() == in.rb()) || in.op == Op::EXTSW ||
            (in.op == Op::RLDICL && in.sh64() == 0) || (in.op == Op::RLWINM && in.sh32() == 0 && in.me32() == 31);
        if (copy) regs[1] = in.rs();
    }

    u32 pc = from_pc;
    std::optional<Inst> guard; // the conditional branch closest to the table code
    for (int i = 0; i < kSliceLimit; i++) {
        if (pc < 4) break;
        pc -= 4;
        if (!visited_->contains(pc)) break;
        const Inst in = insn(pc);
        if (is_terminator(in) || is_call(in)) break;
        if (in.op == Op::BC && !in.lk() && !guard) {
            guard = in;
            continue;
        }
        if (in.op == Op::CMPLI && (in.ra() == regs[0] || in.ra() == regs[1]) && guard) {
            const u32 field = in.crfd();
            const u32 bo = guard->bo() & 0x1C, bi = guard->bi();
            if (bi == field * 4 + 1 && bo == 0x0C) return in.uimm() + 1; // bgt: taken when index > max
            if (bi == field * 4 + 0 && bo == 0x04) return in.uimm();     // bge: taken when index >= max
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<JumpTable> Walker::match_jump_table(u32 bctr_pc, std::optional<u32> toc)
{
    // mtctr rX ; bctr
    const auto ctr_def = find_def(bctr_pc, 0, rt::SPR_CTR);
    if (!ctr_def || ctr_def->in.op != Op::MTSPR || ctr_def->in.spr() != rt::kSprCtr) return std::nullopt;
    const auto xdef = find_gpr_def(ctr_def->in.rs(), ctr_def->pc);
    if (!xdef) return std::nullopt;

    JumpTable jt;
    jt.bctr = bctr_pc;
    std::optional<Def> load;
    std::optional<u64> table;
    u32 index_reg = 0;

    if (xdef->in.op == Op::ADD) {
        // Relative entries: rX = entry + table. The table register must be the load's base.
        for (int k = 0; k < 2 && !load; k++) {
            const u32 entry_reg = k == 0 ? xdef->in.ra() : xdef->in.rb();
            const u32 base_reg = k == 0 ? xdef->in.rb() : xdef->in.ra();
            const auto ld = find_gpr_def(entry_reg, xdef->pc);
            if (!ld || (ld->in.op != Op::LWZX && ld->in.op != Op::LWAX)) continue;
            if (ld->in.ra() != base_reg && ld->in.rb() != base_reg) continue;
            // The table register must not change between the load and the add.
            const auto base_at_add = find_gpr_def(base_reg, xdef->pc);
            const auto base_at_load = find_gpr_def(base_reg, ld->pc);
            if (!base_at_add || !base_at_load || base_at_add->pc != base_at_load->pc) continue;
            const auto value = eval_const(base_reg, ld->pc, toc, 0);
            if (!value) continue;
            load = ld;
            table = value;
            index_reg = ld->in.ra() == base_reg ? ld->in.rb() : ld->in.ra();
            jt.relative = true;
        }
    } else if (xdef->in.op == Op::LWZX) {
        // Absolute entries: rX = table[index].
        load = xdef;
        if ((table = eval_const(xdef->in.ra(), xdef->pc, toc, 0))) {
            index_reg = xdef->in.rb();
        } else if ((table = eval_const(xdef->in.rb(), xdef->pc, toc, 0))) {
            index_reg = xdef->in.ra();
        }
    }
    if (!load || !table) return std::nullopt;
    jt.table = static_cast<u32>(*table);

    // Scaled index: slwi/sldi/clrlsldi/mulli by 4.
    std::optional<u32> count;
    if (const auto sdef = find_gpr_def(index_reg, load->pc)) {
        const Inst& in = sdef->in;
        std::optional<u32> unscaled;
        if (in.op == Op::RLWINM && in.sh32() == 2 && in.me32() == 29) unscaled = in.rs();
        if (in.op == Op::RLDICR && in.sh64() == 2 && in.mb64() == 61) unscaled = in.rs();
        if (in.op == Op::RLDIC && in.sh64() == 2) unscaled = in.rs();
        if (in.op == Op::MULLI && in.simm() == 4) unscaled = in.ra();
        if (unscaled) count = find_bound(*unscaled, sdef->pc);
    }

    const bool bounded = count.has_value() && *count > 0 && *count <= kMaxTableEntries;
    const u32 n = bounded ? *count : kMaxScannedEntries;
    for (u32 i = 0; i < n; i++) {
        const u32 slot = jt.table + 4 * i;
        if (!bounded && visited_->contains(slot)) break; // a scanned table cannot overlap decoded code
        const auto raw = image_.read32(slot);
        const u32 target = raw ? (jt.relative ? jt.table + *raw : *raw) : 0;
        if (!raw || !image_.is_code(target)) {
            if (bounded) {
                warn(bctr_pc, std::format("jump table at {:#010x}: entry {} ({:#010x}) is not code", jt.table, i, target));
                return std::nullopt;
            }
            break;
        }
        jt.targets.push_back(target);
    }
    if (jt.targets.empty()) return std::nullopt;
    jt.bounded = bounded;
    if (!bounded)
        warn(bctr_pc, std::format("jump table at {:#010x}: no bounds check found, scanned {} entries", jt.table,
                          jt.targets.size()));
    return jt;
}

void Walker::branch_to(Function& f, u32 target, u32 pc, std::vector<u32>& work)
{
    if (target == f.entry || (!entries_.contains(target) && image_.is_code(target))) {
        f.labels.insert(target);
        work.push_back(target);
    } else if (entries_.contains(target)) {
        f.tail_calls.insert(target);
    } else {
        f.needs_entry_state = true;
        warn(pc, std::format("branch to {:#010x}, outside the code image: dispatched at runtime", target));
    }
}

Function Walker::explore(u32 entry, std::optional<u32> toc)
{
    Function f;
    f.entry = entry;
    f.toc = toc;
    std::set<u32> visited;
    visited_ = &visited;
    std::vector<u32> work{entry};

    while (!work.empty()) {
        u32 pc = work.back();
        work.pop_back();
        bool fell_through = false;
        for (;;) {
            if (visited.contains(pc)) break;
            if (fell_through && entries_.contains(pc) && pc != entry) {
                f.fallthrough_calls.insert(pc); // runs into the next function: a tail call
                break;
            }
            if (!image_.is_code(pc)) {
                f.fallthrough_calls.insert(pc);
                f.needs_entry_state = true;
                warn(pc - 4, std::format("control flow leaves the code image at {:#010x}", pc));
                break;
            }
            if (visited.size() >= options_.max_function_insns) {
                warn(entry, "function too large, truncated");
                break;
            }
            visited.insert(pc);
            const u32 word = *image_.read32(pc);
            const Inst in = rt::decode(word);
            if (in.op == Op::UNKNOWN) {
                if (diag_) diag_->illegal.push_back(Diagnostic{pc, word, "illegal or unknown instruction"});
                break;
            }
            if (diag_ && !rt::is_supported(in))
                diag_->unsupported.push_back(Diagnostic{pc, word, std::string(rt::unsupported_reason(in))});

            bool falls = true;
            switch (in.op) {
            case Op::B:
            case Op::BC: {
                const u32 target = rt::branch_target(in, pc);
                if (in.lk()) {
                    if (target != pc + 4) f.calls.insert(target); // bcl 20,31,$+4 only reads the PC
                } else {
                    branch_to(f, target, pc, work);
                    falls = !always_taken(in);
                }
                break;
            }
            case Op::BCLR:
                if (!in.lk()) falls = !always_taken(in);
                break;
            case Op::BCCTR:
                if (!in.lk()) {
                    if (always_taken(in)) {
                        if (auto jt = match_jump_table(pc, toc)) {
                            for (u32 t : jt->targets) {
                                f.labels.insert(t);
                                work.push_back(t);
                            }
                            f.jump_tables.emplace(pc, std::move(*jt));
                        } else {
                            f.needs_entry_state = true;
                        }
                        falls = false;
                    } else {
                        f.needs_entry_state = true;
                    }
                }
                break;
            default:
                if (rt::is_unconditional_trap(in)) {
                    f.needs_entry_state = true;
                    falls = false;
                }
                break;
            }
            if (!falls) break;
            pc += 4;
            fell_through = true;
        }
    }

    f.insns.assign(visited.begin(), visited.end());
    f.end = f.insns.empty() ? entry : f.insns.back() + 4;
    visited_ = nullptr;
    return f;
}

// Basic blocks from the explored instructions: a block starts at the entry, at every local branch target and after
// every branch; calls (bl, bctrl, sc) do not end blocks.
void build_blocks(Function& f, const CodeImage& image, const std::set<u32>& entries)
{
    std::set<u32> starts{f.entry};
    starts.insert(f.labels.begin(), f.labels.end());
    for (u32 pc : f.insns) {
        const Inst in = rt::decode(image.read32(pc).value_or(0));
        const bool branch = (in.flags() & rt::OPF_BRANCH) && !in.lk();
        if (branch || is_terminator(in)) starts.insert(pc + 4);
    }
    const std::set<u32> body(f.insns.begin(), f.insns.end());
    for (usize i = 0; i < f.insns.size();) {
        Block b;
        b.start = f.insns[i];
        usize j = i;
        while (j + 1 < f.insns.size() && f.insns[j + 1] == f.insns[j] + 4 && !starts.contains(f.insns[j + 1])) j++;
        const u32 last = f.insns[j];
        b.end = last + 4;
        const Inst in = rt::decode(image.read32(last).value_or(0));
        const bool falls = body.contains(last + 4);
        auto local = [&](u32 t) { return f.labels.contains(t); };
        if (in.op == Op::UNKNOWN || rt::is_unconditional_trap(in)) {
            b.kind = BlockEnd::Stop;
        } else if ((in.op == Op::B || in.op == Op::BC) && !in.lk()) {
            const u32 t = rt::branch_target(in, last);
            const bool always = always_taken(in);
            if (local(t)) {
                b.kind = always ? BlockEnd::Jump : BlockEnd::Conditional;
                b.succs.push_back(t);
            } else {
                b.kind = always ? (entries.contains(t) ? BlockEnd::TailCall : BlockEnd::Indirect) : BlockEnd::CondExit;
            }
            if (!always && falls) b.succs.push_back(last + 4);
        } else if (in.op == Op::BCLR && !in.lk()) {
            b.kind = always_taken(in) ? BlockEnd::Return : BlockEnd::CondExit;
            if (!always_taken(in) && falls) b.succs.push_back(last + 4);
        } else if (in.op == Op::BCCTR && !in.lk()) {
            if (const auto jt = f.jump_tables.find(last); jt != f.jump_tables.end()) {
                b.kind = BlockEnd::Switch;
                std::set<u32> unique(jt->second.targets.begin(), jt->second.targets.end());
                b.succs.assign(unique.begin(), unique.end());
            } else {
                b.kind = always_taken(in) ? BlockEnd::Indirect : BlockEnd::CondExit;
                if (!always_taken(in) && falls) b.succs.push_back(last + 4);
            }
        } else if (falls) {
            b.kind = BlockEnd::FallThrough;
            b.succs.push_back(last + 4);
        } else {
            b.kind = f.fallthrough_calls.contains(last + 4) && entries.contains(last + 4) ? BlockEnd::TailCall : BlockEnd::Indirect;
        }
        f.blocks.push_back(std::move(b));
        i = j + 1;
    }
}

void dedupe(std::vector<Diagnostic>& v)
{
    std::sort(v.begin(), v.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.addr != b.addr ? a.addr < b.addr : a.message < b.message;
    });
    v.erase(std::unique(v.begin(), v.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.addr == b.addr && a.message == b.message;
    }), v.end());
}

} // namespace

const Function* Program::find(u32 entry) const
{
    const auto it = std::lower_bound(functions.begin(), functions.end(), entry,
        [](const Function& f, u32 e) { return f.entry < e; });
    return it != functions.end() && it->entry == entry ? &*it : nullptr;
}

Program analyze(const CodeImage& image, std::span<const Seed> seeds, const AnalyzerOptions& options)
{
    Program prog;
    std::map<u32, std::optional<u32>> toc_of;
    std::vector<u32> queue;
    for (const Seed& s : seeds) {
        if (!image.is_code(s.addr)) {
            prog.warnings.push_back(Diagnostic{s.addr, 0, std::format("seed {:#010x} is not in an executable segment", s.addr)});
            continue;
        }
        if (prog.entries.insert(s.addr).second) {
            toc_of[s.addr] = s.toc ? s.toc : options.toc;
            queue.push_back(s.addr);
        }
    }

    // Discovery: every bl target is a function; iterate until no new ones appear.
    {
        Walker walker(image, prog.entries, options, nullptr);
        while (!queue.empty()) {
            const u32 e = queue.back();
            queue.pop_back();
            const Function f = walker.explore(e, toc_of[e]);
            for (u32 c : f.calls) {
                if (image.is_code(c) && prog.entries.insert(c).second) {
                    toc_of[c] = toc_of[e];
                    queue.push_back(c);
                }
            }
        }
    }

    // Final pass with the complete entry set (tail calls and fall-through into other functions depend on it).
    Walker walker(image, prog.entries, options, &prog);
    for (u32 e : prog.entries) {
        prog.functions.push_back(walker.explore(e, toc_of[e]));
        build_blocks(prog.functions.back(), image, prog.entries);
        for (u32 c : prog.functions.back().calls)
            if (!image.is_code(c))
                prog.warnings.push_back(Diagnostic{e, 0, std::format("function {:#010x} calls {:#010x}, outside the code "
                                                                     "image: dispatched at runtime", e, c)});
    }
    dedupe(prog.unsupported);
    dedupe(prog.illegal);
    dedupe(prog.warnings);
    return prog;
}

} // namespace pspocket::recomp::ppu
