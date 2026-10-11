#pragma once

// Synthetic libsnd3 inputs (D-641, D-642): HD3 bank headers laid out as pspocket reads them (the header and VAG table as
// vgmstream reads a PS3 bank, the tones and programs as the PS2's HD bank's Sample, Program and Split blocks) and
// Standard MIDI Files. Shared by test_snd.cpp and test_snd3_compare.cpp. No game data.

#include <cstring>
#include <initializer_list>
#include <optional>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::hle::test::snd3 {

inline constexpr u32 kP3HD = 0x50334844, kP3PG = 0x50335047, kP3TN = 0x5033544E, kP3VA = 0x50335641;

struct VagSpec {
    u32 offset = 0, rate = 48000, size = 0x400;
};
struct ToneSpec {
    u16 vag = 0;
    u8 vel_low = 0, vel_high = 127, base_note = 60;
    s8 detune = 0;
    u8 pan = 64, priority = 0, volume = 127;
    u16 adsr1 = 0x000F, adsr2 = 0x1FC0;
    u8 spu_attr = 0;
};
struct SplitSpec {
    u16 tone = 0;
    u8 low = 0, high = 127;
    u16 bend_low = 0, bend_high = 0;
    u8 volume = 127, pan = 64;
    s8 transpose = 0, detune = 0;
};
struct ProgramSpec {
    u8 volume = 127, pan = 64;
    s8 transpose = 0, detune = 0;
    std::vector<SplitSpec> splits;
};
struct BankSpec {
    std::vector<VagSpec> vags;
    std::vector<std::optional<ToneSpec>> tones;
    std::vector<std::optional<ProgramSpec>> programs; // none: no P3PG chunk (offset 0)
};

inline void put32(std::vector<u8>& d, usize at, u32 v)
{
    for (u32 i = 0; i < 4; i++) d[at + i] = static_cast<u8>(v >> (24 - 8 * i));
}
inline void put16(std::vector<u8>& d, usize at, u16 v)
{
    d[at] = static_cast<u8>(v >> 8);
    d[at + 1] = static_cast<u8>(v);
}

// A chunk's header: its magic, the size of the rest, the entries' bytes and 0x10 at 0x08, its highest index at 0x14.
inline usize begin_chunk(std::vector<u8>& d, u32 magic, usize entries, usize table)
{
    const usize at = d.size();
    d.resize(at + 0x20 + entries * table, 0);
    put32(d, at, magic);
    put32(d, at + 0x08, static_cast<u32>(entries * table + 0x10));
    put32(d, at + 0x14, static_cast<u32>(entries - 1));
    return at;
}
inline void end_chunk(std::vector<u8>& d, usize at) { put32(d, at + 4, static_cast<u32>(d.size() - at - 8)); }

// The bank's HD3 header: P3HD, then P3PG (when there are programs), P3TN and P3VA.
inline std::vector<u8> build_hd3(const BankSpec& spec)
{
    std::vector<u8> d(0x20, 0);
    usize pg = 0;
    if (!spec.programs.empty()) {
        pg = begin_chunk(d, kP3PG, spec.programs.size(), 4);
        for (usize i = 0; i < spec.programs.size(); i++) {
            if (!spec.programs[i]) {
                put32(d, pg + 0x20 + 4 * i, 0xFFFFFFFF);
                continue;
            }
            const ProgramSpec& p = *spec.programs[i];
            const usize at = d.size();
            put32(d, pg + 0x20 + 4 * i, static_cast<u32>(at - pg));
            d.resize(at + 36 + 20 * p.splits.size(), 0);
            put32(d, at, 36);
            d[at + 4] = static_cast<u8>(p.splits.size());
            d[at + 5] = 20;
            d[at + 6] = p.volume;
            d[at + 7] = p.pan;
            d[at + 8] = static_cast<u8>(p.transpose);
            d[at + 9] = static_cast<u8>(p.detune);
            for (usize k = 0; k < p.splits.size(); k++) {
                const SplitSpec& sp = p.splits[k];
                const usize s = at + 36 + 20 * k;
                put16(d, s, sp.tone);
                d[s + 2] = sp.low;
                d[s + 4] = sp.high;
                d[s + 5] = static_cast<u8>(k);
                put16(d, s + 6, sp.bend_low);
                put16(d, s + 8, sp.bend_high);
                d[s + 16] = sp.volume;
                d[s + 17] = sp.pan;
                d[s + 18] = static_cast<u8>(sp.transpose);
                d[s + 19] = static_cast<u8>(sp.detune);
            }
        }
        end_chunk(d, pg);
    }
    const usize tn = begin_chunk(d, kP3TN, spec.tones.size(), 4);
    for (usize i = 0; i < spec.tones.size(); i++) {
        if (!spec.tones[i]) {
            put32(d, tn + 0x20 + 4 * i, 0xFFFFFFFF);
            continue;
        }
        const ToneSpec& t = *spec.tones[i];
        const usize at = d.size();
        put32(d, tn + 0x20 + 4 * i, static_cast<u32>(at - tn));
        d.resize(at + 44, 0);
        put16(d, at, t.vag);
        d[at + 2] = t.vel_low;
        d[at + 4] = t.vel_high;
        d[at + 11] = t.base_note;
        d[at + 12] = static_cast<u8>(t.detune);
        d[at + 13] = t.pan;
        d[at + 15] = t.priority;
        d[at + 16] = t.volume;
        put16(d, at + 18, t.adsr1);
        put16(d, at + 20, t.adsr2);
        d[at + 41] = t.spu_attr;
    }
    end_chunk(d, tn);
    const usize va = begin_chunk(d, kP3VA, spec.vags.size(), 16);
    for (usize i = 0; i < spec.vags.size(); i++) {
        put32(d, va + 0x20 + 16 * i, spec.vags[i].offset);
        put32(d, va + 0x24 + 16 * i, spec.vags[i].rate);
        put32(d, va + 0x28 + 16 * i, spec.vags[i].size);
        put32(d, va + 0x2C + 16 * i, 0xFFFFFFFF);
    }
    end_chunk(d, va);
    put32(d, 0, kP3HD);
    put32(d, 4, static_cast<u32>(d.size() - 8));
    put32(d, 8, 0x00020000);
    put32(d, 0x10, static_cast<u32>(pg));
    put32(d, 0x14, static_cast<u32>(tn));
    put32(d, 0x18, static_cast<u32>(va));
    return d;
}

// The tests' usual bank: VAG 0 at 24 kHz, VAG 1 at 48 kHz (0x800 bytes on); tone 0 on VAG 0 (base note 60), tone 1 on
// VAG 1 (base 72, priority 5, sent to the effect), tone 2 missing, tone 3 on VAG 0 at half volume, panned right;
// program 0: tone 0 for notes 0-59 and tone 1 for 60-127, program 1 missing, program 2 tone 0 and tone 3 layered over
// every note, transposed up an octave.
inline BankSpec usual_bank()
{
    BankSpec s;
    s.vags = {{0, 24000, 0x800}, {0x800, 48000, 0x800}};
    s.tones = {ToneSpec{.vag = 0}, ToneSpec{.vag = 1, .base_note = 72, .priority = 5, .spu_attr = 0x0C}, std::nullopt,
        ToneSpec{.vag = 0, .pan = 127, .volume = 64}};
    s.programs = {ProgramSpec{.splits = {{.tone = 0, .low = 0, .high = 59}, {.tone = 1, .low = 60, .high = 127}}},
        std::nullopt, ProgramSpec{.transpose = 12, .splits = {{.tone = 0}, {.tone = 3}}}};
    return s;
}

// An SMF track: events after their delta times (variable-length quantities), a tempo, the end of track.
struct Track {
    std::vector<u8> d;
    Track& at(u32 delta, std::initializer_list<u8> bytes)
    {
        u8 v[5];
        u32 n = 0;
        v[n++] = static_cast<u8>(delta & 0x7F);
        while (delta >>= 7) v[n++] = static_cast<u8>(0x80 | (delta & 0x7F));
        while (n) d.push_back(v[--n]);
        d.insert(d.end(), bytes);
        return *this;
    }
    Track& tempo(u32 delta, u32 us)
    {
        return at(delta, {0xFF, 0x51, 0x03, static_cast<u8>(us >> 16), static_cast<u8>(us >> 8), static_cast<u8>(us)});
    }
    Track& end(u32 delta) { return at(delta, {0xFF, 0x2F, 0x00}); }
};

// The file: MThd (format, the tracks' count, ticks a quarter note), then an MTrk chunk a track.
inline std::vector<u8> build_smf(u16 format, u16 division, const std::vector<Track>& tracks)
{
    std::vector<u8> d = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 0, 0, 0};
    d[8] = static_cast<u8>(format >> 8);
    d[9] = static_cast<u8>(format);
    d[10] = static_cast<u8>(tracks.size() >> 8);
    d[11] = static_cast<u8>(tracks.size());
    d[12] = static_cast<u8>(division >> 8);
    d[13] = static_cast<u8>(division);
    for (const Track& t : tracks) {
        const usize at = d.size();
        d.insert(d.end(), {'M', 'T', 'r', 'k', 0, 0, 0, 0});
        put32(d, at + 4, static_cast<u32>(t.d.size()));
        d.insert(d.end(), t.d.begin(), t.d.end());
    }
    return d;
}

// The tests' usual song (division 480, 120 BPM: a tick is 50 samples at 48 kHz): track 0 the tempo and the end at tick
// 960; track 1 channel 0 note 72 from tick 0 to 240 and note 74 (running status, velocity 0 for its off) from 480 to 720.
inline std::vector<u8> usual_song(u32 tempo = 500000)
{
    Track t0, t1;
    t0.tempo(0, tempo).end(960);
    t1.at(0, {0x90, 72, 100}).at(240, {0x80, 72, 0}).at(240, {0x90, 74, 100}).at(240, {74, 0}).end(240);
    return build_smf(1, 480, {t0, t1});
}

inline void place(u8* b, u32 at, const std::vector<u8>& bytes) { std::memcpy(b + at, bytes.data(), bytes.size()); }

} // namespace pspocket::hle::test::snd3
