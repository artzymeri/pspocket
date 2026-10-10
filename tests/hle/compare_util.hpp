#pragma once

// The comparisons with Sony's modules of the libraries pspocket made without them (the firmware-first ones, D-563) and
// of cellVdec's newest codecs (D-597). Each comparison runs the same calls on the same synthetic inputs through
// pspocket's library and through Sony's module (the user's installed system software, firmware_compare.hpp), each half
// in a process of its own, and writes what a game can see as observations: lines (an answer, an output, a structure's
// bytes) compared one for one, data (pictures, samples) compared under a stated rule, and notes, shown but not
// compared, for what may legitimately differ. A difference names the step, pspocket's value and Sony's; the report at
// the end lists them with a count.
//
// How they run: test_hle "[compare]" (or "[compare][libmixer]" and so on). They are hidden ([.]) from a plain run, so
// ctest and the Mac's gate leave them out until a library matches Sony's: then its test loses the [.] together with
// the library's firmware-first marker (D-563), and joins the gate as the other comparisons have.
// - PSPOCKET_TEST_DEV_FLASH=<an installed /dev_flash> (scripts/gate.sh sets it on the Mac): pspocket's against Sony's.
// - PSPOCKET_COMPARE_SELF=1, without the firmware: pspocket's half twice, the second in Sony's place, so the whole
//   comparison runs here. The halves must agree (one that does not repeat itself would show differences on the Mac
//   that are not Sony's), and each test checks that pspocket's own values are sensible.
// - Neither: skipped.
// PSPOCKET_COMPARE_PRINT=1 also prints every observation of both halves (Sony's answers in full).
// A half that makes no progress for a minute (a module stuck on something pspocket's lv2 lacks) says where it is on
// stderr, every minute, so a hang on the Mac names its step.
//
// No game data: the inputs are built in the tests; Sony's answers are only compared, never saved.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/ppu/runtime.h"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/spu/runtime.h"

namespace pspocket::hle::test::compare {

enum class Mode { Firmware, Self };

struct Setup {
    Mode mode = Mode::Firmware;
    std::string flash; // the installed /dev_flash (Firmware)
    bool print = false;
};

inline bool env_on(const char* name)
{
    const char* v = std::getenv(name);
    return v && *v && std::string_view(v) != "0";
}

// How this run compares; SKIPs the test without the firmware and without PSPOCKET_COMPARE_SELF.
inline Setup setup()
{
    Setup s;
    s.print = env_on("PSPOCKET_COMPARE_PRINT");
    if (const std::optional<std::string> flash = test_dev_flash()) {
        s.mode = Mode::Firmware;
        s.flash = *flash;
        return s;
    }
    if (!env_on("PSPOCKET_COMPARE_SELF"))
        SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with (PSPOCKET_COMPARE_SELF=1 runs pspocket's "
             "half twice)");
    s.mode = Mode::Self;
    return s;
}

// What can be checked without the firmware: each function a comparison calls has this name in the NID database under
// `library` (the names Sony's module exports it by, as RPCS3 registers them) and is pspocket's own under that name,
// and the library is still firmware-first (D-563) when `marked`.
inline void check_names(std::string_view library, std::initializer_list<std::string_view> functions, bool marked = true)
{
    CHECK(recomp::NidDb::builtin().has_module(library));
    for (const std::string_view fn : functions) {
        INFO(std::format("{}::{}", library, fn));
        const u32 nid = recomp::nid_of(fn);
        const std::optional<std::string_view> name = recomp::NidDb::builtin().find(library, nid);
        REQUIRE(name.has_value());
        CHECK(*name == fn);
        const Function* f = registry().find(library, nid);
        REQUIRE(f != nullptr);
        CHECK(f->name == fn);
    }
    if (marked) CHECK(registry().is_firmware_first(library));
}

// Firmware code runs under the interpreters (nothing of it is translated in a test).
struct InterpretMisses {
    const ps_ppu_miss_policy ppu = ps_ppu_get_miss_policy();
    const ps_spu_miss_policy spu = ps_spu_get_miss_policy();
    InterpretMisses()
    {
        ps_ppu_set_miss_policy(PS_PPU_MISS_INTERPRET);
        ps_spu_set_miss_policy(PS_SPU_MISS_INTERPRET);
    }
    ~InterpretMisses()
    {
        ps_ppu_set_miss_policy(ppu);
        ps_spu_set_miss_policy(spu);
    }
    InterpretMisses(const InterpretMisses&) = delete;
    InterpretMisses& operator=(const InterpretMisses&) = delete;
};

// ------------------------------------------------------------------------------------------------- formatting

inline std::string hex(const u8* p, usize n)
{
    std::string s = "hex:";
    for (usize i = 0; i < n; i++) s += std::format("{:02x}", p[i]);
    return s;
}

inline std::string hex(const std::vector<u8>& v) { return hex(v.data(), v.size()); }

// An answer as a game reads r3: a word, signed when it is an error code.
inline std::string answer(u64 r3) { return std::format("{:#x}", static_cast<u32>(r3)); }

inline std::string number(double v) { return std::format("{:.9g}", v); }
inline std::string number(float v) { return number(static_cast<double>(v)); }

// ------------------------------------------------------------------------------------------------- observations

// How two halves' data count as the same.
struct Rule {
    enum class Kind { Exact, Steps, Floats } kind = Kind::Exact;
    int steps = 0;        // Steps: bytes within this many steps of each other
    u32 exact_stride = 0; // Steps: the bytes at exact_offset + k * exact_stride equal all the same (an alpha channel)
    u32 exact_offset = 0;
    double tolerance = 0; // Floats: big-endian f32 values within this (relative past 1, absolute below)

    static Rule exact() { return {}; }
    static Rule within(int steps, u32 exact_stride = 0, u32 exact_offset = 0)
    {
        Rule r;
        r.kind = Kind::Steps;
        r.steps = steps;
        r.exact_stride = exact_stride;
        r.exact_offset = exact_offset;
        return r;
    }
    static Rule floats(double tolerance)
    {
        Rule r;
        r.kind = Kind::Floats;
        r.tolerance = tolerance;
        return r;
    }
};

struct Line {
    std::string key, value;
    std::vector<double> numbers; // compared within `tolerance` when both halves have them (else `value` is)
    double tolerance = 0;
    std::string group; // the report counts differences by group (a table of many lines)
};

struct Data {
    std::string key;
    std::vector<u8> bytes;
    Rule rule;
};

// How far a half has got, for the watchdog: shared, so it outlives an Observed moved out of its half.
struct Progress {
    std::atomic<u64> count{0};
    std::mutex mutex;
    std::string last = "(the start)";
};

// What one half showed, in order. Written on the test's thread; the watchdog reads only its progress.
class Observed {
public:
    std::vector<Line> lines;
    std::vector<Data> data;
    std::vector<std::string> notes;
    std::shared_ptr<Progress> progress = std::make_shared<Progress>();

    // The group of the lines from here on (empty: none).
    void group(std::string name) { group_ = std::move(name); }
    void line(std::string key, std::string value)
    {
        step(key);
        lines.push_back({std::move(key), std::move(value), {}, 0, group_});
    }
    // Values compared within `tolerance` (relative to the larger past 1, else absolute).
    void numbers(std::string key, std::vector<double> values, double tolerance)
    {
        std::string text;
        for (const double v : values) text += (text.empty() ? "" : " ") + number(v);
        step(key);
        lines.push_back({std::move(key), std::move(text), std::move(values), tolerance, group_});
    }
    void bytes(std::string key, const u8* p, usize n, Rule rule = {})
    {
        step(key);
        data.push_back({std::move(key), std::vector<u8>(p, p + n), rule});
    }
    void bytes(std::string key, std::vector<u8> v, Rule rule = {})
    {
        step(key);
        data.push_back({std::move(key), std::move(v), rule});
    }
    void note(std::string text)
    {
        step(text);
        notes.push_back(std::move(text));
    }
    // The value of the first line with this key (sanity checks), or "(missing)".
    std::string value(std::string_view key) const
    {
        for (const Line& l : lines)
            if (l.key == key) return l.value;
        return "(missing)";
    }
    const Data* find_data(std::string_view key) const
    {
        for (const Data& d : data)
            if (d.key == key) return &d;
        return nullptr;
    }

private:
    void step(const std::string& what)
    {
        {
            std::lock_guard lock(progress->mutex);
            progress->last = what.substr(0, 160);
        }
        progress->count.fetch_add(1, std::memory_order_release);
    }
    std::string group_;
};

// Says on stderr where a half is when it has made no progress for a minute (then every minute).
class Watchdog {
public:
    Watchdog(std::shared_ptr<Progress> p, std::string label)
        : p_(std::move(p)), label_(std::move(label)), thread_([this] { run(); })
    {
    }
    ~Watchdog()
    {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    std::string last() const
    {
        std::lock_guard lock(p_->mutex);
        return p_->last;
    }
    void run()
    {
        u64 seen = p_->count.load(std::memory_order_acquire);
        auto since = std::chrono::steady_clock::now();
        std::unique_lock lock(m_);
        while (!cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; })) {
            const u64 now = p_->count.load(std::memory_order_acquire);
            if (now != seen) {
                seen = now;
                since = std::chrono::steady_clock::now();
                continue;
            }
            const auto idle = std::chrono::steady_clock::now() - since;
            if (idle >= std::chrono::seconds(60)) {
                std::fprintf(stderr, "[compare] %s: no progress for %lld s; the last step was \"%s\"\n", label_.c_str(),
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(idle).count()),
                    last().c_str());
                std::fflush(stderr);
                since = std::chrono::steady_clock::now();
            }
        }
    }
    std::shared_ptr<Progress> p_;
    std::string label_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

// ------------------------------------------------------------------------------------------------- the halves

// One half: pspocket's library or Sony's module, its exports reached as a game reaches them (a guest call through the
// export's function descriptor). In Self mode the half standing for Sony's is pspocket's again.
class Side {
public:
    Side(Fixture& f, GuestThread& t, const Setup& s, bool sony_half, std::string library, bool start = true)
        : f_(f), t_(t), library_(std::move(library)), sony_half_(sony_half)
    {
        if (sony_half_ && s.mode == Mode::Firmware)
            firmware_ = std::make_unique<FirmwareLibrary>(f, t, s.flash, library_, start);
    }

    bool sony() const { return firmware_ != nullptr; }
    bool sony_half() const { return sony_half_; }
    std::string who() const { return sony() ? "Sony" : sony_half_ ? "pspocket (again)" : "pspocket"; }
    const std::string& library() const { return library_; }
    const FirmwareLibrary* firmware() const { return firmware_.get(); }

    // The export's descriptor, 0 when the module (or pspocket) has none.
    u32 descriptor(std::string_view function) const { return descriptor_in(library_, function); }
    u32 descriptor_in(std::string_view library, std::string_view function) const
    {
        const u32 nid = recomp::nid_of(function);
        if (firmware_) {
            const auto it = firmware_->module->functions.find({std::string(library), nid});
            return it == firmware_->module->functions.end() ? 0 : it->second;
        }
        if (!registry().find(library, nid)) return 0;
        return f_.hle->link_function(library, nid, function);
    }

    // Calls the export with up to eight arguments on `t` (the half's thread when none); nullopt when it has none.
    std::optional<u64> call(std::string_view function, std::initializer_list<u64> args) const
    {
        return call_on(t_, function, args);
    }
    std::optional<u64> call_on(GuestThread& t, std::string_view function, std::initializer_list<u64> args) const
    {
        const u32 d = descriptor(function);
        if (!d) return std::nullopt;
        return call_guest(t.ctx, d, args);
    }

private:
    Fixture& f_;
    GuestThread& t_;
    std::string library_;
    bool sony_half_ = false;
    std::unique_ptr<FirmwareLibrary> firmware_;
};

// A fixture with the process's TLS set up (the guest threads, callbacks and firmware modules want r13).
struct TlsFixture : Fixture {
    TlsFixture() { REQUIRE(hle->initialize_tls({0, 0, 0x100})); }
};

// One half's process: its fixture, thread, side and observations, a watchdog over them.
template <class F = TlsFixture>
struct Half {
    InterpretMisses interpret;
    F f;
    GuestThread t;
    u8* b;
    Side side;
    Observed o;
    Watchdog watchdog;

    Half(const Setup& s, bool sony_half, std::string library, bool start = true)
        : t(f, 0, "game"), b(f.base()), side(f, t, s, sony_half, std::move(library), start),
          watchdog(o.progress, side.library() + " (" + side.who() + ")")
    {
    }

    // Whether guest memory at an address a module gave back can be read (its first and last byte committed).
    bool readable(u32 addr, u32 size) const
    {
        return size && u64{addr} + size <= 0x100000000ull && f.mem.is_committed(addr) &&
               f.mem.is_committed(addr + size - 1);
    }

    // Calls the export and writes its answer as a line keyed by `key` (the call when empty); a function the module
    // does not export is a line of its own. Returns the answer (all ones without one).
    u64 r(std::string_view function, std::initializer_list<u64> args, std::string key = {})
    {
        if (key.empty()) {
            key = std::string(function) + "(";
            bool first = true;
            for (const u64 a : args) {
                key += std::format("{}{:#x}", first ? "" : ", ", a);
                first = false;
            }
            key += ")";
        }
        const std::optional<u64> v = side.call(function, args);
        o.line(key, v ? answer(*v) : std::string("(not exported)"));
        return v ? *v : ~u64{0};
    }
};

// ------------------------------------------------------------------------------------------------- the report

class Report {
public:
    Report(std::string title, const Setup& s) : title_(std::move(title)), setup_(s) {}

    // Compares two halves' observations (ours first).
    void compare(const Observed& ours, const Observed& theirs, std::string_view their_name)
    {
        theirs_name_ = std::string(their_name);
        if (setup_.print) {
            print("pspocket", ours);
            print(theirs_name_, theirs);
        }
        compare_lines(ours.lines, theirs.lines);
        compare_data(ours.data, theirs.data);
        compare_notes(ours.notes, theirs.notes);
    }

    usize differences() const { return differ_; }

    // The summary, with the differences listed; fails the test on any.
    void finish()
    {
        const std::string summary = std::format("{}: {} of {} observations differ{}", title_, differ_, compared_,
            within_ ? std::format("; {} values within the tolerance but not equal", within_) : std::string());
        std::string text = summary + "\n" + text_;
        if (differ_ > kListed) text += std::format("  ... and {} more\n", differ_ - kListed);
        std::string by_group;
        for (const auto& [name, counts] : groups_)
            if (counts.second) by_group += std::format("  {}: {} of {} differ\n", name, counts.second, counts.first);
        if (!by_group.empty()) text += "by group:\n" + by_group;
        if (!notes_.empty()) text += "notes (not compared):\n" + notes_;
        INFO(text);
        CHECK(differ_ == 0);
        WARN(summary);
    }

private:
    static constexpr usize kListed = 80, kLookahead = 64;

    void differ(std::string what)
    {
        if (++differ_ <= kListed) text_ += "  " + what + "\n";
    }

    // The bytes two "hex:" values differ in (at most twelve), else the two values.
    std::string describe(const Line& a, const Line& b) const
    {
        if (a.value.starts_with("hex:") && b.value.starts_with("hex:") && a.value.size() == b.value.size()) {
            std::string s;
            usize shown = 0, n = 0;
            for (usize i = 4; i + 1 < a.value.size(); i += 2) {
                if (a.value.compare(i, 2, b.value, i, 2) == 0) continue;
                n++;
                if (shown++ < 12)
                    s += std::format(" +{:#x} {}|{}", (i - 4) / 2, a.value.substr(i, 2), b.value.substr(i, 2));
            }
            return std::format("{}: {} bytes differ (pspocket|{}):{}{}", a.key, n, theirs_name_, s,
                shown < n ? " ..." : "");
        }
        return std::format("{}: pspocket {} | {} {}", a.key, a.value, theirs_name_, b.value);
    }

    bool same_numbers(const Line& a, const Line& b)
    {
        if (a.numbers.empty() || a.numbers.size() != b.numbers.size()) return a.value == b.value;
        bool same = true, exact = true;
        for (usize i = 0; i < a.numbers.size(); i++) {
            const double x = a.numbers[i], y = b.numbers[i];
            if (x == y || (std::isnan(x) && std::isnan(y))) continue;
            exact = false;
            const double scale = std::max({1.0, std::fabs(x), std::fabs(y)});
            same = same && std::fabs(x - y) <= std::max(a.tolerance, b.tolerance) * scale;
        }
        if (same && !exact) within_++;
        return same;
    }

    // Counts a compared line in its group, and a difference there.
    void count(const Line& l, bool differs)
    {
        if (l.group.empty()) return;
        auto& c = groups_[l.group];
        c.first++;
        c.second += differs;
    }

    void compare_lines(const std::vector<Line>& a, const std::vector<Line>& b)
    {
        usize i = 0, j = 0;
        while (i < a.size() || j < b.size()) {
            if (i < a.size() && j < b.size() && a[i].key == b[j].key) {
                compared_++;
                const bool same = same_numbers(a[i], b[j]);
                count(a[i], !same);
                if (!same) differ(describe(a[i], b[j]));
                i++;
                j++;
                continue;
            }
            // The halves went different ways: the nearest line where they meet again.
            usize skip_a = 0, skip_b = 0;
            bool found = false;
            for (usize k = 1; k <= kLookahead && !found; k++) {
                if (i + k < a.size() && j < b.size() && a[i + k].key == b[j].key) skip_a = k, found = true;
                else if (j + k < b.size() && i < a.size() && a[i].key == b[j + k].key) skip_b = k, found = true;
            }
            if (!found) {
                compared_++;
                const std::string x = i < a.size() ? a[i].key + " -> " + a[i].value : "(nothing)";
                const std::string y = j < b.size() ? b[j].key + " -> " + b[j].value : "(nothing)";
                differ(std::format("the halves part: pspocket {} | {} {}", x, theirs_name_, y));
                if (i < a.size()) count(a[i++], true);
                if (j < b.size()) count(b[j++], true);
                continue;
            }
            for (usize k = 0; k < skip_a; k++, i++) {
                compared_++;
                count(a[i], true);
                differ(std::format("only pspocket: {} -> {}", a[i].key, a[i].value));
            }
            for (usize k = 0; k < skip_b; k++, j++) {
                compared_++;
                count(b[j], true);
                differ(std::format("only {}: {} -> {}", theirs_name_, b[j].key, b[j].value));
            }
        }
    }

    // Big-endian f32 at `i` (4-byte steps).
    static double f32_at(const std::vector<u8>& v, usize i)
    {
        return static_cast<double>(std::bit_cast<f32>(ps_ld32(v.data() + i)));
    }

    void compare_one(const Data& a, const Data& b)
    {
        compared_++;
        if (a.bytes.size() != b.bytes.size()) {
            differ(std::format("{}: pspocket {} bytes | {} {} bytes", a.key, a.bytes.size(), theirs_name_,
                b.bytes.size()));
            return;
        }
        const Rule& r = a.rule;
        usize bad = 0, close = 0, first = a.bytes.size();
        double worst = 0;
        if (r.kind == Rule::Kind::Floats) {
            for (usize i = 0; i + 4 <= a.bytes.size(); i += 4) {
                const double x = f32_at(a.bytes, i), y = f32_at(b.bytes, i);
                if (x == y || (std::isnan(x) && std::isnan(y))) continue;
                const double d = std::fabs(x - y), scale = std::max({1.0, std::fabs(x), std::fabs(y)});
                worst = std::max(worst, d);
                if (d <= r.tolerance * scale) close++;
                else if (bad++ == 0) first = i;
            }
            within_ += close;
            if (bad)
                differ(std::format("{}: {} of {} values differ by more than {}, the first [{}]: pspocket {} | {} {}; "
                                   "the largest difference {}",
                    a.key, bad, a.bytes.size() / 4, r.tolerance, first / 4, number(f32_at(a.bytes, first)),
                    theirs_name_, number(f32_at(b.bytes, first)), number(worst)));
            return;
        }
        for (usize i = 0; i < a.bytes.size(); i++) {
            const int d = std::abs(static_cast<int>(a.bytes[i]) - static_cast<int>(b.bytes[i]));
            if (!d) continue;
            worst = std::max(worst, static_cast<double>(d));
            const bool exact_here =
                r.kind == Rule::Kind::Exact || (r.exact_stride && i % r.exact_stride == r.exact_offset);
            if (!exact_here && d <= r.steps) close++;
            else if (bad++ == 0) first = i;
        }
        within_ += close;
        if (bad)
            differ(std::format("{}: {} of {} bytes differ{}, the first at +{:#x}: pspocket {:#04x} | {} {:#04x}; the "
                               "largest difference {}",
                a.key, bad, a.bytes.size(), r.kind == Rule::Kind::Steps ? std::format(" by more than {}", r.steps) : "",
                first, a.bytes[first], theirs_name_, b.bytes[first], worst));
    }

    void compare_data(const std::vector<Data>& a, const std::vector<Data>& b)
    {
        std::vector<bool> used(b.size(), false);
        for (const Data& x : a) {
            const Data* match = nullptr;
            for (usize k = 0; k < b.size() && !match; k++)
                if (!used[k] && b[k].key == x.key) {
                    used[k] = true;
                    match = &b[k];
                }
            if (match) compare_one(x, *match);
            else {
                compared_++;
                differ(std::format("{}: pspocket {} bytes | {} none", x.key, x.bytes.size(), theirs_name_));
            }
        }
        for (usize k = 0; k < b.size(); k++)
            if (!used[k]) {
                compared_++;
                differ(std::format("{}: pspocket none | {} {} bytes", b[k].key, theirs_name_, b[k].bytes.size()));
            }
    }

    void compare_notes(const std::vector<std::string>& a, const std::vector<std::string>& b)
    {
        for (usize i = 0; i < std::max(a.size(), b.size()); i++) {
            const std::string x = i < a.size() ? a[i] : "(none)", y = i < b.size() ? b[i] : "(none)";
            if (x == y) continue;
            notes_ += std::format("  pspocket: {}\n  {}: {}\n", x, theirs_name_, y);
        }
    }

    static void print(const std::string& who, const Observed& o)
    {
        std::printf("---- %s\n", who.c_str());
        for (const Line& l : o.lines) std::printf("%s -> %s\n", l.key.c_str(), l.value.c_str());
        for (const Data& d : o.data) std::printf("[data] %s: %zu bytes\n", d.key.c_str(), d.bytes.size());
        for (const std::string& n : o.notes) std::printf("[note] %s\n", n.c_str());
        std::fflush(stdout);
    }

    std::string title_;
    Setup setup_;
    std::string theirs_name_ = "Sony";
    usize compared_ = 0, differ_ = 0, within_ = 0;
    std::string text_, notes_;
    std::map<std::string, std::pair<usize, usize>> groups_; // group -> (compared, differ)
};

// The usual shape: both halves run by `half(sony_half)`, compared, reported; `sane` checks pspocket's own half in Self
// mode. Returns pspocket's observations.
template <class HalfFn, class SaneFn>
Observed run(const Setup& s, const std::string& title, HalfFn half, SaneFn sane)
{
    Observed ours = half(false);
    const Observed theirs = half(true);
    Report report(title, s);
    report.compare(ours, theirs, s.mode == Mode::Firmware ? "Sony" : "pspocket (again)");
    if (s.mode == Mode::Self) sane(ours);
    report.finish();
    return ours;
}

} // namespace pspocket::hle::test::compare
