#pragma once

// The CELP codecs' tables (celp_tables.cpp, D-932).

#include "pspocket/core/types.hpp"

namespace pspocket::hle::celp {

// Wideband LSP vector quantisation, integers over 32768 (first stage), 524288 (second) and 1048576 (predicted second):
// the lower ten LSPs' and the upper ten's.
extern const s16 kLspWL[320], kDWL[640], kPdWL[640];
extern const s16 kLspWU[160], kDWU[400], kPdWU[400];

// The regular pulse excitation's gains: the adaptive codebook's, the fixed codebook's first subframe and its
// differential ones.
extern const float kCbaGain[32], kCbfGain[32], kCbfDiffGain[8];
// Their quantisers' decision levels (the encoder's).
extern const float kCbaDec[32], kCbfDec[31], kCbfDiffDec[8];

// The narrowband coder's (celp_tables_nb.cpp): the LSP vector quantisation, integers over 32768 (first stage), 262144
// (second) and 524288 (predicted second); the pitch interpolation filter over 32768; the gain codebooks over 16384,
// [4 * (subframe of 40 samples) + signal mode][64 pairs: the adaptive codebook's gain, the multi-pulse one's].
extern const s16 kLspNb[160], kDNb[480], kPdNb[480];
extern const s16 kNbFil[37];
extern const s16 kNecGain[1024];

struct RpeConfig {
    u32 frame_bytes, frame_size, n_subframes, sbfrm_size, window, offset, D, Np;
};
extern const RpeConfig kRpeConfigs[4];

} // namespace pspocket::hle::celp
