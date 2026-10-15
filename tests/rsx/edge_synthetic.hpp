#pragma once

// A synthetic EDGE geometry job (D-988): a small mesh made in code, encoded as the GPU work item's blob
// (pspocket/core/edge_job.h) the way GTA V's EDGE job's streams hold it (bit-packed fixed point positions, unit vectors
// in 3 bytes, (weight, bone) bytes, 3x4 matrices, big-endian indices). Shared by the CPU tests (tests/spu) and the
// Metal ones (tests/rsx). No game data: every value is made here.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/core/edge_job.h"

namespace pspocket::edge_test {

struct Mesh {
    std::vector<std::array<float, 3>> pos;
    std::vector<std::array<float, 3>> nrm;      // empty: none
    std::vector<std::array<float, 4>> tan;      // empty: none (w = +-1)
    uint32_t skin = 0;                          // 0, 1 (four bones) or 4 (one bone)
    std::vector<std::array<uint8_t, 8>> weights; // skin 1: (weight, bone) x 4; skin 4: byte 0 the bone
    std::vector<std::array<float, 12>> matrices; // rows
    std::vector<uint16_t> indices;
    uint32_t out_format = 0;                    // 0: F32 positions; 2: + X11Y11Z10N normals + I16N tangents
    // fixed point of the positions: integer and fraction bits of each component, and the stream's offsets
    std::array<uint32_t, 3> int_bits{4, 4, 4}, frac_bits{12, 12, 12};
    std::array<int32_t, 3> bias{-32768, -32768, -32768};
    // the screen transform and culling
    std::array<float, 16> combined{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<float, 4> scale{100, -100, 0.5f, 0}, offset{128.5f, 128.5f, 0.5f, 0};
    std::array<float, 4> bmin{0, 0, 0, 0}, bmax{256, 256, 1, 0};
    uint32_t cull_mode = 2, flip = 0, nopix = 1;
    std::array<uint32_t, 4> culled{0, 0, 1, 0}; // the mode's arithmetic as the SPU's lifted code does it (D-988)
};

// The fixed-point position of a vertex as stored: (value * 2^frac - bias) in int + frac bits.
inline uint32_t fixed_raw(float v, uint32_t frac, int32_t bias)
{
    return static_cast<uint32_t>(static_cast<int32_t>(std::lround(std::ldexp(static_cast<double>(v), static_cast<int>(frac)))) - bias);
}

// A unit vector in EDGE's 24 bits: the largest component left out (its index and sign), the other two of 10 bits.
inline uint32_t unit_vector_bits(const float* v, bool w_positive)
{
    uint32_t sel = 0;
    for (uint32_t i = 1; i < 3; i++)
        if (std::fabs(v[i]) > std::fabs(v[sel])) sel = i;
    float k;
    const uint32_t ku = 0x3AB52E7Cu;
    std::memcpy(&k, &ku, 4);
    const auto q = [&](float a) {
        const long r = std::lround((static_cast<double>(a) + 0.70703125) / static_cast<double>(k));
        return static_cast<uint32_t>(std::clamp<long>(r, 0, 1023));
    };
    uint32_t other[2], n = 0;
    for (uint32_t i = 0; i < 3; i++)
        if (i != sel) other[n++] = i;
    return q(v[other[0]]) << 14 | q(v[other[1]]) << 4 | sel << 2 | (w_positive ? 2u : 0u) | (v[sel] > 0 ? 1u : 0u);
}

inline uint32_t append(std::vector<uint8_t>& blob, const std::vector<uint8_t>& data)
{
    const uint32_t at = static_cast<uint32_t>(blob.size());
    blob.insert(blob.end(), data.begin(), data.end());
    blob.resize(at + ((data.size() + 15) & ~size_t{15}) + 16, 0);
    return at;
}

inline std::vector<uint8_t> make_blob(const Mesh& m)
{
    std::vector<uint8_t> blob(sizeof(ps_edge_job), 0);
    ps_edge_job j{};
    j.num_vertexes = static_cast<uint32_t>(m.pos.size());
    j.num_indexes = static_cast<uint32_t>(m.indices.size());
    // positions, bit-packed big-endian
    uint32_t bits = 0;
    for (uint32_t k = 0; k < 3; k++) {
        j.pos_bits[k] = m.int_bits[k] + m.frac_bits[k];
        j.pos_frac[k] = m.frac_bits[k];
        j.pos_bias[k] = m.bias[k];
        bits += j.pos_bits[k];
    }
    j.pos_stride = (bits + 7) / 8;
    j.pos_bit = 0;
    std::vector<uint8_t> stream(j.num_vertexes * j.pos_stride + 4, 0);
    for (uint32_t v = 0; v < j.num_vertexes; v++) {
        uint32_t at = v * j.pos_stride * 8;
        for (uint32_t k = 0; k < 3; k++) {
            const uint32_t raw = fixed_raw(m.pos[v][k], m.frac_bits[k], m.bias[k]);
            for (uint32_t b = 0; b < j.pos_bits[k]; b++, at++)
                if ((raw >> (j.pos_bits[k] - 1 - b)) & 1) stream[at / 8] |= static_cast<uint8_t>(0x80u >> (at % 8));
        }
    }
    stream.resize(j.num_vertexes * j.pos_stride);
    j.pos_data = append(blob, stream);
    // normals and tangents
    j.nrm_offset = j.tan_offset = ~0u;
    if (!m.nrm.empty() || !m.tan.empty()) {
        j.uv_stride = (m.nrm.empty() ? 0 : 3) + (m.tan.empty() ? 0 : 3);
        j.nrm_offset = m.nrm.empty() ? ~0u : 0;
        j.tan_offset = m.tan.empty() ? ~0u : (m.nrm.empty() ? 0 : 3);
        std::vector<uint8_t> uv(j.num_vertexes * j.uv_stride);
        for (uint32_t v = 0; v < j.num_vertexes; v++) {
            uint8_t* q = uv.data() + v * j.uv_stride;
            if (!m.nrm.empty()) {
                const uint32_t b = unit_vector_bits(m.nrm[v].data(), true);
                q[0] = static_cast<uint8_t>(b >> 16), q[1] = static_cast<uint8_t>(b >> 8), q[2] = static_cast<uint8_t>(b);
                q += 3;
            }
            if (!m.tan.empty()) {
                const uint32_t b = unit_vector_bits(m.tan[v].data(), m.tan[v][3] > 0);
                q[0] = static_cast<uint8_t>(b >> 16), q[1] = static_cast<uint8_t>(b >> 8), q[2] = static_cast<uint8_t>(b);
            }
        }
        j.uv_data = append(blob, uv);
    }
    // skinning
    j.skin = m.skin;
    if (m.skin) {
        std::vector<uint8_t> mats(48 * m.matrices.size());
        for (size_t b = 0; b < m.matrices.size(); b++)
            for (uint32_t i = 0; i < 12; i++) {
                uint32_t u;
                std::memcpy(&u, &m.matrices[b][i], 4);
                ps_st32(mats.data() + 48 * b + 4 * i, u);
            }
        j.matrices = append(blob, mats);
        j.matrix_count = static_cast<uint32_t>(m.matrices.size());
        std::vector<uint8_t> w;
        for (const auto& x : m.weights) {
            if (m.skin == 4) w.push_back(x[0]);
            else w.insert(w.end(), x.begin(), x.end());
        }
        j.weights = append(blob, w);
    }
    // the screen transform and culling
    for (uint32_t i = 0; i < 16; i++) j.combined[i] = m.combined[i];
    for (uint32_t i = 0; i < 4; i++) {
        j.scale[i] = m.scale[i];
        j.offset[i] = m.offset[i];
        j.bound_min[i] = m.bmin[i];
        j.bound_max[i] = m.bmax[i];
        j.culled[i] = m.culled[i];
    }
    std::vector<uint8_t> idx(2 * m.indices.size());
    for (size_t i = 0; i < m.indices.size(); i++) ps_st16(idx.data() + 2 * i, m.indices[i]);
    j.indices = append(blob, idx);
    j.cull_mode = m.cull_mode;
    j.flip = m.flip;
    j.nopix = m.nopix;
    // the output vertex
    if (m.out_format == 2) {
        j.out[0] = {PS_EDGE_ATTR_POSITION, PS_EDGE_FMT_F32, 3, 0, 0};
        j.out[1] = {PS_EDGE_ATTR_NORMAL, PS_EDGE_FMT_X11Y11Z10N, 3, 12, 0};
        j.out[2] = {PS_EDGE_ATTR_TANGENT, PS_EDGE_FMT_I16N, 4, 16, 0};
        j.out_count = 3;
        j.out_stride = 24;
    } else {
        j.out[0] = {PS_EDGE_ATTR_POSITION, PS_EDGE_FMT_F32, 3, 0, 0};
        j.out_count = 1;
        j.out_stride = 12;
    }
    j.magic = PS_EDGE_JOB_MAGIC;
    j.size = static_cast<uint32_t>(blob.size());
    std::memcpy(blob.data(), &j, sizeof j);
    return blob;
}

// A mesh of a few skinned quads (a grid) facing the camera, some turned away, some off screen, one degenerate.
inline Mesh grid(uint32_t skin, uint32_t out_format, uint32_t n = 6)
{
    Mesh m;
    m.skin = skin;
    m.out_format = out_format;
    for (uint32_t y = 0; y <= n; y++)
        for (uint32_t x = 0; x <= n; x++) {
            const float fx = -1.0f + 2.0f * static_cast<float>(x) / static_cast<float>(n);
            const float fy = -1.0f + 2.0f * static_cast<float>(y) / static_cast<float>(n);
            m.pos.push_back({fx * 1.3f, fy * 0.9f, 0.25f * fx * fy});
            const float len = std::sqrt(0.04f * fx * fx + 0.09f * fy * fy + 1.0f);
            m.nrm.push_back({0.2f * fx / len, -0.3f * fy / len, 1.0f / len});
            m.tan.push_back({1.0f / std::sqrt(1.01f), 0.1f / std::sqrt(1.01f), 0.0f, (x + y) % 2 ? 1.0f : -1.0f});
            std::array<uint8_t, 8> w{};
            if (skin == 4) w[0] = static_cast<uint8_t>((x + y) % 3);
            else if ((x + y) % 4 == 0) w = {0, 0xFF, 0, 0, 0, 0, 0, 0}; // one bone: weight 255 for bone 0
            else w = {120, static_cast<uint8_t>(x % 3), 100, static_cast<uint8_t>(y % 3), 35, 2, 0, 1};
            m.weights.push_back(w);
        }
    const auto rot = [](float a, float tx, float ty) {
        const float c = std::cos(a), s = std::sin(a);
        return std::array<float, 12>{c, -s, 0, tx, s, c, 0, ty, 0, 0, 1, 0.1f};
    };
    m.matrices = {rot(0.0f, 0.0f, 0.0f), rot(0.3f, 0.2f, -0.1f), rot(-0.5f, -0.3f, 0.25f)};
    const uint32_t row = n + 1;
    for (uint32_t y = 0; y < n; y++)
        for (uint32_t x = 0; x < n; x++) {
            const uint16_t a = static_cast<uint16_t>(y * row + x), b = static_cast<uint16_t>(a + 1);
            const uint16_t c = static_cast<uint16_t>(a + row), d = static_cast<uint16_t>(c + 1);
            if ((x + y) % 5 == 3) { // turned away
                m.indices.insert(m.indices.end(), {a, b, c, b, d, c});
            } else {
                m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
            }
        }
    m.indices.insert(m.indices.end(), {0, 0, 1}); // degenerate: no pixel
    // the view: pull it so part of the grid falls past the right edge
    m.combined = {1.4f, 0, 0, 0.35f, 0, 1.4f, 0, 0, 0, 0, 0.5f, 0.5f, 0, 0, 0.05f, 1.0f};
    return m;
}

} // namespace pspocket::edge_test
