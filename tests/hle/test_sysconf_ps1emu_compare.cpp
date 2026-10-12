// cellSysconfPs1emu against Sony's libsysutil_sysconf_ps1emu (D-918): the same calls to pspocket's library and to
// Sony's module (the user's installed system software; skipped without it, compare_util.hpp), each half in a process of
// its own, both over pspocket's libsysutil (the module is a client of libsysutil's, which pspocket always serves):
// - every export's argument checks and its answer on pspocket's console, whose channels to the system software are
//   closed (D-534): the send's 0x8002b007;
// - then channel 0 opened with a ring and a system side of the test's (another thread) that reads each request and
//   answers it with a reply document: the request's bytes as the system side reads them (the document each half
//   builds), the answer and what the export writes for every kind of reply (results below and above zero, each
//   battery status with levels in and out of its range, power words, missing attributes, a reply too large for the
//   module's buffer);
// - the lv2 calls each call makes, in order.
// No game data: the replies are built in the test.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "compare_util.hpp"
#include "sysutil_cxml.hpp"

using namespace pspocket;
using namespace pspocket::hle::test;
using namespace pspocket::hle::test::compare;
namespace su = pspocket::hle::sysutil;

namespace {

constexpr u32 kOut = kData + 0x100;        // the exports' outputs (poisoned before each call)
constexpr u32 kReply = kData + 0x1000;     // the reply document's bytes (up to 0x8000)
constexpr u32 kRequest = kData + 0xA000;   // what the system side reads (up to 0x2000)

constexpr u32 kNidDiscon = 0x639ABBDE, kNidSwap = 0x6A12D11F, kNidBattery = 0x83E79A23, kNidPower = 0xEFDDAF6C;

// The lv2 calls of the thread making a call (numbers: the objects' IDs are each process's own).
struct Trace {
    std::mutex m;
    std::thread::id caller;
    std::vector<u32> calls;
};
void observe(void* user, u32 n, const u64*)
{
    Trace& tr = *static_cast<Trace*>(user);
    std::lock_guard lock(tr.m);
    if (std::this_thread::get_id() == tr.caller) tr.calls.push_back(n);
}

// Document::Write's writer into kReply.
class ReplyWriter final : public su::DocWriter {
public:
    explicit ReplyWriter(u8* b) : b_(b) {}
    s32 write(PPUContext&, u32 data, u32 size) override
    {
        std::memmove(b_ + kReply + size_, b_ + data, size);
        size_ += size;
        return static_cast<s32>(size);
    }
    u32 size() const { return size_; }

private:
    u8* b_;
    u32 size_ = 0;
};

struct Int {
    const char* name;
    s32 value;
};

struct H : Half<> {
    Trace tr;
    u32 tls = 0;

    H(const Setup& s, bool sony) : Half<>(s, sony, "cellSysconfPs1emu")
    {
        tls = f.hle->alloc_tls();
        REQUIRE(tls);
        tr.caller = std::this_thread::get_id();
        f.kernel->set_syscall_observer(&observe, &tr);
    }
    ~H() { f.kernel->set_syscall_observer(nullptr, nullptr); }

    // A reply document {result, ...} in kReply, followed by bytes of the test's up to `padded` bytes: the packet's
    // size.
    u32 reply(std::initializer_list<Int> ints, u32 padded = 0)
    {
        PPUContext& ctx = t.ctx;
        const u32 memory = su::fixed_memory_begin(ctx, 1);
        REQUIRE(memory);
        hle::StackVar frame(ctx, b, 0x80);
        const u32 doc = frame.addr();
        su::document_construct(ctx, doc);
        su::document_use_fixed_memory(ctx, doc, memory);
        su::document_set_magic(ctx, doc, "SYSC");
        su::Element e;
        REQUIRE(su::document_create_element(ctx, doc, "reply", static_cast<s32>(ints.size()), e) == 0);
        for (const Int& i : ints) REQUIRE(su::util_set_int(ctx, e, i.name, i.value) == 0);
        ReplyWriter w(b);
        REQUIRE(su::document_write(ctx, doc, w) == 0);
        su::document_clear(ctx, doc);
        su::fixed_memory_end(ctx, 1);
        u32 size = w.size();
        for (; size < padded; size++) b[kReply + size] = static_cast<u8>(size * 13 + 5);
        return size;
    }

    // Calls the export `nid`; the answer, the outputs and the lv2 calls as lines keyed by `key`.
    void call(const std::string& key, u32 nid, std::initializer_list<u64> args, bool leave_out_waits = false)
    {
        std::memset(b + kOut, 0xEE, 8);
        {
            std::lock_guard lock(tr.m);
            tr.calls.clear();
        }
        trace("calling: " + key);
        const std::optional<u64> v = side.call_nid(nid, args);
        o.line(key, v ? answer(*v) : std::string("(not exported)"));
        o.line(key + ": the outputs", hex(b + kOut, 8));
        std::string calls;
        {
            std::lock_guard lock(tr.m);
            for (const u32 n : tr.calls)
                if (!(leave_out_waits && n == 107)) calls += std::format(" {}", n);
        }
        o.line(key + ": lv2 calls", calls);
    }

    // The same with the system software answering, as another process would: once the request's packet is complete
    // in channel 0's ring (its state 2, its direction to the system), a thread of the test takes it out of the ring and
    // puts the reply (kReply, `size` bytes) in its place, under the channel's mutex, waking the request. The
    // request's bytes are compared; a request waiting for the reply waits or not depending on when the reply comes,
    // so the waits (sys_cond_wait) are left out of its lv2 calls.
    void answered(const std::string& key, u32 nid, std::initializer_list<u64> args, u32 size)
    {
        std::memset(b + kRequest, 0, 0x2000);
        const u32 g = su::globals(t.ctx);
        const u32 shm = ps_read32(b, g + su::glob::shm);
        auto read = std::make_shared<std::atomic<s64>>(-1);
        std::thread sys([this, size, read, shm, g] {
            GuestThread th(f, 1, "system");
            th.ctx.gpr[13] = tls + hle::kTlsR13Offset;
            u8* base = f.base();
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!(load32_acquire(base, shm + su::ring::state) == 2 &&
                       load32_acquire(base, shm + su::ring::direction) == 1) &&
                std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            const u32 mutex = ps_read32(base, g + su::glob::mutex(0)), cond = ps_read32(base, g + su::glob::cond_data(0));
            THREAD_CHECK(hle::lv2_syscall(th.ctx, 102, {mutex, 0}) == 0);
            const u32 ring = shm + ps_read32(base, shm + su::ring::data), ring_size = ps_read32(base, shm + su::ring::size);
            u32 rd = ps_read32(base, shm + su::ring::read), used = ps_read32(base, shm + su::ring::used);
            const u32 n = std::min<u32>(used, 0x2000);
            for (u32 i = 0; i < n; i++) base[kRequest + i] = base[ring + (rd + i) % ring_size];
            read->store(used);
            rd = (rd + used) % ring_size;
            ps_write32(base, shm + su::ring::read, rd);
            u32 wr = rd; // the ring empty: the reply from where the request ended
            for (u32 i = 0; i < size; i++) base[ring + (wr + i) % ring_size] = base[kReply + i];
            wr = (wr + size) % ring_size;
            ps_write32(base, shm + su::ring::write, wr);
            ps_write32(base, shm + su::ring::used, size);
            ps_write32(base, shm + su::ring::state, 2);
            ps_write32(base, shm + su::ring::direction, 2);
            THREAD_CHECK(hle::lv2_syscall(th.ctx, 108, {cond}) == 0);
            THREAD_CHECK(hle::lv2_syscall(th.ctx, 104, {mutex}) == 0);
        });
        call(key, nid, args, /*leave_out_waits=*/true);
        sys.join();
        const s64 n = read->load();
        o.line(key + ": the system read", std::format("{:#x}", static_cast<u32>(n)));
        if (n > 0 && n <= 0x2000) o.bytes(key + ": the request", b + kRequest, static_cast<usize>(n));
        // What the request left in the ring (a reply larger than its buffer), taken out as the system would.
        ps_write32(b, shm + su::ring::read, ps_read32(b, shm + su::ring::write));
        ps_write32(b, shm + su::ring::used, 0);
        ps_write32(b, shm + su::ring::state, 0);
        ps_write32(b, shm + su::ring::direction, 0);
    }
};

Observed half(const Setup& s, bool sony)
{
    H h(s, sony);
    u8* b = h.b;
    // pspocket's console: the channels closed.
    for (const u32 port : {0u, 3u, 6u, 7u, 0xFFFFFFFFu}) {
        h.call(std::format("getBattery({})", port), kNidBattery, {port, kOut});
        h.call(std::format("getPower({})", port), kNidPower, {port, kOut});
        h.call(std::format("disconGamePad({})", port), kNidDiscon, {port});
    }
    h.call("getBattery without an output", kNidBattery, {1, 0});
    h.call("getPower without an output", kNidPower, {1, 0});
    h.call("getBattery into memory that is not there (not reached)", kNidBattery, {1, 0xDEAD0000u});
    h.call("getBattery with a port in the high word", kNidBattery, {0x100000002ull, kOut});
    for (const auto& [p1, p2] : std::vector<std::pair<u64, u64>>{{0, 1}, {1, 1}, {6, 7}, {7, 7}, {0xFFFFFFFF, 3},
             {0x1FFFFFFFFull, 0xFFFFFFFF}, {0x100000004ull, 4}})
        h.call(std::format("swapGamePad({:#x}, {:#x})", p1, p2), kNidSwap, {p1, p2});

    // Channel 0 opened, a ring laid out, the test answering as the system software.
    const u32 shm = ps_read32(b, su::globals(h.t.ctx) + su::glob::shm);
    REQUIRE(shm);
    std::memset(b + shm, 0, su::ring::kChannel); // channel 0's header and ring
    ps_write32(b, shm + su::ring::data, 0x400);
    ps_write32(b, shm + su::ring::size, 0x7C00);
    struct Battery {
        s32 result, status, level;
    };
    for (const Battery& c : std::vector<Battery>{{0, 0, 1}, {0, 0, 2}, {0, 0, 3}, {0, 0, 0}, {0, 1, 1}, {0, 1, 5},
             {0, 1, 6}, {0, 1, 0}, {0, 2, 0}, {0, 2, 200}, {0, 3, 9}, {0, 4, 1}, {0, 255, 1}, {0, 0x101, 1},
             {0, 1, 0x105}, {7, 1, 3}, {-5, 1, 3}, {static_cast<s32>(0x80020001), 0, 1}})
        h.answered(std::format("getBattery: result {}, status {}, level {}", c.result, c.status, c.level), kNidBattery,
            {2, kOut}, h.reply({{"result", c.result}, {"status", c.status}, {"level", c.level}}));
    h.answered("getBattery: no result", kNidBattery, {2, kOut}, h.reply({{"status", 1}, {"level", 1}}));
    h.answered("getBattery: no status", kNidBattery, {2, kOut}, h.reply({{"result", 0}, {"level", 1}}));
    h.answered("getBattery: no level", kNidBattery, {2, kOut}, h.reply({{"result", 0}, {"status", 2}}));
    h.answered("getBattery: a reply larger than the module's buffer", kNidBattery, {2, kOut},
        h.reply({{"result", 0}, {"status", 1}, {"level", 1}}, 0x6100));
    h.answered("getBattery: a reply just within the buffer", kNidBattery, {5, kOut},
        h.reply({{"result", 0}, {"status", 1}, {"level", 2}}, 0x5000));
    h.answered("getBattery: a reply of the buffer's size", kNidBattery, {5, kOut},
        h.reply({{"result", 0}, {"status", 3}, {"level", 2}}, 0x6000));
    h.answered("getBattery: a reply a byte larger than the buffer", kNidBattery, {5, kOut},
        h.reply({{"result", 0}, {"status", 3}, {"level", 2}}, 0x6001));
    h.answered("getPower: a reply larger than the buffer", kNidPower, {3, kOut},
        h.reply({{"result", 0}, {"status", 3}, {"reserved", 2}}, 0x7000));
    h.answered("disconGamePad: a reply larger than the buffer", kNidDiscon, {3},
        h.reply({{"result", -1}}, 0x6800));
    for (const Battery& c : std::vector<Battery>{{0, 0x1234, 0xABCD}, {0, -1, 0x7FFFFFFF}, {3, 1, 2}, {-9, 1, 2}})
        h.answered(std::format("getPower: result {}, status {:#x}, reserved {:#x}", c.result, c.status, c.level),
            kNidPower, {4, kOut}, h.reply({{"result", c.result}, {"status", c.status}, {"reserved", c.level}}));
    h.answered("getPower: no reserved", kNidPower, {4, kOut}, h.reply({{"result", 0}, {"status", 6}}));
    h.answered("getPower: no status", kNidPower, {4, kOut}, h.reply({{"result", 0}, {"reserved", 6}}));
    for (const s32 result : {0, 9, -3, static_cast<s32>(0x8002B101)}) {
        h.answered(std::format("disconGamePad: result {}", result), kNidDiscon, {6}, h.reply({{"result", result}}));
        h.answered(std::format("swapGamePad: result {}", result), kNidSwap, {static_cast<u64>(-2), 3},
            h.reply({{"result", result}}));
    }
    h.answered("disconGamePad: no result", kNidDiscon, {1}, h.reply({{"status", 0}}));
    h.answered("swapGamePad: no result", kNidSwap, {2, 5}, h.reply({{"level", 0}}));
    return std::move(h.o);
}

} // namespace

TEST_CASE("cellSysconfPs1emu's four exports are pspocket's own", "[hle][sysconf]")
{
    for (const u32 nid : {kNidDiscon, kNidSwap, kNidBattery, kNidPower}) {
        const hle::Function* f = hle::registry().find("cellSysconfPs1emu", nid);
        REQUIRE(f != nullptr);
        CHECK(f->name == std::format("cellSysconfPs1emu_{:08X}", nid));
        CHECK(recomp::NidDb::builtin().find("cellSysconfPs1emu", nid) == f->name);
    }
    CHECK_FALSE(hle::registry().is_firmware_first("cellSysconfPs1emu"));
}

TEST_CASE("cellSysconfPs1emu against Sony's libsysutil_sysconf_ps1emu: checks, closed channels, every kind of reply",
    "[hle][lle][compare][sysconf]")
{
    const Setup s = setup();
    require_firmware_module(s, "cellSysconfPs1emu");
    run(s, "cellSysconfPs1emu against Sony's module", [&](bool sony) { return half(s, sony); }, [](const Observed& o) {
        CHECK(o.value("getBattery(7)") == "0x8002bb01");
        CHECK(o.value("getBattery(0)") == "0x8002b007");
        CHECK(o.value("swapGamePad(0x1, 0x1)") == "0x8002bb01");
        CHECK(o.value("getBattery: result 0, status 1, level 5") == "0x0");
        CHECK(o.value("getBattery: result 0, status 1, level 5: the outputs") == "hex:0105eeeeeeeeeeee");
        CHECK(o.value("getBattery: result 0, status 1, level 6") == "0x8002b102");
        CHECK(o.value("getBattery: result -5, status 1, level 3") == "0xfffffffb");
        CHECK(o.value("getPower: result 0, status 0x1234, reserved 0xabcd: the outputs") == "hex:1234abcdeeeeeeee");
        CHECK(o.value("getBattery: a reply larger than the module's buffer") == "0x8002b006");
    });
}
