#pragma once

// The CELP codecs of the CELP libraries (D-932): MPEG-4 Audio CELP (ISO/IEC 14496-3 subpart 3) as Sony's SPU programs
// code it, done on the host: libcelpenc and libcelpdec's wideband coder (16 kHz, regular pulse excitation) and
// libcelp8enc and libcelp8dec's narrowband one (8 kHz, multi-pulse excitation).

#include "pspocket/core/types.hpp"
#include "celp_tables.hpp"

namespace pspocket::hle::celp {

// Shared arithmetic (celp_math.cpp), in the SPU's single precision: callers are inside a celp::spu::Scope.
float cos_spu(float x);
void lsf_to_lpc20(const float lsf[20], float a[20]);
float acos_spu(float x);
int lpc_to_lsf(const float* a, int np, float* lsf);
// One half of a wideband LSP vector (normalised) from its five indices (pan_lspdec), and the whole vector's
// stabilisation (pan_stab), as Sony's programs compute them.
void wb_lsp_half(const u32 idx[5], const float prev[10], float out[10], bool upper);
void stabilise_lsp20(float lsp[20]);
// The wideband LSP quantiser: normalised LSPs and the previous quantised ones into the quantised LSPs and their indices.
void wb_quantize(const float in[20], const float prev[20], float q[20], u32 idx[10]);
// a / b: the SDK's divf4 and GCC's inline division.
float div_sdk(float a, float b);
float div_gcc(float a, float b);

// The wideband decoder (celp_rpe.cpp): libcelpdec's SPU program's state and frame decoding.
struct RpeDecoder {
    u32 config = 0;
    RpeConfig cfg{};
    float prev_lsp[20] = {}; // the previous frame's LSPs, normalised to [0, 1)
    float syn_mem[20] = {};  // the synthesis filter's last outputs, newest first
    float acb[295] = {};     // the adaptive codebook: the last excitation, newest last
    float prev_fcb_gain = 0;

    void reset(u32 configuration);
    // One frame of `size` bytes into frame_size output samples (in 16-bit units, unclamped); `mute` the frame's user
    // data byte (the excitation's largest samples zeroed when not 0). False (nothing decoded, the state unchanged but
    // for nothing) when the frame's bits run past `size`. Inside a spu::Scope.
    bool decode(const u8* au, u32 size, u32* consumed, float* y, u32 mute);
};

// The wideband encoder (celp_rpe_enc.cpp): libcelpenc's SPU program's state and frame encoding.
struct RpeEncoder {
    u32 config = 0;
    RpeConfig cfg{};
    u32 buffer_len = 0;       // the analysis buffer: three frames and half a subframe
    float window[400] = {};   // the LPC analysis window (squared Hamming)
    float gamma_be[20] = {};  // the bandwidth expansion
    float buffer[740] = {};   // the analysis buffer
    u32 frame_ctr = 0;
    float rfc0 = 0;           // the first reflection coefficient of the last analysis
    float lpc[20] = {};       // the last analysis's LPC coefficients (bandwidth expanded)
    float next_lsp[20] = {};  // their LSPs (normalised), quantised next frame
    float cur_lsp[20] = {};   // the LSPs quantised this frame
    float prev_q[20] = {};    // the previous frame's quantised LSPs
    float q_lsp[20] = {};     // this frame's quantised LSPs
    u32 lsp_idx[10] = {};     // and their indices
    float ap[10][20] = {};    // each subframe's LPC coefficients
    // The excitation analysis's state: the LPC analysis filter's, the perceptual weighting filter's and the local
    // decoder's (weighted synthesis) memories, the adaptive codebook, the energy table of its segments, the last fixed
    // codebook gain; the frame's rap table (zeroed each frame) and the subframe's indices.
    float mem_i[20] = {}, wden[20] = {}, s_state[20] = {};
    float acb[295] = {};
    float e_tbl[10][16] = {};
    float rap[10][16] = {};
    float gp = 0;
    float af_cur = 0; // the first order fit's parameter in use: the previous frame's first reflection coefficient
    float afp = 0;
    u32 lag[10] = {}, shape[10] = {}, again_idx[10] = {}, fgain_idx[10] = {};

    void reset(u32 configuration);
    // One frame of frame_size samples (16-bit units) into an access unit; its size in bytes (0 for the first two
    // frames). Inside a spu::Scope.
    u32 encode(const float* pcm, u8* au);
    // One frame as libcelpenc takes it: frame_size little-endian 16-bit samples, or big-endian floats (full scale 1)
    // times 32767 in the SPU's single precision. Inside a spu::Scope.
    u32 encode_frame(const u8* pcm, bool float_input, u8* au);
    void lpc_analysis();
    void excitation(u32 s);
    u32 pack(u8* au) const;
};

// The header libcelpenc's Start writes for a configuration (its SPU program's): 28 bytes.
u32 rpe_header(u32 configuration, u8* out);

} // namespace pspocket::hle::celp
