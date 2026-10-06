// The functions RPCS3's own HLE modules leave as stubs, answered as RPCS3 answers them (D-182). The table is generated
// from RPCS3's source (scripts/gen_rpcs3_stubs.py); a real implementation in the registry always takes precedence.

#include <algorithm>
#include <array>

#include "pspocket/hle/hle.hpp"

namespace pspocket::hle {
namespace {

constexpr StubFunction kStubs[] = {
#include "rpcs3_stubs.inc"
};

} // namespace

std::span<const StubFunction> rpcs3_stubs() { return kStubs; }

const StubFunction* find_rpcs3_stub(std::string_view module, u32 nid)
{
    const auto it = std::find_if(std::begin(kStubs), std::end(kStubs),
        [&](const StubFunction& s) { return s.nid == nid && s.module == module; });
    return it == std::end(kStubs) ? nullptr : &*it;
}

} // namespace pspocket::hle
