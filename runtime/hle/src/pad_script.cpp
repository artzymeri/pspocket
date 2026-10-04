// Scripted controller presses (pspocket/hle/pad_script.hpp).

#include "pspocket/hle/pad_script.hpp"

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
};

constexpr std::array<ButtonInfo, 16> kButtons{{
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

} // namespace

bool PadPress::down_at(double t) const
{
    if (t < at) return false;
    const double since = every > 0 ? std::fmod(t - at, every) : t - at;
    return since < hold;
}

std::optional<PadPress> parse_pad_press(std::string_view text, std::string* error)
{
    const auto fail = [&](std::string why) -> std::optional<PadPress> {
        if (error) *error = std::format("--press {}: {} (BUTTON@START[+EVERY][:HOLD], seconds)", text, why);
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

    std::string_view rest = text.substr(at + 1);
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
    const auto start = number(rest);
    if (!start) return fail("bad start time");
    p.at = *start;
    if (p.every > 0 && p.hold >= p.every) return fail("the hold time must be shorter than the repeat interval");
    return p;
}

PadInput pad_state_at(const std::vector<PadPress>& presses, double t)
{
    PadInput in;
    for (const PadPress& p : presses) {
        if (!p.down_at(t)) continue;
        in.digital1 = static_cast<u16>(in.digital1 | p.digital1);
        in.digital2 = static_cast<u16>(in.digital2 | p.digital2);
        if (p.pressure >= 0) in.pressure[p.pressure] = 255;
    }
    return in;
}

} // namespace pspocket::hle
