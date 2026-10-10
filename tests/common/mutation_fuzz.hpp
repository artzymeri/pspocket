#pragma once

// Mutation fuzzing of pspocket's own parsers of game data (D-603). A game's files are untrusted input to them: a
// damaged disc, a cut download, a save another version wrote, a header a game filled wrong. Each target takes valid
// inputs the tests' own writers build (synthetic, CLAUDE.md rule 1), corrupts copies of them deterministically and
// hands each to the parser, which must end in an error or a valid result: never a crash, a hang or a report of
// AddressSanitizer or UBSan (the suites run under -DPSPOCKET_SANITIZE=ON too: scripts/asan.sh).
//
//   PSPOCKET_FUZZ_SEED=<n>   the seed (default 0x5EED): a run is the same for the same seed
//   PSPOCKET_FUZZ_ITERS=<n>  rounds (default 1): each target runs n times its default number of inputs
//   PSPOCKET_FUZZ_TRACE=1    prints each input's target, sample and number before it runs, to find the one a crash
//                            or a sanitizer report came from (the same seed gives the same inputs)
//   PSPOCKET_FUZZ_DUMP=FILE  writes each input to FILE before it runs: after a crash, FILE holds the input that made it
//
// Each copy gets one to four mutations: bits flipped, bytes set at random, a run of random bytes, the input cut short
// or extended with random bytes, a part copied over another place, bytes put in or taken out, or a field (a target's
// length, count or offset fields, else any aligned word of the input's first bytes) set to an edge value: 0, 1, all
// ones, the sign bit, the input's size give or take a little, its own value give or take one, doubled, or random.
// A target counts its inputs and how many its parser accepted; one line per target says so at the end.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pspocket/core/types.hpp"

namespace pspocket::test::fuzz {

inline u64 env_u64(const char* name, u64 fallback)
{
    const char* v = std::getenv(name);
    return v && *v ? std::strtoull(v, nullptr, 0) : fallback;
}
inline u64 seed() { return env_u64("PSPOCKET_FUZZ_SEED", 0x5EED); }
inline u64 rounds() { return std::max<u64>(1, env_u64("PSPOCKET_FUZZ_ITERS", 1)); }
inline bool tracing() { return env_u64("PSPOCKET_FUZZ_TRACE", 0) != 0; }
inline const char* dump_path()
{
    const char* p = std::getenv("PSPOCKET_FUZZ_DUMP");
    return p && *p ? p : nullptr;
}

// splitmix64: small, fast and the same everywhere.
class Rng {
public:
    explicit Rng(u64 seed) : s_(seed) {}
    u64 next()
    {
        u64 z = (s_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    u64 below(u64 n) { return n ? next() % n : 0; }
    bool one_in(u64 n) { return below(n) == 0; }
    u8 byte() { return static_cast<u8>(next()); }

private:
    u64 s_;
};

// FNV-1a of a name: each target's and sample's inputs have their own stream of the seed.
inline u64 name_hash(std::string_view s)
{
    u64 h = 0xCBF29CE484222325ull;
    for (const char c : s) h = (h ^ static_cast<u8>(c)) * 0x100000001B3ull;
    return h;
}

// A field worth corrupting: a length, a count or an offset, `size` bytes (1, 2, 4 or 8) at `offset`. A field under a
// stream cipher (a package's entry table, a SELF's metadata: AES-128-CTR) is corrupted in its plain form: `mask` is
// the key stream's bytes there, read as the field (the file holds the plain value XOR mask).
struct Field {
    usize offset = 0;
    u8 size = 4;
    bool little = false; // little-endian (PARAM.SFO, ISO 9660's both-endian pairs, GIF)
    u64 mask = 0;
};

// `count` fields of `size` bytes from `offset`, `stride` apart (`size` when 0).
inline std::vector<Field> fields(usize offset, usize count, u8 size = 4, usize stride = 0, bool little = false)
{
    std::vector<Field> out;
    for (usize i = 0; i < count; i++) out.push_back({offset + i * (stride ? stride : size), size, little});
    return out;
}
inline void append(std::vector<Field>& to, const std::vector<Field>& more) { to.insert(to.end(), more.begin(), more.end()); }

// A field's bytes as a value, in its byte order (with no mask).
inline u64 load_field(std::span<const u8> d, usize offset, u8 size, bool little = false)
{
    u64 v = 0;
    for (u8 i = 0; i < size; i++) {
        const u64 b = d[offset + i];
        v |= little ? b << (8 * i) : b << (8 * (size - 1 - i));
    }
    return v;
}
inline u64 get_field(const std::vector<u8>& d, const Field& f) { return load_field(d, f.offset, f.size, f.little) ^ f.mask; }
inline void set_field(std::vector<u8>& d, const Field& f, u64 v)
{
    v ^= f.mask;
    for (u8 i = 0; i < f.size; i++)
        d[f.offset + i] = static_cast<u8>(f.little ? v >> (8 * i) : v >> (8 * (f.size - 1 - i)));
}

class Mutator {
public:
    explicit Mutator(u64 seed) : rng_(seed) {}
    Rng& rng() { return rng_; }

    // A corrupted copy of `in`, at most `max_size` bytes. `fields` are the target's; without them, any aligned word
    // of the first 512 bytes may be taken for one.
    std::vector<u8> mutate(std::span<const u8> in, std::span<const Field> fields = {}, usize max_size = usize{16} << 20)
    {
        std::vector<u8> d(in.begin(), in.end());
        const u64 n = 1 + rng_.below(4);
        for (u64 i = 0; i < n; i++) apply(d, fields, in.size(), max_size);
        return d;
    }

    // An edge value for a field of `bytes` bytes now holding `current`, in an input of `size` bytes.
    u64 edge_value(u8 bytes, u64 current, usize size)
    {
        const u64 mask = bytes >= 8 ? ~u64{0} : (u64{1} << (8 * bytes)) - 1;
        const u64 small = rng_.below(17);
        u64 v = 0;
        switch (rng_.below(12)) {
        case 0: v = 0; break;
        case 1: v = 1 + rng_.below(4); break;
        case 2: v = mask - rng_.below(4); break;                 // all ones, or nearly
        case 3: v = (mask >> 1) + rng_.below(2); break;          // the sign bit, or the largest positive
        case 4: v = size + small; break;                         // the input's size and past it
        case 5: v = size > small ? size - small : 0; break;      // inside its end
        case 6: v = current + 1 + rng_.below(4); break;          // off by one or a few
        case 7: v = current ? current - 1 - rng_.below(std::min<u64>(current, 4)) : mask; break;
        case 8: v = current * 2 + rng_.below(2); break;
        case 9: v = current << 8; break;
        case 10: v = rng_.below(256); break;
        default: v = rng_.next(); break;
        }
        return v & mask;
    }

private:
    void apply(std::vector<u8>& d, std::span<const Field> fields, usize original, usize max_size)
    {
        const auto pos = [&](usize n) { return n ? static_cast<usize>(rng_.below(n)) : 0; };
        switch (rng_.below(10)) {
        case 0: // bits flipped
            if (!d.empty())
                for (u64 k = 0, n = 1 + rng_.below(8); k < n; k++) d[pos(d.size())] ^= static_cast<u8>(1u << rng_.below(8));
            break;
        case 1: // bytes set at random
            if (!d.empty())
                for (u64 k = 0, n = 1 + rng_.below(8); k < n; k++) d[pos(d.size())] = rng_.byte();
            break;
        case 2: { // a run of random bytes (or of one byte)
            if (d.empty()) break;
            const usize at = pos(d.size());
            const usize len = std::min<usize>(d.size() - at, 1 + rng_.below(rng_.one_in(4) ? 512 : 32));
            const bool same = rng_.one_in(3);
            const u8 fill = same ? (rng_.one_in(2) ? 0 : 0xFF) : 0;
            for (usize k = 0; k < len; k++) d[at + k] = same ? fill : rng_.byte();
            break;
        }
        case 3: { // cut short: anywhere, more often near the start (headers) or the end (the last block)
            if (d.empty()) break;
            usize len = 0;
            switch (rng_.below(3)) {
            case 0: len = pos(std::min<usize>(d.size(), 1024)); break;
            case 1: len = d.size() - 1 - pos(std::min<usize>(d.size(), 64)); break;
            default: len = pos(d.size()); break;
            }
            d.resize(len);
            break;
        }
        case 4: { // extended with random bytes
            const usize extra = std::min<usize>(1 + rng_.below(rng_.one_in(4) ? 4096 : 64), max_size - std::min(max_size, d.size()));
            for (usize k = 0; k < extra; k++) d.push_back(rng_.byte());
            break;
        }
        case 5: { // a part copied over another place (a header's bytes where a table's should be)
            if (d.size() < 2) break;
            const usize len = 1 + pos(std::min<usize>(d.size() / 2, 256));
            const usize from = pos(d.size() - len + 1), to = pos(d.size() - len + 1);
            std::memmove(d.data() + to, d.data() + from, len);
            break;
        }
        case 6: { // bytes put in
            const usize len = std::min<usize>(1 + rng_.below(16), max_size - std::min(max_size, d.size()));
            const usize at = pos(d.size() + 1);
            std::vector<u8> bytes(len);
            for (u8& b : bytes) b = rng_.byte();
            d.insert(d.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin(), bytes.end());
            break;
        }
        case 7: { // bytes taken out
            if (d.empty()) break;
            const usize at = pos(d.size());
            const usize len = std::min<usize>(d.size() - at, 1 + rng_.below(16));
            d.erase(d.begin() + static_cast<std::ptrdiff_t>(at), d.begin() + static_cast<std::ptrdiff_t>(at + len));
            break;
        }
        default: { // a field set to an edge value (twice as likely as each other kind)
            Field f;
            if (!fields.empty() && !rng_.one_in(5)) {
                f = fields[pos(fields.size())];
            } else {
                const usize words = std::min<usize>(d.size(), 512) / 4;
                if (!words) break;
                static constexpr u8 kSizes[] = {1, 2, 4, 4, 8};
                f = {4 * pos(words), kSizes[rng_.below(std::size(kSizes))], false};
            }
            if (f.offset + f.size > d.size()) break; // cut off by an earlier mutation
            set_field(d, f, edge_value(f.size, get_field(d, f), original));
            break;
        }
        }
    }

    Rng rng_;
};

// A valid input of a target, with the fields its mutations corrupt.
struct Sample {
    std::string name;
    std::vector<u8> bytes;
    std::vector<Field> fields;
};

// One target's count: inputs run, and how many its parser accepted (the rest it refused). The line it prints shows
// the fuzzing reached past the first check.
class Tally {
public:
    explicit Tally(std::string name) : name_(std::move(name)) {}
    ~Tally()
    {
        std::printf("fuzz %-24s %8llu inputs, %8llu accepted, %8llu refused (seed %#llx)\n", name_.c_str(),
            static_cast<unsigned long long>(runs_), static_cast<unsigned long long>(accepted_),
            static_cast<unsigned long long>(runs_ - accepted_), static_cast<unsigned long long>(seed()));
        std::fflush(stdout);
    }
    Tally(const Tally&) = delete;
    Tally& operator=(const Tally&) = delete;
    void count(bool accepted)
    {
        runs_++;
        accepted_ += accepted;
    }
    u64 runs() const { return runs_; }
    u64 accepted() const { return accepted_; }
    const std::string& name() const { return name_; }

private:
    std::string name_;
    u64 runs_ = 0, accepted_ = 0;
};

// Runs each sample as it is (which its parser must accept: else the target tests nothing) and then `per_sample` *
// rounds() mutated copies of it (`per_sample` alone when not `scaled`: a target that splits its rounds itself)
// through `body(input, rng)`, which returns whether its parser accepted the input; `rng` is for what the body varies
// besides the input (a buffer size, a call's argument). Each sample's inputs come from their own stream of the seed,
// so adding a sample does not change another's.
template <class Body>
void run(Tally& tally, const std::vector<Sample>& samples, u64 per_sample, Body&& body, usize max_size = usize{16} << 20,
    bool scaled = true)
{
    const bool trace = tracing();
    const char* dump = dump_path();
    for (const Sample& s : samples) {
        Mutator m(seed() ^ name_hash(tally.name()) ^ (name_hash(s.name) << 1));
        const u64 n = per_sample * (scaled ? rounds() : 1);
        for (u64 i = 0; i <= n; i++) {
            std::vector<u8> input = i == 0 ? s.bytes : m.mutate(s.bytes, s.fields, max_size);
            if (trace) {
                std::fprintf(stderr, "fuzz %s / %s #%llu (%zu bytes)\n", tally.name().c_str(), s.name.c_str(),
                    static_cast<unsigned long long>(i), input.size());
                std::fflush(stderr);
            }
            if (dump) {
                if (std::FILE* f = std::fopen(dump, "wb")) {
                    std::fwrite(input.data(), 1, input.size(), f);
                    std::fclose(f);
                }
            }
            const bool accepted = body(input, m.rng());
            if (i == 0) {
                INFO(tally.name() << " / " << s.name << ": the sample as it is");
                CHECK(accepted);
            }
            tally.count(accepted);
        }
    }
}

} // namespace pspocket::test::fuzz
