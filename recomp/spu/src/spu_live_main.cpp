// spu-live: which registers an SPU program may still read from an address on (interprocedural liveness, D-965).
//
//   spu-live --image FILE --base ADDR --entry ADDR [--seed ADDR]... [--relocatable]
//            [--call SITE=TARGET[,TARGET...]]... [--jump SITE=TARGET[,TARGET...]]... [--tail SITE[=TARGET,...]]...
//            [--abi-unknown-calls] [--variadic ADDR[,ADDR...]] [--frames] [--why] --at ADDR[,ADDR...]
//
// For each --at address: the registers live there, and among them the volatile ones of the SPU ABI ($2, $4-$79). A
// native replacement of a function must leave exact every register live where the function returns to (the return
// sites of the call that reached it). --call names the targets of an indirect call (bisl) at SITE (else: any function),
// --jump those of an indirect jump the analyzer does not resolve, --tail marks an indirect jump into another function
// (a call through a pointer in tail position; no targets: any function). --abi-unknown-calls takes an indirect call with
// no targets given as a call of an ABI callee (a callback: it reads only the link, stack, callee-saved and argument
// registers) rather than of any function. --variadic names functions that save every argument register (a printf to
// the PPU): a call reads only the arguments its caller wrote. --frames prints each frame's summary, --why a path to a read of each volatile
// register found live. Ends with where the analysis had to assume every
// register live. Addresses as the program was lifted (its own, not relocated).

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "pspocket/recomp/spu/analyzer.hpp"
#include "pspocket/recomp/spu/image.hpp"
#include "pspocket/recomp/spu/liveness.hpp"
#include "pspocket/spu/context.h"

using namespace pspocket;
using namespace pspocket::recomp::spu;

namespace {

void usage()
{
    std::fprintf(stderr, "usage: spu-live --image FILE --base ADDR --entry ADDR [--seed ADDR]... [--relocatable]\n"
                         "                [--call SITE=T[,T...]]... [--jump SITE=T[,T...]]... [--tail SITE[=T,...]]...\n"
                         "                --at ADDR[,ADDR...]\n");
}

std::optional<u32> parse_u32(const std::string& text)
{
    try {
        size_t used = 0;
        const unsigned long v = std::stoul(text, &used, 0);
        if (used != text.size() || v > 0xFFFFFFFFul) return std::nullopt;
        return static_cast<u32>(v);
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<u32> parse_list(const std::string& text)
{
    std::vector<u32> out;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t comma = text.find(',', at);
        const std::string item = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (const auto v = parse_u32(item.rfind("0x", 0) == 0 ? item : "0x" + item)) out.push_back(*v);
        else {
            std::fprintf(stderr, "spu-live: bad address '%s'\n", item.c_str());
            std::exit(2);
        }
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return out;
}

std::string regs(const pspocket::spu::RegSet& s, bool volatile_only)
{
    std::string o;
    for (u32 r = 0; r < 128; r++) {
        if (!s.test(r)) continue;
        if (volatile_only && !(r == 2 || (r >= 4 && r <= 79))) continue;
        o += (o.empty() ? "$" : " $") + std::to_string(r);
    }
    return o.empty() ? "none" : o;
}

} // namespace

int main(int argc, char** argv)
{
    std::string image_path;
    std::optional<u32> base;
    std::vector<u32> entries, seeds, at;
    AnalyzerOptions analyzer;
    LivenessOptions options;
    bool frames = false, why = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        auto site_list = [&](std::map<u32, std::vector<u32>>& into) {
            const std::string v = value();
            const size_t eq = v.find('=');
            if (eq == std::string::npos) {
                usage();
                std::exit(2);
            }
            into[parse_list(v.substr(0, eq)).at(0)] = parse_list(v.substr(eq + 1));
        };
        if (arg == "--image") image_path = value();
        else if (arg == "--base") base = parse_list(value()).at(0);
        else if (arg == "--entry") entries.push_back(parse_list(value()).at(0));
        else if (arg == "--seed") seeds.push_back(parse_list(value()).at(0));
        else if (arg == "--relocatable") {
            analyzer.relocatable = true;
            analyzer.constant_entries = false;
        } else if (arg == "--call") site_list(options.indirect_calls);
        else if (arg == "--jump") site_list(options.indirect_jumps);
        else if (arg == "--tail") {
            const std::string v = value();
            const size_t eq = v.find('=');
            options.tail_calls[parse_list(v.substr(0, eq)).at(0)] =
                eq == std::string::npos ? std::vector<u32>{} : parse_list(v.substr(eq + 1));
        }
        else if (arg == "--frames") frames = true;
        else if (arg == "--why") why = true;
        else if (arg == "--abi-unknown-calls") options.abi_unknown_calls = true;
        else if (arg == "--variadic") {
            for (u32 v : parse_list(value())) options.abi_callees.insert(v);
        }
        else if (arg == "--at") {
            for (u32 a : parse_list(value())) at.push_back(a);
        } else {
            usage();
            return 2;
        }
    }
    if (image_path.empty() || !base || entries.empty() || at.empty()) {
        usage();
        return 2;
    }
    std::ifstream in(image_path, std::ios::binary);
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto image = load_raw_image(bytes, *base, entries);
    if (!image) {
        std::fprintf(stderr, "spu-live: %s: %s\n", image_path.c_str(), image.error().message.c_str());
        return 2;
    }
    for (u32 s : seeds)
        if (image->in_code(s)) image->seeds.push_back(s);
    const Program program = analyze(*image, analyzer);
    const Liveness live = analyze_liveness(*image, program, options);
    for (u32 a : at) {
        const auto r = live.at(a);
        std::printf("%05x: live %s\n       volatile live: %s\n", a, regs(r, false).c_str(), regs(r, true).c_str());
        if (why)
            for (u32 reg = 2; reg < 80; reg++)
                if (reg != 3 && r.test(reg)) std::printf("       $%u: %s\n", reg, live.why(a, reg).c_str());
    }
    if (frames)
        for (const auto& [e, u] : live.uses) {
            std::printf("frame %05x: uses %s\n             kills %s\n             returns to", e, regs(u, false).c_str(),
                regs(live.kills.at(e), false).c_str());
            for (u32 r : live.returns.count(e) ? live.returns.at(e) : std::set<u32>{}) std::printf(" %05x", r);
            std::printf("\n");
        }
    for (const std::string& u : live.unknowns) std::printf("assumed: %s\n", u.c_str());
    return 0;
}
