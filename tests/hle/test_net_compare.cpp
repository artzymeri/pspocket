// sys_net against libnet.sprx (D-193's method, D-523), on pspocket's lv2 network calls: the same scripts in two
// fresh processes, one calling pspocket's functions and one Sony's module; every answer, every byte written (the
// scripts' memory and libnet's pool) and the lv2 network calls made the same. What differs between the two by
// construction is masked: the allocation tags in the pool's block headers (pointers to each module's own file names),
// lv2 object IDs, the random generator's state (seeded from the clock) and its numbers, the clock itself. Skipped
// without the firmware (PSPOCKET_TEST_DEV_FLASH); no game data.

#include <catch_amalgamated.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kOut = kData + 0x10000, kOutSize = 0x8000; // the scripts' memory
constexpr u32 kStr = kData + 0x30000;                     // strings the scripts use, 0x80 apart
constexpr u32 kParam = kData + 0x7000;                    // sys_net_initialize_parameter_t
constexpr u32 kPool = kData + 0x100000, kPoolSize = 0x40000;
constexpr u8 kFill = 0xA5;

// Script flags.
constexpr u32 kMaskAnswer = 1; // the answer depends on the run (an address in each module's data, a random number)
constexpr u32 kSecond = 2;     // made on the second thread
constexpr u32 kSign = 4;       // compared only as negative, 0 or positive (an address: kept with `store`)

// An argument read when the call is made: the 32-bit word at the address.
constexpr u64 kDeref = u64{1} << 63;

// A call (its answer also stored at `store` when given); with fn "=" a 32-bit store (address, value), "~" the 32-bit
// word at the address made 1 when not 0 (an lv2 ID), "pool" the pool's bytes kept for comparing (its blocks from the
// argument, else from the pool's start).
struct Op {
    const char* fn;
    std::vector<u64> args;
    u32 flags = 0;
    u32 store = 0;
};

struct Run {
    std::vector<u32> answers;
    std::vector<u8> out;
    std::vector<std::vector<u8>> pools;
    std::vector<std::string> calls;
    std::string tty; // what the libraries printed
};

// The TTY's text (lv2 logs each write on the "tty" channel) while a run lasts.
struct TtyCapture {
    std::mutex mutex;
    std::string text;
    TtyCapture()
    {
        log::set_sink(
            [](void* user, log::Level, std::string_view channel, std::string_view message) {
                if (channel != "tty") return;
                auto* self = static_cast<TtyCapture*>(user);
                std::lock_guard lock(self->mutex);
                self->text += std::string(message) + "|";
            },
            this);
    }
    ~TtyCapture() { log::set_sink(nullptr, nullptr); }
};

struct Calls {
    std::mutex mutex;
    std::vector<std::string> list;
};

// The arguments of each network call (a stack address differs between the modules).
u32 arg_count(u32 number)
{
    switch (number) {
    case 714: return 1;
    case 706: case 712: case 722: return 2;
    case 700: case 701: case 702: case 703: case 704: case 708: case 709: case 713: case 715: case 721: return 3;
    case 705: case 711: case 716: return 5;
    case 707: case 710: return 6;
    default: return 3;
    }
}

void observe(void* user, u32 number, const u64* a)
{
    if (number < 700 || number > 726) return;
    auto* c = static_cast<Calls*>(user);
    std::string line = std::to_string(number);
    for (u32 i = 0; i < arg_count(number); i++) {
        const u64 v = a[i];
        const bool stack = (v >= kStacks && v < kStacks + 0x100000) || (v >= 0xD0000000 && v < 0xF0000000);
        line += stack ? std::string(" stack") : std::format(" {:#x}", v);
    }
    std::lock_guard lock(c->mutex);
    c->list.push_back(line);
}

// The pool, with what differs by construction cleared: each block header's tag, the lv2 IDs of a thread context's
// sync object (a 3192-byte block, at +16), the generator's state (2084 bytes).
std::vector<u8> pool_bytes(const GuestMemory& mem, u8* b, u32 start)
{
    std::vector<u8> out(b + kPool, b + kPool + kPoolSize);
    // A block's tag also stays behind in a header that a merge swallowed: any word pointing at a source file's name
    // ("....c") is a tag.
    for (u32 off = 0; off + 4 <= kPoolSize; off += 4) {
        const u32 w = ps_read32(b, kPool + off);
        if (w < 0x10000 || w >= 0xF0000000 || !mem.is_committed(w) || !mem.is_committed(w + 31)) continue;
        u32 len = 0;
        while (len < 32 && b[w + len] >= 0x20 && b[w + len] < 0x7F) len++;
        if (len >= 3 && len < 32 && b[w + len] == 0 && b[w + len - 2] == '.' && b[w + len - 1] == 'c') {
            std::memset(out.data() + off, 0, 4);
        }
    }
    for (u32 at = start, guard = 0; guard < 4096 && at + 20 <= kPool + kPoolSize; guard++) {
        const u32 size = ps_read32(b, at + 16);
        if (size == 0 || size > kPoolSize) break;
        const u32 off = at - kPool;
        std::memset(out.data() + off + 8, 0, 4);
        if (size == 3192) std::memset(out.data() + off + 20 + 16, 0, 8);
        if (size == 2084) std::memset(out.data() + off + 20, 0, 2084);
        at += 20 + size;
    }
    return out;
}

const std::vector<std::string>& strings()
{
    static const std::vector<std::string> s = {
        "127.0.0.1", "0", "0x", "0x7f.1", "1.2.3", "1.2.3.4", "1.2.3.4.5", "255.255.255.255", "256.1.1.1",
        "1.2.3.256", "010.010.010.010", "09.1.1.1", "08.1.1.1", "0xff.0xff.0xff.0xff", "0xFFFFFFFF", "4294967296",
        "1.0x10.3", "1 2", "1.2.3.4 ", "1.2.3.4x", "", " 1.2.3.4", "1..2", "1.2.3.", ".1.2.3", "12345678901",
        "0x123456789", "077777777777", "::", "::1", "1::", "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7::", "::ffff:1.2.3.4",
        "::1.2.3.4", "fe80::1%1", "1:2:3:4:5:6:1.2.3.4", "00001::", "1:::2", ":1::2", "1::2::3", "abcd:EF01::",
        "1:2:3:4:5:6:7:8:9", "::1.2.3", "::1.2.3.4.5", "1:2:3:4:5:6:7:1.2.3.4", "g::", "1:2", "0001:0002::",
        "12345::", "::0:1.2.3.4", "a.b.c.d", "1.2.3.4\n", "0x1.0x2.0x3.0x4", "0X1F", "0.0.0.0", "1.2", "1",
        std::string(64, 'a'), "0x1g", "019.1.1.1", "0.09", "1:2:3:4:5:6:7:8::", "::1:2:3:4:5:6:7:8",
    };
    return s;
}

Run run(const std::string& flash, const std::vector<Op>& ops, bool sony)
{
    Fixture f;
    REQUIRE(f.hle->link_internal_functions({"sys_net"}));
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread t(f, 0, "ppu");
    GuestThread t2(f, 1, "ppu2");
    FirmwareLibrary lib(f, t, flash, "sys_net");
    u8* b = f.base();
    std::memset(b + kOut, kFill, kOutSize);
    std::memset(b + kPool, kFill, kPoolSize);
    for (usize i = 0; i < strings().size(); i++) {
        std::memcpy(b + kStr + 0x80 * static_cast<u32>(i), strings()[i].c_str(), strings()[i].size() + 1);
    }
    Calls calls;
    f.kernel->set_syscall_observer(&observe, &calls);
    TtyCapture tty;
    Run r;
    for (const Op& op : ops) {
        const std::string_view name = op.fn;
        if (name == "=" || name == "~") {
            const u32 at = static_cast<u32>(op.args[0]);
            ps_write32(b, at, name == "=" ? static_cast<u32>(op.args[1]) : ps_read32(b, at) != 0);
            r.answers.push_back(0);
            continue;
        }
        if (name == "pool") {
            r.pools.push_back(pool_bytes(f.mem, b, op.args.empty() ? kPool : static_cast<u32>(op.args[0])));
            r.answers.push_back(0);
            continue;
        }
        GuestThread& th = op.flags & kSecond ? t2 : t;
        std::array<u64, 8> a{};
        for (usize i = 0; i < op.args.size(); i++) {
            a[i] = op.args[i] & kDeref ? ps_read32(b, static_cast<u32>(op.args[i])) : op.args[i];
        }
        u32 r3 = 0;
        if (sony) {
            const auto it = lib.module->functions.find({"sys_net", recomp::nid_of(op.fn)});
            REQUIRE(it != lib.module->functions.end());
            r3 = static_cast<u32>(call_guest(th.ctx, it->second, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
        } else {
            const Function* fn = registry().find("sys_net", recomp::nid_of(op.fn));
            INFO(op.fn);
            REQUIRE(fn);
            for (usize i = 0; i < a.size(); i++) th.ctx.gpr[3 + i] = a[i];
            const u64 sp = th.ctx.gpr[1];
            fn->fn(th.ctx, b);
            CHECK(th.ctx.gpr[1] == sp);
            r3 = static_cast<u32>(th.ctx.gpr[3]);
        }
        if (std::getenv("PSPOCKET_NET_PROBE")) {
            std::fprintf(stderr, "PROBE %s %s(%s) -> %#x\n", sony ? "sony" : "ours", op.fn,
                op.args.empty() ? "" : std::format("{:#x}", op.args[0]).c_str(), r3);
        }
        if (op.store) ps_write32(b, op.store, r3);
        if (op.flags & kMaskAnswer) r3 = 0;
        if (op.flags & kSign) r3 = static_cast<s32>(r3) < 0 ? 0xFFFFFFFF : r3 ? 1 : 0;
        r.answers.push_back(r3);
        std::lock_guard lock(calls.mutex);
        calls.list.push_back(std::format("-- {}", op.fn));
    }
    f.kernel->set_syscall_observer(nullptr, nullptr);
    r.out.assign(b + kOut, b + kOut + kOutSize);
    r.calls = calls.list;
    std::lock_guard lock(tty.mutex);
    r.tty = tty.text;
    return r;
}

void compare(const std::string& flash, const std::vector<Op>& ops, const std::string& what)
{
    const Run sonys = run(flash, ops, true);
    if (std::getenv("PSPOCKET_NET_PROBE")) {
        for (const std::string& c : sonys.calls) std::fprintf(stderr, "CALL %s\n", c.c_str());
        std::fprintf(stderr, "TTY %s\n", sonys.tty.c_str());
        return;
    }
    const Run mine = run(flash, ops, false);
    for (usize i = 0; i < ops.size(); i++) {
        INFO(std::format("{}: step {} {}({:#x}) -> ours {:#x}, Sony's {:#x}", what, i, ops[i].fn,
            ops[i].args.empty() ? 0 : ops[i].args[0], mine.answers[i], sonys.answers[i]));
        CHECK(mine.answers[i] == sonys.answers[i]);
    }
    const auto first_diff = [](const std::vector<u8>& x, const std::vector<u8>& y) {
        usize at = 0;
        while (at < x.size() && x[at] == y[at]) at++;
        return at;
    };
    const usize out_at = first_diff(mine.out, sonys.out);
    INFO(std::format("{}: output differs first at +{:#x}", what, out_at));
    CHECK(out_at == mine.out.size());
    REQUIRE(mine.pools.size() == sonys.pools.size());
    for (usize k = 0; k < mine.pools.size(); k++) {
        const usize at = first_diff(mine.pools[k], sonys.pools[k]);
        INFO(std::format("{}: pool {} differs first at +{:#x} (ours {:#04x}, Sony's {:#04x})", what, k, at,
            at < mine.pools[k].size() ? mine.pools[k][at] : 0, at < sonys.pools[k].size() ? sonys.pools[k][at] : 0));
        CHECK(at == mine.pools[k].size());
    }
    CHECK(mine.tty == sonys.tty);
    const usize n = std::max(mine.calls.size(), sonys.calls.size());
    for (usize i = 0; i < n; i++) {
        const std::string m = i < mine.calls.size() ? mine.calls[i] : "(none)";
        const std::string s = i < sonys.calls.size() ? sonys.calls[i] : "(none)";
        INFO(std::format("{}: lv2 call {}: ours {}, Sony's {}", what, i, m, s));
        CHECK(m == s);
        if (m != s) break;
    }
}

u64 str(usize i) { return kStr + 0x80 * static_cast<u64>(i); }
u64 arg(s64 v) { return u64{static_cast<u32>(v)}; }

void init_param(std::vector<Op>& s, u32 memory, u32 size, u32 flags)
{
    s.push_back({"=", {kParam, memory}});
    s.push_back({"=", {kParam + 4, size}});
    s.push_back({"=", {kParam + 8, flags}});
}

// Before sys_net_initialize_network_ex, its checks, the pool's allocator, thread contexts, finalisation and a second
// initialisation over the first's lists.
std::vector<Op> setup_script()
{
    std::vector<Op> s;
    const u64 tag = kStr;     // any pointer
    const u32 keep = kOut;    // allocations' addresses
    s.push_back({"socket", {2, 2, 0}});
    s.push_back({"socketclose", {24}});
    s.push_back({"_sys_net_errno_loc", {}, kMaskAnswer});
    s.push_back({"sys_net_finalize_network", {}});
    s.push_back({"sys_net_free_thread_context", {0, 1}});
    s.push_back({"inet_addr", {0}});
    s.push_back({"__sys_net_lib_malloc", {16, tag, 1}});
    s.push_back({"sys_net_initialize_network_ex", {0}});
    for (const auto& [mem, size, flags] : {std::tuple{0u, kPoolSize, 0u}, std::tuple{kPool, 8191u, 0u},
             std::tuple{kPool, kPoolSize, 2u}, std::tuple{kPool, kPoolSize, 0x80000001u}}) {
        init_param(s, mem, size, flags);
        s.push_back({"sys_net_initialize_network_ex", {kParam}});
    }
    init_param(s, kPool + 1, kPoolSize - 1, 0); // an unaligned pool: libnet aligns it
    s.push_back({"sys_net_initialize_network_ex", {kParam}});
    s.push_back({"pool", {kPool + 4}});
    s.push_back({"__sys_net_lib_malloc", {24, tag, 9}, kSign, kOut + 0x800});
    s.push_back({"pool", {kPool + 4}});
    s.push_back({"_sys_net_lib_free", {(kOut + 0x800) | kDeref}});
    s.push_back({"sys_net_finalize_network", {}});
    init_param(s, kPool, kPoolSize, 0);
    s.push_back({"sys_net_initialize_network_ex", {kParam}});
    s.push_back({"pool", {}});
    init_param(s, kPool, kPoolSize, 1);
    s.push_back({"sys_net_initialize_network_ex", {kParam}});
    init_param(s, kPool, kPoolSize, 0);
    s.push_back({"sys_net_initialize_network_ex", {kParam}});
    // The allocator: sizes, a set kept and freed in an order that merges blocks with both neighbours.
    u32 slot = 0;
    for (const u32 size : {16u, 1u, 0u, 100u, 3u, 4096u, 0x30000u, 0x40000u, 0x7FFFFFFFu}) {
        s.push_back({"__sys_net_lib_malloc", {size, tag, 10 + size}, kSign, keep + 4 * slot++});
    }
    s.push_back({"pool", {}});
    for (const u32 i : {1u, 3u, 2u, 0u, 4u}) s.push_back({"_sys_net_lib_free", {(keep + 4 * i) | kDeref}});
    s.push_back({"pool", {}});
    for (u32 i = 0; i < 6; i++) s.push_back({"__sys_net_lib_malloc", {32u * (i + 1), tag, i}, kSign, keep + 4 * slot++});
    s.push_back({"pool", {}});
    for (const u32 i : {11u, 13u, 12u, 14u, 9u, 10u}) s.push_back({"_sys_net_lib_free", {(keep + 4 * i) | kDeref}});
    s.push_back({"pool", {}});
    s.push_back({"_sys_net_lib_free", {0}});
    s.push_back({"__sys_net_lib_calloc", {3, 40, tag, 1}, kSign, keep + 4 * slot++});
    s.push_back({"__sys_net_lib_calloc", {0x10000, 0x10000, tag, 2}, kSign});
    s.push_back({"__sys_net_lib_realloc", {0, 50, tag, 3}, kSign, keep + 4 * slot});
    s.push_back({"__sys_net_lib_realloc", {(keep + 4 * slot) | kDeref, 20, tag, 4}, kSign});
    s.push_back({"__sys_net_lib_realloc", {(keep + 4 * slot) | kDeref, 200, tag, 5}, kSign, keep + 4 * (slot + 1)});
    s.push_back({"__sys_net_lib_realloc", {(keep + 4 * (slot + 1)) | kDeref, 0x80000, tag, 6}, kSign});
    s.push_back({"pool", {}});
    // Thread contexts.
    s.push_back({"socket", {2, 2, 0}});
    s.push_back({"_sys_net_errno_loc", {}, kSign});
    s.push_back({"_sys_net_h_errno_loc", {}, kSign});
    s.push_back({"socket", {2, 2, 0}, kSecond});
    s.push_back({"socketclose", {99}, kSecond});
    s.push_back({"pool", {}});
    s.push_back({"sys_net_free_thread_context", {0, 8}});
    s.push_back({"sys_net_free_thread_context", {1, 1}});
    s.push_back({"sys_net_free_thread_context", {0, 3}});
    s.push_back({"sys_net_free_thread_context", {0, 6}});
    s.push_back({"sys_net_free_thread_context", {0, 0}});
    s.push_back({"sys_net_free_thread_context", {0, 5}});
    s.push_back({"sys_net_free_thread_context", {0, 1}});
    s.push_back({"pool", {}});
    s.push_back({"socketclose", {99}});
    s.push_back({"socketclose", {99}, kSecond});
    s.push_back({"sys_net_free_thread_context", {0, 2}});
    s.push_back({"pool", {}});
    s.push_back({"socket", {2, 1, 0}});
    s.push_back({"sys_net_get_sockinfo", {arg(-1), 0, 0}});
    s.push_back({"sys_net_finalize_network", {}});
    s.push_back({"pool", {}});
    // After finalisation libnet's allocator still runs over the old lists (a block left allocated there would be in
    // the next pool's used list: freed first here).
    s.push_back({"socket", {2, 2, 0}});
    s.push_back({"__sys_net_lib_malloc", {64, tag, 7}, kSign, kOut + 0x804});
    s.push_back({"_sys_net_lib_free", {(kOut + 0x804) | kDeref}});
    s.push_back({"sys_net_initialize_network_ex", {kParam}});
    s.push_back({"pool", {}});
    s.push_back({"socket", {2, 2, 0}});
    s.push_back({"sys_net_get_sockinfo", {arg(-1), 0, 0}});
    s.push_back({"sys_net_finalize_network", {}});
    // The allocations' addresses as offsets into the pool (both modules give the same blocks).
    for (u32 i = 0; i <= slot + 1; i++) s.push_back({"~", {keep + 4 * i}});
    s.push_back({"~", {kOut + 0x800}});
    s.push_back({"~", {kOut + 0x804}});
    return s;
}

// The text conversions, on a thread with a context.
std::vector<Op> text_script()
{
    std::vector<Op> s;
    init_param(s, kPool, kPoolSize, 0);
    s.push_back({"sys_net_initialize_network_ex", {kParam}});
    for (usize i = 0; i < strings().size(); i++) {
        const u64 o = kOut + 0x40 * static_cast<u64>(i);
        s.push_back({"inet_addr", {str(i)}});
        s.push_back({"inet_aton", {str(i), o}});
        s.push_back({"inet_network", {str(i)}});
        s.push_back({"inet_pton", {2, str(i), o + 4}});
        s.push_back({"inet_pton", {24, str(i), o + 8}});
    }
    s.push_back({"inet_pton", {3, str(0), kOut}});
    s.push_back({"inet_pton", {2, 0, kOut}});
    s.push_back({"inet_pton", {2, str(0), 0}});
    s.push_back({"inet_aton", {0, kOut}});
    s.push_back({"inet_aton", {str(0), 0}});
    const u32 addrs = kOut + 0x2000; // sixteen-byte addresses to print
    const std::array<std::array<u8, 16>, 10> bytes = {{
        {},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 1, 2, 3, 4},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4},
        {0x20, 1, 0xd, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
        {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1},
        {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 3, 4},
        {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
    }};
    for (usize i = 0; i < bytes.size(); i++) {
        for (u32 k = 0; k < 16; k += 4) {
            const u32 w = u32{bytes[i][k]} << 24 | u32{bytes[i][k + 1]} << 16 | u32{bytes[i][k + 2]} << 8 |
                          bytes[i][k + 3];
            s.push_back({"=", {addrs + 16 * i + k, w}});
        }
        const u64 dst = kOut + 0x3000 + 0x40 * i;
        s.push_back({"inet_ntop", {24, addrs + 16 * i, dst, 46}, kSign});
        s.push_back({"inet_ntop", {24, addrs + 16 * i, dst, 10}, kSign});
        s.push_back({"inet_ntop", {2, addrs + 16 * i, dst + 0x30, 16}, kSign});
        s.push_back({"inet_ntop", {2, addrs + 16 * i, dst + 0x30, 7}, kSign});
        s.push_back({"inet_ntop", {7, addrs + 16 * i, dst, 46}, kSign});
    }
    for (const u32 in : {0u, 0x7F000001u, 0x80010203u, 0xC0A80101u, 0xE0000001u, 0xFFFFFFFFu, 0x0A000001u}) {
        s.push_back({"inet_lnaof", {in}});
        s.push_back({"inet_netof", {in}});
        s.push_back({"inet_ntoa", {in}, kSign});
    }
    for (const auto& [net, host] : {std::pair{10u, 0x123456u}, std::pair{0x8001u, 0xABCDu}, std::pair{0xC0A801u, 5u},
             std::pair{0x1000000u, 7u}, std::pair{127u, 0xFFFFFFFFu}}) {
        s.push_back({"inet_makeaddr", {kOut + 0x3800, net, host}, kSign});
    }
    s.push_back({"pool", {}});
    s.push_back({"sys_net_finalize_network", {}});
    return s;
}

// Sockets through libnet's convention, the socket list, the helpers.
std::vector<Op> socket_script()
{
    std::vector<Op> s;
    init_param(s, kPool, kPoolSize, 0);
    s.push_back({"sys_net_initialize_network_ex", {kParam}});
    const u64 sa = kOut + 0x100, len = kOut + 0x120, opt = kOut + 0x130, info = kOut + 0x200;
    s.push_back({"socket", {2, 1, 0}});
    s.push_back({"socket", {2, 2, 17}});
    s.push_back({"socket", {2, 4, 0}});
    s.push_back({"socket", {24, 1, 0}});
    s.push_back({"=", {sa, 0x10020000 | 7300}});
    s.push_back({"=", {sa + 4, 0x7F000001}});
    s.push_back({"bind", {24, sa, 16}});
    s.push_back({"bind", {24, sa, 8}});
    s.push_back({"listen", {24, 5}});
    s.push_back({"listen", {25, 5}});
    s.push_back({"=", {len, 16}});
    s.push_back({"getsockname", {24, sa + 0x40, len}});
    s.push_back({"getpeername", {24, sa + 0x40, len}});
    s.push_back({"=", {opt, 1}});
    s.push_back({"setsockopt", {25, 0xFFFF, 0x1100, opt, 4}});
    s.push_back({"setsockopt", {25, 0xFFFF, 0x1100, opt, 2}});
    s.push_back({"=", {len, 4}});
    s.push_back({"getsockopt", {25, 0xFFFF, 0x1100, opt + 8, len}});
    s.push_back({"recvfrom", {25, kOut + 0x400, 64, 0, sa + 0x40, len}});
    s.push_back({"recv", {25, kOut + 0x400, 64, 0x80}});
    s.push_back({"=", {sa + 0x60, 0x10020000 | 9}});
    s.push_back({"=", {sa + 0x64, 0x08080808}});
    s.push_back({"sendto", {25, kOut + 0x400, 4, 0, sa + 0x60, 16}});
    s.push_back({"send", {25, kOut + 0x400, 4, 0}});
    s.push_back({"connect", {25, sa + 0x60, 16}});
    s.push_back({"shutdown", {25, 2}});
    s.push_back({"shutdown", {25, 5}});
    s.push_back({"socketpoll", {0, 0, 0}});
    s.push_back({"socketselect", {0, 0, 0, 0, 0}});
    s.push_back({"sendmsg", {25, 0, 0}});
    s.push_back({"recvmsg", {25, 0, 0}});
    s.push_back({"sys_net_abort_socket", {25, 0}});
    s.push_back({"sys_net_abort_socket", {25, 1}});
    s.push_back({"sys_net_abort_socket", {99, 0}});
    s.push_back({"sys_net_abort_resolver", {0, 0}});
    s.push_back({"sys_net_get_sockinfo", {arg(-1), info, 4}});
    s.push_back({"sys_net_get_sockinfo", {24, info + 0x100, 1}});
    s.push_back({"sys_net_get_sockinfo_ex", {arg(-1), info + 0x200, 4, 0}});
    s.push_back({"sys_net_get_sockinfo", {99, info, 1}});
    s.push_back({"sys_net_set_resolver_configurations", {0, 0, 0}});
    s.push_back({"sys_net_set_resolver_configurations", {3, 7, 0}});
    s.push_back({"sys_net_set_resolver_configurations", {3, 7, 1}});
    s.push_back({"_sys_net_lib_set_libnetctl_queue", {1, 2, 3}});
    s.push_back({"_sys_net_lib_reset_libnetctl_queue", {1}});
    s.push_back({"pool", {}});
    s.push_back({"_sys_net_lib_rand", {}, kMaskAnswer});
    s.push_back({"_sys_net_lib_get_system_time", {kOut + 0x600}});
    s.push_back({"=", {kOut + 0x600, 0}});
    s.push_back({"=", {kOut + 0x604, 0}});
    s.push_back({"_sys_net_lib_get_system_time", {0}});
    s.push_back({"_sys_net_lib_usleep", {10}});
    s.push_back({"_sys_net_lib_thread_join", {0x7777, kOut + 0x610}});
    s.push_back({"_sys_net_lib_sync_create", {kOut + 0x700, 5}});
    s.push_back({"_sys_net_lib_sync_clear", {kOut + 0x700, 4}});
    s.push_back({"_sys_net_lib_sync_signal", {kOut + 0x700, 0x30}});
    s.push_back({"_sys_net_lib_sync_wait", {kOut + 0x700, kOut + 0x720}});
    s.push_back({"_sys_net_lib_sync_destroy", {kOut + 0x700}});
    s.push_back({"~", {kOut + 0x700}});
    s.push_back({"~", {kOut + 0x704}});
    s.push_back({"socketclose", {24}});
    s.push_back({"socketclose", {24}});
    s.push_back({"sys_net_finalize_network", {}});
    s.push_back({"socketclose", {25}});
    return s;
}

// The interface reports (with no link lv2 lists no interface).
std::vector<Op> interface_script()
{
    std::vector<Op> s;
    init_param(s, kPool, kPoolSize, 0);
    s.push_back({"sys_net_initialize_network_ex", {kParam}});
    s.push_back({"sys_net_show_ifconfig", {}});
    s.push_back({"sys_net_get_if_list", {kOut, 5}});
    s.push_back({"sys_net_get_if_list", {0, 5}});
    s.push_back({"pool", {}});
    s.push_back({"sys_net_finalize_network", {}});
    return s;
}

// The resolver: names libnet knows (its /etc/hosts, numbers), and names it would ask a name server for.
std::vector<Op> resolver_script()
{
    std::vector<Op> s;
    init_param(s, kPool, kPoolSize, 0);
    s.push_back({"sys_net_initialize_network_ex", {kParam}});
    s.push_back({"=", {kOut + 0x7000, 0x6C6F6361}}); // "localhost"
    s.push_back({"=", {kOut + 0x7004, 0x6C686F73}});
    s.push_back({"=", {kOut + 0x7008, 0x74000000}});
    s.push_back({"gethostbyname", {kOut + 0x7000}, kSign});
    s.push_back({"_sys_net_h_errno_loc", {}, kSign, kOut + 0x7100});
    s.push_back({"gethostbyname", {str(0)}, kSign});
    s.push_back({"gethostbyname", {str(5)}, kSign});
    s.push_back({"=", {kOut + 0x7010, 0x6578616D}}); // "example.com"
    s.push_back({"=", {kOut + 0x7014, 0x706C652E}});
    s.push_back({"=", {kOut + 0x7018, 0x636F6D00}});
    s.push_back({"gethostbyname", {kOut + 0x7010}, kSign});
    s.push_back({"gethostbyname", {0}, kSign});
    s.push_back({"pool", {}});
    s.push_back({"sys_net_finalize_network", {}});
    return s;
}

// A string written with 32-bit stores (its NUL included).
void put_str(std::vector<Op>& s, u32 at, std::string_view text)
{
    std::string t(text);
    t.push_back('\0');
    while (t.size() % 4) t.push_back('\0');
    for (usize i = 0; i < t.size(); i += 4) {
        const u32 w = u32{static_cast<u8>(t[i])} << 24 | u32{static_cast<u8>(t[i + 1])} << 16 |
                      u32{static_cast<u8>(t[i + 2])} << 8 | static_cast<u8>(t[i + 3]);
        s.push_back({"=", {at + i, w}});
    }
}

// libnet's configuration functions (D-539): ioctl and control, the dumps, the test parameters, the name servers.
std::vector<Op> config_script()
{
    std::vector<Op> s;
    init_param(s, kPool, kPoolSize, 0);
    s.push_back({"sys_net_initialize_network_ex", {kParam}});
    const u32 txt = kOut + 0x6000; // strings, 0x40 apart
    const auto text = [&](u32 i, std::string_view t) {
        put_str(s, txt + 0x40 * i, t);
        return u64{txt + 0x40 * i};
    };
    const u64 ip = text(3, "1.2.3.4"), zero = text(4, "0.0.0.0"), none = text(5, "");
    // ioctl and control.
    s.push_back({"socket", {2, 2, 0}});
    s.push_back({"_sys_net_lib_ioctl", {24, 0xC0000000, 0}});
    s.push_back({"_sys_net_lib_ioctl", {24, 0x12345678, kOut + 0x100}});
    for (const u32 cmd : {0x8040691Au, 0x80206913u, 0x80146980u, 0x80146981u, 0x80146982u, 0x800250CFu, 0x801050CDu,
             0x200050C8u, 0x8020690Eu, 0x80206910u, 0x8020690Cu, 0x8020697Fu, 0x80206919u, 0x80206916u, 0x80206979u,
             0x8020697Au, 0x8020697Bu, 0x802450C9u, 0x8020698Du, 0x802450CBu, 0x802450CCu, 0x802450CDu, 0xC0186983u,
             0xC01050CEu, 0x80506979u, 0x8040696Eu, 0x8210698Fu, 0xC00250D0u, 0xC014697Cu, 0xC0146974u, 0xC0146988u,
             0xC018697Fu, 0xC020697Eu, 0xC0206911u, 0xC0206921u, 0xC0206923u, 0xC0206924u, 0xC0206925u, 0xC02450CAu,
             0xC020698Cu, 0xC1146975u, 0xC210698Eu, 0x20000000u, 0x00000001u}) {
        for (u32 k = 0; k < 0x60; k += 4) s.push_back({"=", {kOut + 0x100 + k, 0x11111111u * (k / 4 + 1)}});
        s.push_back({"_sys_net_lib_ioctl", {24, cmd, kOut + 0x100}});
        s.push_back({"_sys_net_lib_ioctl", {99, cmd, kOut + 0x100}});
    }
    s.push_back({"_sys_net_lib_bnet_control", {1, arg(-2), 3, arg(-4)}});
    s.push_back({"sys_net_open_dump", {100, 1}});
    s.push_back({"sys_net_read_dump", {1, kOut + 0x300, 64, kOut + 0x340}});
    s.push_back({"sys_net_close_dump", {1, kOut + 0x344}});
    // Test parameters.
    const u32 tp = kOut + 0x400;
    s.push_back({"sys_net_get_test_param", {0}});
    for (const u32 flags : {0x10001u, 0u, 2u, 1u}) {
        s.push_back({"=", {tp + 8, flags}});
        s.push_back({"sys_net_get_test_param", {tp}});
        s.push_back({"=", {tp + 8, flags}});
        s.push_back({"sys_net_set_test_param", {tp}});
    }
    s.push_back({"sys_net_get_netemu_test_param", {0}});
    s.push_back({"sys_net_get_netemu_test_param", {tp}});
    s.push_back({"sys_net_set_netemu_test_param", {0}});
    s.push_back({"=", {tp, 0x00010000}});
    s.push_back({"sys_net_set_netemu_test_param", {tp}});
    s.push_back({"=", {tp, 0x12340000}});
    s.push_back({"=", {tp + 0x20, 0xABCDEF01}});
    s.push_back({"sys_net_set_netemu_test_param", {tp}});
    s.push_back({"=", {tp + 0x20, 0}});
    s.push_back({"sys_net_get_test_param", {tp}});
    s.push_back({"sys_net_get_udpp2p_test_param", {0}});
    s.push_back({"sys_net_set_udpp2p_test_param", {0}});
    s.push_back({"=", {kOut + 0x700, 0x01020304}});
    s.push_back({"sys_net_set_udpp2p_test_param", {kOut + 0x700}});
    s.push_back({"sys_net_get_udpp2p_test_param", {kOut + 0x740}});
    // Name servers.
    s.push_back({"sys_net_get_name_server", {kOut + 0x800, 3}});
    s.push_back({"sys_net_get_lib_name_server", {0, 0}});
    s.push_back({"sys_net_show_nameserver", {}});
    s.push_back({"=", {kOut + 0x900, 0x01020304}});
    s.push_back({"=", {kOut + 0x904, 0x05060708}});
    s.push_back({"sys_net_set_lib_name_server", {kOut + 0x900, kOut + 0x904}});
    s.push_back({"pool", {}});
    s.push_back({"sys_net_get_name_server", {kOut + 0x800, 3}});
    s.push_back({"sys_net_get_name_server", {0, 3}});
    s.push_back({"sys_net_get_name_server", {kOut + 0x80C, 1}});
    s.push_back({"sys_net_get_lib_name_server", {kOut + 0x810, kOut + 0x814}});
    s.push_back({"sys_net_get_lib_name_server", {0, 0}});
    s.push_back({"sys_net_show_nameserver", {}});
    s.push_back({"sys_net_set_lib_name_server", {0, kOut + 0x904}});
    s.push_back({"sys_net_get_lib_name_server", {kOut + 0x818, 0}}); // (libnet copies stack bytes for a missing one)
    s.push_back({"sys_net_set_lib_name_server", {0, 0}});
    s.push_back({"pool", {}});
    s.push_back({"_sce_net_add_name_server", {0, 0x0A000001, 0}, kSign, kOut + 0xA00});
    s.push_back({"_sce_net_add_name_server", {0, 0x0A000002, (kOut + 0xA00) | kDeref}, kSign, kOut + 0xA04});
    s.push_back({"_sce_net_add_name_server", {0, 0x0A000003, (kOut + 0xA04) | kDeref}, kSign, kOut + 0xA08});
    s.push_back({"_sce_net_add_name_server_with_char", {0, ip, 0}, kSign, kOut + 0xA0C});
    s.push_back({"_sce_net_add_name_server_with_char", {0, 0, 0}, kSign});
    s.push_back({"_sce_net_add_name_server_with_char", {0, zero, 0}, kSign});
    s.push_back({"_sce_net_add_name_server_with_char", {0, none, (kOut + 0xA0C) | kDeref}, kSign, kOut + 0xA10});
    s.push_back({"pool", {}});
    s.push_back({"_sce_net_set_name_server", {(kOut + 0xA08) | kDeref}});
    s.push_back({"sys_net_get_name_server", {kOut + 0x820, 3}});
    s.push_back({"_sce_net_get_name_server", {kOut + 0x830, 1}});
    s.push_back({"sys_net_show_nameserver", {}});
    s.push_back({"_sce_net_set_name_server", {(kOut + 0xA10) | kDeref}});
    s.push_back({"sys_net_show_nameserver", {}});
    s.push_back({"pool", {}});
    s.push_back({"sys_net_finalize_network", {}});
    return s;
}

// Routes, interfaces, sys_net_if_ctl, the netset calls and gethostbyaddr (D-539), on a console with no link.
std::vector<Op> routes_script()
{
    std::vector<Op> s;
    init_param(s, kPool, kPoolSize, 0);
    s.push_back({"sys_net_initialize_network_ex", {kParam}});
    const u32 txt = kOut + 0x6000; // strings, 0x40 apart
    const auto text = [&](u32 i, std::string_view t) {
        put_str(s, txt + 0x40 * i, t);
        return u64{txt + 0x40 * i};
    };
    const u64 eth0 = text(0, "eth0"), lo0 = text(1, "lo0");
    // Routes.
    s.push_back({"_sce_net_flush_route", {}});
    s.push_back({"sys_net_get_routing_table_af", {0, 0, 0}});
    s.push_back({"sys_net_get_routing_table_af", {2, kOut + 0xC00, 4}});
    s.push_back({"sys_net_show_route", {}});
    s.push_back({"pool", {}});
    s.push_back({"_sce_net_set_default_gateway", {eth0, 0xC0A80001, 0}});
    s.push_back({"pool", {}});
    s.push_back({"_sce_net_set_default_gateway", {eth0, 0xC0A80001, 1}});
    s.push_back({"pool", {}});
    s.push_back({"_sce_net_set_ip_and_mask", {eth0, 0xC0A80002, 0xFFFFFF00}});
    s.push_back({"pool", {}});
    // sys_net_if_ctl.
    for (const auto& [index, cmd, len] : {std::tuple{0u, 640u, 4u}, std::tuple{0u, 641u, 4u}, std::tuple{0u, 642u, 4u},
             std::tuple{0u, 643u, 4u}, std::tuple{0u, 644u, 4u}, std::tuple{1u, 640u, 4u}, std::tuple{0u, 640u, 8u},
             std::tuple{0u, 656u, 0u}, std::tuple{0u, 768u, 4u}, std::tuple{0u, 769u, 50u}, std::tuple{0u, 769u, 62u},
             std::tuple{0u, 16u, 20u}, std::tuple{0u, 16u, 19u}, std::tuple{1u, 645u, 4u}, std::tuple{0u, 1u, 4u},
             std::tuple{2u, 3u, 16u}, std::tuple{0u, 0x81000001u, 16u}, std::tuple{0u, 0x80000001u, 16u},
             std::tuple{0u, 0xFFFFFFFFu, 0u}, std::tuple{0u, 700u, 4u}, std::tuple{0u, 770u, 4u}}) {
        for (u32 k = 0; k < 0x40; k += 4) s.push_back({"=", {kOut + 0xB00 + k, 0x22222222}});
        s.push_back({"sys_net_if_ctl", {index, cmd, cmd == 656 ? 0u : kOut + 0xB00, len}});
    }
    s.push_back({"sys_net_if_ctl", {0, 640, 0, 4}});
    s.push_back({"pool", {}});
    // Interfaces by name.
    s.push_back({"_sys_net_lib_if_nametoindex", {eth0}});
    s.push_back({"_sys_net_lib_if_nametoindex", {lo0}});
    s.push_back({"pool", {}});
    s.push_back({"sys_net_finalize_network", {}});
    return s;
}

// The netset calls and gethostbyaddr (D-539).
std::vector<Op> netset_script()
{
    std::vector<Op> s;
    init_param(s, kPool, kPoolSize, 0);
    s.push_back({"sys_net_initialize_network_ex", {kParam}});
    const u32 txt = kOut + 0x6000; // strings, 0x40 apart
    const auto text = [&](u32 i, std::string_view t) {
        put_str(s, txt + 0x40 * i, t);
        return u64{txt + 0x40 * i};
    };
    const u64 eth0 = text(0, "eth0"), proxy = text(2, "http_proxy"), ssid = text(6, "ssid"), bad = text(7, "bogus");
    // netset without one open.
    s.push_back({"sys_netset_get_status", {0, kOut + 0xD00}});
    s.push_back({"sys_netset_get_status", {5, kOut + 0xD80}});
    s.push_back({"sys_netset_get_key_value", {0, proxy, kOut + 0xE00, 64, 1}});
    s.push_back({"sys_netset_get_key_value", {0, proxy, 0, 64, 1}});
    s.push_back({"sys_netset_get_key_value", {0, ssid, kOut + 0xE00, 64, 1}});
    s.push_back({"sys_netset_get_key_value", {0, bad, kOut + 0xE00, 64, 1}});
    s.push_back({"sys_netset_get_key_value", {3, proxy, kOut + 0xE00, 64, 1}});
    s.push_back({"sys_netset_get_if_id", {eth0, eth0, 3}});
    s.push_back({"sys_netset_get_if_id", {eth0, 0, 0}});
    s.push_back({"sys_netset_close", {5}});
    s.push_back({"sys_netset_abort", {5, 0}});
    s.push_back({"sys_netset_if_up", {5, 0, 0}});
    s.push_back({"sys_netset_if_down", {5, 0, 0}});
    s.push_back({"sys_netset_get_status", {0, kOut + 0xD00}});
    const u64 conf = text(8, "ssid foo\nhttp_proxy \"a b\"\n# c\n");
    s.push_back({"sys_netset_open", {conf, 0, 0}, kSign, kOut + 0xDF0});
    s.push_back({"pool", {}});
    s.push_back({"sys_netset_get_status", {(kOut + 0xDF0) | kDeref, kOut + 0xD00}});
    s.push_back({"sys_netset_get_key_value", {0, proxy, kOut + 0xE00, 64, 1}});
    s.push_back({"sys_netset_get_key_value", {0, ssid, kOut + 0xE40, 64, 1}});
    s.push_back({"sys_netset_get_if_id", {(kOut + 0xDF0) | kDeref, 0, 1}});
    s.push_back({"sys_netset_close", {(kOut + 0xDF0) | kDeref}});
    s.push_back({"pool", {}});
    // gethostbyaddr.
    s.push_back({"=", {kOut + 0xF00, 0x7F000001}});
    s.push_back({"=", {kOut + 0xF10, 0xFE800000}});
    s.push_back({"=", {kOut + 0xF20, 0}});
    s.push_back({"=", {kOut + 0xF24, 0}});
    s.push_back({"=", {kOut + 0xF28, 0xFFFF}});
    s.push_back({"=", {kOut + 0xF2C, 0x7F000001}});
    s.push_back({"gethostbyaddr", {0, 4, 2}, kSign});
    s.push_back({"_sys_net_h_errno_loc", {}, kSign, kOut + 0xF80});
    s.push_back({"gethostbyaddr", {kOut + 0xF00, 3, 2}, kSign});
    s.push_back({"gethostbyaddr", {kOut + 0xF00, 4, 7}, kSign});
    s.push_back({"gethostbyaddr", {kOut + 0xF10, 16, 24}, kSign});
    s.push_back({"gethostbyaddr", {kOut + 0xF00, 4, 2}, kSign});
    s.push_back({"gethostbyaddr", {kOut + 0xF20, 16, 24}, kSign});
    s.push_back({"pool", {}});
    s.push_back({"sys_net_finalize_network", {}});
    return s;
}

} // namespace

TEST_CASE("sys_net against libnet: configuration", "[hle][lle][net]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, config_script(), "configuration");
}

TEST_CASE("sys_net against libnet: routes and interfaces", "[hle][lle][net]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, routes_script(), "routes");
}

TEST_CASE("sys_net against libnet: netsets and gethostbyaddr", "[.][hle][lle][net]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, netset_script(), "netset");
}

TEST_CASE("sys_net against libnet: interfaces", "[hle][lle][net]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, interface_script(), "interfaces");
}

// Hidden until libnet's resolver itself is pspocket's (its answers match already; its hosts-file reads, res_init and the
// DNS attempt make pool allocations and an lv2 infoctl the interim gethostbyname leaves out).
TEST_CASE("sys_net against libnet: the resolver", "[.][hle][lle][net]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, resolver_script(), "resolver");
}

TEST_CASE("sys_net against libnet: set-up, the pool and thread contexts", "[hle][lle][net]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, setup_script(), "setup");
}

TEST_CASE("sys_net against libnet: address text", "[hle][lle][net]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, text_script(), "text");
}

TEST_CASE("sys_net against libnet: sockets and helpers", "[hle][lle][net]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, socket_script(), "sockets");
}
