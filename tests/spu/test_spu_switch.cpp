// A context running an image with no lifted program switches to the program registered for it while it runs (D-233):
// at the interpreter's next taken branch after the registration, if the image's code words still hold the program's
// code and the program was lifted for the context's floating-point model. Either way the results equal the
// interpreter's (CLAUDE.md rule 3). The image is the synthetic program-level fuzz program (fuzz_gen.cpp), lifted at
// build time.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <thread>

#include "fuzz_case.h"
#include "pspocket/core/log.hpp"
#include "pspocket/spu/registry.hpp"
#include "spu_test_util.hpp"

using namespace pspocket;
using namespace pspocket::spu;
using namespace pspocket::spu::test;

namespace {

u64 fixed_time() { return 1000; }

struct Quiet {
    log::Level level = log::min_level();
    ps_spu_miss_policy policy = ps_spu_get_miss_policy();
    Quiet()
    {
        ps_spu_set_timebase_source(fixed_time);
        log::set_min_level(log::Level::Fatal);
        // The program's absolute call leaves it for a stub: a miss, interpreted (CLAUDE.md rule 5 aborts dev builds).
        ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
    }
    ~Quiet()
    {
        ps_spu_set_timebase_source(nullptr);
        log::set_min_level(level);
        ps_spu_set_miss_policy(policy);
        ps_spu_unregister_program(&ps_spu_fzp_program);
    }
};

constexpr u32 kAt = PS_SPU_FUZZ_PROGRAM_BASE + 0x2340; // relocated, as a job binary would be
constexpr u32 kIlR21 = 0x40802A95u;                    // il r21, 0x55: run after the first stop

// The program's stop 0x12 is answered and goes on; 0x10, 0x13 and 0x11 leave (D-247).
const ps_spu_hooks kAnswers{[](SPUContext*, u32 code) { return code == 0x12 ? 1 : 0; }, nullptr};

// Two contexts over the same local store contents: the interpreter's, and one that starts with no program.
struct Pair {
    GuestMemory mem;
    LocalStore ls_interp, ls_run;
    SPUContext interp{}, run{};
    explicit Pair(bool patch = false)
    {
        REQUIRE(mem.reserve());
        Rng fill(0x5717);
        for (u32 i = 0; i < PS_SPU_LS_SIZE; i += 8) ps_st64(ls_interp.data() + i, fill.next());
        for (u32 i = 0; i < ps_spu_fuzz_program_size; i++) ls_interp.data()[kAt + i] = ps_spu_fuzz_program[i];
        ps_st32(ls_interp.data() + PS_SPU_FUZZ_PROGRAM_STUB, 0x35000000u); // bi $lr
        if (patch) { // another constant in a code word: the same program to the interpreter, not to its code hash
            u32 at = 0;
            for (u32 i = 0; i < ps_spu_fuzz_program_size; i += 4)
                if (ps_ld32(ls_interp.data() + kAt + i) == kIlR21) at = kAt + i;
            REQUIRE(at != 0);
            ps_st32(ls_interp.data() + at, kIlR21 + 0x80); // il r21, 0x56
        }
        std::memcpy(ls_run.data(), ls_interp.data(), PS_SPU_LS_SIZE);
        ps_spu_context_init(&interp, ls_interp.data(), mem.base());
        ps_spu_context_init(&run, ls_run.data(), mem.base());
        interp.pc = run.pc = ps_spu_fuzz_program_entry - PS_SPU_FUZZ_PROGRAM_BASE + kAt;
        ps_spu_host_attach(&run);
        interp.hooks = run.hooks = &kAnswers;
    }
    ~Pair() { ps_spu_host_detach(&run); }
    void arm(u64 additions)
    {
        ps_spu_attach_program_at(&run, nullptr, kAt); // loaded with none, as SPURS does
        set_pending_home(&run, ps_spu_fzp_program.hash, kAt, ps_spu_fzp_program.code_size, true, additions);
    }
    // Both run to the stop with this code; their registers, channels and local stores agree.
    void step(u32 code)
    {
        REQUIRE(ps_spu_interpret(&interp, 0) == PS_SPU_STOP_SIGNAL);
        REQUIRE(ps_spu_run(&run) == PS_SPU_STOP_SIGNAL);
        CHECK(interp.stop_code == code);
        CHECK(run.stop_code == code);
        CHECK(diff_contexts(interp, run).empty());
        CHECK(std::memcmp(ls_interp.data(), ls_run.data(), PS_SPU_LS_SIZE) == 0);
    }
};

} // namespace

TEST_CASE("SPU switch: a context running an image with no program takes the one registered meanwhile (D-233)", "[spu]")
{
    const Quiet quiet;
    Pair p;
    p.arm(registry_additions());
    const u64 adopted = pending_homes_adopted();
    p.step(0x10); // nothing registered: interpreted throughout
    CHECK(p.run.home == nullptr);
    ps_spu_fzp_register();
    p.step(0x13);
    p.step(0x11);
    CHECK(p.run.home == &ps_spu_fzp_program);
    CHECK(p.run.program == &ps_spu_fzp_program);
    CHECK(pending_homes_adopted() - adopted == 1);
    CHECK(gw(p.run.gpr[23], 0) == 0x7123 + 0x56);
}

TEST_CASE("SPU switch: not when the image's code changed, the program's model differs, or nothing was registered since "
          "(D-233)", "[spu]")
{
    const Quiet quiet;
    SECTION("a code word changed")
    {
        Pair p(true);
        p.arm(registry_additions());
        ps_spu_fzp_register();
        p.step(0x10);
        p.step(0x13);
        p.step(0x11);
        CHECK(p.run.home == nullptr);
        CHECK(gw(p.run.gpr[23], 0) == 0x7123 + 0x57);
    }
    SECTION("lifted for the other floating-point model")
    {
        Pair p;
        p.interp.xfloat = p.run.xfloat = ps_spu_fzp_program.xfloat ^ 1;
        p.arm(registry_additions());
        ps_spu_fzp_register();
        p.step(0x10);
        p.step(0x13);
        p.step(0x11);
        CHECK(p.run.home == nullptr);
    }
    SECTION("registered before the image was looked up: its load would have found it, so no lookup is made")
    {
        ps_spu_fzp_register();
        Pair p;
        p.arm(registry_additions());
        p.step(0x10);
        p.step(0x13);
        p.step(0x11);
        CHECK(p.run.home == nullptr);
    }
    SECTION("another attach forgets the image")
    {
        Pair p;
        p.arm(registry_additions());
        ps_spu_attach_program_at(&p.run, nullptr, kAt);
        ps_spu_fzp_register();
        p.step(0x10);
        p.step(0x13);
        p.step(0x11);
        CHECK(p.run.home == nullptr);
    }
    SECTION("a context without a host keeps nothing pending")
    {
        Pair p;
        ps_spu_host_detach(&p.run);
        p.arm(registry_additions());
        ps_spu_fzp_register();
        p.step(0x10);
        p.step(0x13);
        p.step(0x11);
        CHECK(p.run.home == nullptr);
    }
}

TEST_CASE("SPU switch: a registration racing the run changes nothing but where it runs (D-233)", "[spu]")
{
    const Quiet quiet;
    for (int round = 0; round < 50; round++) {
        Pair p;
        p.arm(registry_additions());
        u32 stops = 0; // assertions stay on the test's thread
        std::thread runner([&] {
            for (int i = 0; i < 3; i++) {
                stops += ps_spu_interpret(&p.interp, 0) == PS_SPU_STOP_SIGNAL;
                stops += ps_spu_run(&p.run) == PS_SPU_STOP_SIGNAL;
            }
        });
        for (int spin = 0; spin < round * 20; spin++) std::this_thread::yield();
        ps_spu_fzp_register();
        runner.join();
        CHECK(stops == 6);
        CHECK(p.run.stop_code == 0x11);
        CHECK(diff_contexts(p.interp, p.run).empty());
        CHECK(std::memcmp(p.ls_interp.data(), p.ls_run.data(), PS_SPU_LS_SIZE) == 0);
        CHECK((p.run.home == nullptr || p.run.home == &ps_spu_fzp_program));
        ps_spu_unregister_program(&ps_spu_fzp_program);
    }
}
