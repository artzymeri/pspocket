#pragma once

// The scan of an indexed draw's indices before it is encoded (D-957): the range of vertices they reach and where the
// restart indices are. NEON on AArch64 (some 15 times faster than a scalar loop over 3,000 16-bit indices on an M4),
// a scalar loop elsewhere and for the vectors that hold a restart index.

#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::rsx {

struct IndexScan {
    u32 min_vi = ~0u; // smallest (index + base) & 0xfffff of the indices that are not restart indices; ~0u: none
    u32 max_vi = 0;   // largest; 0 when none
    bool has_all_ones = false; // an index of all ones (0xffff or 0xffffffff), restart index or not
};

// `count` big-endian indices of `size` bytes (2 or 4) at `indices`. With `restart_on`, an index equal to
// `restart_index` (compared as stored, before the base, RPCS3 BufferUtils.cpp upload_untouched) reaches no vertex and
// its position goes to `restarts` (appended, ascending). `out`'s min and max are widened, has_all_ones or-ed.
void scan_indices_be(const u8* indices, u32 count, u32 size, u32 base, bool restart_on, u32 restart_index,
    IndexScan& out, std::vector<u32>& restarts);

// The same, one index at a time (the reference the tests compare with).
void scan_indices_be_scalar(const u8* indices, u32 count, u32 size, u32 base, bool restart_on, u32 restart_index,
    IndexScan& out, std::vector<u32>& restarts);

} // namespace pspocket::rsx
