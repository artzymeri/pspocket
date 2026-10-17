// The run watchdog (pspocket/loader/watchdog.hpp, D-1043): its checks over synthetic samples, and a deadlock of real
// lv2 threads found within the check's time, while threads that wait with a timeout are not one.

#include "hle_test_util.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "pspocket/loader/watchdog.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;
using loader::Stall;
using loader::StallJudge;
using loader::WatchSample;

namespace {

loader::WatchdogConfig config(double freeze, double deadlock, double livelock)
{
    loader::WatchdogConfig c;
    c.freeze_s = freeze;
    c.deadlock_s = deadlock;
    c.livelock_s = livelock;
    c.first_frame_s = 0; // its own test below
    return c;
}

} // namespace

TEST_CASE("watchdog: a freeze is frames, then none for the freeze time; once per freeze", "[hle][watchdog]")
{
    StallJudge j(config(10, 0, 0));
    WatchSample s;
    for (int i = 0; i <= 40; i++) { // no frame yet: a game loading is no freeze
        s.t = i * 0.5;
        CHECK_FALSE(j.observe(s).has_value());
    }
    s.flips = 100;
    CHECK_FALSE(j.observe(s).has_value());
    int reported = 0;
    for (int i = 1; i <= 30; i++) {
        s.t = 20 + i * 0.5;
        if (const auto r = j.observe(s)) {
            CHECK(r->first == Stall::Freeze);
            CHECK(r->second.find("after frame 100") != std::string::npos);
            CHECK(s.t >= 30);
            reported++;
        }
    }
    CHECK(reported == 1);
    s.flips = 101; // progress, then the next freeze is reported again
    s.t += 0.5;
    CHECK_FALSE(j.observe(s).has_value());
    for (int i = 1; i <= 25; i++) {
        s.t += 0.5;
        if (j.observe(s)) reported++;
    }
    CHECK(reported == 2);
}

TEST_CASE("watchdog: no first frame for the first-frame time is reported once, as a freeze (D-1093)", "[hle][watchdog]")
{
    loader::WatchdogConfig c = config(10, 0, 0);
    c.first_frame_s = 60;
    StallJudge j(c);
    WatchSample s;
    int reported = 0;
    for (int i = 0; i <= 200; i++) { // 100 s, threads polling, no frame (L.A. Noire's "Starting")
        s.t = i * 0.5;
        if (const auto r = j.observe(s)) {
            CHECK(r->first == Stall::Freeze);
            CHECK(r->second.find("no first frame") != std::string::npos);
            CHECK(s.t >= 60);
            reported++;
        }
    }
    CHECK(reported == 1);
    s.flips = 1; // a frame at last: the first-frame check is done
    for (int i = 0; i < 10; i++) {
        s.t += 0.5;
        CHECK_FALSE(j.observe(s).has_value());
    }
}

TEST_CASE("watchdog: a deadlock is every PPU thread blocked for good while the SPUs and the RSX are idle",
    "[hle][watchdog]")
{
    const auto run = [](u64 spu_ns_per_poll, usize stuck, double stuck_for_at_end) {
        StallJudge j(config(0, 5, 0));
        WatchSample s;
        s.ppu_threads = 4;
        s.flips = 7;
        std::optional<std::pair<Stall, std::string>> found;
        for (int i = 0; i <= 30 && !found; i++) {
            s.t = i * 0.5;
            s.ppu_stuck = stuck;
            s.ppu_stuck_for = std::min(s.t, stuck_for_at_end);
            s.spu_cpu_ns += spu_ns_per_poll;
            found = j.observe(s);
        }
        return found;
    };
    const auto dead = run(1'000'000, 4, 100); // 1 ms of SPU time each half second: idle
    REQUIRE(dead.has_value());
    CHECK(dead->first == Stall::Deadlock);
    CHECK(dead->second.find("all 4 PPU threads") != std::string::npos);
    CHECK_FALSE(run(200'000'000, 4, 100).has_value()); // the SPUs busy (40% of a core): they may wake a thread
    CHECK_FALSE(run(0, 3, 100).has_value());           // one PPU thread still runs
    CHECK_FALSE(run(0, 4, 2).has_value());             // the threads keep being woken (each wait short)
}

TEST_CASE("watchdog: a livelock is frames that keep the same picture; the presses meanwhile counted",
    "[hle][watchdog]")
{
    StallJudge j(config(0, 0, 20));
    WatchSample s;
    std::optional<std::pair<Stall, std::string>> found;
    for (int i = 0; i <= 100 && !found; i++) {
        s.t = i * 0.5;
        s.flips = static_cast<u64>(i) * 15;
        s.frames_hashed = static_cast<u64>(i);
        s.frame_hash = i < 10 ? static_cast<u64>(i) + 1 : 0xABCD; // a moving picture, then a still one
        s.pad_changes = static_cast<u64>(i / 8);
        found = j.observe(s);
    }
    REQUIRE(found.has_value());
    CHECK(found->first == Stall::Livelock);
    CHECK(s.t >= 25);
    CHECK(found->second.find("the controller changed") != std::string::npos);

    StallJudge moving(config(0, 0, 20));
    for (int i = 0; i <= 100; i++) {
        s.t = i * 0.5;
        s.flips = static_cast<u64>(i) * 15;
        s.frames_hashed = static_cast<u64>(i);
        s.frame_hash = static_cast<u64>(i % 7) + 1;
        CHECK_FALSE(moving.observe(s).has_value());
    }
    // A thumbnail's hash: the same picture the same, a changed one not, the lowest bits not counted.
    std::vector<u8> a(64 * 36 * 4, 100), b = a, c = a;
    b[(10 * 64 + 10) * 4] ^= 1;
    for (usize i = 0; i < c.size() / 2; i++) c[i] = 200;
    CHECK(loader::frame_thumbnail_hash(a.data(), 64, 36) == loader::frame_thumbnail_hash(b.data(), 64, 36));
    CHECK(loader::frame_thumbnail_hash(a.data(), 64, 36) != loader::frame_thumbnail_hash(c.data(), 64, 36));
}

namespace {

struct Entries {
    std::atomic<u32> queue{0};
    std::atomic<u64> timeout{0};
    std::atomic<int> returned{0};
    static void execute(void* user, PPUContext& ctx, u8*, u32)
    {
        auto* self = static_cast<Entries*>(user);
        lv2::PpuThread* t = lv2::current_thread();
        THREAD_REQUIRE(t != nullptr);
        for (;;) {
            const lv2::Result r = lv2::sys_event_queue_receive(*t, self->queue.load(), 0, self->timeout.load());
            if (r != lv2::err(lv2::CELL_ETIMEDOUT)) break;
        }
        self->returned++;
        ctx.gpr[3] = 0;
        lv2::_sys_ppu_thread_exit(*t, 0);
    }
};

} // namespace

TEST_CASE("watchdog: two lv2 threads waiting on a queue nobody sends to are a deadlock", "[hle][watchdog]")
{
    using namespace std::chrono_literals;
    for (const u64 timeout_us : {u64{0}, u64{100'000}}) {
        INFO("the threads' timeout: " << timeout_us << " us");
        Entries entries;
        entries.timeout = timeout_us;
        lv2::Config kc;
        kc.executor = &Entries::execute;
        kc.executor_user = &entries;
        Fixture f({}, kc);
        GuestThread main(f, 0, "main"); // attached: not counted
        u8* b = f.base();
        const u32 attr = kData, out = kData + 0x40;
        ps_write32(b, attr, lv2::SYS_SYNC_FIFO);
        ps_write32(b, attr + 4, lv2::SYS_PPU_QUEUE);
        REQUIRE(lv2::sys_event_queue_create(*main.t, out, attr, 0, 8) == lv2::CELL_OK);
        entries.queue = ps_read32(b, out);
        const u32 opd = kData + 0x80, param = kData + 0x90;
        ps_write32(b, opd, 0x10000);
        ps_write32(b, param, opd);
        std::vector<u32> ids;
        for (int i = 0; i < 2; i++) {
            REQUIRE(lv2::_sys_ppu_thread_create(*main.t, kData + 0xA0, param, 0, 0, 1000, 0x4000, 1, 0) ==
                lv2::CELL_OK);
            ids.push_back(static_cast<u32>(ps_read64(b, kData + 0xA0)));
            REQUIRE(lv2::sys_ppu_thread_start(*main.t, ids.back()) == lv2::CELL_OK);
        }
        std::atomic<int> stalls{0};
        loader::WatchdogConfig wc = config(0, 1, 0);
        wc.poll_s = 0.1;
        loader::Watchdog dog(*f.kernel, wc, [&](const loader::StallReport& r) {
            THREAD_CHECK(r.kind == Stall::Deadlock);
            THREAD_CHECK(r.title.find("all 2 PPU threads") != std::string::npos);
            THREAD_CHECK(r.report.find("event queue") != std::string::npos);
            stalls++;
        });
        dog.start();
        const auto t0 = std::chrono::steady_clock::now();
        while (!stalls && std::chrono::steady_clock::now() - t0 < 3s) std::this_thread::sleep_for(20ms);
        std::this_thread::sleep_for(300ms);
        dog.stop();
        CHECK(stalls.load() == (timeout_us ? 0 : 1));
        if (!timeout_us) {
            CHECK(dog.reports().size() == 1);
            CHECK(std::chrono::steady_clock::now() - t0 < 3s);
        }
        REQUIRE(lv2::sys_event_queue_destroy(*main.t, entries.queue, lv2::SYS_EVENT_QUEUE_DESTROY_FORCE) ==
            lv2::CELL_OK);
        for (const u32 id : ids) CHECK(lv2::sys_ppu_thread_join(*main.t, id, kData + 0xB0) == lv2::CELL_OK);
        CHECK(entries.returned.load() == 2);
    }
}
