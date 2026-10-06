// sysPrxForUser as RPCS3 has it for the firmware modules that import it (D-168): the C library (memory, strings, ctype,
// malloc, the printf family through RPCS3's formatter with the differences D-168 lists, qsort through a guest
// comparator), the heaps, console output, lwcond on real lv2 threads, mempool, the atexit list, PARAM.SFO, random
// numbers, the mmapper container wrapper and sys_game_process_exitspawn2. Everything is synthetic (no game data): the
// guest code is a few PPU instructions written here and run on the interpreter.

#include "hle_test_util.hpp"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pspocket/core/log.hpp"
#include "pspocket/lv2/syscalls.hpp"
#include "pspocket/ppu/runtime.h"

using namespace pspocket;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kStr = kData + 0x10000, kStr2 = kData + 0x10800, kOut = kData + 0x20000, kFmt = kData + 0x30000;
constexpr u32 kArr = kData + 0x40000, kChunk = kData + 0x80000;
constexpr u32 kCode = kData + 0x1000, kOpd = kData + 0x1100, kCounter = kData + 0x1200;
constexpr u32 kCode2 = kData + 0x1400, kOpd2 = kData + 0x1500, kCounter2 = kData + 0x1600;
constexpr u32 kUnmapped = 0x0FFF0000; // in the main area, never committed by these tests
constexpr u32 kEdgePage = 0x08000000; // one committed host page, with no guest memory after it (EdgePage)
constexpr u64 kMinus1 = ~0ull;        // -1 as an s32 result sign-extended into r3

// The log lines written while it lives.
struct LogLines {
    struct Line {
        log::Level level;
        std::string channel, message;
    };
    std::mutex mutex;
    std::vector<Line> lines;

    LogLines()
    {
        log::set_sink(
            [](void* user, log::Level level, std::string_view channel, std::string_view message) {
                auto* self = static_cast<LogLines*>(user);
                std::lock_guard lock(self->mutex);
                self->lines.push_back({level, std::string(channel), std::string(message)});
            },
            this);
    }
    ~LogLines() { log::set_sink(nullptr, nullptr); }
    LogLines(const LogLines&) = delete;
    LogLines& operator=(const LogLines&) = delete;

    // The lines at `level` on `channel` that contain `text`.
    usize count(log::Level level, std::string_view channel, std::string_view text)
    {
        std::lock_guard lock(mutex);
        usize n = 0;
        for (const Line& l : lines)
            if (l.level == level && l.channel == channel && l.message.find(text) != std::string::npos) n++;
        return n;
    }
    // The messages at `level` on `channel`, in order.
    std::vector<std::string> messages(log::Level level, std::string_view channel)
    {
        std::lock_guard lock(mutex);
        std::vector<std::string> out;
        for (const Line& l : lines)
            if (l.level == level && l.channel == channel) out.push_back(l.message);
        return out;
    }
    // The first line at `level` on `channel` that contains `text`, or "".
    std::string find(log::Level level, std::string_view channel, std::string_view text)
    {
        std::lock_guard lock(mutex);
        for (const Line& l : lines)
            if (l.level == level && l.channel == channel && l.message.find(text) != std::string::npos) return l.message;
        return {};
    }
};

// Guest code the HLE calls back into runs on the PPU interpreter while this lives.
struct InterpretMisses {
    const ps_ppu_miss_policy saved = ps_ppu_get_miss_policy();
    InterpretMisses() { ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET); }
    ~InterpretMisses() { ps_ppu_set_miss_policy(saved); }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// One committed host page at kEdgePage; edge() is the first address after it, which is no guest memory.
struct EdgePage {
    explicit EdgePage(Fixture& f) { REQUIRE(f.mem.commit(kEdgePage, GuestMemory::host_page_size())); }
    static u32 edge() { return kEdgePage + static_cast<u32>(GuestMemory::host_page_size()); }
};

void put(u8* base, u32 addr, std::string_view s)
{
    std::memcpy(base + addr, s.data(), s.size());
    base[addr + s.size()] = 0;
}

std::string cstr(u8* base, u32 addr) { return std::string(reinterpret_cast<const char*>(base + addr)); }

s32 as_s32(u64 r3) { return static_cast<s32>(static_cast<u32>(r3)); }

u64 bits(double d) { return std::bit_cast<u64>(d); }

// A guest function at `code` with its descriptor at `opd`: counter += 1, then counter[1] = r3.
void write_counter_function(u8* b, u32 code, u32 opd, u32 counter)
{
    const u32 words[] = {
        0x3CA00000u | (counter >> 16),    // lis r5, counter@h
        0x60A50000u | (counter & 0xFFFF), // ori r5, r5, counter@l
        0x80850000u,                      // lwz r4, 0(r5)
        0x38840001u,                      // addi r4, r4, 1
        0x90850000u,                      // stw r4, 0(r5)
        0x90650004u,                      // stw r3, 4(r5)
        0x4E800020u,                      // blr
    };
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, code + 4 * i, words[i]);
    ps_write32(b, opd, code);
    ps_write32(b, opd + 4, 0);
}

// A qsort comparator at `code` (descriptor at `opd`): counter += 1, then the s32 at *r3 minus the s32 at *r4.
void write_compare_function(u8* b, u32 code, u32 opd, u32 counter)
{
    const u32 words[] = {
        0x3CE00000u | (counter >> 16),    // lis r7, counter@h
        0x60E70000u | (counter & 0xFFFF), // ori r7, r7, counter@l
        0x81070000u,                      // lwz r8, 0(r7)
        0x39080001u,                      // addi r8, r8, 1
        0x91070000u,                      // stw r8, 0(r7)
        0x80A30000u,                      // lwz r5, 0(r3)
        0x80C40000u,                      // lwz r6, 0(r4)
        0x7C662850u,                      // subf r3, r6, r5
        0x7C6307B4u,                      // extsw r3, r3
        0x4E800020u,                      // blr
    };
    for (u32 i = 0; i < std::size(words); i++) ps_write32(b, code + 4 * i, words[i]);
    ps_write32(b, opd, code);
    ps_write32(b, opd + 4, 0);
}

// Calls an HLE function with up to eight arguments as guest code does, without Catch assertions (for other host
// threads).
template <class... A>
u64 call_quiet(GuestThread& g, std::string_view name, A... args)
{
    static_assert(sizeof...(A) <= 8);
    const hle::Function* fn = nullptr;
    for (const hle::Function& x : hle::registry().functions())
        if (x.name == name) fn = &x;
    if (!fn) std::abort();
    const u64 v[] = {0, static_cast<u64>(args)...};
    for (usize i = 0; i < sizeof...(A); i++) g.ctx.gpr[3 + i] = v[i + 1];
    fn->fn(g.ctx, g.f.base());
    return g.ctx.gpr[3];
}

bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::milliseconds(10000))
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// A host thread that makes guest calls. Joined by join(); when a failed check leaves it running, the destructor ends
// the process (its kernel waits abort, the thread unwinds) and then joins it.
struct HostThread {
    Fixture& f;
    std::thread thread;

    template <class Fn>
    HostThread(Fixture& fx, Fn fn) : f(fx), thread([fn]() mutable {
        try {
            fn();
        } catch (const lv2::ThreadExit&) {
        }
    })
    {
    }
    ~HostThread()
    {
        if (!thread.joinable()) return;
        f.kernel->shutdown(std::chrono::milliseconds(500));
        thread.join();
    }
    HostThread(const HostThread&) = delete;
    HostThread& operator=(const HostThread&) = delete;
    void join() { thread.join(); }
};

// Whether the guest thread published in `t` is parked in the kernel on an object of `kind`.
bool parked(const std::atomic<lv2::PpuThread*>& t, lv2::WaitKind kind)
{
    return wait_for([&] {
        lv2::PpuThread* p = t.load();
        return p && p->waiter.waiting_on().kind == kind;
    });
}

// _sys_sprintf into kOut with the format at kFmt; checks the returned length against what was written.
template <class... A>
std::string sprintf_(GuestThread& g, std::string_view fmt, A... args)
{
    u8* b = g.f.base();
    put(b, kFmt, fmt);
    std::memset(b + kOut, 0x7E, 0x200);
    const u64 n = g.call("_sys_sprintf", kOut, kFmt, static_cast<u64>(args)...);
    const std::string s = cstr(b, kOut);
    CHECK(n == s.size());
    return s;
}

} // namespace

TEST_CASE("sysPrxForUser (D-168): the C library, heaps, lwcond, mempool and the rest under their NIDs", "[hle][libc]")
{
    const std::pair<std::string_view, u32> expected[] = {
        {"_sys_memchr", 0x3BD53C7B}, {"_sys_memcmp", 0xFB5DB080}, {"_sys_memcpy", 0x6BF66EA7},
        {"_sys_memmove", 0x27427742}, {"_sys_memset", 0x68B9B011}, {"_sys_strcat", 0x052D29A6},
        {"_sys_strchr", 0x7498887B}, {"_sys_strcmp", 0x459B4393}, {"_sys_strcpy", 0x99C88692},
        {"_sys_strlen", 0x2D36462B}, {"_sys_strncasecmp", 0x1CA525A2}, {"_sys_strncat", 0x996F7CF8},
        {"_sys_strncmp", 0x04E83D2C}, {"_sys_strncpy", 0xD3039D4D}, {"_sys_strrchr", 0x191F0C4A},
        {"_sys_tolower", 0x4B2F301A}, {"_sys_toupper", 0xEEF75113}, {"_sys_qsort", 0xC4FD6121},
        {"_sys_malloc", 0xBDB18F83}, {"_sys_memalign", 0x318F17E1}, {"_sys_free", 0xF7F7FB20},
        {"_sys_printf", 0x9F04F7AF}, {"_sys_sprintf", 0xA1F9EAFE}, {"_sys_snprintf", 0x06574237},
        {"__sys_look_ctype_table", 0x3EF17F8C}, {"_sys_heap_create_heap", 0xB2FCF2C8},
        {"_sys_heap_delete_heap", 0xAEDE4B03}, {"_sys_heap_malloc", 0x35168520}, {"_sys_heap_memalign", 0x44265C08},
        {"_sys_heap_free", 0x8A561D92}, {"console_putc", 0xE66BAC36}, {"console_write", 0xF57E1D6F},
        {"sys_lwcond_create", 0xDA0EB71A}, {"sys_lwcond_destroy", 0x1C9A942C}, {"sys_lwcond_wait", 0x2A6D9D51},
        {"sys_lwcond_signal", 0xEF87A695}, {"sys_lwcond_signal_all", 0xE9A1BD84}, {"sys_lwcond_signal_to", 0x52AADADF},
        {"sys_mempool_create", 0xCA9A60BF}, {"sys_mempool_destroy", 0x9D3C0F81},
        {"sys_mempool_allocate_block", 0xA146A143}, {"sys_mempool_try_allocate_block", 0x05C65656},
        {"sys_mempool_free_block", 0x608212FC}, {"sys_mempool_get_count", 0x25596F51},
        {"sys_ppu_thread_register_atexit", 0x3DD4A957}, {"sys_ppu_thread_unregister_atexit", 0xAC6FC404},
        {"sys_mmapper_allocate_memory_from_container", 0x70258515}, {"sys_process_get_paramsfo", 0xE75C40F2},
        {"sys_get_random_number", 0x71A8472A}, {"sys_game_process_exitspawn2", 0x67F9FEDB},
    };
    // (test_hle.cpp checks every registered NID against its name.)
    for (const auto& [name, nid] : expected) {
        INFO(name);
        const hle::Function* fn = hle::registry().find("sysPrxForUser", nid);
        REQUIRE(fn != nullptr);
        CHECK(fn->name == name);
    }
}

// ------------------------------------------------------------------------------------------------- memory, strings

TEST_CASE("_sys_memset, _sys_memcpy, _sys_memmove, _sys_memcmp, _sys_memchr: RPCS3's results, other memory refused",
    "[hle][libc]")
{
    LogLines logs;
    Fixture f;
    EdgePage page(f);
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    const u32 edge = EdgePage::edge();

    CHECK(a.call("_sys_memset", kOut, 0x1AB, 16) == kOut); // the value's low byte; the destination back
    for (u32 i = 0; i < 16; i++) CHECK(b[kOut + i] == 0xAB);
    CHECK(b[kOut + 16] == 0);

    put(b, kStr, "0123456789");
    CHECK(a.call("_sys_memcpy", kOut, kStr, 10) == kOut);
    CHECK(std::memcmp(b + kOut, "0123456789", 10) == 0);
    CHECK(a.call("_sys_memmove", kStr + 2, kStr, 8) == kStr + 2); // overlapping
    CHECK(cstr(b, kStr) == "0101234567");

    // memcmp: -1, 0 or 1 from the first byte that differs, unsigned.
    put(b, kStr, "abc\x80");
    put(b, kStr2, "abc\x01");
    CHECK(a.call("_sys_memcmp", kStr, kStr2, 3) == 0);
    CHECK(a.call("_sys_memcmp", kStr, kStr2, 4) == 1);
    CHECK(a.call("_sys_memcmp", kStr2, kStr, 4) == kMinus1);
    CHECK(a.call("_sys_memcmp", kStr, kStr2, 0) == 0);

    // memchr: the byte's address, or NULL; NULL for a null buffer or a size that is not positive.
    CHECK(a.call("_sys_memchr", kStr, 'c', 4) == kStr + 2);
    CHECK(a.call("_sys_memchr", kStr, 0x163, 4) == kStr + 2); // the low byte
    CHECK(a.call("_sys_memchr", kStr, 'z', 4) == 0);
    CHECK(a.call("_sys_memchr", 0, 'a', 4) == 0);
    CHECK(a.call("_sys_memchr", kStr, 'a', 0) == 0);
    CHECK(a.call("_sys_memchr", kStr, 'a', -1) == 0);

    // Scans stop where RPCS3's would: what is found before the end of guest memory is found.
    b[edge - 1] = 'x';
    CHECK(a.call("_sys_memchr", edge - 4, 'x', 100) == edge - 1);
    std::memcpy(b + edge - 4, "abcd", 4);
    put(b, kStr, "abcz");
    CHECK(a.call("_sys_memcmp", edge - 4, kStr, 100) == kMinus1); // 'd' < 'z' before the edge
    CHECK(logs.count(log::Level::Error, "hle", "_sys_mem") == 0);

    // Past it: refused, logged, nothing written.
    CHECK(a.call("_sys_memchr", edge - 4, 'y', 100) == 0);
    put(b, kStr, "abcd");
    CHECK(a.call("_sys_memcmp", edge - 4, kStr, 100) == 0);
    CHECK(a.call("_sys_memset", edge - 4, 0x55, 8) == edge - 4);
    CHECK(b[edge - 4] == 'a');
    std::memset(b + kOut, 0, 16);
    CHECK(a.call("_sys_memcpy", kOut, edge - 4, 8) == kOut);
    CHECK(a.call("_sys_memmove", edge - 4, kOut, 8) == edge - 4);
    CHECK(b[kOut] == 0);
    CHECK(b[edge - 4] == 'a');
    CHECK(logs.count(log::Level::Error, "hle", "_sys_memchr(") == 1);
    CHECK(logs.count(log::Level::Error, "hle", "_sys_memcmp(") == 1);
    CHECK(logs.count(log::Level::Error, "hle", "_sys_memset(") == 1);
    CHECK(logs.count(log::Level::Error, "hle", "_sys_memcpy(") == 1);
    CHECK(logs.count(log::Level::Error, "hle", "_sys_memmove(") == 1);
}

TEST_CASE("_sys_str*: lengths, comparisons, copies and searches as RPCS3", "[hle][libc]")
{
    LogLines logs;
    Fixture f;
    EdgePage page(f);
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    const u32 edge = EdgePage::edge();

    put(b, kStr, "hello");
    CHECK(a.call("_sys_strlen", kStr) == 5);
    CHECK(a.call("_sys_strlen", 0) == 0); // RPCS3: 0 for NULL
    std::memset(b + edge - 4, 'q', 4);    // no NUL before the end of guest memory
    CHECK(a.call("_sys_strlen", edge - 4) == 0);
    CHECK(logs.count(log::Level::Error, "hle", "_sys_strlen(") == 1);

    // Comparisons: unsigned bytes, -1, 0 or 1.
    const auto cmp = [&](std::string_view fn, std::string_view s1, std::string_view s2, auto... n) {
        put(b, kStr, s1);
        put(b, kStr2, s2);
        return as_s32(a.call(fn, kStr, kStr2, static_cast<u64>(n)...));
    };
    CHECK(cmp("_sys_strcmp", "abc", "abd") == -1);
    CHECK(cmp("_sys_strcmp", "abd", "abc") == 1);
    CHECK(cmp("_sys_strcmp", "abc", "abc") == 0);
    CHECK(cmp("_sys_strcmp", "ab", "abc") == -1);
    CHECK(cmp("_sys_strcmp", "\x80", "a") == 1);
    CHECK(cmp("_sys_strncmp", "abcX", "abcY", 3) == 0);
    CHECK(cmp("_sys_strncmp", "abcX", "abcY", 4) == -1);
    CHECK(cmp("_sys_strncmp", "abc", "abc", 100) == 0);
    CHECK(cmp("_sys_strncmp", "a", "b", 0) == 0);
    CHECK(cmp("_sys_strncasecmp", "HeLLo", "hello", 5) == 0);
    CHECK(cmp("_sys_strncasecmp", "ABC", "abd", 3) == -1);
    CHECK(cmp("_sys_strncasecmp", "Zebra", "apple", 5) == 1);
    CHECK(cmp("_sys_strncasecmp", "a[", "A{", 2) == -1); // '[' and '{' are no letters
    CHECK(cmp("_sys_strncasecmp", "a\xC0", "A\xE0", 2) == -1); // past 127: as they are (RPCS3 aborts)
    CHECK(cmp("_sys_strncasecmp", "abcX", "ABCY", 3) == 0);

    // Copies return the destination.
    put(b, kStr, "copy");
    CHECK(a.call("_sys_strcpy", kOut, kStr) == kOut);
    CHECK(cstr(b, kOut) == "copy");
    std::memset(b + kOut, 'X', 8);
    put(b, kStr, "ab");
    CHECK(a.call("_sys_strncpy", kOut, kStr, 5) == kOut); // the rest of the 5 bytes NULs
    CHECK(std::memcmp(b + kOut, "ab\0\0\0XXX", 8) == 0);
    std::memset(b + kOut, 'X', 8);
    put(b, kStr, "abcdef");
    CHECK(a.call("_sys_strncpy", kOut, kStr, 3) == kOut); // no NUL when the string is longer
    CHECK(std::memcmp(b + kOut, "abcXXXXX", 8) == 0);
    CHECK(a.call("_sys_strncpy", 0, kStr, 3) == 0); // RPCS3: NULL when either pointer is
    CHECK(a.call("_sys_strncpy", kOut, 0, 3) == 0);
    CHECK(a.call("_sys_strncpy", kOut, kStr, -1) == kOut);
    CHECK(std::memcmp(b + kOut, "abcXXXXX", 8) == 0);

    put(b, kOut, "foo");
    put(b, kStr, "bar");
    CHECK(a.call("_sys_strcat", kOut, kStr) == kOut);
    CHECK(cstr(b, kOut) == "foobar");
    put(b, kStr, "bazqux");
    std::memset(b + kOut + 7, 'X', 8);
    CHECK(a.call("_sys_strncat", kOut, kStr, 3) == kOut); // three bytes, then a NUL
    CHECK(cstr(b, kOut) == "foobarbaz");
    put(b, kStr, "!");
    CHECK(a.call("_sys_strncat", kOut, kStr, 5) == kOut);
    CHECK(cstr(b, kOut) == "foobarbaz!");

    // Searches: the first or last byte, the NUL itself for 0, or NULL.
    put(b, kStr, "hello");
    CHECK(a.call("_sys_strchr", kStr, 'l') == kStr + 2);
    CHECK(a.call("_sys_strchr", kStr, 0x16C) == kStr + 2); // the low byte
    CHECK(a.call("_sys_strchr", kStr, 'z') == 0);
    CHECK(a.call("_sys_strchr", kStr, 0) == kStr + 5);
    CHECK(a.call("_sys_strrchr", kStr, 'l') == kStr + 3);
    CHECK(a.call("_sys_strrchr", kStr, 'z') == 0);
    CHECK(a.call("_sys_strrchr", kStr, 0) == kStr + 5);

    // Strings that run out of guest memory: refused and logged, nothing written.
    put(b, kOut, "keep");
    CHECK(a.call("_sys_strcpy", kOut, edge - 4) == kOut);
    CHECK(a.call("_sys_strcat", kOut, edge - 4) == kOut);
    CHECK(cstr(b, kOut) == "keep");
    CHECK(a.call("_sys_strchr", edge - 4, 'z') == 0);
    CHECK(a.call("_sys_strcmp", edge - 4, edge - 4) == 0);
    CHECK(logs.count(log::Level::Error, "hle", "_sys_strcpy(") == 1);
    CHECK(logs.count(log::Level::Error, "hle", "_sys_strcat(") == 1);
    CHECK(logs.count(log::Level::Error, "hle", "_sys_strchr(") == 1);
    CHECK(logs.count(log::Level::Error, "hle", "_sys_strcmp(") == 1);
    // ... but not when the answer comes first.
    CHECK(a.call("_sys_strchr", edge - 4, 'q') == edge - 4);
    CHECK(a.call("_sys_strncmp", edge - 4, edge - 4, 4) == 0);
}

TEST_CASE("__sys_look_ctype_table, _sys_tolower, _sys_toupper: RPCS3's table", "[hle][libc]")
{
    LogLines logs;
    Fixture f;
    GuestThread a(f, 0, "a");
    CHECK(a.call("__sys_look_ctype_table", 'A') == 0x41); // upper, hex
    CHECK(a.call("__sys_look_ctype_table", 'g') == 2);
    CHECK(a.call("__sys_look_ctype_table", 'f') == 0x42);
    CHECK(a.call("__sys_look_ctype_table", '5') == 4);
    CHECK(a.call("__sys_look_ctype_table", '\t') == 0x408);
    CHECK(a.call("__sys_look_ctype_table", '\n') == 8);
    CHECK(a.call("__sys_look_ctype_table", ' ') == 0x18);
    CHECK(a.call("__sys_look_ctype_table", '~') == 0x10);
    CHECK(a.call("__sys_look_ctype_table", 0x7F) == 0x20);
    CHECK(a.call("__sys_look_ctype_table", -1) == 0); // EOF
    CHECK(a.call("_sys_tolower", 'A') == 'a');
    CHECK(a.call("_sys_tolower", 'Z') == 'z');
    CHECK(a.call("_sys_tolower", 'a') == 'a');
    CHECK(a.call("_sys_tolower", '[') == '[');
    CHECK(a.call("_sys_tolower", -1) == kMinus1);
    CHECK(a.call("_sys_toupper", 'z') == 'Z');
    CHECK(a.call("_sys_toupper", 'Z') == 'Z');
    CHECK(a.call("_sys_toupper", '{') == '{');

    // Outside -1..127 (RPCS3 aborts): no class, no case; a warning the first time.
    CHECK(a.call("__sys_look_ctype_table", 200) == 0);
    CHECK(a.call("_sys_tolower", 200) == 200);
    CHECK(as_s32(a.call("_sys_toupper", -2)) == -2);
    CHECK(logs.count(log::Level::Warn, "sysPrxForUser", "not in the table") == 1);
}

// ------------------------------------------------------------------------------------------------- malloc, heaps

TEST_CASE("_sys_malloc, _sys_memalign, _sys_free: the main area, 64 KB aligned, as RPCS3's vm::alloc", "[hle][libc]")
{
    LogLines logs;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    const u32 p = static_cast<u32>(a.call("_sys_malloc", 0x100));
    REQUIRE(p != 0);
    CHECK(p % 0x10000 == 0);
    REQUIRE(f.mem.is_committed(p));
    CHECK(b[p] == 0); // zeroed
    b[p + 0xFF] = 1;
    CHECK(a.call("_sys_malloc", 0) == 0);

    const u32 q = static_cast<u32>(a.call("_sys_memalign", 0x100000, 0x20));
    REQUIRE(q != 0);
    CHECK(q % 0x100000 == 0);
    const u32 r = static_cast<u32>(a.call("_sys_memalign", 0x10, 0x20)); // raised to 64 KB
    REQUIRE(r != 0);
    CHECK(r % 0x10000 == 0);
    CHECK(a.call("_sys_memalign", 0x30000, 0x20) == 0); // not a power of two: refused (RPCS3 aborts)
    CHECK(logs.count(log::Level::Error, "hle", "_sys_memalign(size 0x20): alignment 0x30000") == 1);

    a.call("_sys_free", p);
    CHECK_FALSE(f.mem.is_committed(p));
    CHECK(a.call("_sys_malloc", 0x100) == p); // first fit again
    a.call("_sys_free", 0);                    // nothing
    CHECK(logs.count(log::Level::Error, "hle", "not an allocation") == 0);
    a.call("_sys_free", q);
    a.call("_sys_free", r);
}

TEST_CASE("_sys_heap_*: heap IDs as idm hands them out; allocations from the main area", "[hle][libc]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    put(b, kStr, "myheap");
    CHECK(a.call("_sys_heap_create_heap", kStr, 1, 2, 3) == 1);
    CHECK(a.call("_sys_heap_create_heap", 0, 0, 0, 0) == 2);
    CHECK(a.call("_sys_heap_delete_heap", 1) == 0);
    CHECK(a.call("_sys_heap_delete_heap", 77) == 0);           // any ID: CELL_OK (RPCS3)
    CHECK(a.call("_sys_heap_create_heap", kStr, 0, 0, 0) == 3); // the next ID, not the one freed

    const u32 m = static_cast<u32>(a.call("_sys_heap_malloc", 2, 0x40));
    REQUIRE(m != 0);
    CHECK(m % 0x10000 == 0);
    const u32 n = static_cast<u32>(a.call("_sys_heap_memalign", 2, 0x200000, 0x40));
    REQUIRE(n != 0);
    CHECK(n % 0x200000 == 0);
    CHECK(a.call("_sys_heap_free", 2, m) == 0);
    CHECK_FALSE(f.mem.is_committed(m));
    CHECK(a.call("_sys_heap_free", 99, n) == 0); // whichever heap is named
    CHECK_FALSE(f.mem.is_committed(n));

    // After 1023 IDs, the lowest free one, then 0 when none is.
    for (u32 id = 4; id <= 1023; id++) REQUIRE(a.call("_sys_heap_create_heap", kStr, 0, 0, 0) == id);
    CHECK(a.call("_sys_heap_create_heap", kStr, 0, 0, 0) == 1);
    CHECK(a.call("_sys_heap_create_heap", kStr, 0, 0, 0) == 0);
}

// ------------------------------------------------------------------------------------------------- printf

TEST_CASE("_sys_sprintf: the conversions, flags, widths, precisions and lengths RPCS3's formatter has", "[hle][libc]")
{
    LogLines logs;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();

    CHECK(sprintf_(a, "%d %i %u", -5, 42, 7) == "-5 42 7");
    CHECK(sprintf_(a, "%d", 0xFFFFFFFBull) == "-5"); // an int: the slot's low word (RPCS3 takes the whole slot)
    CHECK(sprintf_(a, "%u", -1) == "4294967295");
    CHECK(sprintf_(a, "%lu %zu %ju %llu", -1, -1, -1, -1) ==
        "4294967295 4294967295 18446744073709551615 18446744073709551615"); // long and size_t are 4 bytes
    CHECK(sprintf_(a, "%lld %ld", 0x8000000000000000ull, 0x1FFFFFFFFull) == "-9223372036854775808 -1");
    CHECK(sprintf_(a, "%hd %hhd %hu %hhu", 0x18000, 0x1FF, 0x18000, 0x1FF) == "-32768 -1 32768 255");
    CHECK(sprintf_(a, "[%5d|%-5d|%05d|%+d|% d|%+05d]", 42, 42, 42, 7, 7, -7) == "[   42|42   |00042|+7| 7|-0007]");
    CHECK(sprintf_(a, "[%.3d|%.0d|%.0d|%5.3d|%-6.2d|%05.1d]", 7, 0, 5, 7, 3, 4) == "[007||5|  007|03    |    4]");
    CHECK(sprintf_(a, "[%x|%X|%#x|%#X|%#x|%08x|%#08x|%.4x|%-#6x]", 255, 255, 255, 255, 0, 0xABC, 0xABC, 0x1F, 10) ==
        "[ff|FF|0xff|0XFF|0|00000abc|0x000abc|001f|0xa   ]");
    CHECK(sprintf_(a, "[%o|%#o|%#o|%#.3o|%#.0o]", 8, 8, 0, 8, 0) == "[10|010|0|010|0]");
    CHECK(sprintf_(a, "%llx %hx %hhx", 0x123456789ABCull, 0x12345, 0x1FF) == "123456789abc 2345 ff");
    CHECK(sprintf_(a, "%b|%5b|%B", 5, 2, 0) == "101|   10|0"); // RPCS3's binary

    CHECK(sprintf_(a, "[%c%c%c|%3c|%-3c]", 'a', 'b', 0x163, 'x', 'y') == "[abc|  x|y  ]");
    put(b, kStr, "hello");
    CHECK(sprintf_(a, "[%s|%10s|%-10s|%.2s|%s]", kStr, kStr, kStr, kStr, 0) ==
        "[hello|     hello|hello     |he|(null)]");
    CHECK(sprintf_(a, "%p|%20p|%-18p|", 0x10000, 0xD0001234, 1) ==
        "0000000000010000|    00000000d0001234|0000000000000001  |"); // RPCS3's: 16 digits, no 0x
    CHECK(sprintf_(a, "%%|100%%") == "%|100%");
    CHECK(sprintf_(a, "[%*d|%-*d|%.*d|%*d|%.*d]", 5, 42, 5, 42, 3, 7, -4, 1, -1, 9) == "[   42|42   |007|1   |9]");

    // Floating point: the host's printf on the double in the argument's slot.
    CHECK(sprintf_(a, "%f|%.2f|%e|%g|%8.3f|%-8.1f|%lf|%E|%G", bits(1.5), bits(3.14159), bits(12345.678), bits(0.0001),
              bits(2.5), bits(2.5), bits(2.0), bits(0.5), bits(1e-10)) ==
        "1.500000|3.14|1.234568e+04|0.0001|   2.500|2.5     |2.000000|5.000000E-01|1E-10");
    CHECK(sprintf_(a, "%*.*f|%+.1e|%08.2f|%a", 6, 2, bits(1.0), bits(-0.5), bits(-3.25), bits(1.0)) ==
        "  1.00|-5.0e-01|-0003.25|0x1p+0");
    CHECK(sprintf_(a, "%d %f %d", 1, bits(0.25), 2) == "1 0.250000 2"); // a double takes one slot, as an integer does

    // Arguments past r10: from the caller's parameter save area (r1 + 0x30 + 8n).
    CHECK(sprintf_(a, "%d %d %d %d %d %d %d %d %d %d %s", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, kStr) ==
        "1 2 3 4 5 6 7 8 9 10 hello");

    // A sequence RPCS3 does not know is printed as written, with the rest of the format untouched.
    CHECK(sprintf_(a, "a%yb%dc", 5) == "a%yb%dc");
    CHECK(sprintf_(a, "%d %Lf %d", 1, bits(1.0), 3) == "1 %Lf %d");
    CHECK(sprintf_(a, "%d%n", 1, kOut) == "1%n");
    CHECK(sprintf_(a, "%d %hld", 1, 2) == "1 %hld"); // two lengths
    CHECK(sprintf_(a, "%5*d", 3, 4) == "%5*d");      // a second width
    CHECK(sprintf_(a, "%lc", 'x') == "%lc");
    CHECK(sprintf_(a, "100%") == "100%"); // unfinished
    CHECK(sprintf_(a, "50%-") == "50%-");
    CHECK(logs.count(log::Level::Error, "hle", "_sys_sprintf") == 0);

    // A %s that is not guest memory prints nothing, logged.
    CHECK(sprintf_(a, "[%s]", kUnmapped) == "[]");
    CHECK(logs.count(log::Level::Error, "hle", "the %s argument 0xfff0000 is not guest memory") == 1);

    // The format or the buffer not guest memory: CELL_EFAULT.
    CHECK(a.call("_sys_sprintf", kOut, 0) == r3_of(lv2::CELL_EFAULT));
    CHECK(a.call("_sys_sprintf", kOut, kUnmapped) == r3_of(lv2::CELL_EFAULT));
    put(b, kFmt, "text");
    CHECK(a.call("_sys_sprintf", kUnmapped, kFmt) == r3_of(lv2::CELL_EFAULT));
}

TEST_CASE("_sys_snprintf: RPCS3's count rules; _sys_printf to the TTY", "[hle][libc]")
{
    LogLines logs;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    put(b, kFmt, "hello %s");
    put(b, kStr, "world");
    std::memset(b + kOut, 0x7E, 0x40);
    CHECK(a.call("_sys_snprintf", kOut, 0, kFmt, kStr) == 0); // nothing written
    CHECK(b[kOut] == 0x7E);
    CHECK(a.call("_sys_snprintf", kOut, 6, kFmt, kStr) == 5); // the bytes written, not the text's length
    CHECK(cstr(b, kOut) == "hello");
    CHECK(a.call("_sys_snprintf", kOut, 11, kFmt, kStr) == 10);
    CHECK(cstr(b, kOut) == "hello worl");
    CHECK(a.call("_sys_snprintf", kOut, 12, kFmt, kStr) == 11);
    CHECK(a.call("_sys_snprintf", kOut, 100, kFmt, kStr) == 11);
    CHECK(cstr(b, kOut) == "hello world");
    CHECK(b[kOut + 12] == 0x7E);
    CHECK(a.call("_sys_snprintf", kOut, 1, kFmt, kStr) == 0);
    CHECK(b[kOut] == 0);
    CHECK(a.call("_sys_snprintf", kUnmapped, 4, kFmt, kStr) == r3_of(lv2::CELL_EFAULT));
    CHECK(a.call("_sys_snprintf", kOut, 4, 0) == r3_of(lv2::CELL_EFAULT));

    // _sys_printf: CELL_OK; the text on the TTY's channel 0, a line a log line.
    put(b, kFmt, "frame %d: %s\nsecond line\n");
    CHECK(a.call("_sys_printf", kFmt, 7, kStr) == 0);
    CHECK(logs.count(log::Level::Info, "tty", "frame 7: world") == 1);
    CHECK(logs.count(log::Level::Info, "tty", "second line") == 1);
    // A long text goes out in pieces, nothing lost, through no more than 1 KB of the caller's stack.
    const std::string long_text(3000, 'x');
    put(b, kStr2, long_text);
    put(b, kFmt, "%s");
    const usize before = logs.messages(log::Level::Info, "tty").size();
    CHECK(a.call("_sys_printf", kFmt, kStr2) == 0);
    std::string joined;
    const std::vector<std::string> tty = logs.messages(log::Level::Info, "tty");
    for (usize i = before; i < tty.size(); i++) joined += tty[i];
    CHECK(joined == long_text);
    CHECK(a.call("_sys_printf", 0) == r3_of(lv2::CELL_EFAULT));
}

// ------------------------------------------------------------------------------------------------- qsort

TEST_CASE("_sys_qsort: the guest comparator orders the array, elements moving whole", "[hle][libc]")
{
    InterpretMisses interpret;
    LogLines logs;
    Fixture f;
    EdgePage page(f);
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    write_compare_function(b, kCode, kOpd, kCounter);

    // Records {key, tag}: sorted by key, equal keys in their first order (this sort is stable).
    const s32 keys[] = {5, -3, 9, 0, 5, 2, -3, 7};
    for (u32 i = 0; i < 8; i++) {
        ps_write32(b, kArr + 8 * i, static_cast<u32>(keys[i]));
        ps_write32(b, kArr + 8 * i + 4, i);
    }
    a.call("_sys_qsort", kArr, 8, 8, kOpd);
    const std::pair<s32, u32> sorted[] = {{-3, 1}, {-3, 6}, {0, 3}, {2, 5}, {5, 0}, {5, 4}, {7, 7}, {9, 2}};
    for (u32 i = 0; i < 8; i++) {
        INFO(i);
        CHECK(static_cast<s32>(ps_read32(b, kArr + 8 * i)) == sorted[i].first);
        CHECK(ps_read32(b, kArr + 8 * i + 4) == sorted[i].second);
    }
    const u32 comparisons = ps_read32(b, kCounter);
    CHECK(comparisons > 0);
    CHECK(comparisons <= 24); // n log2 n

    // 500 words.
    std::vector<s32> values(500);
    for (u32 i = 0; i < values.size(); i++) {
        values[i] = static_cast<s32>((i * 7919u) % 1000u) - 500;
        ps_write32(b, kArr + 4 * i, static_cast<u32>(values[i]));
    }
    a.call("_sys_qsort", kArr, 500, 4, kOpd);
    std::sort(values.begin(), values.end());
    bool same = true;
    for (u32 i = 0; i < values.size(); i++) same &= static_cast<s32>(ps_read32(b, kArr + 4 * i)) == values[i];
    CHECK(same);

    // Nothing to sort: the comparator is not called.
    ps_write32(b, kCounter, 0);
    a.call("_sys_qsort", kArr, 1, 8, kOpd);
    a.call("_sys_qsort", kArr, 0, 8, kOpd);
    a.call("_sys_qsort", kArr, 8, 0, kOpd);
    CHECK(ps_read32(b, kCounter) == 0);

    // An array past guest memory, or a comparator that is not guest memory: refused, logged, nothing moved.
    const u32 edge = EdgePage::edge();
    ps_write32(b, edge - 8, 2);
    ps_write32(b, edge - 4, 1);
    a.call("_sys_qsort", edge - 8, 4, 4, kOpd);
    CHECK(ps_read32(b, edge - 8) == 2);
    a.call("_sys_qsort", edge - 8, 2, 4, kUnmapped);
    CHECK(ps_read32(b, edge - 8) == 2);
    CHECK(ps_read32(b, kCounter) == 0);
    CHECK(logs.count(log::Level::Error, "hle", "_sys_qsort(") == 2);
    a.call("_sys_qsort", edge - 8, 2, 4, kOpd); // the same two words, all in guest memory
    CHECK(ps_read32(b, edge - 8) == 1);
    CHECK(ps_read32(b, edge - 4) == 2);
}

// ------------------------------------------------------------------------------------------------- console

TEST_CASE("console_putc, console_write: the TTY's channel 0", "[hle][libc]")
{
    LogLines logs;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    a.call("console_putc", 0x151); // the low byte: 'Q'
    put(b, kStr, "console line\n");
    CHECK(a.call("console_write", kStr, 13) == 0);
    const std::vector<std::string> tty = logs.messages(log::Level::Info, "tty");
    REQUIRE(tty.size() == 2);
    CHECK(tty[0] == "Q");
    CHECK(tty[1] == "console line");
    CHECK(a.call("console_write", kUnmapped, 4) == 0); // CELL_OK whatever the write returned, as RPCS3
    CHECK(logs.count(log::Level::Error, "hle", "console_write(0xfff0000, 4)") == 1);
}

// ------------------------------------------------------------------------------------------------- lwcond

namespace {

constexpr u32 kLwAttr = kData + 0x2000, kLw = kData + 0x2100, kCondAttr = kData + 0x2200, kCond = kData + 0x2300;
constexpr u32 kLwFree = 0xFFFFFFFF, kLwReserved = 0xFFFFFFFD;

void create_lwcond(GuestThread& a, u32 lw, u32 cond, u32 protocol, u32 recursive)
{
    u8* b = a.f.base();
    ps_write32(b, kLwAttr + 0, protocol);
    ps_write32(b, kLwAttr + 4, recursive);
    std::memcpy(b + kLwAttr + 8, "testlwm", 8);
    std::memcpy(b + kCondAttr, "testlwc", 8);
    REQUIRE(a.call("sys_lwmutex_create", lw, kLwAttr) == 0);
    REQUIRE(a.call("sys_lwcond_create", cond, lw, kCondAttr) == 0);
}

u32 owner_of(u8* b, u32 lw) { return ps_read32(b, lw); }
u32 waiters_of(u8* b, u32 lw) { return ps_read32(b, lw + 4); }

// A guest thread on a host thread of its own that locks the lwmutex (1 + relocks times), waits on the lwcond, notes the
// result and the lwmutex's owner and recursion count right after, and unlocks it again.
struct CondWaiter {
    std::atomic<lv2::PpuThread*> t{nullptr};
    std::atomic<u32> id{0};
    std::atomic<u64> result{~0ull};
    std::atomic<u32> owner_after{0}, recursion_after{0};
    HostThread host;

    CondWaiter(Fixture& f, u32 index, u32 lw, u32 cond, u32 relocks)
        : host(f, [this, &f, index, lw, cond, relocks] {
              GuestThread g(f, index, "waiter");
              for (u32 k = 0; k <= relocks; k++) call_quiet(g, "sys_lwmutex_lock", lw, 0);
              id = g.t->id;
              t = g.t;
              const u64 r = call_quiet(g, "sys_lwcond_wait", cond, 0);
              owner_after = ps_read32(f.base(), lw);
              recursion_after = ps_read32(f.base(), lw + 0xC);
              result = r;
              for (u32 k = 0; k <= relocks; k++) call_quiet(g, "sys_lwmutex_unlock", lw);
              t = nullptr;
          })
    {
    }
    bool done() const { return result.load() != ~0ull; }
};

} // namespace

TEST_CASE("sys_lwcond: create, destroy, waits that time out, signals with nobody waiting", "[hle][lwcond]")
{
    LogLines logs;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    create_lwcond(a, kLw, kCond, 2 /* PRIORITY */, 0x10 /* RECURSIVE */);
    CHECK(ps_read32(b, kCond) == kLw);
    CHECK(ps_read32(b, kCond + 4) >> 24 == 0x97); // the kernel's lwcond ID
    CHECK(a.call("sys_lwcond_create", kCond + 0x10, kUnmapped, kCondAttr) == r3_of(lv2::CELL_EFAULT));

    CHECK(a.call("sys_lwcond_wait", kCond, 0) == r3_of(lv2::CELL_EPERM)); // the lwmutex is not the caller's

    // Owned with a recursion count of 1: the wait times out and gives the lwmutex back with its count.
    REQUIRE(a.call("sys_lwmutex_lock", kLw, 0) == 0);
    REQUIRE(a.call("sys_lwmutex_lock", kLw, 0) == 0);
    CHECK(a.call("sys_lwcond_wait", kCond, 2000) == r3_of(lv2::CELL_ETIMEDOUT));
    CHECK(owner_of(b, kLw) == a.t->id);
    CHECK(ps_read32(b, kLw + 0xC) == 1);
    CHECK(waiters_of(b, kLw) == 0);

    // The owner signals nobody: signal and signal_all say CELL_OK, signal_to says what the kernel does.
    CHECK(a.call("sys_lwcond_signal", kCond) == 0);
    CHECK(a.call("sys_lwcond_signal_all", kCond) == 0);
    CHECK(a.call("sys_lwcond_signal_to", kCond, a.t->id) == r3_of(lv2::CELL_EPERM)); // not waiting
    CHECK(a.call("sys_lwcond_signal_to", kCond, 0x01FFFFFF) == r3_of(lv2::CELL_ESRCH)); // no such thread
    CHECK(waiters_of(b, kLw) == 0);
    CHECK(owner_of(b, kLw) == a.t->id);
    REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);
    REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);

    // A free lwmutex is taken around the signal (mode 3), and given back when nobody waits.
    CHECK(a.call("sys_lwcond_signal", kCond) == 0);
    CHECK(owner_of(b, kLw) == kLwFree);
    CHECK(a.call("sys_lwcond_signal_all", kCond) == 0);
    CHECK(a.call("sys_lwcond_signal_to", kCond, a.t->id) == r3_of(lv2::CELL_EPERM));
    CHECK(owner_of(b, kLw) == kLwFree);
    CHECK(waiters_of(b, kLw) == 0);

    // Destroyed: the queue ID dead; a second destroy and a wait find no kernel object (the wait gives the lwmutex
    // back).
    REQUIRE(a.call("sys_lwcond_destroy", kCond) == 0);
    CHECK(ps_read32(b, kCond + 4) == 0xFFFFFFFE);
    CHECK(a.call("sys_lwcond_destroy", kCond) == r3_of(lv2::CELL_ESRCH));
    REQUIRE(a.call("sys_lwmutex_lock", kLw, 0) == 0);
    CHECK(a.call("sys_lwcond_wait", kCond, 0) == r3_of(lv2::CELL_ESRCH));
    CHECK(owner_of(b, kLw) == a.t->id);
    REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);

    // Not guest memory: refused, logged.
    CHECK(a.call("sys_lwcond_destroy", kUnmapped) == r3_of(lv2::CELL_EFAULT));
    CHECK(a.call("sys_lwcond_signal", kUnmapped) == r3_of(lv2::CELL_EFAULT));
    ps_write32(b, kCond + 0x10, kLw + 4); // an lwmutex that is not 8-byte aligned
    CHECK(a.call("sys_lwcond_wait", kCond + 0x10, 0) == r3_of(lv2::CELL_EFAULT));
    CHECK(logs.count(log::Level::Error, "hle", "not guest memory; CELL_EFAULT") == 4);
}

TEST_CASE("sys_lwcond: waiters on other threads, woken by signal, signal_all and signal_to", "[hle][lwcond]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    create_lwcond(a, kLw, kCond, 2, 0x10);

    SECTION("the owner signals: the waiter gets the lwmutex, with its recursion count, when the owner unlocks")
    {
        CondWaiter w(f, 1, kLw, kCond, 1);
        REQUIRE(parked(w.t, lv2::WaitKind::LwCond));
        CHECK(owner_of(b, kLw) == kLwReserved);
        REQUIRE(a.call("sys_lwmutex_lock", kLw, 0) == 0); // given up by the waiter
        CHECK(owner_of(b, kLw) == a.t->id);
        REQUIRE(a.call("sys_lwcond_signal", kCond) == 0);
        CHECK(waiters_of(b, kLw) == 1); // now waiting for the lwmutex
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK_FALSE(w.done());
        REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);
        REQUIRE(wait_for([&] { return w.done(); }));
        w.host.join();
        CHECK(w.result == 0);
        CHECK(w.owner_after == w.id);
        CHECK(w.recursion_after == 1);
    }
    SECTION("signal_all wakes every waiter, one owner at a time")
    {
        CondWaiter w1(f, 1, kLw, kCond, 0);
        REQUIRE(parked(w1.t, lv2::WaitKind::LwCond));
        CondWaiter w2(f, 2, kLw, kCond, 0);
        REQUIRE(parked(w2.t, lv2::WaitKind::LwCond));
        REQUIRE(a.call("sys_lwmutex_lock", kLw, 0) == 0);
        REQUIRE(a.call("sys_lwcond_signal_all", kCond) == 0);
        CHECK(waiters_of(b, kLw) == 2);
        REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);
        REQUIRE(wait_for([&] { return w1.done() && w2.done(); }));
        w1.host.join();
        w2.host.join();
        CHECK(w1.result == 0);
        CHECK(w2.result == 0);
        CHECK(w1.owner_after == w1.id);
        CHECK(w2.owner_after == w2.id);
    }
    SECTION("signal_to wakes the thread named, and only it")
    {
        CondWaiter w1(f, 1, kLw, kCond, 0);
        REQUIRE(parked(w1.t, lv2::WaitKind::LwCond));
        CondWaiter w2(f, 2, kLw, kCond, 0);
        REQUIRE(parked(w2.t, lv2::WaitKind::LwCond));
        REQUIRE(a.call("sys_lwmutex_lock", kLw, 0) == 0);
        REQUIRE(a.call("sys_lwcond_signal_to", kCond, w2.id.load()) == 0);
        REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);
        REQUIRE(wait_for([&] { return w2.done(); }));
        w2.host.join();
        CHECK(w2.result == 0);
        CHECK_FALSE(w1.done());
        REQUIRE(a.call("sys_lwmutex_lock", kLw, 0) == 0);
        REQUIRE(a.call("sys_lwcond_signal_to", kCond, w1.id.load()) == 0);
        REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);
        REQUIRE(wait_for([&] { return w1.done(); }));
        w1.host.join();
        CHECK(w1.result == 0);
    }
    SECTION("a signal from a thread without the lwmutex hands it to the waiter (mode 3)")
    {
        CondWaiter w(f, 1, kLw, kCond, 0);
        REQUIRE(parked(w.t, lv2::WaitKind::LwCond));
        REQUIRE(a.call("sys_lwcond_signal", kCond) == 0);
        REQUIRE(wait_for([&] { return w.done(); }));
        w.host.join();
        CHECK(w.result == 0);
        CHECK(w.owner_after == w.id);
    }
    SECTION("a signal while another thread holds the lwmutex: the waiter locks it again itself (mode 2)")
    {
        CondWaiter w(f, 1, kLw, kCond, 0);
        REQUIRE(parked(w.t, lv2::WaitKind::LwCond));
        std::atomic<bool> holding{false}, release{false};
        std::atomic<u32> holder_id{0};
        HostThread holder(f, [&] {
            GuestThread h(f, 2, "holder");
            call_quiet(h, "sys_lwmutex_lock", kLw, 0);
            holder_id = h.t->id;
            holding = true;
            for (int i = 0; i < 10000 && !release; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            call_quiet(h, "sys_lwmutex_unlock", kLw);
        });
        REQUIRE(wait_for([&] { return holding.load(); }));
        CHECK(owner_of(b, kLw) == holder_id);
        REQUIRE(a.call("sys_lwcond_signal", kCond) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK_FALSE(w.done()); // woken, now waiting for the holder
        release = true;
        holder.join();
        REQUIRE(wait_for([&] { return w.done(); }));
        w.host.join();
        CHECK(w.result == 0); // CELL_EBUSY from the kernel becomes CELL_OK once locked
        CHECK(w.owner_after == w.id);
    }
    CHECK(owner_of(b, kLw) == kLwFree);
    CHECK(waiters_of(b, kLw) == 0);
    REQUIRE(a.call("sys_lwcond_destroy", kCond) == 0);
    REQUIRE(a.call("sys_lwmutex_destroy", kLw) == 0);
}

TEST_CASE("sys_lwcond: an lwmutex with the RETRY protocol is always signalled in mode 2", "[hle][lwcond]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    create_lwcond(a, kLw, kCond, 4 /* RETRY */, 0x20 /* NOT_RECURSIVE */);
    CondWaiter w(f, 1, kLw, kCond, 0);
    REQUIRE(parked(w.t, lv2::WaitKind::LwCond));
    REQUIRE(a.call("sys_lwmutex_lock", kLw, 0) == 0);
    REQUIRE(a.call("sys_lwcond_signal", kCond) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_FALSE(w.done()); // woken, waiting for the lwmutex
    REQUIRE(a.call("sys_lwmutex_unlock", kLw) == 0);
    REQUIRE(wait_for([&] { return w.done(); }));
    w.host.join();
    CHECK(w.result == 0);
    CHECK(w.owner_after == w.id);
    CHECK(owner_of(b, kLw) == kLwFree);
}

// ------------------------------------------------------------------------------------------------- mempool

TEST_CASE("sys_mempool: RPCS3's checks, blocks from the chunk's end, given back, counted, IDs", "[hle][mempool]")
{
    LogLines logs;
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    constexpr u32 kPool = kOut;
    constexpr u64 kEinvalU64 = 0x80010002; // CELL_EINVAL as sys_mempool_get_count's u64
    constexpr u32 kMutexObject = 0x85, kCondObject = 0x86;
    const auto objects = [&](u32 type) {
        REQUIRE(lv2::sys_process_get_number_of_object(*a.t, type, kOut + 0x100) == lv2::CELL_OK);
        return ps_read32(b, kOut + 0x100);
    };

    CHECK(a.call("sys_mempool_create", kPool, kChunk, 0x100, 0x200, 0) == r3_of(lv2::CELL_EINVAL)); // block > chunk
    CHECK(a.call("sys_mempool_create", kPool, kChunk, 0x400, 0x100, 3) == r3_of(lv2::CELL_EINVAL)); // alignment 3
    CHECK(a.call("sys_mempool_create", kPool, kChunk + 4, 0x400, 0x100, 0) == r3_of(lv2::CELL_EINVAL));
    CHECK(a.call("sys_mempool_create", kPool, kChunk, 0x400, 0, 0) == r3_of(lv2::CELL_EINVAL)); // RPCS3 divides by 0
    CHECK(a.call("sys_mempool_create", kUnmapped, kChunk, 0x400, 0x100, 0) == r3_of(lv2::CELL_EFAULT));
    CHECK(objects(kMutexObject) == 0);

    // Four blocks of 0x100 in 0x450 bytes, handed out from the end.
    REQUIRE(a.call("sys_mempool_create", kPool, kChunk, 0x450, 0x100, 2) == 0);
    const u32 pool = ps_read32(b, kPool);
    CHECK(pool == 1);
    CHECK(objects(kMutexObject) == 1);
    CHECK(objects(kCondObject) == 1);
    CHECK(a.call("sys_mempool_get_count", pool) == 4);
    CHECK(a.call("sys_mempool_allocate_block", pool) == kChunk + 0x300);
    CHECK(a.call("sys_mempool_try_allocate_block", pool) == kChunk + 0x200);
    CHECK(a.call("sys_mempool_get_count", pool) == 2);
    CHECK(a.call("sys_mempool_free_block", pool, kChunk + 0x300) == 0);
    CHECK(a.call("sys_mempool_allocate_block", pool) == kChunk + 0x300); // the one given back first
    CHECK(a.call("sys_mempool_allocate_block", pool) == kChunk + 0x100);
    CHECK(a.call("sys_mempool_allocate_block", pool) == kChunk);
    CHECK(a.call("sys_mempool_try_allocate_block", pool) == 0); // none left
    CHECK(a.call("sys_mempool_get_count", pool) == 0);
    // Only a block past the chunk's end is refused (RPCS3's one check).
    CHECK(a.call("sys_mempool_free_block", pool, kChunk + 0x451) == r3_of(lv2::CELL_EINVAL));
    CHECK(a.call("sys_mempool_free_block", pool, kChunk + 0x450) == 0);
    CHECK(a.call("sys_mempool_get_count", pool) == 1);

    // An ID that is no pool.
    CHECK(a.call("sys_mempool_get_count", 99) == kEinvalU64);
    CHECK(a.call("sys_mempool_allocate_block", 99) == 0);
    CHECK(a.call("sys_mempool_try_allocate_block", 99) == 0);
    CHECK(a.call("sys_mempool_free_block", 99, kChunk) == r3_of(lv2::CELL_EINVAL));

    // Many small blocks cost the host nothing: 256 MB of 1-byte blocks (the chunk is not touched).
    REQUIRE(a.call("sys_mempool_create", kPool, 0x20000000, 0x10000000, 1, 0) == 0);
    CHECK(ps_read32(b, kPool) == 2);
    CHECK(a.call("sys_mempool_get_count", 2) == 0x10000000);
    CHECK(a.call("sys_mempool_allocate_block", 2) == 0x2FFFFFFF);

    // Destroyed: the ID and the pool's mutex and condition variable go; the ID is not handed out again before the rest.
    a.call("sys_mempool_destroy", 1);
    CHECK(a.call("sys_mempool_get_count", 1) == kEinvalU64);
    CHECK(objects(kMutexObject) == 1);
    CHECK(objects(kCondObject) == 1);
    a.call("sys_mempool_destroy", 1);
    CHECK(logs.count(log::Level::Error, "sysPrxForUser", "sys_mempool_destroy(1): no such mempool") == 1);
    REQUIRE(a.call("sys_mempool_create", kPool, kChunk, 0x100, 0x100, 0) == 0);
    CHECK(ps_read32(b, kPool) == 3);
    a.call("sys_mempool_destroy", 2);
    a.call("sys_mempool_destroy", 3);
    CHECK(objects(kMutexObject) == 0);
    CHECK(objects(kCondObject) == 0);
}

TEST_CASE("sys_mempool_allocate_block waits for a block given back", "[hle][mempool]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    REQUIRE(a.call("sys_mempool_create", kOut, kChunk, 0x80, 0x80, 0) == 0);
    const u32 pool = ps_read32(b, kOut);
    REQUIRE(a.call("sys_mempool_allocate_block", pool) == kChunk);

    std::atomic<lv2::PpuThread*> waiter{nullptr};
    std::atomic<u64> got{~0ull};
    HostThread t(f, [&] {
        GuestThread w(f, 1, "w");
        waiter = w.t;
        got = call_quiet(w, "sys_mempool_allocate_block", pool);
    });
    REQUIRE(parked(waiter, lv2::WaitKind::Cond));
    CHECK(got == ~0ull);
    REQUIRE(a.call("sys_mempool_free_block", pool, kChunk) == 0);
    t.join();
    CHECK(got == kChunk);
    CHECK(a.call("sys_mempool_get_count", pool) == 0);
    a.call("sys_mempool_destroy", pool);
}

// ------------------------------------------------------------------------------------------------- threads, process

TEST_CASE("sys_ppu_thread_register_atexit: eight slots, RPCS3's errors, run by every sys_ppu_thread_exit",
    "[hle][thread]")
{
    InterpretMisses interpret;
    LogLines logs;
    Fixture f;
    u8* b = f.base();
    write_counter_function(b, kCode, kOpd, kCounter);
    write_counter_function(b, kCode2, kOpd2, kCounter2);
    GuestThread a(f, 0, "a");

    CHECK(a.call("sys_ppu_thread_unregister_atexit", kOpd) == r3_of(lv2::CELL_ESRCH));
    REQUIRE(a.call("sys_ppu_thread_register_atexit", kOpd) == 0);
    CHECK(a.call("sys_ppu_thread_register_atexit", kOpd) == r3_of(lv2::CELL_EPERM));
    CHECK(a.call("sys_ppu_thread_register_atexit", 0) == r3_of(lv2::CELL_EPERM)); // NULL matches a free slot
    for (u32 i = 1; i < 8; i++) REQUIRE(a.call("sys_ppu_thread_register_atexit", kUnmapped + 8 * i) == 0);
    CHECK(a.call("sys_ppu_thread_register_atexit", kOpd2) == r3_of(lv2::CELL_ENOMEM));
    for (u32 i = 2; i < 8; i++) REQUIRE(a.call("sys_ppu_thread_unregister_atexit", kUnmapped + 8 * i) == 0);
    REQUIRE(a.call("sys_ppu_thread_register_atexit", kOpd2) == 0);

    // An exiting thread runs them, skipping one that is not guest memory (logged).
    {
        GuestThread e(f, 1, "exiting");
        CHECK_THROWS_AS(e.call("sys_ppu_thread_exit", 7), lv2::ThreadExit);
    }
    CHECK(ps_read32(b, kCounter) == 1);
    CHECK(ps_read32(b, kCounter2) == 1);
    CHECK(logs.count(log::Level::Error, "hle", "the atexit function 0xfff0008 is not guest memory") == 1);

    REQUIRE(a.call("sys_ppu_thread_unregister_atexit", kOpd2) == 0);
    REQUIRE(a.call("sys_ppu_thread_unregister_atexit", kUnmapped + 8) == 0);
    {
        GuestThread e(f, 2, "exiting");
        CHECK_THROWS_AS(e.call("sys_ppu_thread_exit", 0), lv2::ThreadExit);
    }
    CHECK(ps_read32(b, kCounter) == 2);
    CHECK(ps_read32(b, kCounter2) == 1);
}

TEST_CASE("sys_mmapper_allocate_memory_from_container: lv2's checks and the container's room", "[hle][mmapper]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    constexpr u32 kId = kOut, kMem = kOut + 8;
    constexpr u64 k64K = 0x200; // SYS_MEMORY_PAGE_SIZE_64K
    REQUIRE(lv2::sys_memory_container_create(*a.t, kId, 0x100000) == lv2::CELL_OK);
    const u32 cid = ps_read32(b, kId);
    CHECK(a.call("sys_mmapper_allocate_memory_from_container", 0x8000, cid, k64K, kMem) == r3_of(lv2::CELL_EALIGN));
    CHECK(a.call("sys_mmapper_allocate_memory_from_container", 0x10000, cid, k64K | 0x8000, kMem) ==
        r3_of(lv2::CELL_EINVAL));
    CHECK(a.call("sys_mmapper_allocate_memory_from_container", 0x10000, 0x12345, k64K, kMem) == r3_of(lv2::CELL_ESRCH));
    REQUIRE(a.call("sys_mmapper_allocate_memory_from_container", 0x100000, cid, k64K, kMem) == 0);
    const u32 mem = ps_read32(b, kMem);
    CHECK(mem != 0);
    CHECK(a.call("sys_mmapper_allocate_memory_from_container", 0x10000, cid, k64K, kMem) == r3_of(lv2::CELL_ENOMEM));
    REQUIRE(a.call("sys_mmapper_free_memory", mem) == 0);
    CHECK(a.call("sys_mmapper_allocate_memory_from_container", 0x10000, cid, k64K, kMem) == 0);
}

TEST_CASE("sys_process_get_paramsfo: the title ID at +1 of 0x40 bytes, CELL_ENOENT without one", "[hle]")
{
    {
        lv2::Config config;
        config.title_id = "BLES01807";
        Fixture f({}, config);
        GuestThread a(f, 0, "a");
        u8* b = f.base();
        std::memset(b + kOut, 0xEE, 0x50);
        REQUIRE(a.call("sys_process_get_paramsfo", kOut) == 0);
        CHECK(b[kOut] == 0);
        CHECK(std::memcmp(b + kOut + 1, "BLES01807", 9) == 0);
        CHECK(b[kOut + 10] == 0);
        CHECK(b[kOut + 0x3F] == 0);
        CHECK(b[kOut + 0x40] == 0xEE);
        CHECK(a.call("sys_process_get_paramsfo", kUnmapped) == r3_of(lv2::CELL_EFAULT));
    }
    {
        Fixture f;
        GuestThread a(f, 0, "a");
        CHECK(a.call("sys_process_get_paramsfo", kOut) == r3_of(lv2::CELL_ENOENT));
    }
}

TEST_CASE("sys_get_random_number: up to 4096 bytes from the host's random source", "[hle]")
{
    Fixture f;
    GuestThread a(f, 0, "a");
    u8* b = f.base();
    std::memset(b + kOut, 0, 0x1100);
    CHECK(a.call("sys_get_random_number", kOut, 4097) == r3_of(lv2::CELL_EINVAL));
    CHECK(std::all_of(b + kOut, b + kOut + 0x1100, [](u8 v) { return v == 0; }));
    REQUIRE(a.call("sys_get_random_number", kOut, 4096) == 0);
    CHECK(std::count(b + kOut, b + kOut + 4096, 0) < 100); // about 16 of 4096 bytes are 0 by chance
    CHECK(b[kOut + 4096] == 0);
    b[kStr + 3] = 0xEE;
    REQUIRE(a.call("sys_get_random_number", kStr, 3) == 0);
    CHECK(b[kStr + 3] == 0xEE);
    CHECK(a.call("sys_get_random_number", 0, 0) == 0);
    CHECK(a.call("sys_get_random_number", kUnmapped, 16) == r3_of(lv2::CELL_EFAULT));
}

TEST_CASE("sys_game_process_exitspawn2: RPCS3's flags, and the atexitspawn or at_Exitspawn handler first", "[hle]")
{
    InterpretMisses interpret;
    constexpr u32 kPath = kStr;
    const std::string kEboot = "/dev_hdd0/game/TEST00000/USRDIR/EBOOT.BIN";
    // The block's address, from the error line that reports the request.
    const auto block_of = [](LogLines& logs, std::string_view what) {
        const std::string line = logs.find(log::Level::Error, "sysPrxForUser", what);
        const auto at = line.find("parameter block ");
        REQUIRE(at != std::string::npos);
        return static_cast<u32>(std::stoul(line.substr(at + 16), nullptr, 16));
    };
    struct Case {
        std::string_view function;
        u64 flags, block_flags;
        int handler; // 0 atexitspawn, 1 at_Exitspawn, -1 neither
    };
    const Case cases[] = {
        {"sys_game_process_exitspawn2", 0x12345, 0x40, 0},                         // top bits 0: kept
        {"sys_game_process_exitspawn2", (1ull << 62) | 0x1F0, (1ull << 62) | 0xF0, 1}, // top bits 1: kept
        {"sys_game_process_exitspawn2", (2ull << 62) | 0xFFF, 0xF0, 0},             // top bits 2: cleared
        {"sys_game_process_exitspawn2", (3ull << 62) | 0x30, 0x30, 0},
        {"sys_game_process_exitspawn", 0x12345, 0x40 | (1ull << 63), -1}, // bit 63 set: neither
    };
    for (const Case& c : cases) {
        INFO(c.function << " flags " << c.flags);
        LogLines logs;
        Fixture f;
        GuestThread a(f, 0, "a");
        u8* b = f.base();
        write_counter_function(b, kCode, kOpd, kCounter);
        write_counter_function(b, kCode2, kOpd2, kCounter2);
        a.call("_sys_process_atexitspawn", kOpd);
        a.call("_sys_process_at_Exitspawn", kOpd2);
        put(b, kPath, kEboot);
        CHECK_THROWS_AS(a.call(c.function, kPath, 0, 0, 0, 0, 1001, c.flags), lv2::ThreadExit);
        CHECK(f.kernel->exited());
        const u32 block = block_of(logs, std::string(c.function) + "(path \"" + kEboot + "\"");
        REQUIRE(block != 0);
        CHECK(ps_read64(b, block + 0x00) == 0x85);
        CHECK(ps_read64(b, block + 0x18) == 1001);
        CHECK(ps_read64(b, block + 0x20) == c.block_flags);
        CHECK(ps_read32(b, kCounter) == (c.handler == 0 ? 1u : 0u));
        CHECK(ps_read32(b, kCounter2) == (c.handler == 1 ? 1u : 0u));
    }
    {
        // No handler registered: the process just ends.
        LogLines logs;
        Fixture f;
        GuestThread a(f, 0, "a");
        put(f.base(), kPath, kEboot);
        CHECK_THROWS_AS(a.call("sys_game_process_exitspawn2", kPath, 0, 0, 0, 0, 1001, 0), lv2::ThreadExit);
        CHECK(f.kernel->exited());
        CHECK(logs.count(log::Level::Info, "sysPrxForUser", "handler") == 0);
    }
}

TEST_CASE("sys_libc: memcpy, memset, memmove and memcmp under their C names (RPCS3, D-184)", "[hle][libc]")
{
    Fixture f;
    GuestThread g(f, 0, "libc");
    u8* b = f.base();
    for (const char* name : {"memcpy", "memset", "memmove", "memcmp"}) {
        INFO(name);
        CHECK(GuestThread::find(name).module == "sys_libc");
    }
    const u32 a = kData + 0x100, c = kData + 0x200;
    std::memcpy(b + a, "abcdefgh", 8);
    CHECK(g.call("memcpy", c, a, 8) == c);
    CHECK(std::memcmp(b + c, "abcdefgh", 8) == 0);
    CHECK(g.call("memmove", a + 2, a, 6) == a + 2); // overlapping
    CHECK(std::memcmp(b + a, "ababcdef", 8) == 0);
    CHECK(g.call("memset", c, 'z', 3) == c);
    CHECK(std::memcmp(b + c, "zzzdefgh", 8) == 0);
    CHECK(g.call("memcmp", a, a, 8) == 0);
    CHECK(g.call("memcmp", a, c, 8) == 0xFFFFFFFFull); // 'a' < 'z': -1, zero-extended as RPCS3's u32
    CHECK(g.call("memcmp", c, a, 8) == 1);
}
