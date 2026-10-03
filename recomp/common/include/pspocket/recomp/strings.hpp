#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pspocket/core/types.hpp"

// Printable-string extraction and middleware detection.
//
// Middleware is detected by a data table of byte signatures (middleware_signatures()): identifiers and copyright
// strings that a library leaves in the binaries that link it. The same table classifies SPU programs as Sony
// middleware, third-party middleware or custom code (the Phase 0 split, plan section 10).
namespace pspocket::recomp {

struct FoundString {
    u64 offset = 0;
    std::string text;
};

// Runs of printable ASCII (0x20..0x7e and tab) at least min_length long. Stops after max_count strings.
std::vector<FoundString> extract_strings(std::span<const u8> data, usize min_length, u64 base_offset = 0,
                                         usize max_count = ~usize{0});

enum class MiddlewareOrigin : u8 { Sony, ThirdParty };

std::string_view middleware_origin_name(MiddlewareOrigin origin);

struct MiddlewareSignature {
    std::string_view product; // what is reported, e.g. "Edge zlib"
    std::string_view vendor;
    MiddlewareOrigin origin;
    std::string_view pattern; // bytes to find
    bool ignore_case = false;
    // True when finding the pattern inside an SPU program means the program *is* that middleware (Edge zlib,
    // MLAA, a Havok job). False for runtime libraries that custom programs link (SPURS, sync) and for the
    // game's own engine (RAGE): those never make an SPU program "middleware".
    bool identifies_program = false;
};

// The signature table. Several patterns may name the same product.
std::span<const MiddlewareSignature> middleware_signatures();

struct MiddlewareHit {
    std::string_view product;
    std::string_view vendor;
    MiddlewareOrigin origin = MiddlewareOrigin::Sony;
    u32 count = 0;          // occurrences of all of the product's patterns
    u64 first_offset = 0;   // smallest offset of any occurrence (in base_offset coordinates)
    bool identifies_program = false;
    std::vector<std::string_view> patterns; // the patterns that matched
};

// One hit per product found in data, in table order.
std::vector<MiddlewareHit> detect_middleware(std::span<const u8> data, u64 base_offset = 0);

enum class SpuProgramClass : u8 { SonyMiddleware, ThirdPartyMiddleware, Custom };

std::string_view spu_program_class_name(SpuProgramClass c);

// Sony if a program-identifying Sony signature matched, else third-party if a program-identifying third-party
// signature matched, else custom.
SpuProgramClass classify_spu_program(const std::vector<MiddlewareHit>& hits);

} // namespace pspocket::recomp
