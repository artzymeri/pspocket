// sceNp's product category functions (D-921) against Sony's libsysutil_np (D-193's method): the same made-up store data
// in two copies, one handled by Sony's exports and one by pspocket's; after every call the answer (addresses as offsets
// into each copy) and every byte of each copy and its category, info and price objects must be the same. Needs
// PSPOCKET_TEST_DEV_FLASH (skipped without it). Synthetic data only.

#include <catch_amalgamated.hpp>

#include <cstring>
#include <format>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/recomp/nid.hpp"

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

// Each side's region: the category object (+0), infos (+0x40, +0x80), the price (+0xC0), the data (+0x100).
constexpr u32 kOurs = kData + 0x10000, kTheirs = kData + 0x40000, kRegion = 0x20000;
constexpr u32 kPc = 0, kInfo = 0x40, kInfo2 = 0x80, kPrice = 0xC0, kStore = 0x100;

struct Commerce {
    Fixture f;
    GuestThread t{f, 0, "ppu"};
    u8* b = f.base();
    std::optional<FirmwareLibrary> sony;
    usize mismatches = 0, calls = 0;

    Commerce()
    {
        if (const auto flash = test_dev_flash()) sony.emplace(f, t, *flash, "sceNp", false);
    }

    u64 ours(std::string_view name, const std::vector<u64>& args)
    {
        const Function* fn = registry().find("sceNp", recomp::nid_of(name));
        REQUIRE(fn != nullptr);
        for (usize i = 0; i < args.size(); i++) t.ctx.gpr[3 + i] = args[i];
        fn->fn(t.ctx, b);
        return t.ctx.gpr[3];
    }

    u64 theirs(std::string_view name, const std::vector<u64>& args)
    {
        const auto it = sony->module->functions.find({std::string("sceNp"), recomp::nid_of(name)});
        REQUIRE(it != sony->module->functions.end());
        u64 a[8] = {};
        for (usize i = 0; i < args.size(); i++) a[i] = args[i];
        return call_guest(t.ctx, it->second, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]});
    }

    // `name` with arguments made for each side's base; `pointer`: the answer is an address into the region.
    void same(std::string_view name, const std::function<std::vector<u64>(u32)>& args, bool pointer, const std::string& what)
    {
        calls++;
        const u64 r1 = ours(name, args(kOurs)), r2 = theirs(name, args(kTheirs));
        const u32 a1 = static_cast<u32>(r1), a2 = static_cast<u32>(r2);
        const bool answer = pointer ? a1 - kOurs == a2 - kTheirs : a1 == a2;
        // Addresses in the objects (info->pc, info->data, pc->data) differ by the bases: compare them as offsets.
        std::vector<u8> o(b + kOurs, b + kOurs + kRegion), s(b + kTheirs, b + kTheirs + kRegion);
        for (const u32 at : {kPc + 4, kInfo, kInfo + 4, kInfo2, kInfo2 + 4}) {
            for (auto* v : {&o, &s}) {
                const u32 base = v == &o ? kOurs : kTheirs;
                u32 w = static_cast<u32>((*v)[at] << 24 | (*v)[at + 1] << 16 | (*v)[at + 2] << 8 | (*v)[at + 3]);
                if (w >= base && w < base + kRegion) w -= base;
                for (u32 k = 0; k < 4; k++) (*v)[at + k] = static_cast<u8>(w >> (24 - 8 * k));
            }
        }
        if (!answer || o != s) {
            if (++mismatches <= 12) {
                usize at = 0;
                while (at < kRegion && o[at] == s[at]) at++;
                UNSCOPED_INFO(std::format("{} {}: ours {:#x}, Sony's {:#x}; first differing byte +{:#x}", name, what, a1, a2, at));
                CHECK(false);
            }
        }
    }
};

// Store data: version, status, currency (code + decimals), category, child categories, SKUs; strings full to their
// fields (no NULs) so each info's terminators show.
std::vector<u8> store(std::mt19937& rng, u32 version, u32 status, u32 decimals, u32 children, u32 skus)
{
    std::vector<u8> d(784 + children * 760 + skus * 1004 + 64);
    for (u8& x : d) x = static_cast<u8>('A' + rng() % 26);
    const auto put32 = [&](u32 at, u32 v) {
        for (u32 k = 0; k < 4; k++) d[at + k] = static_cast<u8>(v >> (24 - 8 * k));
    };
    put32(0, version);
    put32(4, status);
    put32(12, decimals);
    put32(776, children);
    put32(780 + children * 760, skus);
    for (u32 i = 0; i < skus; i++) put32(784 + children * 760 + i * 1004 + 1000, rng() % 100000);
    return d;
}

} // namespace

TEST_CASE("sceNp: the product category functions are libsysutil_np's (D-921)", "[hle][np][compare]")
{
    Commerce c;
    if (!c.sony) SKIP("PSPOCKET_TEST_DEV_FLASH is not set");
    u8* b = c.b;
    std::mt19937 rng(921);
    static const char* kGetters[] = {"sceNpCommerceGetCurrencyCode", "sceNpCommerceGetCategoryId",
        "sceNpCommerceGetCategoryName", "sceNpCommerceGetCategoryDescription", "sceNpCommerceGetCategoryImageURL",
        "sceNpCommerceGetSkuId", "sceNpCommerceGetProductId", "sceNpCommerceGetProductName", "sceNpCommerceGetSkuName",
        "sceNpCommerceGetSkuDescription", "sceNpCommerceGetSkuImageURL", "sceNpCommerceGetSkuUserData"};
    for (u32 round = 0; round < 200; round++) {
        const u32 children = rng() % 4, skus = rng() % 4;
        const u32 version = rng() % 8 ? 1 : rng() % 3;
        const u32 status = rng() % 6 ? 0 : (rng() % 2 ? rng() % 300 : rng());
        const u32 decimals = rng() % 4 ? rng() % 4 : rng() % 40;
        const std::vector<u8> data = store(rng, version, status, decimals, children, skus);
        // The size the category is given: all, or cut anywhere.
        u32 size = static_cast<u32>(data.size());
        if (rng() % 3 == 0) size = rng() % (size + 1);
        const std::string what = std::format("round {} v{} status {:#x} dec {} children {} skus {} size {}", round,
            version, status, decimals, children, skus, size);
        for (const u32 base : {kOurs, kTheirs}) {
            std::memset(b + base, 0, kRegion);
            std::memcpy(b + base + kStore, data.data(), data.size());
        }
        c.same("sceNpCommerceInitProductCategory", [&](u32 base) {
            return std::vector<u64>{base + kPc, base + kStore, size};
        }, false, what);
        c.same("sceNpCommerceGetCurrencyInfo", [&](u32 base) { return std::vector<u64>{base + kPc, base + kInfo}; }, false, what);
        c.same("sceNpCommerceGetCurrencyDecimals", [&](u32 base) { return std::vector<u64>{base + kInfo}; }, false, what);
        c.same("sceNpCommerceGetCategoryInfo", [&](u32 base) { return std::vector<u64>{base + kPc, base + kInfo}; }, false, what);
        c.same("sceNpCommerceGetNumOfChildCategory", [&](u32 base) { return std::vector<u64>{base + kPc, base + kPrice}; }, false, what);
        c.same("sceNpCommerceGetNumOfChildProductSku", [&](u32 base) { return std::vector<u64>{base + kPc, base + kPrice}; }, false, what);
        for (u32 i = 0; i < children + 2; i++) {
            c.same("sceNpCommerceGetChildCategoryInfo", [&](u32 base) {
                return std::vector<u64>{base + kPc, i, base + kInfo2};
            }, false, what + std::format(" child {}", i));
        }
        for (u32 i = 0; i < skus + 2; i++) {
            c.same("sceNpCommerceGetChildProductSkuInfo", [&](u32 base) {
                return std::vector<u64>{base + kPc, i, base + kInfo};
            }, false, what + std::format(" sku {}", i));
            // A void function leaves r3 as it came: an address into the region.
            c.same("sceNpCommerceGetSkuPrice", [&](u32 base) { return std::vector<u64>{base + kInfo, base + kPrice}; }, true, what);
            for (const char* g : kGetters)
                c.same(g, [&](u32 base) { return std::vector<u64>{base + kInfo}; }, true, what + std::format(" sku {}", i));
        }
        c.same("sceNpCommerceDestroyProductCategory", [&](u32 base) { return std::vector<u64>{base + kPc}; }, true, what);
    }
    INFO(std::format("{} calls on each side", c.calls));
    CHECK(c.mismatches == 0);
}
