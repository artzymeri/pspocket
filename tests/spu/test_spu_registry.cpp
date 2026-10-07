// The lifted-program registry while SPU threads run (D-231): programs registered in batches are found whole or not at
// all, an unregistered program's table stays valid for a context still running it, the first of equal keys wins, and
// a program is found for an image met earlier only while the image's code words still hold its code. Synthetic
// programs only (CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "pspocket/core/be.h"
#include "pspocket/spu/registry.hpp"
#include "pspocket/spu/runtime.h"
#include "spu_test_util.hpp"

using namespace pspocket;
using namespace pspocket::spu;

namespace {

// Distinct bodies, so no two share an address.
void fn_0(SPUContext* ctx, uint8_t*) { ctx->pc = 0x100; }
void fn_1(SPUContext* ctx, uint8_t*) { ctx->pc = 0x200; }
void fn_2(SPUContext* ctx, uint8_t*) { ctx->pc = 0x300; }
void fn_3(SPUContext* ctx, uint8_t*) { ctx->pc = 0x400; }
constexpr ps_spu_fn kFns[] = {fn_0, fn_1, fn_2, fn_3};

constexpr u32 kStart = 0x2000, kSize = 0x100, kEntries = 8;

// Every word of a program code (ps_spu_find_program_by_code looks only at programs that list their code words).
constexpr u32 kAllCode[(kSize / 4 + 31) / 32] = {~0u, ~0u};

// Program i: its own image bytes (so its own hash), relocatable, every word code, an entry every 0x10 bytes.
struct Programs {
    std::vector<std::vector<u8>> images;
    std::vector<std::vector<ps_spu_entry>> entries;
    std::vector<ps_spu_program> programs;
    explicit Programs(u32 n, u32 seed)
    {
        images.resize(n);
        entries.resize(n);
        programs.resize(n);
        for (u32 i = 0; i < n; i++) {
            images[i].resize(kSize);
            for (u32 b = 0; b < kSize; b += 4) ps_st32(images[i].data() + b, (seed << 24) ^ (i * 0x10001u) ^ (b * 0x9E37u));
            for (u32 k = 0; k < kEntries; k++) entries[i].push_back({kStart + 0x10 * k, kFns[(i + k) % 4]});
            const u64 hash = ps_spu_hash64(images[i].data(), kSize);
            programs[i] = ps_spu_program{"spu_registry_test", hash, kStart, kSize, PS_SPU_XFLOAT_APPROXIMATE, 1, kEntries,
                entries[i].data(), nullptr, kAllCode, ps_spu_code_hash(images[i].data(), kSize, kAllCode)};
        }
    }
    ~Programs()
    {
        for (ps_spu_program& p : programs) ps_spu_unregister_program(&p);
    }
    // Whole: every entry of program i in its table, as a context attached to it looks it up.
    bool whole(const ps_spu_program* p, u32 i) const
    {
        if (p != &programs[i] || !p->table) return false;
        SPUContext ctx{};
        ps_spu_attach_program_at(&ctx, p, kStart);
        for (const ps_spu_entry& e : entries[i])
            if (ps_spu_lookup(&ctx, e.addr) != e.fn) return false;
        return true;
    }
};

} // namespace

TEST_CASE("SPU registry: programs registered in batches while threads look them up are found whole (D-231)", "[spu]")
{
    constexpr u32 kPrograms = 500, kBatch = 50;
    Programs set(kPrograms, 1);
    const u64 additions = registry_additions(), removals = registry_removals();
    std::atomic<bool> done{false};
    std::atomic<u64> broken{0}, found{0}, looked{0};
    std::vector<std::thread> readers;
    for (u32 t = 0; t < 4; t++) {
        readers.emplace_back([&, t] {
            test::Rng rng(0x5EED + t);
            while (!done.load(std::memory_order_acquire)) {
                const u32 i = static_cast<u32>(rng.next() % kPrograms);
                const ps_spu_program* by_key = ps_spu_find_program(set.programs[i].hash, kStart + 0x40 * t, kSize);
                const ps_spu_program* by_code = ps_spu_find_program_by_code(set.images[i].data(), kSize);
                for (const ps_spu_program* p : {by_key, by_code}) {
                    if (p && !set.whole(p, i)) broken.fetch_add(1);
                    if (p) found.fetch_add(1);
                }
                looked.fetch_add(1);
            }
        });
    }
    for (u32 b = 0; b < kPrograms / kBatch; b++) {
        const RegistryBatch batch;
        for (u32 i = b * kBatch; i < (b + 1) * kBatch; i++) ps_spu_register_program(&set.programs[i]);
    }
    // Long enough that every reader also finds programs after the last batch.
    while (found.load() < 2000) std::this_thread::yield();
    done.store(true, std::memory_order_release);
    for (std::thread& r : readers) r.join();
    CHECK(broken.load() == 0);
    CHECK(looked.load() > 0);
    CHECK(registry_additions() - additions == kPrograms / kBatch);
    CHECK(registry_removals() == removals);
    for (u32 i = 0; i < kPrograms; i++) CHECK(set.whole(ps_spu_find_program(set.programs[i].hash, kStart, kSize), i));

    // Outside a batch, each registration is an addition; registering one again is none.
    const u64 before = registry_additions();
    ps_spu_unregister_program(&set.programs[0]);
    CHECK(registry_removals() == removals + 1);
    ps_spu_register_program(&set.programs[0]);
    ps_spu_register_program(&set.programs[0]);
    CHECK(registry_additions() == before + 1);
}

TEST_CASE("SPU registry: a context keeps running a program unregistered under it (D-231)", "[spu]")
{
    Programs set(1, 2);
    ps_spu_program& p = set.programs[0];
    ps_spu_register_program(&p);
    SPUContext ctx{};
    ps_spu_attach_program_at(&ctx, ps_spu_find_program(p.hash, kStart, kSize), kStart);
    REQUIRE(ctx.program == &p);
    const u64 generation = ps_spu_registry_generation();
    ps_spu_unregister_program(&p);
    CHECK(ps_spu_registry_generation() != generation);
    CHECK(ps_spu_find_program(p.hash, kStart, kSize) == nullptr);
    // Its table is still there (an address sanitizer run would report a use after free otherwise).
    for (const ps_spu_entry& e : set.entries[0]) CHECK(ps_spu_lookup(&ctx, e.addr) == e.fn);
    // Registered again, it is found with the same table.
    const ps_spu_fn* table = p.table;
    ps_spu_register_program(&p);
    CHECK(ps_spu_find_program(p.hash, kStart, kSize) == &p);
    CHECK(p.table == table);
}

TEST_CASE("SPU registry: of two programs with one key, the first registered is found (D-155)", "[spu]")
{
    Programs set(2, 3);
    ps_spu_program& first = set.programs[0];
    ps_spu_program& second = set.programs[1];
    second.hash = first.hash;
    second.code_hash = first.code_hash;
    ps_spu_register_program(&first);
    ps_spu_register_program(&second);
    CHECK(ps_spu_find_program(first.hash, kStart, kSize) == &first);
    CHECK(ps_spu_find_program_by_code(set.images[0].data(), kSize) == &first);
    ps_spu_unregister_program(&first);
    CHECK(ps_spu_find_program(first.hash, kStart, kSize) == &second);
}

TEST_CASE("SPU registry: an image met earlier is its program while its code words hold the program's code (D-231)",
    "[spu]")
{
    test::LocalStore ls;
    constexpr u32 kAt = 0x3000, kElsewhere = 0x5000;
    // The first 32 words are code, the rest data.
    for (u32 b = 0; b < kSize; b += 4) ps_st32(ls.data() + kAt + b, 0x40000000u | (b * 0x1234u));
    const u32 code_words[kSize / 4 / 32] = {0xFFFFFFFFu, 0};
    const ps_spu_entry entries[] = {{kAt, fn_0}};
    const u64 hash = ps_spu_hash64(ls.data() + kAt, kSize);
    ps_spu_program fixed{"spu_registry_fixed", hash, kAt, kSize, PS_SPU_XFLOAT_APPROXIMATE, 0, 1, entries, nullptr,
        code_words, ps_spu_code_hash(ls.data() + kAt, kSize, code_words)};
    ps_spu_register_program(&fixed);

    CHECK(find_program_checked(hash, kAt, kSize, false, ls.data()) == &fixed);
    // A data word changed since the image was met: still the program.
    ps_st32(ls.data() + kAt + 40 * 4, 0xDEADBEEFu);
    CHECK(find_program_checked(hash, kAt, kSize, false, ls.data()) == &fixed);
    // A code word changed: not.
    ps_st32(ls.data() + kAt + 3 * 4, ps_ld32(ls.data() + kAt + 3 * 4) ^ 0x100u);
    CHECK(find_program_checked(hash, kAt, kSize, false, ls.data()) == nullptr);
    ps_st32(ls.data() + kAt + 3 * 4, ps_ld32(ls.data() + kAt + 3 * 4) ^ 0x100u);
    CHECK(find_program_checked(hash, kAt, kSize, false, ls.data()) == &fixed);
    // Elsewhere, a program that is not relocatable is not found.
    std::memcpy(ls.data() + kElsewhere, ls.data() + kAt, kSize);
    CHECK(find_program_checked(hash, kElsewhere, kSize, true, ls.data()) == nullptr);
    ps_spu_unregister_program(&fixed);

    // A relocatable one is, by its key or, when its whole hash is new, by its code words (D-208).
    ps_spu_program moving = fixed;
    moving.name = "spu_registry_moving";
    moving.relocatable = 1;
    moving.table = nullptr;
    ps_spu_register_program(&moving);
    CHECK(find_program_checked(hash, kElsewhere, kSize, true, ls.data()) == &moving);
    ps_st32(ls.data() + kElsewhere + 50 * 4, 0x12345678u);
    const u64 changed = ps_spu_hash64(ls.data() + kElsewhere, kSize);
    CHECK(find_program_checked(changed, kElsewhere, kSize, true, ls.data()) == &moving);
    CHECK(find_program_checked(changed, kElsewhere, kSize, false, ls.data()) == nullptr);
    ps_spu_unregister_program(&moving);

    // A program with no code hash has nothing to be checked against: never.
    ps_spu_program unknown = fixed;
    unknown.name = "spu_registry_unknown";
    unknown.code_hash = 0;
    unknown.table = nullptr;
    ps_spu_register_program(&unknown);
    CHECK(ps_spu_find_program(hash, kAt, kSize) == &unknown);
    CHECK(find_program_checked(hash, kAt, kSize, false, ls.data()) == nullptr);
    ps_spu_unregister_program(&unknown);
}
