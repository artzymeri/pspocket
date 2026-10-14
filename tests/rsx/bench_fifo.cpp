// FIFO reading speed (hidden: `test_rsx "[.bench]"`, not run by ctest). Command buffers shaped like a busy game's
// (Assassin's Creed's training: a vertex program and its constants uploaded for most draws, a few dozen registers
// set around each), read by the front end with a backend that only records; prints nanoseconds a word. For
// before/after numbers when the FIFO reader changes (CLAUDE.md rule 8).

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <time.h>
#include <vector>

#include "rsx_test_util.hpp"

using namespace pspocket;
using namespace pspocket::rsx;
using namespace pspocket::rsx::test;

namespace {

// Fills the command buffer with draws; returns the end offset and the number of draws.
std::pair<u32, u32> fill(Fixture& f)
{
    auto b = f.at(0);
    u32 draws = 0;
    std::vector<u32> words(32);
    while (b.offset() < Fixture::kCmdSize - 0x1000) {
        // A vertex program of 20 instructions, loaded at 0, in packets of 32 words (as cellGcm splits them).
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_LOAD, {0, 0});
        for (u32 p = 0; p < 80; p += 32) {
            const u32 n = std::min<u32>(32, 80 - p);
            for (u32 i = 0; i < n; i++) words[i] = 0x401f9c6c + (p + i) * 0x10001;
            b.method(m::NV4097_SET_TRANSFORM_PROGRAM, std::span<const u32>(words.data(), n));
        }
        b.method(m::NV4097_SET_TRANSFORM_PROGRAM_START, 0);
        // 16 constants (a few matrices) in packets of 32 words.
        b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, draws % 64);
        for (u32 p = 0; p < 64; p += 32) {
            for (u32 i = 0; i < 32; i++) words[i] = 0x3f800000 + (draws + p + i) * 0x100;
            b.method(m::NV4097_SET_TRANSFORM_CONSTANT, std::span<const u32>(words.data(), 32));
        }
        // Registers set one or a few at a time.
        b.method(m::NV4097_SET_BLEND_ENABLE, draws & 1);
        b.method(m::NV4097_SET_DEPTH_FUNC, {0x203, 1, 1});
        b.method(m::NV4097_SET_ALPHA_REF, 0x80);
        b.method(m::NV4097_SET_CULL_FACE_ENABLE, 1);
        for (u32 t = 0; t < 4; t++) {
            b.method(m::NV4097_SET_TEXTURE_OFFSET + t * 8, {0x100000u * t, 0x0001a901, 0x00030303, 0x80000000});
            b.method(m::NV4097_SET_TEXTURE_FILTER + t * 8, {0x02022000, 0x01000100});
        }
        for (u32 a = 0; a < 4; a++) b.method(m::NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + a, 0x1000u * a);
        b.method(m::NV4097_SET_SHADER_PROGRAM, 0x00010001);
        b.method(m::NV4097_SET_BEGIN_END, 5);
        b.method(m::NV4097_DRAW_ARRAYS, 0x02000000);
        b.method(m::NV4097_SET_BEGIN_END, 0);
        draws++;
    }
    return {b.offset(), draws};
}

// Reads the buffer kRounds times; with `guest`, another thread polls the get register and writes put again
// meanwhile, as a game's PPU thread does while it waits for room in its command buffer.
void run(bool guest)
{
    Fixture f;
    const auto [end, draws] = fill(f);
    std::atomic<bool> stop{false};
    std::thread poller;
    if (guest) {
        poller = std::thread([&] {
            auto* get = reinterpret_cast<volatile u32*>(f.base() + f.layout().get_addr());
            auto* put = reinterpret_cast<volatile u32*>(f.base() + f.layout().put_addr());
            u32 sink = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                sink += *get;
                *put = *put; // the same value: the line still changes hands
            }
            (void)sink;
        });
    }
    constexpr u32 kRounds = 40;
    std::chrono::duration<double> spent{};
    u64 words_read = 0, cpu_ns = 0;
    for (u32 r = 0; r < kRounds; r++) {
        f.backend.events.clear();
        f.rsx->set_fifo_pointers(0, 0);
        f.set_put(end);
        const auto start = std::chrono::steady_clock::now();
        const u64 cpu_start = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
        REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Idle);
        cpu_ns += clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
        spent += std::chrono::steady_clock::now() - start;
        words_read += end / 4;
    }
    stop = true;
    if (poller.joinable()) poller.join();
    REQUIRE(f.backend.of(RecordingBackend::Kind::Draw).size() == draws);
    REQUIRE(f.failures.empty());
    const double ns = spent.count() * 1e9 / static_cast<double>(words_read);
    // The thread's CPU time too: on a Mac busy with other work the wall time counts the time the thread waited.
    const double cpu = static_cast<double>(cpu_ns) / static_cast<double>(words_read);
    std::printf("FIFO%s: %.2f ns a word, %.2f ns of CPU (%u draws of %u words, %u rounds)\n",
        guest ? " with a polling guest" : "", ns, cpu, draws, end / 4 / draws, kRounds);
}

} // namespace

TEST_CASE("bench: FIFO reading, draws with their vertex programs and constants", "[.bench]")
{
    run(false);
    run(true);
}
