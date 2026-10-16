// Scripted controller presses (pspocket/hle/pad_script.hpp).

#include "pspocket/hle/pad_script.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

namespace pspocket::hle {
namespace {

struct ButtonInfo {
    std::string_view name;
    u16 digital1, digital2;
    int pressure; // PadInput::pressure: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2
    int axis = -1; // a stick direction (PadPress::axis)
    u8 axis_value = 128;
};

constexpr std::array<ButtonInfo, 24> kButtons{{
    {"left", 0x80, 0, 1},
    {"down", 0x40, 0, 3},
    {"right", 0x20, 0, 0},
    {"up", 0x10, 0, 2},
    {"start", 0x08, 0, -1},
    {"r3", 0x04, 0, -1},
    {"l3", 0x02, 0, -1},
    {"select", 0x01, 0, -1},
    {"square", 0, 0x80, 7},
    {"cross", 0, 0x40, 6},
    {"circle", 0, 0x20, 5},
    {"triangle", 0, 0x10, 4},
    {"r1", 0, 0x08, 9},
    {"l1", 0, 0x04, 8},
    {"r2", 0, 0x02, 11},
    {"l2", 0, 0x01, 10},
    {"rs-left", 0, 0, -1, 0, 0},
    {"rs-right", 0, 0, -1, 0, 255},
    {"rs-up", 0, 0, -1, 1, 0},
    {"rs-down", 0, 0, -1, 1, 255},
    {"ls-left", 0, 0, -1, 2, 0},
    {"ls-right", 0, 0, -1, 2, 255},
    {"ls-up", 0, 0, -1, 3, 0},
    {"ls-down", 0, 0, -1, 3, 255},
}};

// A non-negative decimal number, the whole of `s`. strtod rather than std::from_chars: the iOS app targets iOS 17,
// whose libc++ has no floating-point from_chars.
std::optional<double> number(std::string_view s)
{
    if (s.empty() || s.size() > 31 || !(std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '.')) return std::nullopt;
    char text[32];
    std::memcpy(text, s.data(), s.size());
    text[s.size()] = 0;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end != text + s.size() || !(v >= 0) || !std::isfinite(v)) return std::nullopt;
    return v;
}

// The longest prefix of `s` that is a non-negative decimal number, and its length (0: none).
std::pair<double, usize> leading_number(std::string_view s)
{
    usize n = 0;
    while (n < s.size() && (std::isdigit(static_cast<unsigned char>(s[n])) || s[n] == '.')) n++;
    if (!n) return {0, 0};
    const auto v = number(s.substr(0, n));
    return v ? std::pair{*v, n} : std::pair{0.0, usize{0}};
}

// An anchor at the start of `s` (after "@"): its kind and how many characters it took; Boot and 0 when `s` does not
// start with one. "+Ns" after it is left for the caller.
std::optional<PadAnchor> anchor_prefix(std::string_view s, usize& used, std::string* why)
{
    usize at = 0;
    if (s.starts_with("after:")) at = 6;
    const std::string_view r = s.substr(at);
    PadAnchor a;
    if (r.starts_with("flip:")) {
        const auto [v, n] = leading_number(r.substr(5));
        if (!n || v != std::floor(v)) {
            if (why) *why = "bad frame number after flip:";
            return std::nullopt;
        }
        a.kind = PadAnchor::Kind::Flip;
        a.flip = static_cast<u64>(v);
        used = at + 5 + n;
        return a;
    }
    if (r.starts_with("call:")) {
        usize n = 5;
        while (n < r.size() && (std::isalnum(static_cast<unsigned char>(r[n])) || r[n] == '_')) n++;
        if (n == 5) {
            if (why) *why = "no function name after call:";
            return std::nullopt;
        }
        a.kind = PadAnchor::Kind::Call;
        a.call = std::string(r.substr(5, n - 5));
        used = at + n;
        return a;
    }
    if (r.starts_with("play")) {
        a.kind = PadAnchor::Kind::Play;
        used = at + 4;
        return a;
    }
    if (at) {
        if (why) *why = "after: needs flip:N, call:NAME or play";
        return std::nullopt;
    }
    used = 0;
    return a; // Boot
}

} // namespace

std::string PadAnchor::text() const
{
    std::string out;
    switch (kind) {
    case Kind::Boot: out = "boot"; break;
    case Kind::Flip: out = std::format("flip:{}", flip); break;
    case Kind::Call: out = "call:" + call; break;
    case Kind::Play: out = "play"; break;
    }
    if (offset > 0) out += std::format("+{}s", offset);
    return out;
}

std::optional<PadAnchor> parse_pad_anchor(std::string_view text, std::string* error)
{
    std::string why;
    usize used = 0;
    auto a = anchor_prefix(text, used, &why);
    if (a && used == 0) why = "not an anchor (play, flip:N, call:NAME)";
    if (a && used) {
        std::string_view rest = text.substr(used);
        if (rest.starts_with('+') && rest.ends_with('s')) {
            const auto v = number(rest.substr(1, rest.size() - 2));
            if (v) {
                a->offset = *v;
                rest = {};
            }
        }
        if (rest.empty()) return a;
        why = std::format("'{}' after the anchor", rest);
    }
    if (error) *error = std::format("wait:{}: {}", text, why);
    return std::nullopt;
}

bool PadPress::down_at(double t) const
{
    if (!anchored) return false;
    const double from = *anchored + anchor.offset + at;
    if (t < from) return false;
    const double since = every > 0 ? std::fmod(t - from, every) : t - from;
    return since < hold;
}

std::optional<PadPress> parse_pad_press(std::string_view text, std::string* error, const PadAnchor* base)
{
    const auto fail = [&](std::string why) -> std::optional<PadPress> {
        if (error) {
            *error = std::format("--press {}: {} (BUTTON@START[+EVERY][:HOLD], seconds; START may be flip:N, "
                                 "after:call:NAME or play, with +Ns)", text, why);
        }
        return std::nullopt;
    };
    const usize at = text.find('@');
    if (at == std::string_view::npos) return fail("no '@'");
    PadPress p;
    p.button = std::string(text.substr(0, at));
    const ButtonInfo* info = nullptr;
    for (const ButtonInfo& b : kButtons) {
        if (b.name == p.button) info = &b;
    }
    if (!info) return fail(std::format("unknown button '{}'", p.button));
    p.digital1 = info->digital1;
    p.digital2 = info->digital2;
    p.pressure = info->pressure;
    p.axis = info->axis;
    p.axis_value = info->axis_value;

    std::string_view rest = text.substr(at + 1);
    std::string why;
    usize used = 0;
    const auto anchor = anchor_prefix(rest, used, &why);
    if (!anchor) return fail(why);
    if (used) {
        // An anchor, then [+Ns][+EVERY][:HOLD]: written as seconds 0 for the parse below.
        p.anchor = *anchor;
        p.anchored.reset();
        rest = rest.substr(used);
        if (rest.starts_with('+')) {
            const usize end = rest.find_first_of("+:", 1);
            const std::string_view first = rest.substr(1, end == std::string_view::npos ? std::string_view::npos : end - 1);
            if (first.ends_with('s')) {
                const auto off = number(first.substr(0, first.size() - 1));
                if (!off) return fail("bad offset");
                p.anchor.offset = *off;
                rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
            }
        }
        if (!rest.empty() && rest[0] != '+' && rest[0] != ':') return fail(std::format("'{}' after the anchor", rest));
    } else if (base && base->kind != PadAnchor::Kind::Boot) {
        p.anchor = *base;
        p.anchored.reset();
    }
    if (const usize colon = rest.find(':'); colon != std::string_view::npos) {
        const auto hold = number(rest.substr(colon + 1));
        if (!hold || *hold <= 0) return fail("bad hold time");
        p.hold = *hold;
        rest = rest.substr(0, colon);
    }
    if (const usize plus = rest.find('+'); plus != std::string_view::npos) {
        const auto every = number(rest.substr(plus + 1));
        if (!every || *every <= 0) return fail("bad repeat interval");
        p.every = *every;
        rest = rest.substr(0, plus);
    }
    if (used) {
        if (!rest.empty()) return fail("bad repeat interval");
        p.at = 0;
    } else {
        const auto start = number(rest);
        if (!start) return fail("bad start time");
        p.at = *start;
    }
    if (p.every > 0 && p.hold >= p.every) return fail("the hold time must be shorter than the repeat interval");
    return p;
}

std::vector<std::string> resolve_pad_anchors(std::vector<PadPress>& presses, const PadClock& clock)
{
    std::vector<std::string> fired;
    for (PadPress& p : presses) {
        if (p.anchored) continue;
        bool now = false;
        switch (p.anchor.kind) {
        case PadAnchor::Kind::Boot: now = true; break;
        case PadAnchor::Kind::Flip: now = clock.flips >= p.anchor.flip; break;
        case PadAnchor::Kind::Call: now = clock.called && clock.called(p.anchor.call); break;
        case PadAnchor::Kind::Play: now = clock.playing; break;
        }
        if (!now) continue;
        p.anchored = clock.t;
        PadAnchor bare = p.anchor;
        bare.offset = 0;
        const std::string text = bare.text();
        if (std::find(fired.begin(), fired.end(), text) == fired.end()) fired.push_back(text);
    }
    return fired;
}

bool PlayDetector::update(double t, u64 flips, u64 pad_reads)
{
    if (playing_) return true;
    // The rates over the last second; the streak of seconds in which both were high enough.
    window_.push_back({t, flips, pad_reads});
    while (window_.size() > 2 && window_[1].t <= t - 1.0) window_.erase(window_.begin());
    const Sample& first = window_.front();
    const double span = t - first.t;
    if (span < 0.95) return false;
    const double fps = static_cast<double>(flips - first.flips) / span;
    const double reads = static_cast<double>(pad_reads - first.reads) / span;
    if (fps < 20 || reads < 10) {
        streak_ = -1;
        return false;
    }
    if (streak_ < 0) streak_ = first.t; // the second just measured began the streak
    if (t - streak_ >= 3.0) {
        playing_ = true;
        since_ = t;
    }
    return playing_;
}

PadInput pad_state_at(const std::vector<PadPress>& presses, double t)
{
    PadInput in;
    for (const PadPress& p : presses) {
        if (!p.down_at(t)) continue;
        in.digital1 = static_cast<u16>(in.digital1 | p.digital1);
        in.digital2 = static_cast<u16>(in.digital2 | p.digital2);
        if (p.pressure >= 0) in.pressure[p.pressure] = 255;
        if (p.axis >= 0) {
            u8* const axes[4] = {&in.right_x, &in.right_y, &in.left_x, &in.left_y};
            *axes[p.axis] = p.axis_value;
        }
    }
    return in;
}

} // namespace pspocket::hle
