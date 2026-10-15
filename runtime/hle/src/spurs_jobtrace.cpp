// PSPOCKET_JOB_TRACE=HASH: what the jobs of one SPU program read and write (D-987). A diagnostic for finding out what a
// job does to the world outside its local store (GTA V's EDGE geometry job before its GPU path): every job whose binary
// has the program key HASH is counted (jobs, GETs, PUTs and atomics, their bytes) and every
// PSPOCKET_JOB_TRACE_EVERY-th (default 200) is written out whole: its descriptor and DMA list, then each MFC command it
// made (pc, command, LS and guest address, size) with the first bytes of what moved (up to 2 KB of a PUT of at most
// 4.5 KB: the command blocks GTA V's graphics jobs write into the RSX's FIFO). PSPOCKET_JOB_TRACE_LS=N also saves the
// whole local store at the entry of every N-th traced job (ls-<pid>-<seq>.bin: local store, then the 128 registers and the pc the job starts with). Into PSPOCKET_JOB_TRACE_DIR (default
// inventory/job-trace): game output, never committed (CLAUDE.md rule 1).

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <unistd.h>

#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/core/timebase.h"
#include "pspocket/spu/runtime.h"
#include "spurs_native.hpp"

namespace pspocket::hle::spurs {
namespace {

struct Config {
    u64 hash = 0;
    u32 every = 200;
    u32 ls_every = 0;
    std::string dir = "inventory/job-trace";
    Config()
    {
        if (const char* e = std::getenv("PSPOCKET_JOB_TRACE")) hash = std::strtoull(e, nullptr, 16);
        if (const char* e = std::getenv("PSPOCKET_JOB_TRACE_EVERY")) every = std::max(1u, static_cast<u32>(std::strtoul(e, nullptr, 0)));
        if (const char* e = std::getenv("PSPOCKET_JOB_TRACE_LS")) ls_every = static_cast<u32>(std::strtoul(e, nullptr, 0));
        if (const char* e = std::getenv("PSPOCKET_JOB_TRACE_DIR")) dir = e;
    }
};
const Config& config()
{
    static const Config c;
    return c;
}

std::mutex g_mutex;
FILE* g_file = nullptr;
std::atomic<u64> g_jobs{0}, g_gets{0}, g_get_bytes{0}, g_puts{0}, g_put_bytes{0}, g_atomics{0}, g_ns{0};

FILE* out_file()
{
    if (!g_file) {
        std::filesystem::create_directories(config().dir);
        const std::string path = std::format("{}/trace-{}.txt", config().dir, getpid());
        g_file = std::fopen(path.c_str(), "w");
        if (g_file) PS_LOG_INFO("spurs", "job trace of program {:016x} into {}", config().hash, path);
    }
    return g_file;
}

// One job's lines, written out at its end in one go.
struct Tracing {
    bool full = false;
    std::string text;
    u64 start = 0;
};
thread_local Tracing t_tracing;

std::string hex(const u8* ls, u32 lsa, u32 n)
{
    std::string s;
    s.reserve(n * 2 + n / 4);
    for (u32 i = 0; i < n; i++) {
        if (i && !(i & 3)) s += ' ';
        s += std::format("{:02x}", ls[(lsa + i) & PS_SPU_LS_MASK]);
    }
    return s;
}

bool is_get(u32 cmd) { return (cmd & 0xF0) == 0x40 || cmd == PS_MFC_GETLLAR; }

void observer(void*, const SPUContext* ctx, u32 pc, u32 cmd, u32 lsa, u32 ea, u32 size)
{
    Tracing& t = t_tracing;
    if (cmd == PS_MFC_GETLLAR || cmd == PS_MFC_PUTLLC || cmd == PS_MFC_PUTLLUC) {
        g_atomics.fetch_add(1, std::memory_order_relaxed);
    } else if (is_get(cmd)) {
        g_gets.fetch_add(1, std::memory_order_relaxed);
        g_get_bytes.fetch_add(size, std::memory_order_relaxed);
    } else {
        g_puts.fetch_add(1, std::memory_order_relaxed);
        g_put_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    if (!t.full) return;
    const u32 rel = ctx->reloc;
    u32 n = is_get(cmd) ? std::min<u32>(size, 256) : (size <= 0x1200 ? std::min<u32>(size, 2048) : 64);
    if (cmd == PS_MFC_GETLLAR || cmd == PS_MFC_PUTLLC || cmd == PS_MFC_PUTLLUC) n = 128;
    t.text += std::format("M {:05x} {:02x} {:05x} {:08x} {:x} {}\n", (pc - rel) & PS_SPU_LS_MASK, cmd, lsa, ea, size,
        hex(ctx->ls, lsa, n));
}

} // namespace

bool job_trace_wanted(u64 hash) { return config().hash && hash == config().hash; }

void job_trace_begin(const JobTraceInfo& j)
{
    const u64 seq = g_jobs.fetch_add(1, std::memory_order_relaxed);
    Tracing& t = t_tracing;
    t.full = seq % config().every == 0;
    t.text.clear();
    t.start = ps_host_now_ns();
    if (t.full) {
        t.text += std::format("J {} spu {} job {:x} bin {:x} at {:05x} in {:05x}+{:x} out {:05x}+{:x} t {}\n", seq, j.spu,
            j.job_ea, j.binary_ea, j.binary_ls, j.in_ls, j.size_in, j.out_ls, j.size_out, t.start);
        t.text += std::format("D {}\n", hex(j.ls, j.desc_ls, j.desc_size));
        for (u32 k = 0; k < j.dma_list_size / 8; k++) {
            t.text += std::format("L {} {:x} {:08x}\n", k, ps_ld32(j.ls + j.desc_ls + 0x30 + 8 * k),
                ps_ld32(j.ls + j.desc_ls + 0x34 + 8 * k));
        }
        if (config().ls_every && (seq / config().every) % config().ls_every == 0) {
            const std::string path = std::format("{}/ls-{}-{}.bin", config().dir, getpid(), seq);
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                // local store, then the registers as the job is entered with them (gpr, pc)
                std::fwrite(j.ls, 1, PS_SPU_LS_SIZE, f);
                std::fwrite(j.ctx->gpr, 1, sizeof j.ctx->gpr, f);
                std::fwrite(&j.ctx->pc, 1, 4, f);
                std::fclose(f);
            }
        }
    }
    ps_spu_set_mfc_observer(observer, nullptr);
}

void job_trace_end(const SPUContext&)
{
    ps_spu_set_mfc_observer(nullptr, nullptr);
    Tracing& t = t_tracing;
    const u64 ns = ps_host_now_ns() - t.start;
    g_ns.fetch_add(ns, std::memory_order_relaxed);
    const u64 jobs = g_jobs.load(std::memory_order_relaxed);
    const bool totals = jobs % 5000 == 0;
    if (!t.full && !totals) return;
    if (t.full) t.text += std::format("X {}\n", ns);
    if (totals) {
        t.text += std::format("T jobs {} gets {} {} puts {} {} atomics {} ns {} t {}\n", jobs, g_gets.load(), g_get_bytes.load(),
            g_puts.load(), g_put_bytes.load(), g_atomics.load(), g_ns.load(), ps_host_now_ns());
    }
    std::lock_guard lock(g_mutex);
    if (FILE* f = out_file()) {
        std::fwrite(t.text.data(), 1, t.text.size(), f);
        std::fflush(f);
    }
    t.full = false;
}

} // namespace pspocket::hle::spurs
