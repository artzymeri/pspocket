#pragma once

// libsysutil's sub-libraries (D-925) against Sony's modules, from the user's installed system software (D-193's method,
// firmware_compare.hpp; skipped without it): the same script of calls runs in two fresh processes, both with the Sony
// module of the library loaded and started (so their memory and lv2 IDs agree), one calling the module's exports and
// one pspocket's. Sony's module is linked as the loader links any firmware module: its calls into libsysutil (the cxml
// documents, FixedMemory, the packets) go to pspocket's own libsysutil, which D-534 compared with Sony's, so both runs
// share one libsysutil and its channels. After every call the answer, the script's memory, libsysutil's globals and
// FixedMemory, the channels' shared memory, the lv2 calls the script's thread made (with their arguments; addresses on
// either run's stack as "stack") and the requests the system side read must be the same.
//
// The system side: pspocket's console has none (its channels are closed, D-534), and both runs first get that. A
// script may then open a channel: the test lays out its ring as the system software does and answers the requests on
// it from a thread of its own, with replies the script queued (cxml documents built here). That thread acts when the
// script's thread waits on the channel (the observer sees its sys_cond_wait, before lv2 runs it): with a reply queued
// for a wait for data, it takes the channel's lv2 mutex (free once the waiter is waiting), reads the request out of
// the ring, writes the reply in, signals and unlocks; a writer waiting for room while replies are queued gets the ring
// read out; any other wait closes the channel, so nothing waits for ever. So the lv2 calls of the script's thread are
// the same every time.

#include <catch_amalgamated.hpp>

#include <array>
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/recomp/nid.hpp"
#include "sysutil.hpp"
#include "sysutil_cxml.hpp" // runtime/hle/src

namespace pspocket::hle::test::sub {

// ------------------------------------------------------------------------------------------------- replies

// A cxml document built on the host: what the test's system side answers with.
class Cxml {
public:
    // A new element (its index), a child of `parent` (-1: none; the first element is the root).
    int element(std::string name, int parent = -1)
    {
        elems_.push_back({std::move(name), {}, parent, {}});
        const int i = static_cast<int>(elems_.size()) - 1;
        if (parent >= 0) elems_[static_cast<usize>(parent)].children.push_back(i);
        return i;
    }
    Cxml& i32(int e, std::string name, s32 v) { return attr(e, std::move(name), 1, static_cast<u32>(v), 0); }
    Cxml& f32(int e, std::string name, float v) { return attr(e, std::move(name), 2, std::bit_cast<u32>(v), 0); }
    Cxml& str(int e, std::string name, std::string v)
    {
        Attr a{std::move(name), 3, 0, static_cast<u32>(v.size()), {}, std::move(v)};
        elems_[static_cast<usize>(e)].attrs.push_back(std::move(a));
        return *this;
    }
    Cxml& file(int e, std::string name, std::vector<u8> data)
    {
        Attr a{std::move(name), 6, 0, static_cast<u32>(data.size()), std::move(data), {}};
        elems_[static_cast<usize>(e)].attrs.push_back(std::move(a));
        return *this;
    }

    // The document as Document::Write lays it out (version 0x110, the tables 16-byte aligned from 0x40).
    std::vector<u8> bytes(std::string_view magic = "CXML") const
    {
        std::vector<u8> strings, files, tree;
        std::map<std::string, u32> string_at;
        const auto add = [&](const std::string& s) {
            if (const auto it = string_at.find(s); it != string_at.end()) return it->second;
            const u32 off = static_cast<u32>(strings.size());
            strings.insert(strings.end(), s.begin(), s.end());
            strings.push_back(0);
            string_at.emplace(s, off);
            return off;
        };
        std::vector<u32> off(elems_.size());
        u32 at = 0;
        for (usize i = 0; i < elems_.size(); i++) {
            off[i] = at;
            at += 0x1C + 16 * static_cast<u32>(elems_[i].attrs.size());
        }
        tree.resize(at);
        const auto put = [](std::vector<u8>& v, u32 o, u32 x) { ps_write32(v.data(), o, x); };
        for (usize i = 0; i < elems_.size(); i++) {
            const Elem& e = elems_[i];
            const u32 o = off[i];
            put(tree, o, add(e.name));
            put(tree, o + 4, static_cast<u32>(e.attrs.size()));
            put(tree, o + 8, e.parent >= 0 ? off[static_cast<usize>(e.parent)] : 0xFFFFFFFF);
            u32 prev = 0xFFFFFFFF, next = 0xFFFFFFFF;
            if (e.parent >= 0) {
                const auto& sib = elems_[static_cast<usize>(e.parent)].children;
                for (usize k = 0; k < sib.size(); k++) {
                    if (sib[k] != static_cast<int>(i)) continue;
                    if (k > 0) prev = off[static_cast<usize>(sib[k - 1])];
                    if (k + 1 < sib.size()) next = off[static_cast<usize>(sib[k + 1])];
                }
            }
            put(tree, o + 12, prev);
            put(tree, o + 16, next);
            put(tree, o + 20, e.children.empty() ? 0xFFFFFFFF : off[static_cast<usize>(e.children.front())]);
            put(tree, o + 24, e.children.empty() ? 0xFFFFFFFF : off[static_cast<usize>(e.children.back())]);
            for (usize k = 0; k < e.attrs.size(); k++) {
                const Attr& a = e.attrs[k];
                const u32 ao = o + 0x1C + 16 * static_cast<u32>(k);
                u32 v1 = a.v1, v2 = a.v2;
                if (a.type == 3) v1 = add(a.str);
                if (a.type == 6) {
                    v1 = static_cast<u32>(files.size());
                    files.insert(files.end(), a.data.begin(), a.data.end());
                    while (files.size() % 16) files.push_back(0);
                }
                put(tree, ao, add(a.name));
                put(tree, ao + 4, a.type);
                put(tree, ao + 8, v1);
                put(tree, ao + 12, v2);
            }
        }
        std::vector<u8> out(0x40);
        std::memcpy(out.data(), magic.data(), std::min<usize>(magic.size(), 4));
        put(out, 4, 0x110);
        const std::vector<u8>* tables[6] = {&tree, nullptr, &strings, nullptr, nullptr, &files};
        for (u32 n = 0; n < 6; n++) {
            const u32 size = tables[n] ? static_cast<u32>(tables[n]->size()) : 0;
            put(out, 8 + 8 * n, static_cast<u32>(out.size()));
            put(out, 12 + 8 * n, size);
            if (tables[n]) out.insert(out.end(), tables[n]->begin(), tables[n]->end());
            while (out.size() % 16) out.push_back(0);
        }
        return out;
    }

private:
    struct Attr {
        std::string name;
        u32 type, v1, v2;
        std::vector<u8> data;
        std::string str;
    };
    struct Elem {
        std::string name;
        std::vector<Attr> attrs;
        int parent;
        std::vector<int> children;
    };
    Cxml& attr(int e, std::string name, u32 type, u32 v1, u32 v2)
    {
        elems_[static_cast<usize>(e)].attrs.push_back({std::move(name), type, v1, v2, {}, {}});
        return *this;
    }
    std::vector<Elem> elems_;
};

// A reply whose root is `root` with the int attributes given (the common shape of the modules' replies).
inline Cxml reply(std::string root, std::initializer_list<std::pair<std::string, s32>> ints)
{
    Cxml c;
    const int e = c.element(std::move(root));
    for (const auto& [n, v] : ints) c.i32(e, n, v);
    return c;
}

// ------------------------------------------------------------------------------------------------- the trace

inline u64 fnv(const u8* p, usize n)
{
    u64 h = 0xcbf29ce484222325ull;
    for (usize i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

inline std::string hex(const u8* p, usize n)
{
    std::string s;
    for (usize i = 0; i < n; i++) s += std::format("{:02x}", p[i]);
    return s;
}

inline bool on_stack(u64 v) { return v >= kStacks && v < kStacks + 0x100000; }

class SystemSide;

struct Trace {
    std::mutex m;
    std::thread::id script;
    u8* base = nullptr;
    SystemSide* system = nullptr;
    std::vector<std::string> calls;
    // Memory each run has at its own address, named alike in the lv2 calls' arguments (a module's data: Sony's in its
    // image, pspocket's in a block of its own laid out the same). Set before the calls that use it.
    std::vector<std::tuple<u32, u32, std::string>> regions;
};

void observe(void* user, u32 n, const u64* a);

// ------------------------------------------------------------------------------------------------- the system side

class SystemSide {
public:
    SystemSide(Fixture& f, u32 globals) : f_(f), g_(globals)
    {
        thread_ = std::thread([this] { run(); });
        std::unique_lock lock(m_);
        cv_.wait(lock, [this] { return ready_; });
    }
    ~SystemSide()
    {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    u8* b() const { return f_.base(); }
    u32 shm() const { return ps_read32(b(), g_); }
    u32 header(u32 ch) const { return shm() + 0x8000 * ch; }

    // The channel's ring laid out as the system software lays it (0x7C00 bytes at 0x400), empty, the channel open.
    void open(u32 ch)
    {
        const u32 h = header(ch);
        ps_write32(b(), h + 0x00, 0x400);
        ps_write32(b(), h + 0x04, 0x7C00);
        for (u32 o = 0x08; o < 0x1C; o += 4) ps_write32(b(), h + o, 0);
        ps_write8(b(), h + 0x1E, 0);
    }
    void reply(u32 ch, std::vector<u8> bytes)
    {
        std::lock_guard lock(m_);
        replies_[ch].push_back(std::move(bytes));
    }
    void reply(u32 ch, const Cxml& doc) { reply(ch, doc.bytes()); }
    usize pending() const
    {
        std::lock_guard lock(m_);
        return replies_[0].size() + replies_[1].size();
    }

    // From the observer, on the script's thread holding the channel's mutex, before its sys_cond_wait.
    void on_wait(u32 cond)
    {
        for (u32 ch = 0; ch < 2; ch++) {
            for (u32 k = 0; k < 4; k++) {
                if (ps_read32(b(), g_ + 24 * ch + 8 + 4 * k) != cond) continue;
                {
                    std::lock_guard lock(m_);
                    const bool alive = !replies_[ch].empty();
                    jobs_.push_back({ch, k == 0 && alive ? Job::Reply : k == 1 && alive ? Job::Drain : Job::Close});
                }
                cv_.notify_all();
                return;
            }
        }
    }

    std::vector<std::string> take_requests()
    {
        std::lock_guard lock(m_);
        std::vector<std::string> r = std::move(requests_);
        requests_.clear();
        return r;
    }

private:
    struct Job {
        u32 ch;
        enum Kind { Reply, Drain, Close } kind;
    };

    u32 sc(GuestThread& t, u32 n, std::initializer_list<u64> args)
    {
        return static_cast<u32>(lv2_syscall(t.ctx, n, args));
    }

    void run()
    {
        GuestThread t(f_, 1, "system");
        {
            std::lock_guard lock(m_);
            ready_ = true;
        }
        cv_.notify_all();
        for (;;) {
            Job job{};
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_ && jobs_.empty()) return;
                job = jobs_.front();
                jobs_.pop_front();
            }
            const u32 mutex = ps_read32(b(), g_ + 24 * job.ch + 4);
            const u32 h = header(job.ch);
            sc(t, 102, {mutex, 0}); // free once the script's thread waits
            if (job.kind == Job::Close) {
                ps_write8(b(), h + 0x1E, 1);
                for (u32 k = 0; k < 4; k++) sc(t, 109, {ps_read32(b(), g_ + 24 * job.ch + 8 + 4 * k)});
                sc(t, 104, {mutex});
                continue;
            }
            const u32 data = h + ps_read32(b(), h), size = ps_read32(b(), h + 4);
            // The request: what the ring holds, read whole (it is never larger than the ring here).
            u32 rd = ps_read32(b(), h + 0x0C);
            const u32 used = ps_read32(b(), h + 0x10);
            std::vector<u8> request(used);
            for (u32 i = 0; i < used; i++) request[i] = ps_read8(b(), data + (rd + i) % size);
            rd = (rd + used) % size;
            {
                std::lock_guard lock(m_);
                requests_.push_back(std::format("channel {} state {} direction {}: {}", job.ch,
                    ps_read32(b(), h + 0x14), ps_read32(b(), h + 0x18), hex(request.data(), request.size())));
            }
            ps_write32(b(), h + 0x0C, rd);
            if (job.kind == Job::Drain) {
                // A writer waiting for room: the packet read out of the ring, the room signalled.
                ps_write32(b(), h + 0x10, 0);
                ps_write32(b(), h + 0x14, 0);
                ps_write32(b(), h + 0x18, 0);
                sc(t, 108, {ps_read32(b(), g_ + 24 * job.ch + 12)});
                sc(t, 104, {mutex});
                continue;
            }
            std::vector<u8> bytes;
            {
                std::lock_guard lock(m_);
                bytes = std::move(replies_[job.ch].front());
                replies_[job.ch].pop_front();
            }
            // The reply, from where the writer is.
            u32 wr = ps_read32(b(), h + 0x08);
            for (u32 i = 0; i < bytes.size(); i++) ps_write8(b(), data + (wr + i) % size, bytes[i]);
            wr = (wr + static_cast<u32>(bytes.size())) % size;
            ps_write32(b(), h + 0x08, wr);
            ps_write32(b(), h + 0x10, static_cast<u32>(bytes.size()));
            ps_write32(b(), h + 0x14, 2);
            ps_write32(b(), h + 0x18, 2);
            sc(t, 108, {ps_read32(b(), g_ + 24 * job.ch + 8)});
            sc(t, 104, {mutex});
        }
    }

    Fixture& f_;
    u32 g_;
    std::thread thread_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool ready_ = false, stop_ = false;
    std::deque<Job> jobs_;
    std::deque<std::vector<u8>> replies_[2];
    std::vector<std::string> requests_;
};

// The arguments an lv2 call takes (the registers past them hold whatever the caller left: not compared); 8 for the
// calls not listed.
inline u32 lv2_args(u32 n)
{
    switch (n) {
    case 41: case 96: case 98: case 99: case 101: case 103: case 104: case 106: case 108: case 109: case 135: case 137:
    case 141: case 329: case 331: case 342: case 349: case 804: case 807: case 813: case 814: case 30:
        return 1;
    case 44: case 97: case 100: case 102: case 107: case 110: case 129: case 136: case 324: case 335: case 341: case 343:
    case 805: case 808: case 809: case 811: case 812:
        return 2;
    case 105: case 130: case 134: case 348: case 806:
        return 3;
    case 128: case 138: case 330: case 332: case 337: case 350: case 403: case 802: case 803: case 818:
        return 4;
    case 95: case 362:
        return 5;
    case 801:
        return 6;
    case 328:
        return 7;
    default:
        return 8;
    }
}

inline void observe(void* user, u32 n, const u64* a)
{
    auto& tr = *static_cast<Trace*>(user);
    if (std::this_thread::get_id() != tr.script) return;
    const auto arg = [&tr](u64 v) {
        if (on_stack(v)) return std::string("stack");
        for (const auto& [at, size, name] : tr.regions)
            if (v >= at && v < u64{at} + size) return std::format("{}+{:#x}", name, v - at);
        return std::format("{:#x}", v);
    };
    std::string s = std::format("sc {}(", n);
    for (u32 i = 0; i < lv2_args(n); i++) s += std::format("{}{}", i ? ", " : "", arg(a[i]));
    s += ")";
    if (n == 403) s += std::format(" \"{}\"", std::string(reinterpret_cast<const char*>(tr.base + static_cast<u32>(a[1])), static_cast<u32>(a[2])));
    {
        std::lock_guard lock(tr.m);
        tr.calls.push_back(std::move(s));
    }
    if (n == 107 && tr.system) tr.system->on_wait(static_cast<u32>(a[0]));
}

// ------------------------------------------------------------------------------------------------- a run

struct Step {
    std::string call;
    std::vector<u8> small; // the script's memory compared byte for byte
    u64 rest = 0;          // the rest of it
    std::vector<u8> sysutil; // libsysutil's globals and FixedMemory
    u64 shm = 0;
    std::vector<std::string> lv2, requests;
};

inline constexpr u32 kSmall = 0x4000;   // [kData, kData + kSmall): byte for byte
inline constexpr u32 kRegion = 0x40000; // [kData, kData + kRegion): the script's memory

// One run of a script, in its own process: Sony's module's exports, or pspocket's functions.
class Script {
public:
    // `libraries`: the module's libraries (the first loads it); `start`: whether its module_start runs (in both runs).
    Script(const std::string& flash, bool sony, std::vector<std::string> libraries, bool start = true)
        : sony_(sony), libraries_(std::move(libraries)), t_(f_, 0, "ppu")
    {
        REQUIRE(f_.hle->initialize_tls({0, 0, 0x100}));
        b_ = f_.base();
        // libsysutil's channels started first, as a game's process has them, then Sony's module (in both runs).
        globals_ = sysutil::globals(t_.ctx);
        REQUIRE(sysutil::packets_start(t_.ctx) == 0);
        lib_.emplace(f_, t_, flash, libraries_.front(), start);
        std::memset(b_ + kData, 0, kRegion);
        tr_.base = b_;
        tr_.script = std::this_thread::get_id();
        system_.emplace(f_, globals_);
        tr_.system = &*system_;
        f_.kernel->set_syscall_observer(&observe, &tr_);
        step("start");
    }
    ~Script()
    {
        f_.kernel->set_syscall_observer(nullptr, nullptr);
        system_.reset();
    }

    bool sony() const { return sony_; }
    u8* base() const { return b_; }
    Fixture& fixture() { return f_; }
    GuestThread& thread() { return t_; }
    std::vector<Step>& steps() { return steps_; }
    SystemSide& system() { return *system_; }
    u32 globals() const { return globals_; }
    const loader::PrxModule& module() const { return *lib_->module; }

    // A call of `name` (a function known only by its NID is "<library>_<NID>"), recorded as a step. f1 = `f`. The 32 KB
    // below the stack pointer are zeroed first, in both runs, so a module's stack bytes it never writes read alike.
    u32 call(std::string_view name, std::vector<u64> args, double f = 0)
    {
        t_.ctx.fpr[1] = f;
        zero_stack();
        if (std::getenv("PSPOCKET_TEST_VERBOSE")) std::fprintf(stderr, "%s %s\n", sony_ ? "sony" : "mine", std::string(name).c_str());
        u32 v = 0;
        const u64 sp = t_.ctx.gpr[1];
        if (sony_) {
            std::array<u64, 8> a{};
            std::copy(args.begin(), args.end(), a.begin());
            v = static_cast<u32>(call_guest(t_.ctx, descriptor(name), {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}));
        } else {
            const Function& fn = GuestThread::find(name);
            for (usize i = 0; i < args.size(); i++) t_.ctx.gpr[3 + i] = args[i];
            fn.fn(t_.ctx, b_);
            v = static_cast<u32>(t_.ctx.gpr[3]);
        }
        CHECK(t_.ctx.gpr[1] == sp);
        std::string text = std::format("{}(", name);
        for (usize i = 0; i < args.size(); i++) text += std::format("{}{:#x}", i ? ", " : "", args[i]);
        if (f != 0) text += std::format(", f1 {}", f);
        text += std::format(") -> {:#x}", v);
        step(std::move(text));
        return v;
    }
    // A void function's call (r3 left as its code last had it: not compared).
    void call_void(std::string_view name, std::vector<u64> args, double f = 0)
    {
        call(name, std::move(args), f);
        Step& last = steps_.back();
        last.call = last.call.substr(0, last.call.rfind(" -> ")) + " -> (void)";
    }

    // The system's event `event_code` with its two values, to the dispatcher registered for it (as the system software
    // would send it; pspocket's console sends none), recorded as a step.
    void dispatch(u32 event_code, u64 value1, u64 value2)
    {
        zero_stack();
        const u32 fn = sysutil_dispatcher(event_code);
        const u64 sp = t_.ctx.gpr[1];
        if (fn) call_guest(t_.ctx, fn, {value1, value2});
        CHECK(t_.ctx.gpr[1] == sp);
        step(std::format("event {:#x}({:#x}, {:#x}){}", event_code, value1, value2, fn ? "" : ": no dispatcher"));
    }

    // Memory this run has at `at` named `name` in the lv2 calls' arguments.
    void name_region(u32 at, u32 size, std::string name) { tr_.regions.emplace_back(at, size, std::move(name)); }

    // The state after a step.
    void step(std::string text)
    {
        Step s;
        s.call = std::move(text);
        s.small.assign(b_ + kData, b_ + kData + kSmall);
        s.rest = fnv(b_ + kData + kSmall, kRegion - kSmall);
        s.sysutil.assign(b_ + globals_, b_ + globals_ + 0x60);
        s.sysutil.insert(s.sysutil.end(), b_ + globals_ + sysutil::glob::fixed, b_ + globals_ + sysutil::glob::kSize);
        if (const u32 shm = ps_read32(b_, globals_)) s.shm = fnv(b_ + shm, 0x10000);
        {
            std::lock_guard lock(tr_.m);
            s.lv2 = std::move(tr_.calls);
            tr_.calls.clear();
        }
        s.requests = system_->take_requests();
        steps_.push_back(std::move(s));
    }

    void w8(u32 at, u8 v) { ps_write8(b_, at, v); }
    void w32(u32 at, u32 v) { ps_write32(b_, at, v); }
    u32 r32(u32 at) const { return ps_read32(b_, at); }
    void str(u32 at, std::string_view s)
    {
        std::memcpy(b_ + at, s.data(), s.size());
        b_[at + s.size()] = 0;
    }

private:
    void zero_stack()
    {
        const u32 sp = static_cast<u32>(t_.ctx.gpr[1]);
        std::memset(b_ + sp - 0x8000, 0, 0x8000);
    }

    u32 descriptor(std::string_view name) const
    {
        for (const std::string& lib : libraries_) {
            u32 nid = 0;
            if (name.size() == lib.size() + 9 && name.starts_with(lib) && name[lib.size()] == '_') {
                nid = static_cast<u32>(std::stoul(std::string(name.substr(lib.size() + 1)), nullptr, 16));
            } else {
                nid = recomp::nid_of(name);
            }
            const auto it = lib_->module->functions.find({lib, nid});
            if (it != lib_->module->functions.end()) return it->second;
        }
        FAIL(std::format("Sony's module exports no {}", name));
        return 0;
    }

    bool sony_;
    std::vector<std::string> libraries_;
    Fixture f_;
    GuestThread t_;
    std::optional<FirmwareLibrary> lib_;
    std::optional<SystemSide> system_;
    u8* b_ = nullptr;
    u32 globals_ = 0;
    Trace tr_;
    std::vector<Step> steps_;
};

using Body = std::function<void(Script&)>;

inline usize lv2_calls(const std::vector<Step>& r)
{
    usize n = 0;
    for (const Step& s : r) n += s.lv2.size();
    return n;
}

inline void compare(const std::vector<Step>& mine, const std::vector<Step>& sony)
{
    const usize n = std::max(mine.size(), sony.size());
    usize differ = 0;
    for (usize i = 0; i < n && differ < 12; i++) {
        if (i >= mine.size() || i >= sony.size()) {
            UNSCOPED_INFO(std::format("step {}: only one run has it", i));
            differ++;
            continue;
        }
        const Step& o = mine[i];
        const Step& s = sony[i];
        std::string why;
        if (o.call != s.call) why += std::format(" answer: pspocket {} | Sony {};", o.call, s.call);
        if (o.small != s.small) {
            usize at = 0;
            while (at < o.small.size() && o.small[at] == s.small[at]) at++;
            const usize from = at & ~usize{3};
            why += std::format(" memory at {:#x}: pspocket {} | Sony {};", kData + at, hex(o.small.data() + from, 16),
                hex(s.small.data() + from, 16));
        }
        if (o.rest != s.rest) why += " the rest of the script's memory differs;";
        if (o.sysutil != s.sysutil) {
            usize at = 0;
            while (at < o.sysutil.size() && o.sysutil[at] == s.sysutil[at]) at++;
            why += std::format(" libsysutil's globals/FixedMemory at +{:#x};", at);
        }
        if (o.shm != s.shm) why += " the channels' shared memory;";
        if (o.requests != s.requests) {
            why += std::format(" requests: pspocket {} | Sony {};", o.requests.empty() ? "(none)" : o.requests.front(),
                s.requests.empty() ? "(none)" : s.requests.front());
        }
        if (o.lv2 != s.lv2) {
            const usize m = std::max(o.lv2.size(), s.lv2.size());
            for (usize k = 0; k < m; k++) {
                const std::string a = k < o.lv2.size() ? o.lv2[k] : "(none)", c = k < s.lv2.size() ? s.lv2[k] : "(none)";
                if (a != c) {
                    why += std::format(" lv2 call {}: pspocket {} | Sony {};", k, a, c);
                    break;
                }
            }
        }
        if (!why.empty()) {
            UNSCOPED_INFO(std::format("step {} ({}):{}", i, s.call, why));
            differ++;
        }
    }
    CHECK(differ == 0);
}

// Runs `body` against Sony's module and pspocket's and compares them; WARNs the counts.
inline void compare_script(const char* what, std::vector<std::string> libraries, const Body& body, bool start = true)
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    std::vector<Step> sony, mine;
    usize requests = 0;
    {
        Script s(*flash, true, libraries, start);
        body(s);
        CHECK(s.system().pending() == 0);
        sony = std::move(s.steps());
    }
    if (std::getenv("PSPOCKET_TEST_DUMP")) { // Sony's run, step by step (for reading a module's paths)
        for (const Step& st : sony) {
            std::fprintf(stderr, "%s\n", st.call.c_str());
            for (const std::string& c : st.lv2) std::fprintf(stderr, "    %s\n", c.c_str());
            for (const std::string& r : st.requests) std::fprintf(stderr, "    request %s\n", r.substr(0, 160).c_str());
        }
    }
    {
        Script s(*flash, false, libraries, start);
        body(s);
        mine = std::move(s.steps());
    }
    for (const Step& s : sony) requests += s.requests.size();
    INFO(std::format("{}: {} calls, {} lv2 calls, {} requests answered", what, sony.size(), lv2_calls(sony), requests));
    compare(mine, sony);
    WARN(std::format("compared with Sony's module: {}: {} calls, {} lv2 calls, {} requests answered", what,
        sony.size(), lv2_calls(sony), requests));
}

} // namespace pspocket::hle::test::sub
