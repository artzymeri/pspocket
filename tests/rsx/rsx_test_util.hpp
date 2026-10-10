#pragma once

// Shared fixture for the RSX tests: a reserved guest address space with a command buffer in IO-mapped main memory,
// RSX local memory, the RSX context regions, and a backend that records what the front end asks of it.
// Everything is synthetic: command buffers are built in code with CommandBuilder.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pspocket/core/guest_memory.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/core/memory_map.h"
#include "pspocket/rsx/backend.hpp"
#include "pspocket/rsx/command_builder.hpp"
#include "pspocket/rsx/context.hpp"
#include "pspocket/rsx/gcm.hpp"
#include "pspocket/rsx/rsx.hpp"

namespace pspocket::rsx::test {

struct RecordingBackend : Backend {
    enum class Kind { Draw, Clear, Flip, Sync, Invalidate, Flush };
    struct Event {
        Kind kind;
        u32 a = 0; // clear mask, flip buffer id, address
        u32 b = 0; // size
        DrawClause draw{};
        std::optional<DisplayBuffer> display{};
    };

    Rsx* rsx = nullptr;
    std::vector<Event> events;

    void attach(Rsx& r) override { rsx = &r; }
    void draw(const RsxState& s) override { events.push_back({Kind::Draw, 0, 0, s.draw, {}}); }
    void clear(u32 mask, const RsxState&) override
    {
        events.push_back({Kind::Clear, mask, 0, {}, {}});
        if (on_clear) on_clear(mask);
    }
    std::function<void(u32 mask)> on_clear; // what a test does at a clear, in the middle of the FIFO
    void flip(const FlipRequest& r, const RsxState&) override
    {
        events.push_back({Kind::Flip, r.buffer_id, static_cast<u32>(r.flip_index), {}, r.buffer});
    }
    void sync() override { events.push_back({Kind::Sync}); }
    void invalidate_memory(u32 addr, u32 size) override { events.push_back({Kind::Invalidate, addr, size, {}, {}}); }
    bool flush_memory(u32 addr, u32 size, const char* = "") override
    {
        events.push_back({Kind::Flush, addr, size, {}, {}});
        return false;
    }

    std::vector<u32> clears() const
    {
        std::vector<u32> out;
        for (const auto& e : events) {
            if (e.kind == Kind::Clear) out.push_back(e.a);
        }
        return out;
    }
    std::vector<const Event*> of(Kind k) const
    {
        std::vector<const Event*> out;
        for (const auto& e : events) {
            if (e.kind == k) out.push_back(&e);
        }
        return out;
    }
};

inline Config test_config()
{
    Config c;
    c.unhandled_policy = FailPolicy::Log; // tests inspect failures instead of aborting
    c.unsupported_policy = FailPolicy::Log;
    return c;
}

struct Fixture {
    static constexpr u32 kCmdEa = 0x00200000;   // command buffer EA (main memory)
    static constexpr u32 kCmdSize = 0x00100000; // IO mapped at offset 0
    static constexpr u32 kMainDataEa = 0x00400000; // more main memory, IO mapped at 0x00100000
    static constexpr u32 kMainDataIo = 0x00100000;
    static constexpr u32 kLocalSize = 0x01000000; // committed local memory for tests (16 MB)

    GuestMemory mem;
    RecordingBackend backend;
    Config config;
    std::unique_ptr<Rsx> rsx;
    std::vector<Failure> failures;

    explicit Fixture(Config cfg = test_config()) : config(cfg)
    {
        PS_CHECK(mem.reserve());
        PS_CHECK(mem.commit(kCmdEa, kCmdSize));
        PS_CHECK(mem.commit(kMainDataEa, 0x00100000));
        PS_CHECK(mem.commit(PS_MEM_RSX_LOCAL_BASE, kLocalSize));
        PS_CHECK(initialize_context_memory(mem, config.layout));
        rsx = std::make_unique<Rsx>(mem, backend, config);
        rsx->on_failure = [this](const Failure& f) { failures.push_back(f); };
        PS_CHECK(rsx->io_map().map(0, kCmdEa, kCmdSize));
        PS_CHECK(rsx->io_map().map(kMainDataIo, kMainDataEa, 0x00100000));
        rsx->set_fifo_pointers(0, 0);
    }

    u8* base() const { return mem.base(); }
    const ContextLayout& layout() const { return config.layout; }

    // A builder writing at FIFO offset `offset` of the command buffer.
    CommandBuilder at(u32 offset) { return CommandBuilder(mem.base(), kCmdEa + offset, offset); }

    // The test stands in for the PPU, whose store after a sync (lwsync, then stw) is a release store and whose load
    // before one an acquire load: a running RSX thread reads put and the labels the test writes, and the test polls the
    // labels the RSX releases, ordered as the guest's are (D-585; the RSX's side is D-077 and D-587).
    void set_put(u32 offset) { write32(layout().put_addr(), offset); }
    u32 get_register() const { return read32(layout().get_addr()); }
    u32 read32(u32 ea) const
    {
        return ps_bswap32(__atomic_load_n(reinterpret_cast<const u32*>(mem.base() + ea), __ATOMIC_ACQUIRE));
    }
    void write32(u32 ea, u32 v) { __atomic_store_n(reinterpret_cast<u32*>(mem.base() + ea), ps_bswap32(v), __ATOMIC_RELEASE); }

    // Sets put to the builder's current offset and runs the FIFO on this thread.
    Rsx::RunStatus submit(const CommandBuilder& b)
    {
        set_put(b.offset());
        return rsx->run_until_idle();
    }

    const RsxState& state() const { return rsx->state(); }
};

} // namespace pspocket::rsx::test
