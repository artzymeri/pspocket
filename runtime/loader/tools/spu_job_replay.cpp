// spu-job-replay: SPURS jobs replayed from a job trace (PSPOCKET_JOB_TRACE with PSPOCKET_JOB_TRACE_LS, D-987).
//
//   spu-job-replay TRACE_DIR --key HASH --size BYTES [--coverage FILE] [--probe ADDR --probe-dir DIR] [--max N] [--show] [--library FILE]
//
// Each ls-<pid>-<seq>.bin of the directory holds a job's local store and registers as it was entered; trace-<pid>.txt
// holds what the job read from and wrote to guest memory then (its "M" lines). The job runs again from that state (its
// lifted program, as in the game: native replacements included) with the guest memory it read filled in (what its
// GETs and GETLLARs found), and what it writes is compared with what it wrote in the game: a faithful replay is the
// oracle a reimplementation of the job (EDGE on the GPU) is checked against. HASH and BYTES: the job's program key
// and code size, its binary loaded at a job page from LS 0x4C00 (the job chain's). --coverage FILE interprets the jobs
// instead and writes how many times each instruction ran ("address count" lines, the program's own addresses): the
// paths the game's jobs take. --probe ADDR (repeatable) with --probe-dir DIR interprets the jobs too and saves the
// state at each entry of the function at ADDR and at its return (DIR/<seq>-<addr>-<n>-in.bin and -out.bin: local
// store, the 128 registers, the pc): the states a reimplementation of one stage is checked on. --show prints each
// replayed write.
//
// The trace and the dumps hold the game's data: they stay in the gitignored inventory/ (CLAUDE.md rule 1).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/game/host.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/fp.h"
#include "pspocket/spu/replace.hpp"
#include "pspocket/spu/runtime.h"

#if PSPOCKET_HAVE_GAME_SPU
extern "C" void pspocket_game_spu_register(void);
#endif

using namespace pspocket;

namespace {

struct Mfc {
    u32 pc = 0, cmd = 0, lsa = 0, ea = 0, size = 0;
    std::vector<u8> data;
};

struct TracedJob {
    u64 seq = 0;
    u32 binary_ls = 0;
    std::vector<Mfc> mfc;
};

std::vector<u8> unhex(const std::vector<std::string>& words)
{
    std::vector<u8> out;
    for (const std::string& w : words)
        for (size_t i = 0; i + 1 < w.size(); i += 2) out.push_back(static_cast<u8>(std::stoul(w.substr(i, 2), nullptr, 16)));
    return out;
}

// The jobs of a trace file written out whole, by sequence number.
std::map<u64, TracedJob> read_trace(const std::filesystem::path& path)
{
    std::map<u64, TracedJob> jobs;
    std::ifstream f(path);
    std::string line;
    TracedJob* cur = nullptr;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string kind;
        in >> kind;
        if (kind == "J") {
            TracedJob j;
            std::string word;
            in >> j.seq;
            for (int k = 0; k < 7 && in >> word; k++) {
                if (word == "at") in >> std::hex >> j.binary_ls >> std::dec;
            }
            cur = &(jobs[j.seq] = j);
        } else if (kind == "M" && cur) {
            Mfc m;
            in >> std::hex >> m.pc >> m.cmd >> m.lsa >> m.ea >> m.size >> std::dec;
            std::vector<std::string> words;
            std::string w;
            while (in >> w) words.push_back(w);
            m.data = unhex(words);
            cur->mfc.push_back(std::move(m));
        } else if (kind == "X") {
            cur = nullptr;
        }
    }
    return jobs;
}

bool is_get(u32 cmd) { return (cmd & 0xF0) == 0x40 || cmd == PS_MFC_GETLLAR; }

std::vector<Mfc>* g_writes = nullptr;
void observe(void*, const SPUContext* ctx, u32 pc, u32 cmd, u32 lsa, u32 ea, u32 size)
{
    Mfc m{(pc - ctx->reloc) & PS_SPU_LS_MASK, cmd, lsa, ea, size, {}};
    const u32 n = is_get(cmd) ? std::min<u32>(size, 256) : (size <= 0x1200 ? std::min<u32>(size, 2048) : 64);
    for (u32 i = 0; i < ((cmd == PS_MFC_PUTLLC || cmd == PS_MFC_GETLLAR) ? 128u : n); i++)
        m.data.push_back(ctx->ls[(lsa + i) & PS_SPU_LS_MASK]);
    g_writes->push_back(std::move(m));
}

} // namespace

int main(int argc, char** argv)
{
    std::string dir, coverage_path, library, probe_dir;
    std::vector<u32> probes;
    u64 max = 0, key = 0;
    u32 code_size = 0;
    bool show = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--coverage") coverage_path = value();
        else if (a == "--max") max = std::strtoull(value().c_str(), nullptr, 0);
        else if (a == "--show") show = true;
        else if (a == "--probe") probes.push_back(static_cast<u32>(std::strtoul(value().c_str(), nullptr, 16)));
        else if (a == "--probe-dir") probe_dir = value();
        else if (a == "--key") key = std::strtoull(value().c_str(), nullptr, 16);
        else if (a == "--size") code_size = static_cast<u32>(std::strtoul(value().c_str(), nullptr, 0));
        else if (a == "--library") library = value();
        else if (dir.empty()) dir = a;
        else {
            std::fprintf(stderr, "usage: spu-job-replay TRACE_DIR --key HASH --size BYTES [--coverage FILE] [--probe ADDR --probe-dir DIR] [--max N] [--show] [--library FILE]\n");
            return 64;
        }
    }
    if (dir.empty()) {
        std::fprintf(stderr, "usage: spu-job-replay TRACE_DIR --key HASH --size BYTES [--coverage FILE] [--probe ADDR --probe-dir DIR] [--max N] [--show] [--library FILE]\n");
        return 64;
    }
    static GuestMemory memory;
    if (!memory.reserve()) {
        std::fprintf(stderr, "spu-job-replay: cannot reserve guest memory\n");
        return 1;
    }
    if (!library.empty()) {
        game::LoadOptions options;
        options.check_image_words = false;
        std::string error;
        if (!game::load_library(library, memory, nullptr, &error, options)) {
            std::fprintf(stderr, "spu-job-replay: %s\n", error.c_str());
            return 1;
        }
    } else {
#if PSPOCKET_HAVE_GAME_SPU
        pspocket_game_spu_register();
#else
        std::fprintf(stderr, "spu-job-replay: this build has no game's SPU programs built in: give --library\n");
        return 64;
#endif
    }
    log::set_min_level(log::Level::Warn);

    std::vector<std::filesystem::path> dumps;
    std::map<std::string, std::map<u64, TracedJob>> traces; // by pid
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (name.starts_with("ls-") && name.ends_with(".bin")) dumps.push_back(e.path());
    }
    std::sort(dumps.begin(), dumps.end());
    if (max && dumps.size() > max) dumps.resize(max);

    u8* ls = static_cast<u8*>(std::aligned_alloc(128, PS_SPU_LS_SIZE));
    std::map<u32, u64> coverage;
    const u64 fp = ps_spu_fpenv_enter();
    (void)fp;
    u64 replayed = 0, same = 0, differ = 0, skipped = 0;
    for (const auto& path : dumps) {
        const std::string name = path.filename().string(); // ls-<pid>-<seq>.bin
        const size_t d1 = name.find('-', 3);
        const std::string pid = name.substr(3, d1 - 3);
        const u64 seq = std::strtoull(name.substr(d1 + 1).c_str(), nullptr, 10);
        if (!traces.count(pid)) traces[pid] = read_trace(std::filesystem::path(dir) / std::format("trace-{}.txt", pid));
        const auto it = traces[pid].find(seq);
        std::ifstream f(path, std::ios::binary);
        SPUContext ctx{};
        std::vector<u8> image(PS_SPU_LS_SIZE);
        f.read(reinterpret_cast<char*>(image.data()), PS_SPU_LS_SIZE);
        ps_spu_v128 gpr[128];
        u32 pc = 0;
        f.read(reinterpret_cast<char*>(gpr), sizeof gpr);
        f.read(reinterpret_cast<char*>(&pc), 4);
        if (!f || it == traces[pid].end()) {
            skipped++;
            continue;
        }
        const TracedJob& job = it->second;
        // What it read from guest memory, where it read it.
        for (const Mfc& m : job.mfc) {
            const u32 lo = m.ea & ~0xFFFu, hi = (m.ea + std::max<u32>(m.size, 128) + 0xFFFu) & ~0xFFFu;
            for (u32 p = lo; p < hi; p += 0x1000)
                if (!memory.is_committed(p)) memory.commit(p, 0x1000);
            if (is_get(m.cmd)) std::memcpy(memory.host(m.ea), m.data.data(), m.data.size());
        }
        std::memcpy(ls, image.data(), PS_SPU_LS_SIZE);
        ps_spu_context_init(&ctx, ls, memory.base());
        std::memcpy(ctx.gpr, gpr, sizeof gpr);
        ctx.pc = pc;
        const u32 tag = ps_ld32(ls + 0x4940 + 0x24); // the job's context (CellSpursJobContext2): its tag
        ps_spu_wrch(&ctx, 0x2570, PS_MFC_WrTagMask, 1u << tag);
        // Its program, found in local store as the job chain finds it.
        std::vector<Mfc> writes;
        g_writes = &writes;
        ps_spu_set_mfc_observer(observe, nullptr);
        if (!coverage_path.empty() || !probes.empty()) {
            // Probes: a call's return is where its $0 pointed with the stack pointer back where it was.
            struct Open {
                u32 addr, ret, sp, n;
            };
            std::vector<Open> open;
            std::map<u32, u32> calls;
            const auto save = [&](u32 addr, u32 n, const char* what) {
                const std::string out = std::format("{}/{}-{:05x}-{}-{}.bin", probe_dir, seq, addr, n, what);
                if (FILE* pf = std::fopen(out.c_str(), "wb")) {
                    std::fwrite(ls, 1, PS_SPU_LS_SIZE, pf);
                    std::fwrite(ctx.gpr, 1, sizeof ctx.gpr, pf);
                    std::fwrite(&ctx.pc, 1, 4, pf);
                    std::fclose(pf);
                }
            };
            for (u64 steps = 0; steps < 200'000'000; steps++) {
                const u32 at = (ctx.pc - job.binary_ls + 0x4C00) & PS_SPU_LS_MASK;
                coverage[at]++;
                for (size_t k = open.size(); k-- > 0;) {
                    if (ctx.pc == open[k].ret && ctx.gpr[1].u32[3] == open[k].sp) {
                        save(open[k].addr, open[k].n, "out");
                        open.erase(open.begin() + static_cast<std::ptrdiff_t>(k));
                    }
                }
                if (std::find(probes.begin(), probes.end(), at) != probes.end()) {
                    const u32 n = calls[at]++;
                    save(at, n, "in");
                    open.push_back({at, ctx.gpr[0].u32[3], ctx.gpr[1].u32[3], n});
                }
                if (ps_spu_interpret(&ctx, 1) != PS_SPU_STOP_LIMIT) break;
            }
        } else {
            const ps_spu_program* p = ps_spu_find_program(key, 0x4C00, code_size);
            if (!p) {
                std::fprintf(stderr, "spu-job-replay: program %016llx is not built in\n", static_cast<unsigned long long>(key));
                return 1;
            }
            ps_spu_attach_program_at(&ctx, p, job.binary_ls);
            ps_spu_run(&ctx);
        }
        ps_spu_set_mfc_observer(nullptr, nullptr);
        replayed++;
        const bool ended = ctx.stop_reason == PS_SPU_STOP_SIGNAL && ctx.stop_code == 0x2570;
        // Compare the writes with the game's.
        std::vector<const Mfc*> want, got;
        for (const Mfc& m : job.mfc)
            if (!is_get(m.cmd)) want.push_back(&m);
        for (const Mfc& m : writes)
            if (!is_get(m.cmd)) got.push_back(&m);
        bool ok = ended && want.size() == got.size();
        for (size_t k = 0; ok && k < want.size(); k++) {
            ok = want[k]->ea == got[k]->ea && want[k]->size == got[k]->size && want[k]->cmd == got[k]->cmd &&
                 (want[k]->cmd == PS_MFC_PUTLLC || want[k]->data == got[k]->data);
        }
        (ok ? same : differ)++;
        if (!ok || show) {
            std::printf("%s %s: stop %u code %#x pc %05x, writes %zu (game %zu)\n", ok ? "SAME" : "DIFFER", name.c_str(),
                ctx.stop_reason, ctx.stop_code, ctx.pc, got.size(), want.size());
            if (show || !ok)
                for (const Mfc* m : got)
                    std::printf("   %05x %02x %08x %x\n", m->pc, m->cmd, m->ea, m->size);
        }
    }
    std::printf("%llu jobs replayed: %llu wrote what the game's wrote, %llu differ; %llu skipped\n",
        static_cast<unsigned long long>(replayed), static_cast<unsigned long long>(same),
        static_cast<unsigned long long>(differ), static_cast<unsigned long long>(skipped));
    if (!coverage_path.empty()) {
        std::ofstream out(coverage_path);
        for (const auto& [at, n] : coverage) out << std::format("{:05x} {}\n", at, n);
    }
    return differ ? 1 : 0;
}
