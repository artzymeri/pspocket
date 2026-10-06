// SPU analyzer: functions, block starts, call targets and jump tables (pspocket/recomp/spu/analyzer.hpp).

#include "pspocket/recomp/spu/analyzer.hpp"

#include <algorithm>
#include <deque>
#include <format>

#include "pspocket/spu/disasm.hpp"

namespace pspocket::recomp::spu {

using namespace ::pspocket::spu;

namespace {

class Analyzer {
public:
    Analyzer(const LsImage& image, const AnalyzerOptions& options, Program& program)
        : image_(image), options_(options), program_(program)
    {
    }

    void run()
    {
        std::deque<u32> pending(image_.entries.begin(), image_.entries.end());
        std::map<u32, Function> functions;
        const auto trace_pending = [&] {
            while (!pending.empty()) {
                const u32 entry = pending.front();
                pending.pop_front();
                if (functions.contains(entry) || !image_.in_code(entry)) continue;
                Function f = function(entry, pending);
                functions.emplace(entry, std::move(f));
            }
        };
        // Code addresses formed by ila/il wait until the entries and the calls are traced, then go in address order: one
        // inside a function's code is a label of it, entered through its dispatch switch; only the others start
        // functions. A constant is often a number that happens to point into code: started as functions of their own,
        // the firmware's SPU decoders' constants copied the code each reached, a function every 16 bytes into one
        // long body (one 57 KB program lifted to 288 MB of C, D-190).
        const auto drain = [&] {
            trace_pending();
            while (!constants_.empty()) {
                const std::set<u32> batch = std::move(constants_);
                constants_.clear();
                for (const u32 c : batch) {
                    if (functions.contains(c) || !image_.in_code(c)) continue;
                    if (Function* holder = holding(functions, c)) {
                        holder->labels.insert(c);
                        continue;
                    }
                    pending.push_back(c);
                    trace_pending();
                }
            }
        };
        drain();
        // Seeds: a seed inside a function's code is entered through its dispatch switch, as a label. Started as a
        // function of its own it would copy everything it reaches (one job of GTA V with seeds from its runs: 66 times
        // its size). Only seeds no function holds yet start functions, in address order.
        std::vector<u32> seeds = image_.seeds;
        std::sort(seeds.begin(), seeds.end());
        for (u32 seed : seeds) {
            if (!image_.in_code(seed) || functions.contains(seed)) continue;
            if (Function* holder = holding(functions, seed)) {
                holder->labels.insert(seed);
                continue;
            }
            pending.push_back(seed);
            drain();
        }
        if (options_.discover) discover(functions, pending, drain);
        for (auto& [entry, f] : functions) {
            program_.entries.insert(entry);
            program_.functions.push_back(std::move(f));
        }
        std::sort(program_.unsupported.begin(), program_.unsupported.end(),
            [](const Diagnostic& a, const Diagnostic& b) { return a.addr < b.addr; });
        program_.unsupported.erase(std::unique(program_.unsupported.begin(), program_.unsupported.end(),
                                       [](const Diagnostic& a, const Diagnostic& b) { return a.addr == b.addr; }),
            program_.unsupported.end());
    }

private:
    bool valid_code(u32 addr) const { return image_.in_code(addr) && is_supported(decode(image_.word(addr))); }

    // The function whose code holds addr, if any.
    static Function* holding(std::map<u32, Function>& functions, u32 addr)
    {
        for (auto& [entry, f] : functions)
            if (std::binary_search(f.insns.begin(), f.insns.end(), addr)) return &f;
        return nullptr;
    }

    // AnalyzerOptions::discover: rounds until nothing new is reached (code found leads to more such places).
    template <class Drain>
    void discover(std::map<u32, Function>& functions, std::deque<u32>& pending, const Drain& drain)
    {
        std::set<u32> before;
        for (const auto& [entry, f] : functions) before.insert(entry);
        discover_rounds(functions, pending, drain);
        for (auto& [entry, f] : functions) f.discovered = !before.contains(entry);
        // Where discovered code met code reached before: a label of a function holding it (the lifter leaves to the
        // run loop there, which enters that function at the label).
        for (const u32 b : boundaries_)
            for (auto& [entry, f] : functions)
                if (std::binary_search(f.insns.begin(), f.insns.end(), b)) {
                    f.labels.insert(b);
                    break;
                }
    }

    template <class Drain>
    void discover_rounds(std::map<u32, Function>& functions, std::deque<u32>& pending, const Drain& drain)
    {
        std::vector<bool> reached((image_.code_end + 3) / 4, false);
        std::set<u32> marked; // functions whose instructions are in `reached`
        const auto mark = [&] {
            for (const auto& [entry, f] : functions)
                if (marked.insert(entry).second)
                    for (const u32 a : f.insns) reached[a / 4] = true;
        };
        for (int round = 0; round < 16; round++) {
            mark();
            std::vector<u32> starts;
            for (u32 a = image_.code_start; a + 4 < image_.code_end; a += 4) {
                const u32 word = image_.word(a);
                const Inst inst = decode(word);
                const u32 fl = inst.flags();
                const bool ends = word != 0 && // a zero word is "stop 0": padding, not code
                    (((fl & OPF_BRANCH) && !(fl & OPF_COND) && !(fl & OPF_CALL)) || (fl & OPF_STOP));
                if (!ends) continue;
                u32 c = a + 4;
                while (c < image_.code_end) {
                    const Inst pad = decode(image_.word(c));
                    if (pad.op != Op::NOP && pad.op != Op::LNOP && !(pad.flags() & OPF_HINT)) break;
                    c += 4;
                }
                if (c < image_.code_end && image_.word(c) != 0 && valid_code(c) && !reached[c / 4]) starts.push_back(c);
            }
            if (starts.empty()) break;
            stop_ = &reached;
            for (const u32 c : starts) {
                mark(); // a start an earlier one of this round reached is code of that function, not another copy
                if (reached[c / 4] || functions.contains(c)) continue;
                pending.push_back(c);
                drain();
            }
            stop_ = nullptr;
        }
    }

    Function function(u32 entry, std::deque<u32>& pending)
    {
        Function f;
        f.entry = entry;
        f.labels.insert(entry);
        std::set<u32> visited;
        std::deque<u32> work{entry};
        std::vector<u32> constants;
        bool indirect = false;
        std::set<u32> tables_scanned;

        for (;;) {
            while (!work.empty()) {
                u32 a = work.front();
                work.pop_front();
                trace(f, a, visited, work, constants, indirect, pending);
            }
            // Jump tables: bi through a register with a table of code addresses whose base an ila formed.
            bool grew = false;
            if (indirect) {
                for (u32 base : constants) {
                    if (!tables_scanned.insert(base).second) continue;
                    for (usize k = 0; k < options_.max_jump_table; k++) {
                        const u32 at = base + static_cast<u32>(k) * 4;
                        if (at + 4 > image_.ls.size()) break;
                        const u32 target = image_.word(at);
                        if (!valid_code(target)) break;
                        f.labels.insert(target);
                        if (!visited.contains(target)) {
                            work.push_back(target);
                            grew = true;
                        }
                    }
                }
            }
            if (!grew) break;
        }

        if (options_.constant_entries)
            for (u32 c : constants)
                if (valid_code(c)) constants_.insert(c);

        f.insns.assign(visited.begin(), visited.end());
        // Labels must be lifted instructions.
        for (auto it = f.labels.begin(); it != f.labels.end();) it = visited.contains(*it) ? std::next(it) : f.labels.erase(it);
        return f;
    }

    void trace(Function& f, u32 a, std::set<u32>& visited, std::deque<u32>& work, std::vector<u32>& constants,
        bool& indirect, std::deque<u32>& pending)
    {
        for (;;) {
            if (!image_.in_code(a)) return; // leaves the code range: the lifter exits to the run loop
            // Code found by discovery stops where it meets code already reached: that code is entered there, as a
            // label of the function holding it, instead of being copied (discover()).
            if (stop_ && a != f.entry && (*stop_)[a / 4] && !visited.contains(a)) {
                boundaries_.push_back(a);
                return;
            }
            if (!visited.insert(a).second) return;
            const u32 word = image_.word(a);
            const Inst inst = decode(word);
            if (!is_supported(inst)) {
                visited.erase(a);
                program_.unsupported.push_back(Diagnostic{a, word,
                    std::format("{} ({})", disassemble(word, a), unsupported_reason(inst))});
                return;
            }
            const RegUse use = reg_use(inst);
            f.used |= use.read;
            f.used |= use.write;
            f.written |= use.write;
            const u32 fl = inst.flags();
            const u32 next = a + 4;
            // Instructions that may stop and be resumed (blocking channels, halts, exceptions) restart in place.
            if (use.runtime) f.labels.insert(a);

            switch (inst.op) {
            case Op::STOP:
            case Op::STOPD: f.labels.insert(next); break;
            case Op::ILA: constants.push_back(inst.i18()); break;
            case Op::IL:
                if (inst.si16() >= 0) constants.push_back(static_cast<u32>(inst.si16()));
                break;
            default: break;
            }

            if (fl & OPF_BRANCH) {
                if (inst.info().fmt == Fmt::RI16_BR && (fl & OPF_ABS) && options_.relocatable) {
                    if (fl & OPF_CALL) f.labels.insert(next); // brasl: returns here through the run loop
                    else return;                               // bra
                } else if (inst.info().fmt == Fmt::RI16_BR) {
                    const u32 target = branch_target(inst, a);
                    if (fl & OPF_CALL) {
                        f.calls.insert(target);
                        pending.push_back(target);
                        f.labels.insert(next);
                    } else {
                        f.labels.insert(target);
                        work.push_back(target);
                        if (!(fl & OPF_COND)) return; // br / bra
                    }
                } else {
                    // indirect: bi, bisl, biz, binz, bihz, bihnz, iret, bisled
                    if (inst.op == Op::BI || inst.op == Op::IRET) {
                        if (inst.op == Op::BI && inst.ra() != 0) indirect = true;
                        return;
                    }
                    indirect = indirect || inst.op != Op::BISL;
                    f.labels.insert(next);
                }
            }
            a = next;
        }
    }

    const LsImage& image_;
    const AnalyzerOptions& options_;
    Program& program_;
    std::set<u32> constants_;                 // code addresses formed by ila/il, not traced yet (run())
    const std::vector<bool>* stop_ = nullptr; // discover(): code reached before it
    std::vector<u32> boundaries_;             // where discovered code met it
};

} // namespace

const Function* Program::find(u32 entry) const
{
    const auto it = std::lower_bound(functions.begin(), functions.end(), entry,
        [](const Function& f, u32 e) { return f.entry < e; });
    return it != functions.end() && it->entry == entry ? &*it : nullptr;
}

Program analyze(const LsImage& image, const AnalyzerOptions& options)
{
    Program program;
    Analyzer(image, options, program).run();
    return program;
}

} // namespace pspocket::recomp::spu
