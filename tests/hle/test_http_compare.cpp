// cellHttp against libhttp.sprx (D-193's method, D-524): the same scripts of calls in two fresh processes, one calling
// pspocket's functions (runtime/hle/src/cellHttp*.cpp) and one Sony's module; every answer, every byte written to the
// scripts' memory, libhttp's pool (its heap, the same blocks at the same places) and the lv2 calls the script's thread
// makes the same. Both processes load libhttp (and libssl, which it imports) so memory is laid out alike. The console
// is offline (pspocket's lv2 network has only loopback). Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH); no game
// data.

#include <catch_amalgamated.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "cellHttp.hpp" // runtime/hle/src: where pspocket keeps libhttp's globals
#include "firmware_compare.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

constexpr u32 kOut = kData + 0x10000, kOutSize = 0x10000;    // the scripts' memory
constexpr u32 kStr = kData + 0x30000, kStrSize = 0x40000;    // strings and structures the scripts use
constexpr u32 kPool = kData + 0x100000, kPoolSize = 0x40000;         // cellHttpInit's pool
constexpr u32 kCookiePool = kData + 0x180000, kCookiePoolSize = 0x30000; // cellHttpInitCookie's
constexpr u8 kFill = 0xA5;

// Script flags.
constexpr u32 kMaskAnswer = 1; // the answer depends on the run
constexpr u32 kSign = 2;       // compared only as negative, 0 or positive

// An argument read when the call is made: the 32-bit word at the address.
constexpr u64 kDeref = u64{1} << 63;

// A call (its answer also stored at `store` when given). `fn` is a cellHttp function, or "ssl:" / "net:" and a
// cellSsl or sys_net one; "=" stores a 32-bit word (address, value), "s" a string (address, index into strings()),
// "pool" keeps the pool's bytes for comparing.
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
    // Where each library keeps its globals: Sony's module's segments, or pspocket's block.
    std::vector<std::pair<u32, u32>> globals;
};

struct Calls {
    std::mutex mutex;
    std::thread::id thread;
    bool on = false; // only while a cellHttp call runs
    std::vector<std::string> list;
};

bool stack_address(u64 v) { return (v >= kStacks && v < kStacks + 0x100000) || (v >= 0xD0000000 && v < 0xF0000000); }

// An lv2 object's ID (they differ between the runs: libssl makes objects in Sony's).
bool lv2_id(u32 v)
{
    const u32 top = v >> 24;
    return top == 0x85 || top == 0x86 || top == 0x88 || top == 0x8d || (top >= 0x95 && top <= 0x98);
}

// The arguments compared of each lv2 call libhttp makes.
u32 arg_count(u32 number)
{
    switch (number) {
    case 1: return 0;
    case 101: case 106: case 141: case 714: return 1;
    case 25: case 100: case 351: case 706: case 712: case 722: return 2;
    case 105: case 700: case 701: case 702: case 703: case 704: case 708: case 709: case 713: case 715: case 721: return 3;
    case 705: case 711: case 716: return 5;
    case 707: case 710: return 6;
    default: return 4;
    }
}

void observe(void* user, u32 number, const u64* a)
{
    auto* c = static_cast<Calls*>(user);
    if (std::this_thread::get_id() != c->thread || !c->on) return;
    std::string line = std::to_string(number);
    for (u32 i = 0; i < arg_count(number); i++) {
        const u32 v = static_cast<u32>(a[i]);
        line += stack_address(a[i]) ? std::string(" stack") : lv2_id(v) ? std::string(" id") : std::format(" {:#x}", a[i]);
    }
    std::lock_guard lock(c->mutex);
    c->list.push_back(line);
}

// Strings the scripts put into memory (by index).
const std::vector<std::string>& strings()
{
    static const std::vector<std::string> s = {
        "",                                   // 0
        "http://www.example.com/index.html",  // 1
        "My Agent/1.0",                       // 2
        "X-Test",                             // 3
        "value one",                          // 4
        "x-test",                             // 5
        "value two, longer than the first",   // 6
        "Accept",                             // 7
        "*/*",                                // 8
        "proxy.example.com:8080",             // 9
        "http",                               // 10
        "user",                               // 11
        "secret",                             // 12
        "/path",                              // 13
        "http://proxy.example.net:3128/x",    // 14
        "Missing",                            // 15
        "bad host:99999",                     // 16
        std::string(300, 'u'),                // 17
        "www.example.com",                    // 18
        "/dir/page.html",                     // 19
        "https",                              // 20
        "a=b",                                // 21
        "c=d; path=/; domain=.example.com",   // 22
        "e=f; secure",                        // 23
        "g=h; expires=2037-10-21T07:28:00Z",  // 24
        "  name =  value  ; Path=/dir ;  DOMAIN=example.com", // 25
        "bare",                               // 26
        "=onlyvalue",                         // 27
        "x=y; domain=com",                    // 28
        "x=y; domain=other.org",              // 29
        "   ",                                // 30
        "secure",                             // 31
        "a=b2; secure; httponly; max-age=10; expires=1999-01-01T00:00:00Z", // 32
        "192.168.1.1",                        // 33
        "ip=1; domain=192.168.1.1",           // 34
        "q=\x81\xfe; path=/\xc3",          // 35
        "sess=1",                             // 36
        "long=1; expires=2099-12-31T23:59:59Z; path=/dir/", // 37
        "k1=v1; expires=2090-01-01T00:00:00Z", // 38
        "k2=v2; expires=2090-01-01T00:00:00Z; domain=.example.com; path=/", // 39
        "www.example.co.jp",                  // 40
        "j=1; domain=.co.jp",                 // 41
        "j=2; domain=example.co.jp; expires=2090-01-01T00:00:00Z", // 42
        "/",                                  // 43
        "nopath",                             // 44
        "z=; path=",                          // 45
        "w=1;secure;path=/dir",               // 46
        "",                                   // 47
        "big=" + std::string(5000, 'v'),      // 48
        "mid=" + std::string(1500, 'm') + "; expires=2090-01-01T00:00:00Z", // 49
    };
    return s;
}

// An authentication cache entry in libhttp's export format.
std::vector<u8> auth_entry(const std::string& user, const std::string& pass, u8 scheme, u32 word32,
    const std::vector<std::string>& strs, const std::string& uri_text, u8 m2 = 2, u8 m6 = 6, u8 mend = 0xFF)
{
    std::vector<u8> v(561, 0);
    v[0] = m2;
    for (u32 i = 0; i < 16; i++) v[1 + i] = static_cast<u8>(0x10 + i);
    v[17] = 3;
    std::memcpy(v.data() + 18, user.data(), user.size());
    v[274] = 4;
    std::memcpy(v.data() + 275, pass.data(), pass.size());
    v[531] = 5;
    v[532] = scheme;
    const auto be = [&](u32 at, u32 w) {
        for (u32 k = 0; k < 4; k++) v[at + k] = static_cast<u8>(w >> (24 - 8 * k));
    };
    be(533, word32);
    for (u32 i = 0; i < 6; i++) be(537 + 4 * i, i < strs.size() ? static_cast<u32>(strs[i].size()) : 0);
    for (const std::string& t : strs) v.insert(v.end(), t.begin(), t.end());
    v.push_back(m6);
    v.insert(v.end(), uri_text.begin(), uri_text.end());
    v.push_back(0);
    v.push_back(mend);
    return v;
}

// Byte blocks the scripts put into memory (by index).
const std::vector<std::vector<u8>>& blobs()
{
    static const std::vector<std::vector<u8>> b = [] {
        const std::vector<u8> head = {1, 0, 0, 0, 4, 0, 0, 0, 0};
        const auto e1 = auth_entry("alice", "secret", 1, 0x12345678, {"realm", "", "abc", "", "dgst", "xy"},
            "http://www.example.com/private/");
        const auto e2 = auth_entry("bob", "pw", 0, 7, {"r2"}, "https://user@host.example.net:8443/a?b");
        const auto bad_marker = auth_entry("x", "y", 1, 0, {}, "http://h/", 2, 7);
        const auto bad_end = auth_entry("x", "y", 1, 0, {}, "http://h/", 2, 6, 0xFE);
        const auto bad_uri = auth_entry("x", "y", 2, 0, {"q"}, "");
        std::vector<std::vector<u8>> v;
        const auto join = [&](std::initializer_list<const std::vector<u8>*> parts) {
            std::vector<u8> out;
            for (const auto* p : parts) out.insert(out.end(), p->begin(), p->end());
            return out;
        };
        v.push_back(join({&head, &e1}));                    // 0
        v.push_back(join({&head, &e1, &e2}));               // 1
        v.push_back(join({&head, &bad_marker}));            // 2
        v.push_back(join({&head, &bad_end}));               // 3
        v.push_back(join({&head, &bad_uri}));               // 4
        std::vector<u8> wrong_head = join({&head, &e2});
        wrong_head[0] = 2;
        v.push_back(wrong_head);                            // 5
        v.push_back(join({&head, &e2, &e1, &e2}));          // 6
        return v;
    }();
    return b;
}

// Hosts "h000.example.com" .. for the cookie limits (strings from index 100).
constexpr u32 kHostBase = 100, kHosts = 320;
const std::vector<std::string>& all_strings()
{
    static const std::vector<std::string> s = [] {
        std::vector<std::string> v = strings();
        v.resize(kHostBase);
        for (u32 i = 0; i < kHosts; i++) v.push_back(std::format("h{:03}.example.com", i));
        return v;
    }();
    return s;
}

// A string's address in the strings area (0x200 apart; the two long ones at the end).
u64 str(u32 i)
{
    if (i == 48) return kStr + 0x200 * 420;
    if (i == 49) return kStr + 0x200 * 420 + 0x1400;
    return kStr + 0x200 * u64{i};
}

Run run(const std::string& flash, const std::vector<Op>& ops, bool sony)
{
    Fixture f;
    REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
    GuestThread t(f, 0, "ppu");
    loader::LleModules lle(flash);
    REQUIRE(lle.index());
    lle.force("cellHttp");
    lle.force("cellSsl");
    std::string why;
    const loader::PrxModule* http = lle.load("cellHttp", *f.hle, &why);
    INFO(why);
    REQUIRE(http);
    const loader::PrxModule* ssl = lle.load("cellSsl", *f.hle, &why);
    INFO(why);
    REQUIRE(ssl);
    for (const u32 s : lle.take_starts()) call_guest(t.ctx, s, {0, 0});
    u8* b = f.base();
    std::memset(b + kOut, kFill, kOutSize);
    std::memset(b + kPool, kFill, kPoolSize);
    std::memset(b + kCookiePool, kFill, kCookiePoolSize);
    std::memset(b + kStr, 0, kStrSize);
    for (u32 i = 0; i < all_strings().size(); i++) {
        const std::string& text = all_strings()[i];
        REQUIRE((text.size() < 0x200 || i == 48 || i == 49));
        std::memcpy(b + str(i), text.c_str(), text.size() + 1);
    }
    Calls calls;
    calls.thread = std::this_thread::get_id();
    f.kernel->set_syscall_observer(&observe, &calls);
    Run r;
    usize step = 0;
    for (const Op& op : ops) {
        step++;
        const std::string_view name = op.fn;
        if (name == "=") {
            ps_write32(b, static_cast<u32>(op.args[0]), static_cast<u32>(op.args[1]));
            r.answers.push_back(0);
            continue;
        }
        if (name == "s") {
            const std::string& text = strings()[op.args[1]];
            std::memcpy(b + op.args[0], text.c_str(), text.size() + 1);
            r.answers.push_back(0);
            continue;
        }
        if (name == "b") { // a byte block (args: address, index into blobs())
            const std::vector<u8>& blob = blobs()[op.args[1]];
            std::memcpy(b + op.args[0], blob.data(), blob.size());
            r.answers.push_back(0);
            continue;
        }
        if (name == "~") { // an address the run chose, made comparable: its offset into the pool, or 1
            const u32 at = static_cast<u32>(op.args[0]);
            const u32 v = ps_read32(b, at);
            ps_write32(b, at, v >= kPool && v < kPool + kPoolSize ? v - kPool : v != 0);
            r.answers.push_back(0);
            continue;
        }
        if (name == "pool") { // the pool at args[0] (cellHttpInit's when none), args[1] bytes
            const u32 at = op.args.empty() ? kPool : static_cast<u32>(op.args[0]);
            const u32 size = op.args.size() < 2 ? kPoolSize : static_cast<u32>(op.args[1]);
            r.pools.emplace_back(b + at, b + at + size);
            r.answers.push_back(0);
            continue;
        }
        std::array<u64, 8> a{};
        for (usize i = 0; i < op.args.size(); i++) {
            a[i] = op.args[i] & kDeref ? ps_read32(b, static_cast<u32>(op.args[i])) : op.args[i];
        }
        std::string library = "cellHttp";
        std::string fn(name);
        if (name.starts_with("ssl:")) library = "cellSsl", fn = name.substr(4);
        if (name.starts_with("net:")) library = "sys_net", fn = name.substr(4);
        const u32 nid = recomp::nid_of(fn);
        u32 r3 = 0;
        const loader::PrxModule* m = library == "cellSsl" ? ssl : library == "cellHttp" ? http : nullptr;
        if (sony && m) {
            const auto it = m->functions.find({library, nid});
            INFO(fn);
            REQUIRE(it != m->functions.end());
            calls.on = library == "cellHttp";
            r3 = static_cast<u32>(call_guest(t.ctx, it->second, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
            calls.on = false;
        } else {
            const Function* fp = registry().find(library, nid);
            INFO(fn);
            REQUIRE(fp);
            for (usize i = 0; i < a.size(); i++) t.ctx.gpr[3 + i] = a[i];
            const u64 sp = t.ctx.gpr[1];
            calls.on = library == "cellHttp";
            fp->fn(t.ctx, b);
            calls.on = false;
            CHECK(t.ctx.gpr[1] == sp);
            r3 = static_cast<u32>(t.ctx.gpr[3]);
        }
        if (std::getenv("PSPOCKET_HTTP_PROBE")) {
            std::fprintf(stderr, "PROBE %zu %s %s(%s) -> %#x\n", step - 1, sony ? "sony" : "ours", fn.c_str(),
                op.args.empty() ? "" : std::format("{:#x}", op.args[0]).c_str(), r3);
        }
        if (op.store) ps_write32(b, op.store, r3);
        if (op.flags & kMaskAnswer) r3 = 0;
        if (op.flags & kSign) r3 = static_cast<s32>(r3) < 0 ? 0xFFFFFFFF : r3 ? 1 : 0;
        r.answers.push_back(r3);
        std::lock_guard lock(calls.mutex);
        calls.list.push_back(std::format("-- {}", fn));
    }
    f.kernel->set_syscall_observer(nullptr, nullptr);
    r.out.assign(b + kOut, b + kOut + kOutSize);
    r.calls = calls.list;
    if (sony) {
        for (const auto& [at, size] : http->segments) r.globals.emplace_back(at, size);
    } else {
        r.globals.emplace_back(http::lib().data, http::kDataSize);
    }
    return r;
}

bool in(const std::vector<std::pair<u32, u32>>& ranges, u32 v)
{
    for (const auto& [at, size] : ranges)
        if (v >= at && v < at + size) return true;
    return false;
}

u32 word(const std::vector<u8>& v, usize at)
{
    return u32{v[at]} << 24 | u32{v[at + 1]} << 16 | u32{v[at + 2]} << 8 | v[at + 3];
}

// Where two pools differ first (word by word), what differs by construction set aside: the heap's magic (liblv2 makes
// it from the timebase; with it every chunk's footer, the heap's address XOR the magic), lv2 IDs (lwmutexes' sleep
// queues, the mutexes and conditions libhttp makes), and pointers to each library's own globals.
usize pool_diff(const Run& mine, const Run& sonys, usize k)
{
    const std::vector<u8>& x = mine.pools[k];
    const std::vector<u8>& y = sonys.pools[k];
    const u32 magic_x = word(x, 0x30), magic_y = word(y, 0x30); // the malloc_state at +16, its magic at +32
    for (usize at = 0; at + 4 <= x.size(); at += 4) {
        const u32 a = word(x, at), b = word(y, at);
        if (a == b) continue;
        // A footer, or part of one another write left (each byte the same, or the same once unmasked).
        bool footer = true;
        for (u32 sh = 0; sh < 32; sh += 8) {
            const u32 bx = (a >> sh) & 0xFF, by = (b >> sh) & 0xFF;
            if (bx != by && (bx ^ ((magic_x >> sh) & 0xFF)) != (by ^ ((magic_y >> sh) & 0xFF))) footer = false;
        }
        if (footer) continue;
        if (lv2_id(a) && lv2_id(b) && (a >> 24) == (b >> 24)) continue;
        if (in(mine.globals, a) && in(sonys.globals, b)) continue;
        return at;
    }
    return x.size();
}

void dump(const char* who, const Run& r)
{
    for (const std::string& c : r.calls) std::fprintf(stderr, "%s CALL %s\n", who, c.c_str());
    for (u32 i = 0; i < 0x400; i += 16) {
        std::string line;
        for (u32 k = 0; k < 16; k++) line += std::format("{:02x}", r.out[i + k]) + (k % 4 == 3 ? " " : "");
        std::fprintf(stderr, "%s OUT +%03x %s\n", who, i, line.c_str());
    }
}

void compare(const std::string& flash, const std::vector<Op>& all_ops, const std::string& what)
{
    // Probing: PSPOCKET_HTTP_ONLY="from-to" runs those steps of a script only (and every store before them).
    std::vector<Op> ops = all_ops;
    if (const char* only = std::getenv("PSPOCKET_HTTP_ONLY")) { // "a-b,c-d": those steps, and every store
        std::vector<std::pair<usize, usize>> ranges;
        for (const char* p = only; *p;) {
            char* end = nullptr;
            const usize from = std::strtoul(p, &end, 10);
            const usize to = std::strtoul(end + 1, &end, 10);
            ranges.emplace_back(from, to);
            p = *end == ',' ? end + 1 : end;
        }
        std::vector<Op> kept;
        for (usize i = 0; i < all_ops.size(); i++) {
            const std::string_view n = all_ops[i].fn;
            bool in_range = n == "=" || n == "s" || n == "b";
            for (const auto& [a, z] : ranges) in_range = in_range || (i >= a && i <= z);
            if (in_range) kept.push_back(all_ops[i]);
        }
        ops = kept;
    }
    const Run sonys = run(flash, ops, true);
    if (std::getenv("PSPOCKET_HTTP_PROBE")) {
        dump("sony", sonys);
        if (std::string_view(std::getenv("PSPOCKET_HTTP_PROBE")) != "both") return;
    }
    const Run mine = run(flash, ops, false);
    if (std::getenv("PSPOCKET_HTTP_PROBE")) dump("ours", mine);
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
        const usize at = pool_diff(mine, sonys, k);
        INFO(std::format("{}: pool {} differs first at +{:#x} (ours {:#04x}, Sony's {:#04x})", what, k, at,
            at < mine.pools[k].size() ? mine.pools[k][at] : 0, at < sonys.pools[k].size() ? sonys.pools[k][at] : 0));
        if (at != mine.pools[k].size() && std::getenv("PSPOCKET_HTTP_DUMP")) {
            for (usize i = (at > 0x40 ? at - 0x40 : 0) & ~usize{15}; i < at + 0x60 && i < mine.pools[k].size(); i += 16) {
                std::string a, b2;
                for (usize j = 0; j < 16; j += 4) {
                    a += std::format("{:08x} ", word(mine.pools[k], i + j));
                    b2 += std::format("{:08x} ", word(sonys.pools[k], i + j));
                }
                std::fprintf(stderr, "pool %zu +%05zx ours %s| Sony's %s\n", k, i, a.c_str(), b2.c_str());
            }
        }
        CHECK(at == mine.pools[k].size());
    }
    const usize n = std::max(mine.calls.size(), sonys.calls.size());
    for (usize i = 0; i < n; i++) {
        const std::string m = i < mine.calls.size() ? mine.calls[i] : "(none)";
        const std::string s = i < sonys.calls.size() ? sonys.calls[i] : "(none)";
        INFO(std::format("{}: lv2 call {}: ours {}, Sony's {}", what, i, m, s));
        CHECK(m == s);
        if (m != s) break;
    }
}

void uri(std::vector<Op>& s, u64 at, u32 scheme, u32 host, u32 user, u32 pass, u32 path, u32 port);

std::vector<Op> probe_script()
{
    std::vector<Op> s;
    if (const char* list = std::getenv("PSPOCKET_HTTP_COOKIES")) { // a cookie probe: indices into strings()
        uri(s, kOut + 0x40, 10, 18, 0, 0, 19, 80);
        s.push_back({"cellHttpInitCookie", {kCookiePool, kCookiePoolSize}});
        uri(s, kOut + 0x80, 10, 33, 0, 0, 19, 80);
        uri(s, kOut + 0xA0, 10, 40, 0, 0, 43, 80);
        for (const char* p = list; *p;) {
            u64 at = kOut + 0x40;
            if (*p == 'i') at = kOut + 0x80, p++;
            if (*p == 'j') at = kOut + 0xA0, p++;
            const u32 i = static_cast<u32>(std::strtoul(p, const_cast<char**>(&p), 10));
            if (*p == ',') p++;
            s.push_back({"cellHttpAddCookieWithClientId", {at, str(i), i % 2 + 1}});
        }
        if (std::getenv("PSPOCKET_HTTP_GET")) s.push_back({"cellHttpGetCookie", {kOut + 0x40, kOut + 0x1000, 0x100, kOut}});
        s.push_back({"cellHttpSessionCookieFlush", {1}});
        if (std::getenv("PSPOCKET_HTTP_GET2")) s.push_back({"cellHttpGetCookie", {kOut + 0x40, kOut + 0x1000, 0x100, kOut}});
        s.push_back({"cellHttpCookieFlush", {}});
        return s;
    }
    const u64 o = kOut;
    s.push_back({"cellHttpInit", {kPool, kPoolSize}});
    s.push_back({"cellHttpGetMemoryInfo", {o, o + 4, o + 8}});
    s.push_back({"cellHttpCreateClient", {o + 0x40}});
    s.push_back({"cellHttpGetMemoryInfo", {o + 0x50, o + 0x54, o + 0x58}});
    s.push_back({"cellHttpClientGetUserAgent", {(o + 0x40) | kDeref, o + 0x100, 0x80, o + 0x44}});
    s.push_back({"pool", {}});
    return s;
}

// A CellHttpHeader {name, value} at `at`.
void header(std::vector<Op>& s, u64 at, u32 name, u32 value)
{
    s.push_back({"=", {at, str(name)}});
    s.push_back({"=", {at + 4, str(value)}});
}

// A CellHttpUri {scheme, hostname, username, password, path, port} at `at` (0 for no string).
void uri(std::vector<Op>& s, u64 at, u32 scheme, u32 host, u32 user, u32 pass, u32 path, u32 port)
{
    s.push_back({"=", {at, scheme ? str(scheme) : 0}});
    s.push_back({"=", {at + 4, host ? str(host) : 0}});
    s.push_back({"=", {at + 8, user ? str(user) : 0}});
    s.push_back({"=", {at + 12, pass ? str(pass) : 0}});
    s.push_back({"=", {at + 16, path ? str(path) : 0}});
    s.push_back({"=", {at + 20, port}});
}

// Every client function: before cellHttpInit, on clients that are not (0, unmapped memory, memory that holds no client,
// a destroyed one), every setting set and read back, headers, the user agent, proxies, callbacks.
std::vector<Op> client_script()
{
    std::vector<Op> s;
    const u64 o = kOut;
    const u64 id = o + 0x40, id2 = o + 0x44, id3 = o + 0x48;
    const u64 c = id | kDeref;
    const std::vector<const char*> getters = {"cellHttpClientGetResponseBufferMax", "cellHttpClientGetKeepAlive",
        "cellHttpClientGetPipeline", "cellHttpClientGetAutoRedirect", "cellHttpClientGetAutoAuthentication",
        "cellHttpClientGetCookieStatus", "cellHttpClientGetAuthenticationCacheStatus", "cellHttpClientGetTotalPoolSize",
        "cellHttpClientGetPerHostPoolSize", "cellHttpClientGetPerHostKeepAliveMax", "cellHttpClientGetPerPipelineMax",
        "cellHttpClientGetConnectionWaitStatus", "cellHttpClientGetConnectionWaitTimeout", "cellHttpClientGetRecvTimeout",
        "cellHttpClientGetSendTimeout", "cellHttpClientGetConnTimeout", "cellHttpClientGetSendBufferSize",
        "cellHttpClientGetRecvBufferSize", "cellHttpClientGetSslVersion", "cellHttpClientGetMinSslVersion"};
    u64 slot = o + 0x400;
    const auto read_all = [&](u64 client) {
        for (const char* g : getters) {
            s.push_back({g, {client, slot}});
            slot += 8;
        }
        s.push_back({"cellHttpClientGetVersion", {client, slot, slot + 4}});
        s.push_back({"cellHttpClientGetVersion", {client, 0, slot + 4}});
        s.push_back({"cellHttpClientGetVersion", {client, slot, 0}});
        slot += 8;
        // A null pointer to each (libhttp faults on GetMinSslVersion's: it stores without checking).
        for (const char* g : getters)
            if (std::string_view(g) != "cellHttpClientGetMinSslVersion") s.push_back({g, {client, 0}});
    };
    const auto set_all = [&](u64 client, u64 v) {
        for (const char* f : {"cellHttpClientSetResponseBufferMax", "cellHttpClientSetKeepAlive", "cellHttpClientSetPipeline",
                 "cellHttpClientSetAutoRedirect", "cellHttpClientSetAutoAuthentication", "cellHttpClientSetCookieStatus",
                 "cellHttpClientSetCacheStatus", "cellHttpClientSetAuthenticationCacheStatus",
                 "cellHttpClientSetTotalPoolSize", "cellHttpClientSetPerHostPoolSize",
                 "cellHttpClientSetPerHostKeepAliveMax", "cellHttpClientSetPerPipelineMax",
                 "cellHttpClientSetConnectionWaitStatus", "cellHttpClientSetConnectionWaitTimeout",
                 "cellHttpClientSetRecvTimeout", "cellHttpClientSetSendTimeout", "cellHttpClientSetConnTimeout",
                 "cellHttpClientSetSendBufferSize", "cellHttpClientSetRecvBufferSize", "cellHttpClientSetSslVersion",
                 "cellHttpClientSetMinSslVersion"}) {
            s.push_back({f, {client, v}});
        }
        s.push_back({"cellHttpClientSetVersion", {client, v, v + 1}});
    };
    // Before cellHttpInit.
    s.push_back({"cellHttpCreateClient", {id}});
    s.push_back({"cellHttpDestroyClient", {0}});
    read_all(0);
    set_all(0, 1);
    s.push_back({"cellHttpClientSetConnectionStateCallback", {0, 0x1000, 0x2000}});
    s.push_back({"cellHttpGetMemoryInfo", {o, 0, 0}});
    s.push_back({"cellHttpEnd", {}});
    s.push_back({"cellHttpGetProxy", {0, 0, 0, o + 0x10}});
    s.push_back({"cellHttpSetProxy", {0}});
    // cellHttpInit's checks.
    s.push_back({"cellHttpInit", {0, kPoolSize}});
    s.push_back({"cellHttpInit", {kPool, 0}});
    s.push_back({"cellHttpInit", {kPool, kPoolSize}});
    s.push_back({"cellHttpInit", {kPool, kPoolSize}});
    s.push_back({"pool", {}});
    s.push_back({"cellHttpGetMemoryInfo", {o, o + 4, o + 8}});
    s.push_back({"cellHttpGetMemoryInfo", {0, 0, o + 0x30}});
    s.push_back({"cellHttpCreateClient", {0}});
    s.push_back({"cellHttpCreateClient", {id}});
    s.push_back({"cellHttpCreateClient", {id2}});
    s.push_back({"pool", {}});
    s.push_back({"cellHttpGetMemoryInfo", {o + 0x80, o + 0x84, o + 0x88}});
    // Not clients.
    for (const u64 bad : {u64{0}, u64{0x1234}, u64{0xC0000000}, str(1)}) {
        read_all(bad);
        set_all(bad, 2);
        s.push_back({"cellHttpClientSetConnectionStateCallback", {bad, 0x1000, 0x2000}});
        s.push_back({"cellHttpDestroyClient", {bad}});
    }
    // The defaults, then every setting at several values.
    read_all(c);
    for (const u64 v : {u64{0}, u64{1}, u64{2}, u64{3}, u64{4}, u64{5}, u64{8}, u64{9}, u64{16}, u64{17}, u64{64},
             u64{65}, u64{256}, u64{1499}, u64{1500}, u64{0x7FFFF}, u64{0x80000}, u64{0x80001}, u64{0xFFFFFFFF},
             u64{0x100}, u64{0x12345678ABCD}}) {
        set_all(c, v);
        read_all(c);
    }
    s.push_back({"cellHttpClientSetKeepAlive", {c, 0}});
    s.push_back({"cellHttpClientSetPipeline", {c, 1}});
    s.push_back({"cellHttpClientGetPipeline", {c, slot++}});
    s.push_back({"cellHttpClientSetKeepAlive", {c, 1}});
    s.push_back({"cellHttpClientSetAutoRedirect", {c, 1}});
    s.push_back({"cellHttpClientSetAutoAuthentication", {c, 1}});
    s.push_back({"cellHttpClientSetPipeline", {c, 0x100}});
    read_all(c);
    // The user agent.
    s.push_back({"cellHttpClientGetUserAgent", {c, 0, 0, 0}});
    s.push_back({"cellHttpClientGetUserAgent", {c, 0, 0, o + 0x100}});
    s.push_back({"cellHttpClientGetUserAgent", {c, o + 0x120, 0, o + 0x104}});
    s.push_back({"cellHttpClientGetUserAgent", {c, o + 0x120, 41, o + 0x108}});
    s.push_back({"cellHttpClientGetUserAgent", {c, o + 0x180, 42, o + 0x10C}});
    s.push_back({"cellHttpClientSetUserAgent", {c, 0}});
    s.push_back({"cellHttpClientSetUserAgent", {c, str(2)}});
    s.push_back({"cellHttpClientGetUserAgent", {c, o + 0x1C0, 0x40, o + 0x110}});
    s.push_back({"cellHttpClientSetUserAgent", {c, str(0)}});
    s.push_back({"cellHttpClientGetUserAgent", {c, o + 0x200, 0x40, o + 0x114}});
    s.push_back({"cellHttpClientSetUserAgent", {c, str(17)}});
    s.push_back({"cellHttpClientGetUserAgent", {c, o + 0x240, 0x40, o + 0x118}});
    s.push_back({"pool", {}});
    // Headers.
    const u64 h = o + 0x300;
    header(s, h, 3, 4);
    header(s, h + 8, 5, 6);
    header(s, h + 16, 7, 8);
    header(s, h + 24, 0, 8);
    s.push_back({"cellHttpClientAddHeader", {c, 0}});
    s.push_back({"cellHttpClientAddHeader", {c, h}});
    s.push_back({"cellHttpClientAddHeader", {c, h + 16}});
    s.push_back({"cellHttpClientSetHeader", {c, h + 8}});
    s.push_back({"cellHttpClientSetHeader", {c, 0}});
    s.push_back({"cellHttpClientAddHeader", {c, h + 24}});
    s.push_back({"pool", {}});
    const u64 hp = o + 0x800; // a pool for copies
    s.push_back({"cellHttpClientGetHeader", {c, o + 0x340, str(5), hp, 0x100, o + 0x348}});
    s.push_back({"cellHttpClientGetHeader", {c, o + 0x350, str(15), hp + 0x100, 0x100, o + 0x358}});
    s.push_back({"cellHttpClientGetHeader", {c, o + 0x350, 0, hp + 0x100, 0x100, o + 0x358}});
    s.push_back({"cellHttpClientGetHeader", {c, 0, str(7), 0, 0, o + 0x35C}});
    s.push_back({"cellHttpClientGetHeader", {c, o + 0x360, str(7), hp + 0x200, 2, o + 0x368}});
    for (const u32 size : {0u, 8u, 0x20u, 0x200u}) {
        s.push_back({"cellHttpClientGetAllHeaders", {c, o + 0x370, o + 0x374, hp + 0x300, size, o + 0x378}});
        s.push_back({"cellHttpClientGetAllHeaders", {c, 0, o + 0x374, 0, size, o + 0x37C}});
    }
    s.push_back({"cellHttpClientGetAllHeaders", {c, 0, 0, 0, 0, 0}});
    s.push_back({"cellHttpClientGetAllHeaders", {c, o + 0x370, 0, hp + 0x600, 0x200, 0}});
    s.push_back({"cellHttpClientDeleteHeader", {c, 0}});
    s.push_back({"cellHttpClientDeleteHeader", {c, str(15)}});
    s.push_back({"cellHttpClientDeleteHeader", {c, str(7)}});
    s.push_back({"cellHttpClientDeleteHeader", {c, str(3)}});
    s.push_back({"cellHttpClientGetAllHeaders", {c, o + 0x380, o + 0x384, hp + 0x800, 0x200, o + 0x388}});
    s.push_back({"pool", {}});
    // Proxies: the global one copied into clients made after it.
    const u64 px = o + 0x3A0;
    uri(s, px, 10, 9, 11, 12, 13, 8080);
    s.push_back({"cellHttpGetProxy", {o + 0x3C0, hp + 0xA00, 0x100, o + 0x3E0}});
    s.push_back({"cellHttpSetProxy", {px}});
    s.push_back({"pool", {}});
    s.push_back({"cellHttpGetProxy", {0, 0, 0, o + 0x3E4}});
    s.push_back({"cellHttpGetProxy", {o + 0x3C0, hp + 0xA00, 0x100, o + 0x3E0}});
    s.push_back({"cellHttpGetProxy", {o + 0x3C0, hp + 0xB00, 4, o + 0x3E8}});
    s.push_back({"cellHttpCreateClient", {id3}});
    s.push_back({"cellHttpClientGetProxy", {id3 | kDeref, o + 0x1000, hp + 0xC00, 0x100, o + 0x3EC}});
    s.push_back({"cellHttpClientGetProxy", {c, o + 0x1020, hp + 0xD00, 0x100, o + 0x3F0}});
    uri(s, px + 0x40, 0, 14, 0, 0, 0, 0);
    s.push_back({"cellHttpClientSetProxy", {c, px + 0x40}});
    s.push_back({"cellHttpClientGetProxy", {c, o + 0x1040, hp + 0xE00, 0x100, o + 0x3F4}});
    uri(s, px + 0x80, 10, 0, 0, 0, 0, 0);
    s.push_back({"cellHttpClientSetProxy", {c, px + 0x80}});
    s.push_back({"cellHttpSetProxy", {px + 0x80}});
    uri(s, px + 0xC0, 10, 16, 0, 0, 0, 0);
    s.push_back({"cellHttpClientSetProxy", {c, px + 0xC0}});
    s.push_back({"cellHttpSetProxy", {px + 0xC0}});
    s.push_back({"cellHttpClientSetProxy", {c, 0}});
    s.push_back({"cellHttpClientGetProxy", {c, o + 0x1060, hp + 0xF00, 0x100, o + 0x3F8}});
    s.push_back({"cellHttpSetProxy", {0}});
    s.push_back({"cellHttpGetProxy", {0, 0, 0, o + 0x3FC}});
    s.push_back({"pool", {}});
    // Callbacks.
    for (const char* f : {"cellHttpClientSetAuthenticationCallback", "cellHttpClientSetTransactionStateCallback",
             "cellHttpClientSetRedirectCallback", "cellHttpClientSetCookieSendCallback",
             "cellHttpClientSetCookieRecvCallback", "cellHttpClientSetSslCallback",
             "cellHttpClientSetSslIdDestroyCallback", "cellHttpClientSetConnectionStateCallback"}) {
        s.push_back({f, {c, 0x1000, 0x2000}});
        s.push_back({f, {id2 | kDeref, 0, 0}});
    }
    s.push_back({"pool", {}});
    // Destroyed clients.
    s.push_back({"cellHttpDestroyClient", {c}});
    s.push_back({"cellHttpDestroyClient", {c}});
    read_all(c);
    s.push_back({"cellHttpDestroyClient", {id3 | kDeref}});
    s.push_back({"pool", {}});
    s.push_back({"cellHttpGetMemoryInfo", {o + 0xC0, o + 0xC4, o + 0xC8}});
    s.push_back({"cellHttpEnd", {}});
    s.push_back({"cellHttpEnd", {}});
    s.push_back({"cellHttpDestroyClient", {id2 | kDeref}});
    s.push_back({"pool", {}});
    for (const u64 at : {id, id2, id3}) s.push_back({"~", {at}});
    return s;
}

} // namespace

// Without the firmware: libhttp's answers for the set-up and a client, as the comparisons found them.
TEST_CASE("cellHttp: set-up and a client as libhttp", "[hle][http]")
{
    Fixture f;
    GuestThread g(f, 0, "http");
    u8* b = f.base();
    constexpr u32 kHeap = kData + 0x100000, kCookies = kData + 0x180000, kId = kData + 0x1000, kUa = kData + 0x1100;
    const u64 kAlready = r3_of_code(0x80710001), kNotInit = r3_of_code(0x80710002), kNoBuffer = r3_of_code(0x80710004),
              kBadClient = r3_of_code(0x80710010), kInsufficient = r3_of_code(0x80710006);

    CHECK(g.call("cellHttpEnd") == kNotInit);
    CHECK(g.call("cellHttpCreateClient", kId) == kNotInit);
    CHECK(g.call("cellHttpsInit", 0, 0) == kNotInit); // cellHttpInit first
    CHECK(g.call("cellHttpInit", 0, 0x10000) == kNoBuffer);
    CHECK(g.call("cellHttpInit", kHeap, 0) == kNoBuffer);
    CHECK(g.call("cellHttpInit", kHeap, 0x10000) == 0);
    CHECK(g.call("cellHttpInit", 0, 0) == kAlready);

    CHECK(g.call("cellHttpCreateClient", 0) == kNoBuffer);
    REQUIRE(g.call("cellHttpCreateClient", kId) == 0);
    const u32 client = ps_read32(b, kId);
    CHECK(client > kHeap);
    CHECK(client < kHeap + 0x10000);
    CHECK(g.call("cellHttpClientGetUserAgent", client, kUa, 41, kId + 4) == kInsufficient);
    CHECK(ps_read32(b, kId + 4) == 42);
    REQUIRE(g.call("cellHttpClientGetUserAgent", client, kUa, 64, 0) == 0);
    CHECK(std::string(reinterpret_cast<const char*>(b + kUa)) == "PS3Application libhttp/4.9.2-000 (CellOS)");
    CHECK(g.call("cellHttpClientGetKeepAlive", client, kId + 8) == 0);
    CHECK(ps_read8(b, kId + 8) == 1);
    CHECK(g.call("cellHttpClientGetResponseBufferMax", client, kId + 8) == 0);
    CHECK(ps_read32(b, kId + 8) == 5000);
    CHECK(g.call("cellHttpClientSetResponseBufferMax", client, 100) == 0);
    CHECK(g.call("cellHttpClientGetResponseBufferMax", client, kId + 8) == 0);
    CHECK(ps_read32(b, kId + 8) == 1500);
    CHECK(g.call("cellHttpClientGetConnTimeout", client, kId + 8) == 0);
    CHECK(ps_read64(b, kId + 8) == 30000000);
    CHECK(g.call("cellHttpClientGetKeepAlive", kData, kId + 8) == kBadClient); // memory holding no client
    CHECK(g.call("cellHttpDestroyClient", client) == 0);
    CHECK(g.call("cellHttpDestroyClient", client) == kBadClient);
    CHECK(g.call("cellHttpEnd") == 0);
    CHECK(g.call("cellHttpEnd") == kNotInit);

    // Cookies: a pool and a size first, then initialised; cellHttpInit isn't needed.
    CHECK(g.call("cellHttpEndCookie") == kNotInit);
    CHECK(g.call("cellHttpInitCookie", 0, 0x1000) == kNoBuffer);
    CHECK(g.call("cellHttpInitCookie", kCookies, 0) == kNoBuffer);
    CHECK(g.call("cellHttpInitCookie", kCookies, 0x1000) == 0);
    CHECK(g.call("cellHttpInitCookie", 0, 0x1000) == kNoBuffer);
    CHECK(g.call("cellHttpInitCookie", kCookies, 0x1000) == kAlready);
    CHECK(g.call("cellHttpEndCookie") == 0);
    CHECK(g.call("cellHttpEndCookie") == kNotInit);
}

// The cookie store: Set-Cookie values of every shape, the Cookie header for URIs, export and import, the flushes, the
// limits (50 cookies a domain, 300 in all, 4096-byte values).
std::vector<Op> cookie_script()
{
    std::vector<Op> s;
    const u64 o = kOut;
    const u64 u = o + 0x40, u2 = o + 0x60, u3 = o + 0x80, u4 = o + 0xA0, u5 = o + 0xC0;
    const u64 req = o + 0x100;
    uri(s, u, 10, 18, 0, 0, 19, 80);   // http://www.example.com/dir/page.html
    uri(s, u2, 20, 18, 0, 0, 43, 443); // https://www.example.com/
    uri(s, u3, 10, 33, 0, 0, 19, 80);  // http://192.168.1.1/dir/page.html
    uri(s, u4, 10, 40, 0, 0, 43, 80);  // http://www.example.co.jp/
    uri(s, u5, 10, 18, 0, 0, 0, 80);   // no path
    u64 out = o + 0x1000;
    const auto get = [&](u64 at_uri, u32 size) {
        s.push_back({"cellHttpGetCookie", {at_uri, out, size, req}});
        s.push_back({"cellHttpGetCookie", {at_uri, 0, 0, req + 4}});
        out += 0x100;
    };
    // Before cellHttpInitCookie.
    s.push_back({"cellHttpAddCookieWithClientId", {u, str(21), 1}});
    s.push_back({"cellHttpGetCookie", {u, out, 0x100, req}});
    s.push_back({"cellHttpCookieExport", {o + 0x2000, 0x1000, req}});
    s.push_back({"cellHttpCookieExportWithClientId", {o + 0x2000, 0x1000, req, 1}});
    s.push_back({"cellHttpCookieImport", {o + 0x2000, 0x1000}});
    s.push_back({"cellHttpCookieFlush", {}});
    s.push_back({"cellHttpSessionCookieFlush", {1}});
    s.push_back({"cellHttpEndCookie", {}});
    s.push_back({"cellHttpInitCookie", {kCookiePool, kCookiePoolSize}});
    s.push_back({"pool", {kCookiePool, kCookiePoolSize}});
    // Arguments.
    s.push_back({"cellHttpAddCookieWithClientId", {0, str(21), 1}});
    s.push_back({"cellHttpAddCookieWithClientId", {u, 0, 1}});
    s.push_back({"cellHttpGetCookie", {0, out, 0x100, req}});
    // Cookies of every shape, for client 1 and 2.
    for (const u32 i : {21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 32u, 35u, 36u, 37u, 38u, 39u, 45u, 46u, 47u})
        s.push_back({"cellHttpAddCookieWithClientId", {u, str(i), i % 2 + 1}});
    s.push_back({"cellHttpAddCookieWithClientId", {u3, str(34), 1}});
    s.push_back({"cellHttpAddCookieWithClientId", {u3, str(21), 1}});
    s.push_back({"cellHttpAddCookieWithClientId", {u4, str(41), 1}});
    s.push_back({"cellHttpAddCookieWithClientId", {u4, str(42), 1}});
    // (A URI without a host or path is left out: libhttp then frees whatever its stack holds as a cookie list.)
    s.push_back({"pool", {kCookiePool, kCookiePoolSize}});
    for (const u64 at : {u, u2, u3, u4, u5}) get(at, 0x100);
    get(u, 4);
    get(u, 20);
    // Export: the size, then into a buffer, per client, too small.
    s.push_back({"cellHttpCookieExport", {0, 0, req + 8}});
    s.push_back({"cellHttpCookieExport", {o + 0x2000, 19, req + 12}});
    s.push_back({"cellHttpCookieExport", {o + 0x2000, 0x1000, req + 16}});
    s.push_back({"cellHttpCookieExportWithClientId", {o + 0x3000, 0x1000, req + 20, 1}});
    s.push_back({"cellHttpCookieExportWithClientId", {o + 0x3400, 0x1000, req + 24, 2}});
    s.push_back({"cellHttpCookieExport", {o + 0x3800, 0x60, req + 28}});
    // Session cookies of client 1 flushed; everything flushed; re-imported.
    s.push_back({"cellHttpSessionCookieFlush", {1}});
    get(u, 0x100);
    s.push_back({"pool", {kCookiePool, kCookiePoolSize}});
    s.push_back({"cellHttpCookieFlush", {}});
    get(u, 0x100);
    s.push_back({"pool", {kCookiePool, kCookiePoolSize}});
    s.push_back({"cellHttpCookieImportWithClientId", {o + 0x2000, 0x1000, 7}});
    get(u, 0x100);
    get(u4, 0x100);
    s.push_back({"cellHttpCookieImport", {o + 0x3000, 0x1000}});
    get(u, 0x100);
    s.push_back({"pool", {kCookiePool, kCookiePoolSize}});
    // A broken export: a byte changed (the MD5 doesn't hold), a length past the size.
    s.push_back({"=", {o + 0x2018, 0x12345678}});
    s.push_back({"cellHttpCookieImportWithClientId", {o + 0x2000, 0x1000, 3}});
    s.push_back({"cellHttpCookieImportWithClientId", {o + 0x3000, 8, 3}});
    // Replacing a cookie of the same name; 60 cookies on one domain; 310 in all; a value too long.
    s.push_back({"cellHttpAddCookieWithClientId", {u, str(21), 1}});
    s.push_back({"cellHttpAddCookieWithClientId", {u, str(21), 1}});
    get(u, 0x100);
    for (u32 i = 0; i < 60; i++) {
        const u64 text = o + 0x4000 + 0x20 * i;
        s.push_back({"=", {text, 0x6E3D0000 | ((0x30 + i / 10) << 8) | (0x30 + i % 10)}}); // "n=" and two digits
        s.push_back({"=", {text + 4, 0}});
        s.push_back({"cellHttpAddCookieWithClientId", {u, text, 1}});
    }
    s.push_back({"pool", {kCookiePool, kCookiePoolSize}});
    get(u, 0x1000);
    s.push_back({"cellHttpAddCookieWithClientId", {u, str(17), 1}});
    s.push_back({"cellHttpEndCookie", {}});
    s.push_back({"pool", {kCookiePool, kCookiePoolSize}});
    s.push_back({"cellHttpEndCookie", {}});
    return s;
}

// The store's limits: 320 hosts' cookies (more than 300), large values (80 KB in all), a value past 4096 bytes.
std::vector<Op> cookie_limits_script()
{
    std::vector<Op> s;
    const u64 o = kOut;
    s.push_back({"cellHttpInitCookie", {kCookiePool, kCookiePoolSize}});
    for (u32 i = 0; i < kHosts; i++) {
        const u64 u = o + 0x40;
        uri(s, u, 10, kHostBase + i, 0, 0, 19, 80);
        s.push_back({"cellHttpAddCookieWithClientId", {u, str(i % 3 == 0 ? 38 : i % 3 == 1 ? 21 : 49), i % 4}});
        if (i % 40 == 39) s.push_back({"pool", {kCookiePool, kCookiePoolSize}});
        if (i % 16 == 5) {
            uri(s, o + 0x80, 10, kHostBase + i / 2, 0, 0, 19, 80);
            s.push_back({"cellHttpGetCookie", {o + 0x80, o + 0x1000 + 0x40 * (i / 16), 0x40, o + 0x800 + 4 * (i / 16)}});
        }
    }
    s.push_back({"cellHttpCookieExport", {0, 0, o + 0x900}});
    s.push_back({"cellHttpCookieExport", {o + 0x2000, 0x8000, o + 0x904}});
    s.push_back({"cellHttpAddCookieWithClientId", {o + 0x40, str(48), 1}});
    s.push_back({"pool", {kCookiePool, kCookiePoolSize}});
    s.push_back({"cellHttpCookieImport", {o + 0x2000, 0x8000}});
    s.push_back({"pool", {kCookiePool, kCookiePoolSize}});
    s.push_back({"cellHttpEndCookie", {}});
    return s;
}

// The authentication cache: its maximum, exports of an empty and an imported cache, imports of every shape.
std::vector<Op> auth_script()
{
    std::vector<Op> s;
    const u64 o = kOut;
    const u64 in = o + 0x4000, ex = o + 0x8000;
    s.push_back({"cellHttpAuthCacheGetEntryMax", {o}});
    s.push_back({"cellHttpAuthCacheSetEntryMax", {3}});
    s.push_back({"cellHttpAuthCacheFlush", {}});
    s.push_back({"cellHttpAuthCacheExport", {ex, 0x1000, o + 4}});
    s.push_back({"cellHttpAuthCacheImport", {in, 0x1000}});
    s.push_back({"cellHttpInit", {kPool, kPoolSize}});
    s.push_back({"cellHttpAuthCacheGetEntryMax", {0}});
    s.push_back({"cellHttpAuthCacheGetEntryMax", {o + 8}});
    s.push_back({"cellHttpAuthCacheExport", {0, 0, 0}});
    s.push_back({"cellHttpAuthCacheExport", {0, 0, o + 12}});
    s.push_back({"cellHttpAuthCacheExport", {ex, 8, o + 16}});
    s.push_back({"cellHttpAuthCacheExport", {ex, 9, o + 20}});
    s.push_back({"pool", {}});
    s.push_back({"b", {in, 0}});
    s.push_back({"cellHttpAuthCacheImport", {0, 0x1000}});
    s.push_back({"cellHttpAuthCacheImport", {in, 0}});
    s.push_back({"cellHttpAuthCacheImport", {in, 8}});
    s.push_back({"cellHttpAuthCacheImport", {in, 9}});
    s.push_back({"cellHttpAuthCacheImport", {in, 560}});
    s.push_back({"cellHttpAuthCacheImport", {in, static_cast<u64>(blobs()[0].size())}});
    s.push_back({"pool", {}});
    s.push_back({"cellHttpAuthCacheExport", {0, 0, o + 24}});
    s.push_back({"cellHttpAuthCacheExport", {ex, 0x1000, o + 28}});
    s.push_back({"cellHttpAuthCacheExport", {ex + 0x1000, 600, o + 32}});
    s.push_back({"cellHttpAuthCacheExport", {ex + 0x1400, 0x250, o + 36}});
    s.push_back({"b", {in, 1}});
    s.push_back({"cellHttpAuthCacheImport", {in, static_cast<u64>(blobs()[1].size())}});
    s.push_back({"pool", {}});
    s.push_back({"cellHttpAuthCacheExport", {ex + 0x2000, 0x1000, o + 40}});
    for (u32 i : {2u, 3u, 4u, 5u}) {
        s.push_back({"b", {in, i}});
        s.push_back({"cellHttpAuthCacheImport", {in, static_cast<u64>(blobs()[i].size())}});
        s.push_back({"pool", {}});
    }
    s.push_back({"b", {in, 6}});
    s.push_back({"cellHttpAuthCacheImport", {in, static_cast<u64>(blobs()[6].size())}});
    s.push_back({"cellHttpAuthCacheSetEntryMax", {10}});
    s.push_back({"cellHttpAuthCacheImport", {in, static_cast<u64>(blobs()[6].size())}});
    s.push_back({"cellHttpAuthCacheGetEntryMax", {o + 44}});
    s.push_back({"pool", {}});
    s.push_back({"cellHttpAuthCacheExport", {ex + 0x3000, 0x1000, o + 48}});
    s.push_back({"cellHttpAuthCacheFlush", {}});
    s.push_back({"pool", {}});
    s.push_back({"cellHttpAuthCacheExport", {ex + 0x4000, 0x1000, o + 52}});
    s.push_back({"b", {in, 1}});
    s.push_back({"cellHttpAuthCacheImport", {in, static_cast<u64>(blobs()[1].size())}});
    s.push_back({"cellHttpEnd", {}});
    s.push_back({"pool", {}});
    s.push_back({"cellHttpAuthCacheExport", {ex, 0x1000, o + 56}});
    s.push_back({"cellHttpInit", {kPool, kPoolSize}});
    s.push_back({"cellHttpAuthCacheGetEntryMax", {o + 60}});
    s.push_back({"cellHttpAuthCacheExport", {ex + 0x5000, 0x1000, o + 64}});
    s.push_back({"cellHttpEnd", {}});
    return s;
}

TEST_CASE("cellHttp against libhttp: the authentication cache", "[hle][lle][http]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, auth_script(), "auth");
}

TEST_CASE("cellHttp against libhttp: cookie limits", "[hle][lle][http]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, cookie_limits_script(), "cookie limits");
}

TEST_CASE("cellHttp against libhttp: cookies", "[hle][lle][http]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, cookie_script(), "cookies");
}

TEST_CASE("cellHttp against libhttp: clients", "[hle][lle][http]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, client_script(), "clients");
}

TEST_CASE("cellHttp against libhttp: probe", "[.][hle][lle][http]")
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    compare(*flash, probe_script(), "probe");
}
