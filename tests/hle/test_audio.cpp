// cellAudio (synthetic: no game data): init and quit, ports and their configuration, the audio thread's periods (read
// indices, cleared blocks, notify events and their Ex variants), the Add*Data functions, block tags and timestamps,
// port levels, the host sink, and the end of the thread.

#include "hle_test_util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "pspocket/hle/audio.hpp"
#include "pspocket/lv2/sync.hpp"
#include "pspocket/lv2/syscalls.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace std::chrono_literals;

namespace {

constexpr u32 kParam = kData + 0x100, kPortNum = kData + 0x140, kConfig = kData + 0x180, kQueueId = kData + 0x200,
              kOtherQueueId = kData + 0x204, kKey = kData + 0x208, kAttr = kData + 0x300, kEvents = kData + 0x400,
              kCount = kData + 0x800, kSrc = kData + 0x1000;
const u64 kAlreadyInit = r3_of_code(0x80310701), kNotInit = r3_of_code(0x80310703),
          kParamError = r3_of_code(0x80310704), kPortFull = r3_of_code(0x80310705),
          kAlreadyRun = r3_of_code(0x80310706), kNotOpen = r3_of_code(0x80310707), kNotRun = r3_of_code(0x80310708),
          kTransEvent = r3_of_code(0x80310709);
constexpr u64 kFirstKey = 0x80004D494F323221ull;
constexpr u64 kFirstSource = 0x10E000000ull; // pid 1 << 32, plus the first made-up event port ID (0x0e000000)
constexpr u32 kStatusClose = 0x1010, kStatusReady = 1, kStatusRun = 2;
constexpr double kPeriodMs = 256.0 * 1000.0 / 48000.0;

struct PortConfig {
    u32 read_index_addr = 0;
    u32 status = 0;
    u64 channels = 0;
    u64 blocks = 0;
    u32 size = 0;
    u32 addr = 0;
};

// cellAudioPortOpen with a CellAudioPortParam {nChannel, nBlock, attr, level} at kParam.
u64 open_port(GuestThread& g, u64 channels, u64 blocks, u64 attr = 0, f32 level = 0.0f)
{
    u8* b = g.f.base();
    ps_write64(b, kParam, channels);
    ps_write64(b, kParam + 8, blocks);
    ps_write64(b, kParam + 16, attr);
    ps_writef32(b, kParam + 24, level);
    ps_write32(b, kPortNum, 0xFFFFFFFF);
    return g.call("cellAudioPortOpen", kParam, kPortNum);
}

u32 open_ok(GuestThread& g, u64 channels, u64 blocks, u64 attr = 0, f32 level = 0.0f)
{
    REQUIRE(open_port(g, channels, blocks, attr, level) == 0);
    return ps_read32(g.f.base(), kPortNum);
}

PortConfig port_config(GuestThread& g, u32 port)
{
    REQUIRE(g.call("cellAudioGetPortConfig", port, kConfig) == 0);
    u8* b = g.f.base();
    return {ps_read32(b, kConfig), ps_read32(b, kConfig + 4), ps_read64(b, kConfig + 8), ps_read64(b, kConfig + 16),
        ps_read32(b, kConfig + 24), ps_read32(b, kConfig + 28)};
}

double ms_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The events a queue holds, taken without waiting (at most 16).
u32 queued_events(GuestThread& g, u32 queue)
{
    REQUIRE(lv2::sys_event_queue_tryreceive(*g.t, queue, kEvents, 16, kCount) == 0);
    return ps_read32(g.f.base(), kCount);
}

} // namespace

TEST_CASE("cellAudio: the functions GTA V imports", "[hle][audio]")
{
    struct Expected {
        std::string_view name;
        u32 nid;
    };
    const Expected expected[] = {
        {"cellAudioInit", 0x0B168F92},
        {"cellAudioQuit", 0xCA5AC370},
        {"cellAudioPortOpen", 0xCD7BC431},
        {"cellAudioPortClose", 0x4129FE2D},
        {"cellAudioPortStart", 0x89BE28F2},
        {"cellAudioPortStop", 0x5B1E2C73},
        {"cellAudioGetPortConfig", 0x74A66AF0},
        {"cellAudioCreateNotifyEventQueue", 0x04AF134E},
        {"cellAudioSetNotifyEventQueue", 0x377E0CD9},
        {"cellAudioRemoveNotifyEventQueue", 0xFF3626FD},
        {"cellAudioAddData", 0xDAB029AA},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const Expected& e : expected) {
        INFO(e.name);
        const hle::Function* fn = hle::registry().find("cellAudio", e.nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == e.name);
    }
}

TEST_CASE("cellAudio: init and quit; nothing works before init or after quit", "[hle][audio]")
{
    Fixture f;
    GuestThread g(f, 0, "audio");

    CHECK(g.call("cellAudioQuit") == kNotInit);
    CHECK(open_port(g, 2, 8) == kNotInit);
    CHECK(g.call("cellAudioGetPortConfig", 0, kConfig) == kNotInit);
    CHECK(g.call("cellAudioPortStart", 0) == kNotInit);
    CHECK(g.call("cellAudioPortStop", 0) == kNotInit);
    CHECK(g.call("cellAudioPortClose", 0) == kNotInit);
    CHECK(g.call("cellAudioSetNotifyEventQueue", kFirstKey) == kNotInit);
    CHECK(g.call("cellAudioRemoveNotifyEventQueue", kFirstKey) == kNotInit);
    CHECK(g.call("cellAudioAddData", 0, kSrc, 256) == kNotInit);

    // GTA V leaves unrelated values in r3..r10 (run 019); RPCS3's cellAudioInit takes no arguments.
    REQUIRE(g.call("cellAudioInit", 0x3, 0x1, 0x1b20000, 0x1, 0x8) == 0);
    CHECK(g.call("cellAudioInit") == kAlreadyInit);
    CHECK(open_ok(g, 2, 8) == 0);
    const PortConfig c = port_config(g, 0);
    REQUIRE(g.call("cellAudioQuit") == 0); // closes the port, frees its buffer and the read indices
    CHECK_FALSE(f.mem.is_committed(c.addr));
    CHECK_FALSE(f.mem.is_committed(c.read_index_addr));
    CHECK(g.call("cellAudioQuit") == kNotInit);
    CHECK(open_port(g, 2, 8) == kNotInit);
    CHECK(g.call("cellAudioPortStart", 0) == kNotInit);

    // Again, from closed ports.
    REQUIRE(g.call("cellAudioInit") == 0);
    CHECK(port_config(g, 0).status == kStatusClose);
    CHECK(open_ok(g, 2, 8) == 0);
    REQUIRE(g.call("cellAudioQuit") == 0);
}

TEST_CASE("cellAudio: ports: parameters, configuration, start, stop, close, port numbers", "[hle][audio]")
{
    Fixture f; // SDK 3.6: 8 ports
    GuestThread g(f, 0, "audio");
    u8* b = f.base();
    REQUIRE(g.call("cellAudioInit") == 0);

    // RPCS3's checks, in its order: pointers, channels (2, 8 or 0), blocks (2, 4, 8, 16 or 32).
    CHECK(g.call("cellAudioPortOpen", 0, kPortNum) == kParamError);
    CHECK(g.call("cellAudioPortOpen", kParam, 0) == kParamError);
    for (const u64 channels : {1, 4, 6}) CHECK(open_port(g, channels, 8) == kParamError);
    for (const u64 blocks : {0, 1, 3, 64}) CHECK(open_port(g, 2, blocks) == kParamError);

    // Port 0: stereo, 8 blocks. Server index 1: its read index is the second 16-byte slot of the index area.
    CHECK(open_ok(g, 2, 8) == 0);
    const PortConfig c0 = port_config(g, 0);
    CHECK(c0.status == kStatusReady);
    CHECK(c0.channels == 2);
    CHECK(c0.blocks == 8);
    CHECK(c0.size == 2 * 8 * 256 * 4);
    CHECK(c0.addr % 0x10000 == 0);
    CHECK(f.mem.is_committed(c0.addr + 0xFFFF)); // 64 KB at least, as RPCS3's shared memory
    const u32 area = c0.read_index_addr - 16;
    CHECK(area % 0x10000 == 0);
    CHECK(ps_read64(b, c0.read_index_addr) == 0);

    // Port 1: 8 channels, 16 blocks, its own level; server index 2. Port 2: no channels, one sample per frame.
    CHECK(open_ok(g, 8, 16, 0x1000, 0.5f) == 1);
    const PortConfig c1 = port_config(g, 1);
    CHECK(c1.read_index_addr == area + 32);
    CHECK(c1.size == 8 * 16 * 256 * 4);
    CHECK(c1.addr % 0x10000 == 0);
    CHECK(c1.addr != c0.addr);
    CHECK(open_ok(g, 0, 2) == 2);
    CHECK(port_config(g, 2).size == 2 * 256 * 4);
    for (u32 i = 3; i < 8; i++) CHECK(open_ok(g, 2, 2) == i);
    CHECK(open_port(g, 2, 2) == kPortFull);

    CHECK(g.call("cellAudioGetPortConfig", 8, kConfig) == kParamError);
    CHECK(g.call("cellAudioGetPortConfig", 0, 0) == kParamError);
    CHECK(g.call("cellAudioPortStart", 8) == kParamError);
    CHECK(g.call("cellAudioPortStop", 8) == kParamError);
    CHECK(g.call("cellAudioPortClose", 8) == kParamError);

    // Starting clears the whole ring (RPCS3); a started port can't be started again; stopping goes back to ready.
    ps_writef32(b, c0.addr + 7 * 2048 + 4, 1.0f); // block 7
    CHECK(g.call("cellAudioPortStop", 0) == kNotRun);
    REQUIRE(g.call("cellAudioPortStart", 0) == 0);
    CHECK(ps_readf32(b, c0.addr + 7 * 2048 + 4) == 0.0f);
    CHECK(port_config(g, 0).status == kStatusRun);
    CHECK(g.call("cellAudioPortStart", 0) == kAlreadyRun);
    REQUIRE(g.call("cellAudioPortStop", 0) == 0);
    CHECK(port_config(g, 0).status == kStatusReady);
    CHECK(g.call("cellAudioPortStop", 0) == kNotRun);

    // Closing frees the buffer. A closed port keeps its fields (RPCS3); its number and server index are the next ones
    // handed out (RPCS3's free lists are stacks).
    REQUIRE(g.call("cellAudioPortClose", 0) == 0);
    const PortConfig closed = port_config(g, 0);
    CHECK(closed.status == kStatusClose);
    CHECK(closed.read_index_addr == c0.read_index_addr);
    CHECK(closed.addr == c0.addr);
    CHECK_FALSE(f.mem.is_committed(c0.addr));
    CHECK(g.call("cellAudioPortClose", 0) == kNotOpen);
    CHECK(g.call("cellAudioPortStart", 0) == kNotOpen);
    CHECK(g.call("cellAudioPortStop", 0) == kNotRun);
    CHECK(open_ok(g, 2, 32) == 0);
    const PortConfig reopened = port_config(g, 0);
    CHECK(reopened.read_index_addr == c0.read_index_addr);
    CHECK(reopened.blocks == 32);
    REQUIRE(g.call("cellAudioQuit") == 0);
}

TEST_CASE("cellAudio: SDK 3.5 and older get 4 ports and notify queues 2 events deep", "[hle][audio]")
{
    lv2::Config kconfig;
    kconfig.sdk_version = 0x00350001;
    Fixture f({}, kconfig);
    GuestThread g(f, 0, "audio");
    u8* b = f.base();
    REQUIRE(g.call("cellAudioInit") == 0);
    for (u32 i = 0; i < 4; i++) CHECK(open_ok(g, 2, 2) == i);
    CHECK(open_port(g, 2, 2) == kPortFull);

    REQUIRE(g.call("cellAudioCreateNotifyEventQueue", kQueueId, kKey) == 0);
    const u32 queue = ps_read32(b, kQueueId);
    REQUIRE(g.call("cellAudioSetNotifyEventQueue", ps_read64(b, kKey)) == 0);
    std::this_thread::sleep_for(50ms); // about 9 periods
    CHECK(queued_events(g, queue) == 2);
    REQUIRE(g.call("cellAudioQuit") == 0);
}

TEST_CASE("cellAudio: the audio thread plays a block of every started port each 5.33 ms", "[hle][audio]")
{
    Fixture f;
    GuestThread g(f, 0, "audio");
    u8* b = f.base();
    REQUIRE(g.call("cellAudioInit") == 0);
    const u32 played = open_ok(g, 2, 32);
    const u32 idle = open_ok(g, 2, 32);
    const PortConfig cp = port_config(g, played);
    const PortConfig ci = port_config(g, idle);

    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(g.call("cellAudioPortStart", played) == 0);
    std::this_thread::sleep_for(50ms);
    REQUIRE(g.call("cellAudioPortStop", played) == 0);
    const double ms = ms_since(t0);
    const u64 index = ps_read64(b, cp.read_index_addr);
    INFO("read index " << index << " after " << ms << " ms");
    // About 9 periods: loose for a loaded machine, but never faster than 48 kHz allows (while the 32-block ring has
    // not wrapped).
    CHECK(index >= 3);
    if (ms < 30 * kPeriodMs) CHECK(static_cast<double>(index) <= 1.5 * ms / kPeriodMs + 3);
    CHECK(ps_read64(b, ci.read_index_addr) == 0); // opened, not started
    std::this_thread::sleep_for(20ms);
    CHECK(ps_read64(b, cp.read_index_addr) == index); // a stopped port keeps its read index

    // A played block is cleared: data added at the read index of a running port is gone a period later.
    for (u32 i = 0; i < 512; i++) ps_writef32(b, kSrc + 4 * i, 1.0f);
    REQUIRE(g.call("cellAudioPortStart", played) == 0);
    g.ctx.fpr[1] = 1.0;
    REQUIRE(g.call("cellAudioAddData", played, kSrc, 256) == 0);
    std::this_thread::sleep_for(30ms);
    REQUIRE(g.call("cellAudioPortStop", played) == 0);
    bool silent = true;
    for (u32 i = 0; i < cp.size; i += 4) silent = silent && ps_readf32(b, cp.addr + i) == 0.0f;
    CHECK(silent);
    REQUIRE(g.call("cellAudioQuit") == 0);
}

TEST_CASE("cellAudio: notify event queues: creation, registration, an event every period, removal", "[hle][audio]")
{
    Fixture f;
    GuestThread g(f, 0, "audio");
    u8* b = f.base();
    lv2::PpuThread& t = *g.t;

    // cellAudioCreateNotifyEventQueue works before cellAudioInit (RPCS3). Keys count up from 0x80004D494F323221.
    REQUIRE(g.call("cellAudioCreateNotifyEventQueue", kQueueId, kKey) == 0);
    const u32 q1 = ps_read32(b, kQueueId);
    const u64 key1 = ps_read64(b, kKey);
    CHECK(key1 == kFirstKey);
    REQUIRE(g.call("cellAudioCreateNotifyEventQueue", kQueueId, kKey) == 0);
    const u32 q2 = ps_read32(b, kQueueId);
    const u64 key2 = ps_read64(b, kKey);
    CHECK(key2 == kFirstKey + 1); // the first key is taken
    CHECK(q2 != q1);
    CHECK(g.call("cellAudioCreateNotifyEventQueue", kQueueId, 0) == kParamError); // checked before a queue is made
    CHECK(g.call("cellAudioSetNotifyEventQueue", key1) == kNotInit);

    REQUIRE(g.call("cellAudioInit") == 0);
    REQUIRE(g.call("cellAudioSetNotifyEventQueue", key1) == 0);
    CHECK(g.call("cellAudioSetNotifyEventQueue", key1) == kTransEvent);   // already set
    CHECK(g.call("cellAudioSetNotifyEventQueue", 0x1234) == kTransEvent); // no such queue
    // A keyed queue the game made itself is found by its key too (RPCS3 lv2_event_queue::find).
    ps_write32(b, kAttr, lv2::SYS_SYNC_FIFO);
    ps_write32(b, kAttr + 4, lv2::SYS_PPU_QUEUE);
    ps_write64(b, kAttr + 8, 0);
    REQUIRE(lv2::sys_event_queue_create(t, kOtherQueueId, kAttr, 0x8000000012345678ull, 8) == 0);
    REQUIRE(g.call("cellAudioSetNotifyEventQueue", 0x8000000012345678ull) == 0);
    REQUIRE(g.call("cellAudioRemoveNotifyEventQueue", 0x8000000012345678ull) == 0);

    // CELL_AUDIO_EVENT_MIX every period: the source is pid 1 << 32 plus a made-up event port ID; data 0, 0, 0.
    REQUIRE(lv2::sys_event_queue_receive(t, q1, 0, 1'000'000) == 0);
    CHECK(g.ctx.gpr[4] == kFirstSource);
    CHECK(g.ctx.gpr[5] == 0); // CELL_AUDIO_EVENT_MIX
    CHECK(g.ctx.gpr[6] == 0);
    CHECK(g.ctx.gpr[7] == 0);
    REQUIRE(lv2::sys_event_queue_drain(t, q1) == 0);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; i++) REQUIRE(lv2::sys_event_queue_receive(t, q1, 0, 1'000'000) == 0);
    const double ms = ms_since(t0);
    INFO("20 events in " << ms << " ms");
    CHECK(ms > 15 * kPeriodMs); // about 20 periods
    CHECK(ms < 2000);

    // The next registration gets the next source (the game's own queue had the second). A queue holds 8 events
    // (SDK 3.6); the thread drops the rest.
    REQUIRE(g.call("cellAudioSetNotifyEventQueue", key2) == 0);
    REQUIRE(lv2::sys_event_queue_receive(t, q2, 0, 1'000'000) == 0);
    CHECK(g.ctx.gpr[4] == kFirstSource + 0x200);
    std::this_thread::sleep_for(80ms);
    CHECK(queued_events(g, q2) == 8);

    // A removed queue gets no more events.
    REQUIRE(g.call("cellAudioRemoveNotifyEventQueue", key1) == 0);
    CHECK(g.call("cellAudioRemoveNotifyEventQueue", key1) == kTransEvent);
    std::this_thread::sleep_for(10ms); // an event picked before the removal may still arrive
    REQUIRE(lv2::sys_event_queue_drain(t, q1) == 0);
    std::this_thread::sleep_for(30ms);
    CHECK(queued_events(g, q1) == 0);

    // A destroyed queue is neither removed nor set again.
    REQUIRE(lv2::sys_event_queue_destroy(t, q2, 0) == 0);
    CHECK(g.call("cellAudioRemoveNotifyEventQueue", key2) == kTransEvent);
    CHECK(g.call("cellAudioSetNotifyEventQueue", key2) == kTransEvent);

    // cellAudioQuit stops the events. A queue that stays registered gets them again after the next cellAudioInit.
    REQUIRE(g.call("cellAudioSetNotifyEventQueue", key1) == 0);
    REQUIRE(g.call("cellAudioQuit") == 0); // joins the thread: nothing is sent after it returns
    REQUIRE(lv2::sys_event_queue_drain(t, q1) == 0);
    std::this_thread::sleep_for(30ms);
    CHECK(queued_events(g, q1) == 0);
    REQUIRE(g.call("cellAudioInit") == 0);
    REQUIRE(lv2::sys_event_queue_receive(t, q1, 0, 1'000'000) == 0);
    CHECK(g.ctx.gpr[4] == kFirstSource + 0x300); // the fourth registration
    // The test ends without cellAudioQuit: the end of the process stops the thread.
}

TEST_CASE("cellAudio: cellAudioAddData adds to the block at the read index", "[hle][audio]")
{
    Fixture f;
    GuestThread g(f, 0, "audio");
    u8* b = f.base();
    REQUIRE(g.call("cellAudioInit") == 0);
    const u32 port = open_ok(g, 2, 32); // opened, not started: the read index stays at block 0
    const PortConfig c = port_config(g, port);
    for (u32 i = 0; i < 512; i++) ps_writef32(b, kSrc + 4 * i, static_cast<f32>(i + 1) * 0.25f);
    const auto block0 = [&](u32 i) { return ps_readf32(b, c.addr + 4 * i); };

    g.ctx.fpr[1] = 0.5; // the volume is a float argument: f1
    REQUIRE(g.call("cellAudioAddData", port, kSrc, 256) == 0);
    g.ctx.fpr[1] = 2.0;
    REQUIRE(g.call("cellAudioAddData", port, kSrc, 256) == 0);
    bool added = true; // both channels of all 256 frames, added up
    for (u32 i = 0; i < 512; i++) added = added && block0(i) == static_cast<f32>(i + 1) * 0.25f * 2.5f;
    CHECK(added);
    CHECK(ps_readf32(b, c.addr + 2048) == 0.0f); // block 1

    // RPCS3's checks: the port number, the source pointer and its alignment, exactly 256 samples.
    g.ctx.fpr[1] = 1.0;
    CHECK(g.call("cellAudioAddData", 8, kSrc, 256) == kParamError);
    CHECK(g.call("cellAudioAddData", port, 0, 256) == kParamError);
    CHECK(g.call("cellAudioAddData", port, kSrc + 2, 256) == kParamError);
    CHECK(g.call("cellAudioAddData", port, kSrc, 128) == kParamError);

    // The volume is clamped to [-16, 16]; NaN and infinities add nothing.
    const f32 before = block0(0);
    g.ctx.fpr[1] = 100.0;
    REQUIRE(g.call("cellAudioAddData", port, kSrc, 256) == 0);
    CHECK(block0(0) == before + 0.25f * 16.0f);
    g.ctx.fpr[1] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(g.call("cellAudioAddData", port, kSrc, 256) == 0);
    g.ctx.fpr[1] = -std::numeric_limits<double>::infinity();
    REQUIRE(g.call("cellAudioAddData", port, kSrc, 256) == 0);
    CHECK(block0(0) == before + 4.0f);

    // A port without channels, and one never opened: accepted, nothing written (RPCS3).
    const u32 mono = open_ok(g, 0, 2);
    const PortConfig cm = port_config(g, mono);
    g.ctx.fpr[1] = 1.0;
    CHECK(g.call("cellAudioAddData", mono, kSrc, 256) == 0);
    CHECK(ps_readf32(b, cm.addr) == 0.0f);
    CHECK(g.call("cellAudioAddData", 7, kSrc, 256) == 0);
    // A port closed since it was opened has no buffer any more (RPCS3 would write into the unmapped one).
    REQUIRE(g.call("cellAudioPortClose", port) == 0);
    CHECK(g.call("cellAudioAddData", port, kSrc, 256) == kNotOpen);
    REQUIRE(g.call("cellAudioQuit") == 0);
}

TEST_CASE("cellAudio: the host sink gets the stereo mix of the started ports every period", "[hle][audio]")
{
    Fixture f;
    GuestThread g(f, 0, "audio");
    u8* b = f.base();
    struct Seen {
        std::mutex mutex;
        u32 calls = 0, wrong_size = 0, silent = 0, mixed = 0;
    } seen;
    hle::set_audio_sink(*f.hle, [&](const f32* samples, u32 frames) {
        bool silent = true, mixed = true;
        for (u32 i = 0; i < hle::kAudioOutChannels * frames; i++) {
            silent = silent && samples[i] == 0.0f;
            mixed = mixed && std::fabs(samples[i] - (i % 2 ? -0.275f : 0.425f)) < 1e-5f;
        }
        std::lock_guard lock(seen.mutex);
        seen.calls++;
        seen.wrong_size += frames != hle::kAudioBlockFrames;
        seen.silent += silent;
        seen.mixed += mixed;
    });
    const auto snapshot = [&] {
        std::lock_guard lock(seen.mutex);
        return std::array<u32, 4>{seen.calls, seen.wrong_size, seen.silent, seen.mixed};
    };

    REQUIRE(g.call("cellAudioInit") == 0);
    std::this_thread::sleep_for(30ms);
    const auto quiet = snapshot();
    CHECK(quiet[0] >= 2);        // a period every 5.33 ms
    CHECK(quiet[2] == quiet[0]); // silence while no port is started

    // A stereo port; an 8-channel port at level 0.5, whose front left and right reach a stereo output without a
    // downmixer; a port without channels, whose sample goes to both sides. Left: 0.25 + 0.1 * 0.5 + 0.125 = 0.425;
    // right: -0.5 + 0.2 * 0.5 + 0.125 = -0.275. Every block is filled, so the periods after the first find them full.
    const u32 p2 = open_ok(g, 2, 32), p8 = open_ok(g, 8, 32, 0x1000, 0.5f), p0 = open_ok(g, 0, 32);
    const PortConfig c2 = port_config(g, p2), c8 = port_config(g, p8), c0 = port_config(g, p0);
    for (const u32 p : {p2, p8, p0}) REQUIRE(g.call("cellAudioPortStart", p) == 0);
    const f32 surround[8] = {0.1f, 0.2f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    for (u32 i = 0; i < 32 * 256; i++) {
        ps_writef32(b, c2.addr + 8 * i, 0.25f);
        ps_writef32(b, c2.addr + 8 * i + 4, -0.5f);
        for (u32 ch = 0; ch < 8; ch++) ps_writef32(b, c8.addr + 32 * i + 4 * ch, surround[ch]);
        ps_writef32(b, c0.addr + 4 * i, 0.125f);
    }
    std::this_thread::sleep_for(40ms);
    const auto playing = snapshot();
    CHECK(playing[1] == 0);
    CHECK(playing[3] >= 2);

    hle::set_audio_sink(*f.hle, {}); // detached: no more calls once it returns
    const u32 calls = snapshot()[0];
    std::this_thread::sleep_for(20ms);
    CHECK(snapshot()[0] == calls);
    REQUIRE(g.call("cellAudioQuit") == 0);
}

namespace {

// A speaker behind a sink with a buffer query, like platform::AudioOutput's: it plays 48 kHz in real time from what was
// pushed, from the first push on, and keeps every block.
struct FakeSpeaker {
    using Clock = std::chrono::steady_clock;
    std::mutex mutex;
    std::vector<std::array<f32, 2 * hle::kAudioBlockFrames>> blocks;
    u64 pushed = 0, played = 0; // frames
    u64 most_queued = 0;
    Clock::time_point last{};

    // What real time has played since the last call. With mutex held.
    void play()
    {
        if (!pushed) return;
        const u64 frames =
            static_cast<u64>(std::chrono::duration<double>(Clock::now() - last).count() * hle::kAudioSampleRate);
        last += std::chrono::nanoseconds(frames * 1'000'000'000ull / hle::kAudioSampleRate);
        played = std::min(pushed, played + frames);
    }

    void attach(hle::HleProcess& p)
    {
        hle::set_audio_sink(
            p,
            [this](const f32* samples, u32 frames) {
                std::lock_guard lock(mutex);
                if (!pushed) last = Clock::now();
                play();
                auto& block = blocks.emplace_back();
                std::copy(samples, samples + 2 * std::min(frames, hle::kAudioBlockFrames), block.begin());
                pushed += frames;
                most_queued = std::max(most_queued, pushed - played);
            },
            [this] {
                std::lock_guard lock(mutex);
                play();
                return hle::AudioBufferLevel{static_cast<u32>(pushed - played), hle::kAudioBlockFrames};
            });
    }

    size_t count()
    {
        std::lock_guard lock(mutex);
        return blocks.size();
    }
};

} // namespace

TEST_CASE("cellAudio: a sink with a buffer query gets RPCS3's buffering mode", "[hle][audio]")
{
    Fixture f;
    GuestThread g(f, 0, "audio");
    u8* b = f.base();
    FakeSpeaker speaker;
    speaker.attach(*f.hle);

    // The thread starts by queueing the target's worth of silence (34 ms, 9 blocks), then keeps the speaker near it.
    REQUIRE(g.call("cellAudioInit") == 0);
    std::this_thread::sleep_for(20ms);
    CHECK(speaker.count() >= 9);

    // Two ports of 4 blocks. Once each block has been played, it holds RPCS3's tags (-0.0f), the 8-channel port's
    // marks too, and zeros elsewhere. The game has never written these ports, so the thread doesn't wait for them: it
    // plays silence.
    const u32 p2 = open_ok(g, 2, 4), p8 = open_ok(g, 8, 4);
    const PortConfig c2 = port_config(g, p2), c8 = port_config(g, p8);
    REQUIRE(g.call("cellAudioPortStart", p2) == 0);
    REQUIRE(g.call("cellAudioPortStart", p8) == 0);
    std::this_thread::sleep_for(100ms);
    REQUIRE(g.call("cellAudioPortStop", p2) == 0);
    REQUIRE(g.call("cellAudioPortStop", p8) == 0);
    const auto check_tagged = [&](const PortConfig& c, u32 channels) {
        const u32 floats = channels * 256;
        u32 tags = 0, zeros = 0;
        for (u32 block = 0; block < 4; block++) {
            for (u32 i = 0; i < floats; i++) {
                bool tag = false;
                for (u32 nr = 0; nr < 6; nr++) {
                    tag |= i == (channels == 2 ? 1 + 102 * nr : 2 + 409 * nr);
                    tag |= channels == 8 && i == nr * 51 * 8 + 1;
                }
                const u32 bits = ps_read32(b, c.addr + 4 * (block * floats + i));
                if (tag) tags += bits == 0x80000000;
                else zeros += bits == 0;
            }
        }
        const u32 per_block = channels == 2 ? 6 : 12;
        CHECK(tags == 4 * per_block);
        CHECK(zeros == 4 * (floats - per_block));
    };
    check_tagged(c2, 2);
    check_tagged(c8, 8);

    // A block the game adds at the read index is played whole, once: left and right 0.25 (an untouched block of the
    // 8-channel port adds nothing).
    REQUIRE(g.call("cellAudioPortStart", p2) == 0);
    REQUIRE(g.call("cellAudioPortStart", p8) == 0);
    for (u32 i = 0; i < 512; i++) ps_writef32(b, kSrc + 4 * i, 0.25f);
    const size_t before = speaker.count();
    g.ctx.fpr[1] = 1.0;
    REQUIRE(g.call("cellAudioAddData", p2, kSrc, 256) == 0);
    std::this_thread::sleep_for(60ms);
    {
        std::lock_guard lock(speaker.mutex);
        u32 whole = 0, other = 0;
        for (size_t i = before; i < speaker.blocks.size(); i++) {
            const auto& block = speaker.blocks[i];
            const bool silent = std::all_of(block.begin(), block.end(), [](f32 v) { return v == 0.0f; });
            const bool game = std::all_of(block.begin(), block.end(), [](f32 v) { return v == 0.25f; });
            whole += game;
            other += !silent && !game;
        }
        CHECK(whole == 1);
        CHECK(other == 0);
        // The periods keep the speaker's buffer at the target: it never held more than the starting silence and a
        // block (a period above the target lasts at least as long as the speaker takes to play a block).
        CHECK(speaker.most_queued <= 10 * 256);
    }

    // Detached: no more blocks once set_audio_sink returns (the thread goes back to the fixed clock).
    hle::set_audio_sink(*f.hle, {});
    const size_t detached = speaker.count();
    std::this_thread::sleep_for(20ms);
    CHECK(speaker.count() == detached);
    REQUIRE(g.call("cellAudioQuit") == 0);
}

TEST_CASE("cellAudio: the audio thread stops when the guest exits the process, and with the process", "[hle][audio]")
{
    std::atomic<u32> calls{0};
    {
        Fixture f;
        GuestThread g(f, 0, "audio");
        hle::set_audio_sink(*f.hle, [&](const f32*, u32) { calls++; });
        REQUIRE(g.call("cellAudioInit") == 0);
        std::this_thread::sleep_for(20ms);
        CHECK(calls.load() > 0);
        try {
            lv2::sys_process_exit3(*g.t, 0);
        } catch (const lv2::ThreadExit&) {
        }
        REQUIRE(f.kernel->exited());
        std::this_thread::sleep_for(15ms); // the thread notices at its next period
        const u32 n = calls.load();
        std::this_thread::sleep_for(30ms);
        CHECK(calls.load() == n);
    }

    // A process that ends with the thread running and a port playing: the HleProcess joins the thread.
    const auto t0 = std::chrono::steady_clock::now();
    {
        Fixture f;
        GuestThread g(f, 0, "audio");
        REQUIRE(g.call("cellAudioInit") == 0);
        const u32 port = open_ok(g, 8, 32);
        REQUIRE(g.call("cellAudioPortStart", port) == 0);
        std::this_thread::sleep_for(20ms);
    }
    CHECK(ms_since(t0) < 2000);
}

TEST_CASE("cellAudio: the Ex notify queues, flags, and acknowledgements", "[hle][audio]")
{
    Fixture f;
    GuestThread g(f, 0, "audio");
    u8* b = f.base();
    lv2::PpuThread& t = *g.t;
    constexpr u32 kDecimate2 = 0x08000000, kNoMix = 0x40000000;

    // CELL_AUDIO_CREATEEVENTFLAG_SPU makes an SPU queue; any other flag is a parameter error.
    CHECK(g.call("cellAudioCreateNotifyEventQueueEx", kQueueId, kKey, 2) == kParamError);
    REQUIRE(g.call("cellAudioCreateNotifyEventQueueEx", kQueueId, kKey, 1) == 0);
    const auto spu = f.kernel->event_queue(ps_read32(b, kQueueId));
    REQUIRE(spu);
    CHECK(spu->type == lv2::SYS_SPU_QUEUE);
    CHECK(ps_read64(b, kKey) == kFirstKey);
    REQUIRE(g.call("cellAudioCreateNotifyEventQueueEx", kQueueId, kKey, 0) == 0);
    const u32 q = ps_read32(b, kQueueId);
    const u64 key = ps_read64(b, kKey);
    CHECK(f.kernel->event_queue(q)->type == lv2::SYS_PPU_QUEUE);

    CHECK(g.call("cellAudioSendAck", kFirstSource) == kNotInit);
    REQUIRE(g.call("cellAudioInit") == 0);
    CHECK(g.call("cellAudioSetNotifyEventQueueEx", key, 1) == kParamError); // only the top 5 bits are flags
    REQUIRE(g.call("cellAudioSetNotifyEventQueueEx", key, kDecimate2) == 0);

    // DECIMATE_2: an event every second period.
    REQUIRE(lv2::sys_event_queue_receive(t, q, 0, 1'000'000) == 0);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10; i++) REQUIRE(lv2::sys_event_queue_receive(t, q, 0, 1'000'000) == 0);
    const double ms = ms_since(t0);
    INFO("10 events in " << ms << " ms");
    CHECK(ms > 15 * kPeriodMs); // about 20 periods
    CHECK(g.call("cellAudioSendAck", g.ctx.gpr[4]) == 0);
    CHECK(g.call("cellAudioSendAck", 0x1234) == 0); // no such source: accepted (RPCS3)

    // Removal needs the flags the queue was set with.
    CHECK(g.call("cellAudioRemoveNotifyEventQueue", key) == kTransEvent);
    CHECK(g.call("cellAudioRemoveNotifyEventQueueEx", key, kNoMix) == kTransEvent);
    CHECK(g.call("cellAudioRemoveNotifyEventQueueEx", key, 2) == kParamError);
    REQUIRE(g.call("cellAudioRemoveNotifyEventQueueEx", key, kDecimate2) == 0);

    // NOMIX: registered, but no events.
    REQUIRE(g.call("cellAudioSetNotifyEventQueueEx", key, kNoMix) == 0);
    REQUIRE(lv2::sys_event_queue_drain(t, q) == 0);
    std::this_thread::sleep_for(30ms);
    CHECK(queued_events(g, q) == 0);
    REQUIRE(g.call("cellAudioQuit") == 0);
}

TEST_CASE("cellAudio: block tags and timestamps", "[hle][audio]")
{
    Fixture f;
    GuestThread g(f, 0, "audio");
    u8* b = f.base();
    const u64 kTagNotFound = r3_of_code(0x8031070F);
    const u32 kTag = kData + 0x900, kStamp = kData + 0x908;

    CHECK(g.call("cellAudioGetPortBlockTag", 0, 0, kTag) == kNotInit);
    REQUIRE(g.call("cellAudioInit") == 0);
    CHECK(g.call("cellAudioGetPortBlockTag", 0, 0, kTag) == kNotOpen);
    CHECK(g.call("cellAudioGetPortTimestamp", 8, 0, kStamp) == kParamError);
    const u32 port = open_ok(g, 2, 8);
    const PortConfig c = port_config(g, port);

    // Opened, not started: block n plays n periods after the port's current block.
    REQUIRE(g.call("cellAudioGetPortBlockTag", port, 0, kTag) == 0);
    const u64 tag0 = ps_read64(b, kTag);
    REQUIRE(g.call("cellAudioGetPortBlockTag", port, 5, kTag) == 0);
    CHECK(ps_read64(b, kTag) == tag0 + 5);
    CHECK(g.call("cellAudioGetPortBlockTag", port, 8, kTag) == kParamError);
    CHECK(g.call("cellAudioGetPortBlockTag", port, 0, 0) == 0); // a null pointer: no error (RPCS3)

    // The current block's tag has a timestamp; a tag not reached yet has none; earlier tags are a period apart.
    REQUIRE(g.call("cellAudioGetPortTimestamp", port, tag0, kStamp) == 0);
    const u64 stamp0 = ps_read64(b, kStamp);
    CHECK(stamp0 <= lv2::guest_system_time_us());
    CHECK(g.call("cellAudioGetPortTimestamp", port, tag0 + 1, kStamp) == kTagNotFound);
    if (tag0 > 0) {
        REQUIRE(g.call("cellAudioGetPortTimestamp", port, tag0 - 1, kStamp) == 0);
        CHECK(ps_read64(b, kStamp) == stamp0 - 5333);
    }

    // Played: the tag of the block at the read index is the port's latest period, with a later time.
    REQUIRE(g.call("cellAudioPortStart", port) == 0);
    std::this_thread::sleep_for(30ms);
    REQUIRE(g.call("cellAudioPortStop", port) == 0);
    const u64 cur = ps_read64(b, c.read_index_addr);
    REQUIRE(g.call("cellAudioGetPortBlockTag", port, cur, kTag) == 0);
    const u64 tag = ps_read64(b, kTag);
    CHECK(tag > tag0);
    REQUIRE(g.call("cellAudioGetPortTimestamp", port, tag, kStamp) == 0);
    CHECK(ps_read64(b, kStamp) > stamp0);
    REQUIRE(g.call("cellAudioQuit") == 0);
}

TEST_CASE("cellAudio: cellAudioAdd2chData and cellAudioAdd6chData", "[hle][audio]")
{
    Fixture f;
    GuestThread g(f, 0, "audio");
    u8* b = f.base();
    REQUIRE(g.call("cellAudioInit") == 0);
    const u32 p2 = open_ok(g, 2, 2), p8 = open_ok(g, 8, 2);
    const PortConfig c2 = port_config(g, p2), c8 = port_config(g, p8);
    for (u32 i = 0; i < 256 * 6; i++) ps_writef32(b, kSrc + 4 * i, static_cast<f32>(i % 6 + 1)); // 1..6 per frame

    // Stereo samples (here 1, 2 per frame) into a stereo port, and into an 8-channel port's front left and right.
    g.ctx.fpr[1] = 2.0;
    for (u32 i = 0; i < 256 * 2; i++) ps_writef32(b, kSrc + 0x4000 + 4 * i, static_cast<f32>(i % 2 + 1));
    REQUIRE(g.call("cellAudioAdd2chData", p2, kSrc + 0x4000, 256) == 0);
    CHECK(ps_readf32(b, c2.addr + 0) == 2.0f);
    CHECK(ps_readf32(b, c2.addr + 4) == 4.0f);
    CHECK(ps_readf32(b, c2.addr + 8 * 255 + 4) == 4.0f);
    REQUIRE(g.call("cellAudioAdd2chData", p8, kSrc + 0x4000, 256) == 0);
    CHECK(ps_readf32(b, c8.addr + 0) == 2.0f);
    CHECK(ps_readf32(b, c8.addr + 4) == 4.0f);
    CHECK(ps_readf32(b, c8.addr + 8) == 0.0f); // centre untouched
    CHECK(g.call("cellAudioAdd2chData", p2, kSrc, 128) == kParamError);
    CHECK(g.call("cellAudioAdd2chData", p2, kSrc + 1, 256) == kParamError);
    CHECK(g.call("cellAudioAdd2chData", 8, kSrc, 256) == kParamError);

    // 5.1 samples (L, C, R, SL, SR, LFE = 1..6) into an 8-channel port's L, R, C, LFE, rear L, rear R.
    g.ctx.fpr[1] = 1.0;
    REQUIRE(g.call("cellAudioAdd6chData", p8, kSrc) == 0);
    const f32 expected[8] = {2.0f + 1.0f, 4.0f + 3.0f, 2.0f, 6.0f, 4.0f, 5.0f, 0.0f, 0.0f};
    for (u32 ch = 0; ch < 8; ch++) {
        INFO("channel " << ch);
        CHECK(ps_readf32(b, c8.addr + 32 * 0 + 4 * ch) == expected[ch]);
    }
    CHECK(ps_readf32(b, c8.addr + 32 * 255 + 4 * 3) == 6.0f);
    // A stereo port takes no 5.1 data: accepted, nothing written (RPCS3).
    const f32 before = ps_readf32(b, c2.addr);
    CHECK(g.call("cellAudioAdd6chData", p2, kSrc) == 0);
    CHECK(ps_readf32(b, c2.addr) == before);
    CHECK(g.call("cellAudioAdd6chData", p8, 0) == kParamError);
    REQUIRE(g.call("cellAudioPortClose", p8) == 0);
    CHECK(g.call("cellAudioAdd6chData", p8, kSrc) == kNotOpen);
    REQUIRE(g.call("cellAudioQuit") == 0);
}

TEST_CASE("cellAudio: cellAudioSetPortLevel moves the level over 13 ms; accessories and personal devices", "[hle][audio]")
{
    Fixture f;
    GuestThread g(f, 0, "audio");
    u8* b = f.base();
    std::mutex mutex;
    std::vector<f32> left; // the left channel of every block mixed
    hle::set_audio_sink(*f.hle, [&](const f32* samples, u32 frames) {
        std::lock_guard lock(mutex);
        for (u32 i = 0; i < frames; i++) left.push_back(samples[2 * i]);
    });

    REQUIRE(g.call("cellAudioInit") == 0);
    g.ctx.fpr[1] = 0.5;
    CHECK(g.call("cellAudioSetPortLevel", 0) == kNotOpen);
    const u32 port = open_ok(g, 2, 32);
    const PortConfig c = port_config(g, port);
    REQUIRE(g.call("cellAudioPortStart", port) == 0); // clears the ring: filled after
    for (u32 i = 0; i < 32 * 256 * 2; i++) ps_writef32(b, c.addr + 4 * i, 1.0f);
    std::this_thread::sleep_for(15ms);
    g.ctx.fpr[1] = 0.0;
    REQUIRE(g.call("cellAudioSetPortLevel", port) == 0);
    std::this_thread::sleep_for(40ms); // well within the 32 filled blocks (170 ms)
    REQUIRE(g.call("cellAudioPortStop", port) == 0);
    {
        std::lock_guard lock(mutex);
        // Full volume, then down to silence through values between, never back up.
        const auto first_full = std::find(left.begin(), left.end(), 1.0f); // silence until the port starts
        REQUIRE(first_full != left.end());
        const auto first_lower = std::find_if(first_full, left.end(), [](f32 v) { return v < 1.0f; });
        REQUIRE(first_lower != left.end());
        const auto first_zero = std::find(first_lower, left.end(), 0.0f);
        REQUIRE(first_zero != left.end());
        const auto steps = first_zero - first_lower;
        INFO(steps << " samples to silence");
        CHECK(steps >= 600);
        CHECK(steps <= 630);
        CHECK(std::is_sorted(first_lower, first_zero, std::greater<f32>()));
    }
    hle::set_audio_sink(*f.hle, {});
    g.ctx.fpr[1] = -1.0;
    CHECK(g.call("cellAudioSetPortLevel", port) == 0); // a negative level: ignored

    g.ctx.fpr[1] = 0.5;
    CHECK(g.call("cellAudioMiscSetAccessoryVolume", 0) == 0);
    CHECK(g.call("cellAudioSetPersonalDevice", 0, 4) == 0);
    CHECK(g.call("cellAudioSetPersonalDevice", 3, 0x8000) == 0); // the primary device
    CHECK(g.call("cellAudioSetPersonalDevice", 4, 0) == kParamError);
    CHECK(g.call("cellAudioSetPersonalDevice", 0, 5) == kParamError);
    CHECK(g.call("cellAudioUnsetPersonalDevice", 3) == 0);
    CHECK(g.call("cellAudioUnsetPersonalDevice", static_cast<u64>(-1)) == kParamError);
    REQUIRE(g.call("cellAudioQuit") == 0);
    CHECK(g.call("cellAudioMiscSetAccessoryVolume", 0) == kNotInit);
    CHECK(g.call("cellAudioUnsetPersonalDevice", 0) == kNotInit);
}
