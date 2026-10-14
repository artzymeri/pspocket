// Interprocedural register liveness over a whole SPU program (pspocket/recomp/spu/liveness.hpp, D-965).

#include "pspocket/recomp/spu/liveness.hpp"

#include <algorithm>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

#include "pspocket/spu/context.h"

namespace pspocket::recomp::spu {

using ::pspocket::spu::Inst;
using ::pspocket::spu::Op;
using ::pspocket::spu::RegSet;

namespace {

constexpr u32 kOut = 0xFFFFFFFFu; // out of the program (a return from its entry: nothing live)

RegSet all_regs()
{
    RegSet r;
    r.bits = {~0ull, ~0ull};
    return r;
}
RegSet minus(RegSet a, const RegSet& b)
{
    a.bits[0] &= ~b.bits[0];
    a.bits[1] &= ~b.bits[1];
    return a;
}
RegSet both(RegSet a, const RegSet& b)
{
    a.bits[0] &= b.bits[0];
    a.bits[1] &= b.bits[1];
    return a;
}

enum class Kind {
    normal,   // falls through (or ends the code: unknown)
    jump,     // to targets, and falls through when conditional
    call,     // brsl $lr / bisl $lr: targets are frame entries, comes back at a + 4
    tail,     // a jump to another frame's entry: it returns where this frame returns
    ret,      // bi $lr (biz... $lr when conditional: falls through too)
    unknown,  // control goes where the analysis cannot follow
};

struct Node {
    u32 addr = 0;
    Kind kind = Kind::normal;
    bool falls = false; // also continues at a + 4
    std::vector<u32> targets;
    RegSet read, write;
    std::vector<u32> frames; // frame entries it belongs to
    std::string why;         // for unknown
    // An indirect call or tail call with no known target taken as a call of an ABI callee (LivenessOptions::
    // abi_unknown_calls): it reads `abi_use`, writes nothing it must.
    bool abi = false;
    RegSet abi_use;
    bool abi_only_use = false; // a direct call of an abi_callees target: abi_use instead of the callee's uses
};

} // namespace

RegSet Liveness::at(u32 addr) const
{
    const auto it = live_in.find(addr);
    return it == live_in.end() ? RegSet{} : it->second;
}

Liveness analyze_liveness(const LsImage& image, const Program& program, const LivenessOptions& options)
{
    Liveness out;
    std::unordered_map<u32, Node> nodes;
    std::map<u32, std::set<u32>> dynamic_of; // indirect-jump labels of the functions an instruction is in
    for (const Function& f : program.functions)
        for (u32 a : f.insns) {
            nodes[a].addr = a;
            dynamic_of[a].insert(f.dynamic.begin(), f.dynamic.end());
        }
    std::set<u32> function_entries;
    for (const Function& f : program.functions) function_entries.insert(f.entry);
    const auto has = [&](u32 a) { return nodes.contains(a); };
    // The value of $r before the instruction at `a` when the straight code before it makes it a constant from a link:
    // `brsl $r, next` (the get-pc idiom), then `ai`/`ori` on it (`brsl $2, next; ai $2, $2, 8; bid $2` continues after
    // the bid with interrupts off).
    const std::function<std::optional<u32>(u32, u32, int)> link_value = [&](u32 a, u32 r, int depth) -> std::optional<u32> {
        for (u32 b = a - 4; depth > 0 && has(b); b -= 4, depth--) {
            const Inst p = ::pspocket::spu::decode(image.word(b));
            const ::pspocket::spu::RegUse u = ::pspocket::spu::reg_use(p);
            if (!u.write.test(r)) continue;
            if (p.op == Op::BRSL && p.rt() == r) return (b + 4) & PS_SPU_PC_MASK;
            if (p.op == Op::AI && p.rt() == r)
                if (const auto v = link_value(b, p.ra(), depth - 1)) return (*v + static_cast<u32>(p.si10())) & PS_SPU_LS_MASK;
            if (p.op == Op::ORI && p.rt() == r && p.si10() == 0) return link_value(b, p.ra(), depth - 1);
            return std::nullopt;
        }
        return std::nullopt;
    };
    const auto link_constant = [&](u32 a, u32 r) -> std::optional<u32> {
        const auto v = link_value(a, r, 8);
        if (v && image.in_code(*v & PS_SPU_PC_MASK)) return *v & PS_SPU_PC_MASK;
        return std::nullopt;
    };

    // An ABI callee reached through a register: it may read the link and stack pointers, the callee-saved registers
    // (to save them) and the argument registers the caller wrote in the straight code before the call; it need not
    // write anything.
    const auto abi_callee = [&](Node& n) {
        n.abi = true;
        n.abi_use.set(0);
        n.abi_use.set(1);
        for (u32 r = 80; r < 128; r++) n.abi_use.set(r);
        for (u32 b = n.addr - 4, k = 0; k < 32 && image.in_code(b); b -= 4, k++) {
            const Inst p = ::pspocket::spu::decode(image.word(b));
            if (p.flags() & ::pspocket::spu::OPF_BRANCH) break;
            const ::pspocket::spu::RegUse u = ::pspocket::spu::reg_use(p);
            for (u32 r = 3; r < 75; r++)
                if (u.write.test(r)) n.abi_use.set(r);
        }
    };

    // ---- each instruction's kind, successors and registers ----
    std::set<u32> frame_entries(image.entries.begin(), image.entries.end());
    std::map<u32, std::set<u32>> return_sites; // frame entry -> return sites
    for (u32 e : image.entries) return_sites[e].insert(kOut);
    // Every instruction of the analyzer's functions, then whatever their control flow reaches in the code range that
    // the analyzer did not lift (code behind an indirect jump it could not follow).
    std::deque<u32> pending;
    for (const auto& [a, n] : nodes) pending.push_back(a);
    std::set<u32> classified;
    while (!pending.empty()) {
        const u32 a = pending.front();
        pending.pop_front();
        if (!classified.insert(a).second) continue;
        Node& n = nodes[a];
        n.addr = a;
        const Inst inst = ::pspocket::spu::decode(image.word(a));
        const ::pspocket::spu::RegUse use = ::pspocket::spu::reg_use(inst);
        n.read = use.read;
        n.write = use.write;
        const u32 next = (a + 4) & PS_SPU_PC_MASK;
        const auto unknown = [&](std::string why) {
            n.kind = Kind::unknown;
            n.why = std::format("{:05x}: {}", a, why);
        };
        switch (inst.op) {
        case Op::BR: n.kind = Kind::jump; n.targets = {::pspocket::spu::branch_target(inst, a)}; break;
        case Op::BRZ:
        case Op::BRNZ:
        case Op::BRHZ:
        case Op::BRHNZ:
            n.kind = Kind::jump;
            n.falls = true;
            n.targets = {::pspocket::spu::branch_target(inst, a)};
            break;
        case Op::BRA: unknown("absolute branch"); break;
        case Op::BRASL: unknown("absolute call"); break;
        case Op::BRSL: {
            const u32 t = ::pspocket::spu::branch_target(inst, a);
            if (t == next) {
                n.kind = Kind::normal; // the get-pc idiom: the link register is the data
                n.falls = true;
            } else if (inst.rt() == 0) {
                n.kind = Kind::call;
                n.targets = {t};
                frame_entries.insert(t);
                return_sites[t].insert(next);
                // A variadic callee (printf to the PPU through a mailbox) saves every argument register; only those
                // the caller wrote carry arguments.
                if (options.abi_callees.contains(t)) {
                    abi_callee(n);
                    n.abi = false; // its kills are its own
                    n.abi_only_use = true;
                }
            } else {
                unknown(std::format("call with link ${}", inst.rt()));
            }
            break;
        }
        case Op::BISL: {
            if (inst.rt() != 0) {
                unknown(std::format("indirect call with link ${}", inst.rt()));
                break;
            }
            n.kind = Kind::call;
            const auto it = options.indirect_calls.find(a);
            if (it != options.indirect_calls.end()) n.targets = it->second;
            else if (options.abi_unknown_calls) {
                abi_callee(n);
                out.unknowns.push_back(std::format("{:05x}: indirect call never seen: an ABI callee", a));
                break;
            } else n.targets.assign(function_entries.begin(), function_entries.end());
            for (u32 t : n.targets) {
                frame_entries.insert(t);
                return_sites[t].insert(next);
            }
            if (it == options.indirect_calls.end())
                out.unknowns.push_back(std::format("{:05x}: indirect call: any function", a));
            break;
        }
        case Op::BI:
        case Op::BIZ:
        case Op::BINZ:
        case Op::BIHZ:
        case Op::BIHNZ: {
            n.falls = inst.op != Op::BI;
            if (inst.ra() == 0) {
                n.kind = Kind::ret;
                break;
            }
            if (const auto it = options.tail_calls.find(a); it != options.tail_calls.end()) {
                // A jump to another function through a pointer: it returns to this frame's return sites.
                n.kind = Kind::tail;
                if (!it->second.empty()) n.targets = it->second;
                else if (options.abi_unknown_calls) {
                    abi_callee(n);
                    out.unknowns.push_back(std::format("{:05x}: indirect tail call never seen: an ABI callee", a));
                } else n.targets.assign(function_entries.begin(), function_entries.end());
                break;
            }
            n.kind = Kind::jump;
            std::set<u32> t = dynamic_of[a];
            if (const auto it = options.indirect_jumps.find(a); it != options.indirect_jumps.end())
                t.insert(it->second.begin(), it->second.end());
            if (const auto c = link_constant(a, inst.ra())) t.insert(*c);
            if (t.empty()) unknown(std::format("indirect jump through ${}", inst.ra()));
            else n.targets.assign(t.begin(), t.end());
            break;
        }
        case Op::IRET:
        case Op::BISLED: unknown("interrupt return / bisled"); break;
        case Op::STOP:
        case Op::STOPD: unknown("stop (the host may read registers)"); break;
        default:
            n.kind = Kind::normal;
            n.falls = true;
            break;
        }
        if (n.kind == Kind::unknown) out.unknowns.push_back(n.why);
        std::vector<u32> next_addrs = n.targets;
        if (n.falls || n.kind == Kind::call) next_addrs.push_back((a + 4) & PS_SPU_PC_MASK);
        for (u32 t : next_addrs)
            if (!classified.contains(t) && image.in_code(t)) pending.push_back(t);
    }
    // Function entries that are only targets of indirect calls given above may not be nodes yet: they are now.
    const auto succ = [&](const Node& n) {
        std::vector<u32> s;
        if (n.kind == Kind::jump) s = n.targets;
        if (n.falls || n.kind == Kind::call) s.push_back((n.addr + 4) & PS_SPU_PC_MASK);
        return s;
    };

    // ---- frames: what each entry reaches without a call ----
    for (u32 e : frame_entries) {
        if (!has(e)) continue;
        std::deque<u32> work{e};
        std::set<u32> seen{e};
        while (!work.empty()) {
            const u32 a = work.front();
            work.pop_front();
            Node& n = nodes[a];
            n.frames.push_back(e);
            if (n.kind == Kind::unknown || n.kind == Kind::ret) {
                if (n.kind == Kind::ret && !n.falls) continue;
                if (n.kind == Kind::unknown) continue;
            }
            for (u32 s : succ(n))
                if (has(s) && seen.insert(s).second) work.push_back(s);
        }
    }
    out.returns = return_sites;

    // ---- summaries (uses, kills) and the context liveness, to a fixpoint ----
    std::map<u32, RegSet> uses, kills;
    for (u32 e : frame_entries) {
        uses[e] = RegSet{};
        kills[e] = all_regs();
    }
    std::vector<u32> order;
    for (const auto& [a, n] : nodes) order.push_back(a);
    std::sort(order.begin(), order.end());

    // Backward liveness; `context`: returns continue at their frames' return sites, else nothing is live after them.
    const auto solve = [&](bool context, std::unordered_map<u32, RegSet>& live) {
        for (bool changed = true; changed;) {
            changed = false;
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                const Node& n = nodes[*it];
                RegSet in;
                const auto live_at = [&](u32 a) { return has(a) ? live[a] : all_regs(); };
                const u32 next = (n.addr + 4) & PS_SPU_PC_MASK;
                switch (n.kind) {
                case Kind::unknown: in = all_regs(); break;
                case Kind::normal: in = n.falls ? live_at(next) : all_regs(); break;
                case Kind::jump:
                    for (u32 t : n.targets) in |= live_at(t);
                    if (n.falls) in |= live_at(next);
                    break;
                case Kind::call: {
                    RegSet use = n.abi_use, keep = n.abi ? RegSet{} : all_regs();
                    for (u32 t : n.targets) {
                        if (!n.abi_only_use) use |= uses[t];
                        keep = both(keep, kills[t]);
                    }
                    in = use;
                    in |= minus(live_at(next), keep);
                    break;
                }
                case Kind::tail: {
                    RegSet use = n.abi_use, keep = n.abi ? RegSet{} : all_regs(), after;
                    for (u32 t : n.targets) {
                        use |= uses[t];
                        keep = both(keep, kills[t]);
                    }
                    if (context)
                        for (u32 e : n.frames)
                            for (u32 r : return_sites[e])
                                if (r != kOut) after |= live_at(r);
                    in = use;
                    in |= minus(after, keep);
                    break;
                }
                case Kind::ret:
                    if (context)
                        for (u32 e : n.frames)
                            for (u32 r : return_sites[e])
                                if (r != kOut) in |= live_at(r);
                    if (n.falls) in |= live_at(next);
                    break;
                }
                in = minus(in, n.write);
                in |= n.read;
                if (!(live[n.addr] == in)) {
                    live[n.addr] = in;
                    changed = true;
                }
            }
        }
    };

    std::unordered_map<u32, RegSet> summary;
    for (bool changed = true; changed;) {
        changed = false;
        solve(false, summary);
        for (u32 e : frame_entries) {
            if (!has(e)) continue;
            const RegSet u = summary[e];
            if (!(u == uses[e])) {
                uses[e] = u;
                changed = true;
            }
        }
        // Kills: the registers written on every path from the entry to a return (must-def, forward).
        for (u32 e : frame_entries) {
            if (!has(e)) continue;
            std::unordered_map<u32, RegSet> md_in;
            std::unordered_map<u32, bool> reached;
            std::deque<u32> work{e};
            md_in[e] = RegSet{};
            reached[e] = true;
            RegSet at_returns = all_regs();
            bool returns = false;
            while (!work.empty()) {
                const u32 a = work.front();
                work.pop_front();
                const Node& n = nodes[a];
                RegSet o = md_in[a];
                o |= n.write;
                if (n.kind == Kind::call) {
                    RegSet k = n.abi ? RegSet{} : all_regs();
                    for (u32 t : n.targets) k = both(k, kills[t]);
                    o |= k;
                }
                if (n.kind == Kind::tail) {
                    RegSet k = n.abi ? RegSet{} : all_regs();
                    for (u32 t : n.targets) k = both(k, kills[t]);
                    o |= k;
                    at_returns = both(at_returns, o);
                    returns = true;
                    continue;
                }
                if (n.kind == Kind::ret) {
                    at_returns = both(at_returns, md_in[a]);
                    returns = true;
                    if (!n.falls) continue;
                }
                if (n.kind == Kind::unknown) continue;
                for (u32 s : succ(n)) {
                    if (!has(s)) continue;
                    if (!reached[s]) {
                        reached[s] = true;
                        md_in[s] = o;
                        work.push_back(s);
                    } else {
                        const RegSet m = both(md_in[s], o);
                        if (!(m == md_in[s])) {
                            md_in[s] = m;
                            work.push_back(s);
                        }
                    }
                }
            }
            const RegSet k = returns ? at_returns : all_regs();
            if (!(k == kills[e])) {
                kills[e] = k;
                changed = true;
            }
        }
    }
    std::unordered_map<u32, RegSet> context;
    solve(true, context);
    for (const auto& [a, r] : context) out.live_in[a] = r;
    out.uses = uses;
    out.kills = kills;

    // A path from `from` to a read of `reg` before any write of it (for the report: why it is live there).
    auto shared = std::make_shared<std::unordered_map<u32, Node>>(std::move(nodes));
    auto shared_returns = std::make_shared<std::map<u32, std::set<u32>>>(return_sites);
    out.why = [shared, shared_returns, uses, kills](u32 from, u32 reg) -> std::string {
        const auto& ns = *shared;
        std::map<u32, u32> parent;
        std::deque<u32> work{from};
        parent[from] = from;
        const auto path = [&](u32 a, const std::string& last) {
            std::vector<u32> p;
            for (u32 x = a; x != from; x = parent[x]) p.push_back(x);
            std::string o = std::format("{:05x}", from);
            for (auto it = p.rbegin(); it != p.rend(); ++it) o += std::format(" {:05x}", *it);
            return o + ": " + last;
        };
        while (!work.empty()) {
            const u32 a = work.front();
            work.pop_front();
            const auto it = ns.find(a);
            if (it == ns.end()) return path(a, "leaves the analysed code");
            const Node& n = it->second;
            if (n.read.test(reg)) return path(a, "reads it");
            std::vector<u32> next;
            if (n.kind == Kind::unknown) return path(a, "unknown: " + n.why);
            if ((n.kind == Kind::call || n.kind == Kind::tail) && n.abi && n.abi_use.test(reg))
                return path(a, "an ABI callee reached through a register may read it");
            if (n.abi_only_use && n.abi_use.test(reg)) return path(a, "passes it to a variadic callee");
            if (n.kind == Kind::call || n.kind == Kind::tail) {
                bool all_kill = !n.abi;
                for (u32 t : n.targets) {
                    if (!n.abi_only_use && uses.count(t) && uses.at(t).test(reg))
                        return path(a, std::format("calls {:05x}, which reads it", t));
                    if (!kills.count(t) || !kills.at(t).test(reg)) all_kill = false;
                }
                if (all_kill) continue;
            }
            if (n.write.test(reg)) continue;
            if (n.kind == Kind::jump) next = n.targets;
            if (n.kind == Kind::ret || n.kind == Kind::tail)
                for (u32 e : n.frames)
                    for (u32 r : (*shared_returns)[e])
                        if (r != kOut) next.push_back(r);
            if (n.falls || n.kind == Kind::call) next.push_back((a + 4) & PS_SPU_PC_MASK);
            for (u32 x : next)
                if (!parent.count(x)) {
                    parent[x] = a;
                    work.push_back(x);
                }
        }
        return "not live";
    };
    return out;
}

} // namespace pspocket::recomp::spu
