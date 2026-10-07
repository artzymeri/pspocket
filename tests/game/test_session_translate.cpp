// SPU programs translated while the game runs (D-236): images saved as a session saves them are lifted, compiled,
// signed and loaded by the session's translator on its own thread; the program it registers runs as the interpreter
// does. Copies that differ only in data become one program; an image whose program is registered already is left.
// Synthetic images only (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/codegen/toolchain.hpp"
#include "pspocket/core/guest_memory.hpp"
#include "pspocket/game/host.hpp"
#include "pspocket/prepare/session.hpp"
#include "pspocket/recomp/spu/asm.hpp"
#include "pspocket/spu/context.h"
#include "pspocket/spu/registry.hpp"
#include "pspocket/spu/runtime.h"

using namespace pspocket;
namespace as = pspocket::recomp::spu::as;
namespace fs = std::filesystem;

namespace {

constexpr u32 kLs = 0x4C00; // where SPURS puts a job's binary

// A job: r4 = 2 * imm + its data word, then `stop 0x42` (and back to its caller); its data follows.
std::vector<u8> job(u32 data, s32 imm = 0x123)
{
    const std::vector<u32> words = {as::il(3, imm), as::a(4, 3, 3), as::lqr(5, 0x18), as::a(4, 4, 5), as::stop(0x42),
        as::bi(0), as::lnop(), as::lnop(), data, data, data, data};
    std::vector<u8> bytes(words.size() * 4);
    for (size_t i = 0; i < words.size(); i++) ps_st32(bytes.data() + 4 * i, words[i]);
    return bytes;
}

// Saved as hle's dump saves an image met with no program (spu_<hash>_<ls>.bin and .txt).
std::string save(const fs::path& dir, const std::vector<u8>& bytes)
{
    const std::string name = std::format("spu_{:016x}_{:05x}", ps_spu_hash64(bytes.data(), bytes.size()), kLs);
    std::ofstream(dir / (name + ".bin"), std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::ofstream(dir / (name + ".txt")) << std::format(
        "kind job\nguest 0x10000\nbase {:#x}\nsize {:#x}\nentry {:#x}\n", kLs, bytes.size(), kLs);
    return name;
}

prepare::SessionOptions options(const fs::path& dir, GuestMemory& memory)
{
    prepare::SessionOptions o;
    o.images_dir = (dir / "images").string();
    o.work_dir = (dir / "work").string();
    o.settle_seconds = 0;
    o.poll_seconds = 0.05;
    prepare::Toolchain& t = o.library.toolchain;
    t.resource_dir = PSPOCKET_CODEGEN_RESOURCE_DIR;
    t.sysroot = PSPOCKET_CODEGEN_SYSROOT;
    t.include_dirs = {PSPOCKET_SOURCE_DIR "/runtime/ppu/include", PSPOCKET_SOURCE_DIR "/runtime/spu/include",
        PSPOCKET_SOURCE_DIR "/runtime/core/include", PSPOCKET_SOURCE_DIR "/runtime/game/include"};
    t.entry_source = PSPOCKET_GAME_LIBRARY_ENTRY_SOURCE;
    t.target = "arm64-apple-macos13.0";
    t.cpu = "native";
    t.platform = "macos";
    t.min_os = "13.0";
    o.library.title_id = "TEST00000";
    o.load = [&memory](const std::string& path, std::string* error) {
        const spu::RegistryBatch batch;
        game::LoadedLibrary out;
        return game::load_library(path, memory, &out, error);
    };
    return o;
}

bool wait_until(const std::function<bool()>& done, double seconds)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

// Runs the job at kLs, lifted (with `program`) or interpreted (null); returns r4.
u32 run_job(const std::vector<u8>& bytes, const ps_spu_program* program, GuestMemory& memory)
{
    std::vector<u8> ls(PS_SPU_LS_SIZE + 128);
    u8* aligned = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(ls.data()) + 127) & ~uintptr_t{127});
    std::memcpy(aligned + kLs, bytes.data(), bytes.size());
    SPUContext ctx{};
    ps_spu_context_init(&ctx, aligned, memory.base());
    ctx.pc = kLs;
    ps_spu_attach_program_at(&ctx, program, kLs);
    const u32 reason = program ? ps_spu_run(&ctx) : ps_spu_interpret(&ctx, 0);
    CHECK(reason == PS_SPU_STOP_SIGNAL);
    CHECK(ctx.stop_code == 0x42);
    if (program) CHECK(ctx.program == program);
    return ctx.gpr[4].u32[3];
}

} // namespace

TEST_CASE("session translation: a saved SPU image is translated in the background and its program runs (D-236)")
{
    const fs::path dir = fs::temp_directory_path() / std::format("pspocket-session-{}", getpid());
    fs::remove_all(dir);
    fs::create_directories(dir / "images");
    GuestMemory memory;
    REQUIRE(memory.reserve());
    const ps_spu_miss_policy policy = ps_spu_get_miss_policy();
    ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);

    // Two copies of one job with other data (one program, D-208), and a job registered already.
    const std::vector<u8> a = job(0x1000), b = job(0x2000), known = job(0x3000, 0x77);
    const std::string name_a = save(dir / "images", a), name_b = save(dir / "images", b);
    const ps_spu_entry none[] = {{0, nullptr}};
    ps_spu_program registered{"spu_session_known", ps_spu_hash64(known.data(), known.size()), kLs,
        static_cast<u32>(known.size()), PS_SPU_XFLOAT_APPROXIMATE, 1, 0, none, nullptr, nullptr, 0};
    ps_spu_register_program(&registered);
    save(dir / "images", known);

    prepare::SessionTranslator session(options(dir, memory));
    std::string error;
    REQUIRE(session.start(&error));
    const auto size = static_cast<u32>(a.size());
    REQUIRE(wait_until([&] { return ps_spu_find_program_by_code(a.data(), size) != nullptr; }, 120));
    // One program for both copies (lifted from the first by name), found for either by its code words.
    const ps_spu_program* program = ps_spu_find_program_by_code(a.data(), size);
    CHECK((std::string(program->name) == name_a || std::string(program->name) == name_b));
    CHECK(ps_spu_find_program_by_code(b.data(), size) == program);
    session.stop();
    const prepare::SessionStats st = session.stats();
    CHECK(st.images == 3);
    CHECK(st.known == 1);
    CHECK(st.lifted == 1);
    CHECK(st.loaded == 1);
    CHECK(st.failed == 0);
    CHECK_FALSE(st.stopped_on_failure);

    // It runs as the interpreter runs the image, wherever its data says.
    CHECK(run_job(a, program, memory) == run_job(a, nullptr, memory));
    CHECK(run_job(b, program, memory) == run_job(b, nullptr, memory));
    CHECK(run_job(b, program, memory) == 0x246 + 0x2000);

    ps_spu_unregister_program(&registered);
    ps_spu_set_miss_policy(policy);
    fs::remove_all(dir);
}

TEST_CASE("session translation: a program predicted too slow waits for the next launch; a refused load stops the "
          "translator (D-236)")
{
    const fs::path dir = fs::temp_directory_path() / std::format("pspocket-session-refused-{}", getpid());
    fs::remove_all(dir);
    fs::create_directories(dir / "images");
    GuestMemory memory;
    REQUIRE(memory.reserve());
    save(dir / "images", job(0x4000, 0x55)); // code no earlier test's library has
    SECTION("too slow")
    {
        prepare::SessionOptions o = options(dir, memory);
        o.max_program_seconds = 1e-9;
        prepare::SessionTranslator session(std::move(o));
        std::string error;
        REQUIRE(session.start(&error));
        REQUIRE(wait_until([&] { return session.stats().deferred == 1; }, 60));
        session.stop();
        CHECK(session.stats().loaded == 0);
    }
    SECTION("refused")
    {
        prepare::SessionOptions o = options(dir, memory);
        o.load = [](const std::string&, std::string* error) {
            *error = "refused by the test";
            return false;
        };
        prepare::SessionTranslator session(std::move(o));
        std::string error;
        REQUIRE(session.start(&error));
        REQUIRE(wait_until([&] { return session.stats().stopped_on_failure; }, 120));
        session.stop();
        CHECK(session.stats().loaded == 0);
        CHECK(session.stats().failed == 1);
    }
    fs::remove_all(dir);
}

namespace {

constexpr size_t kPage = 16384;
u8* g_fault_page = nullptr;
std::atomic<u64> g_faults{0};

// What a game's write tracking does (D-134): a write to a protected page, let through by the handler.
void on_test_fault(int, siginfo_t* info, void*)
{
    u8* at = static_cast<u8*>(info->si_addr);
    if (at >= g_fault_page && at < g_fault_page + kPage) {
        mprotect(g_fault_page, kPage, PROT_READ | PROT_WRITE);
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
}

} // namespace

TEST_CASE("session translation: compiles and links survive the faults a game takes meanwhile (D-236)")
{
    // Regression, run 218: LLVM's signal handlers, installed over the game's by the first compile, deleted the
    // compiles' temporary files at each fault before passing it on: every program "did not compile".
    codegen::install_llvm_signal_handlers();
    struct sigaction sa{}, old_segv{}, old_bus{};
    sa.sa_sigaction = on_test_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    void* page = mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(page != MAP_FAILED);
    g_fault_page = static_cast<u8*>(page);
    std::atomic<bool> done{false};
    std::thread faulter([&] {
        for (u8 v = 0; !done.load(); v++) {
            mprotect(g_fault_page, kPage, PROT_READ);
            *reinterpret_cast<volatile u8*>(g_fault_page) = v;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    const fs::path dir = fs::temp_directory_path() / std::format("pspocket-session-faults-{}", getpid());
    fs::remove_all(dir);
    fs::create_directories(dir / "images");
    GuestMemory memory;
    REQUIRE(memory.reserve());
    save(dir / "images", job(0x5000, 0x99)); // code no earlier test's library has
    const u64 faults = g_faults.load();
    prepare::SessionTranslator session(options(dir, memory));
    std::string error;
    REQUIRE(session.start(&error));
    REQUIRE(wait_until([&] { return session.stats().loaded + session.stats().failed > 0; }, 120));
    session.stop();
    done = true;
    faulter.join();
    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGBUS, &old_bus, nullptr);
    munmap(page, kPage);
    CHECK(g_faults.load() - faults > 10);
    CHECK(session.stats().loaded == 1);
    CHECK(session.stats().failed == 0);
    fs::remove_all(dir);
}
