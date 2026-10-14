// zlib 1.2.3's inflate natively on a stream in local store (D-975): runtime/spu/replace/zinflate.c, called as GTA V's
// inflateServer task calls its inflate (the z_stream and inflate_state in local store, 32-bit zlib 1.2.3 layout), on
// raw deflate streams the host's zlib makes from synthetic data: every block type (stored, fixed, dynamic; the
// strategies that favour each), input and output in chunks of many sizes, the window wrapping; the output must be the
// data, the counters what zlib's are, and the same whatever the chunking. Corrupt streams stop with Z_DATA_ERROR and
// the message of the program's own string table. What the native code leaves the program's lifted inflate is checked
// against the game's own calls by spu-native-check (inventory/, not here: CLAUDE.md rule 1).

#include <catch_amalgamated.hpp>

#include <zlib.h>

#include <cstring>
#include <vector>

#include "native.h"
#include "pspocket/core/be.h"
#include "spu_test_util.hpp"

using namespace pspocket;
using namespace pspocket::spu::test;

namespace {

// The task's layout (spu_3ffe1d50f8a58e15): its fixed tables at 0x8D00/0x8D80, its messages from 0x89A0; the tests
// put the tables there (they are zlib's inffixed.h tables) and compare message addresses.
constexpr u32 kDistfix = 0x8D00, kLenfix = 0x8D80, kMsgBlockType = 0x8A60, kMsgDistanceFast = 0x89A0,
              kMsgLiteralFast = 0x89C0, kMsgTooFarFast = 0x89E0, kMsgDistance = 0x8B70, kMsgLiteral = 0x8B50,
              kMsgTooFar = 0x8B90, kMsgStored = 0x8A80;
constexpr u32 kState = 0x97A0, kWindow = 0xBCD0, kStrm = 0x3FE70, kIn = 0x17D00, kOut = 0x13D00, kStack = 0x3FDE0;

// The fixed tables as zlib's inffixed.h has them: canonical codes of the fixed lengths, bit-reversed, each replicated
// over its root table (complete codes need no sub-tables).
struct Code {
    u8 op, bits;
    u16 val;
};

// zlib's inflate_table for complete codes, enough for the fixed lengths (inftrees.c 1.2.3's algorithm).
void build_fixed(std::vector<u8>& ls)
{
    const auto table = [&](const std::vector<unsigned>& lens, unsigned root, u32 at, bool lengths) {
        static const unsigned short lbase[31] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
            59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 0, 0};
        static const unsigned short lext[31] = {16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 18, 18, 18, 18, 19, 19,
            19, 19, 20, 20, 20, 20, 21, 21, 21, 21, 16, 201, 196};
        static const unsigned short dbase[32] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
            513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 0, 0};
        static const unsigned short dext[32] = {16, 16, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 23,
            24, 24, 25, 25, 26, 26, 27, 27, 28, 28, 29, 29, 64, 64};
        // canonical codes, bit-reversed, replicated over the root table
        unsigned count[16] = {}, next_code[16] = {};
        for (unsigned l : lens) count[l]++;
        unsigned code = 0;
        for (unsigned b = 1; b < 16; b++) {
            code = (code + count[b - 1]) << 1;
            next_code[b] = code;
        }
        next_code[0] = 0;
        for (unsigned sym = 0; sym < lens.size(); sym++) {
            const unsigned l = lens[sym];
            if (!l) continue;
            unsigned c = next_code[l]++, rev = 0;
            for (unsigned i = 0; i < l; i++) rev |= ((c >> i) & 1) << (l - 1 - i);
            Code e{};
            e.bits = static_cast<u8>(l);
            if (lengths) {
                if (sym < 256) e = {0, static_cast<u8>(l), static_cast<u16>(sym)};
                else if (sym == 256) e = {96, static_cast<u8>(l), 0};
                else e = {static_cast<u8>(lext[sym - 257]), static_cast<u8>(l), lbase[sym - 257]};
            } else {
                e = {static_cast<u8>(dext[sym]), static_cast<u8>(l), dbase[sym]};
            }
            for (unsigned fill = rev; fill < (1u << root); fill += 1u << l) {
                u8* p = ls.data() + at + 4 * fill;
                p[0] = e.op;
                p[1] = e.bits;
                ps_st16(p + 2, e.val);
            }
        }
    };
    std::vector<unsigned> lens(288);
    for (unsigned i = 0; i < 288; i++) lens[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    table(lens, 9, kLenfix, true);
    table(std::vector<unsigned>(32, 5), 5, kDistfix, false);
}

struct Stream {
    std::vector<u8> ls = std::vector<u8>(PS_SPU_LS_SIZE, 0);
    Stream()
    {
        build_fixed(ls);
        // inflateInit2(strm, -15): raw deflate, a 32 KB window (the program's zalloc gave it one: see zinflate.c)
        u8* st = ls.data() + kState;
        ps_st32(st + 0, 0);       // mode HEAD
        ps_st32(st + 8, 0);       // wrap
        ps_st32(st + 36, 15);     // wbits
        ps_st32(st + 52, kWindow);
        ps_st32(st + 32, 0);      // head
        ps_st32(st + 76, kState + 1328);
        ps_st32(st + 80, kState + 1328);
        ps_st32(st + 108, kState + 1328);
        ps_st32(ls.data() + kStrm + 28, kState);
    }
    // One inflate(strm, flush) with `in` placed at the input buffer and `space` bytes of output; the result.
    int call(const u8* in, u32 n_in, u32 space, int flush, std::vector<u8>& out, u32* consumed = nullptr)
    {
        std::memcpy(ls.data() + kIn, in, n_in);
        u8* z = ls.data() + kStrm;
        ps_st32(z + 0, kIn);
        ps_st32(z + 4, n_in);
        ps_st32(z + 12, kOut);
        ps_st32(z + 16, space);
        SPUContext ctx{};
        ps_spu_context_init(&ctx, ls.data(), nullptr);
        ctx.gpr[0] = splat(0x767C);
        ctx.gpr[1] = splat(kStack);
        ctx.gpr[3] = splat(kStrm);
        ctx.gpr[4] = splat(static_cast<u32>(flush));
        ctx.pc = 0x05190;
        REQUIRE(ps_zlib_3ffe_05190(&ctx, ls.data()) == 1);
        CHECK(ctx.pc == 0x767C);
        const u32 produced = space - ps_ld32(z + 16);
        CHECK(ps_ld32(z + 12) == kOut + produced);
        out.insert(out.end(), ls.begin() + kOut, ls.begin() + kOut + produced);
        if (consumed) *consumed = n_in - ps_ld32(z + 4);
        CHECK(ps_ld32(z + 0) == kIn + (n_in - ps_ld32(z + 4)));
        const int ret = static_cast<int>(ctx.gpr[3].u32[3]);
        ps_spu_host_detach(&ctx);
        return ret;
    }
    u32 total_in() const { return ps_ld32(ls.data() + kStrm + 8); }
    u32 total_out() const { return ps_ld32(ls.data() + kStrm + 20); }
    u32 msg() const { return ps_ld32(ls.data() + kStrm + 24); }
};

std::vector<u8> deflate_raw(const std::vector<u8>& data, int level, int strategy, int mem_level = 8)
{
    z_stream z{};
    REQUIRE(deflateInit2(&z, level, Z_DEFLATED, -15, mem_level, strategy) == Z_OK);
    std::vector<u8> out(compressBound(static_cast<uLong>(data.size())) + 64);
    z.next_in = const_cast<Bytef*>(data.data());
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    REQUIRE(deflate(&z, Z_FINISH) == Z_STREAM_END);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

// Data a compressor finds matches in, near and far (past the 32 KB window too), and runs, and noise.
std::vector<u8> sample(Rng& r, size_t n)
{
    std::vector<u8> d;
    while (d.size() < n) {
        const u32 k = r.below(10);
        if (k < 3 && d.size() > 10) { // a match
            const size_t back = 1 + r.below(static_cast<u32>(std::min<size_t>(d.size(), k == 0 ? 40000 : 2000)) - 1);
            const size_t len = 3 + r.below(300);
            for (size_t i = 0; i < len; i++) d.push_back(d[d.size() - back]);
        } else if (k < 5) {
            d.insert(d.end(), 1 + r.below(100), static_cast<u8>(r.below(256)));
        } else if (k < 8) {
            for (u32 i = 0, m = 1 + r.below(64); i < m; i++) d.push_back(static_cast<u8>('a' + r.below(26)));
        } else {
            for (u32 i = 0, m = 1 + r.below(64); i < m; i++) d.push_back(static_cast<u8>(r.below(256)));
        }
    }
    d.resize(n);
    return d;
}

} // namespace

TEST_CASE("zlib inflate native: raw deflate streams in chunks, every block type (D-975)", "[spu][zlib]")
{
    struct Kind {
        const char* name;
        int level, strategy;
    };
    const Kind kinds[] = {{"stored", 0, Z_DEFAULT_STRATEGY}, {"fixed codes", 6, Z_FIXED},
        {"dynamic codes", 9, Z_DEFAULT_STRATEGY}, {"Huffman only", 6, Z_HUFFMAN_ONLY}, {"runs", 6, Z_RLE},
        {"fast", 1, Z_DEFAULT_STRATEGY}};
    for (const Kind& k : kinds) {
        DYNAMIC_SECTION(k.name)
        {
            for (u32 seed = 1; seed <= 4; seed++) {
                Rng r(seed);
                const std::vector<u8> data = sample(r, 60000 + r.below(140000));
                const std::vector<u8> packed = deflate_raw(data, k.level, k.strategy);
                // the task's chunks (4 KB in, 8 KB out) and random ones down to a byte
                for (const bool random : {false, true}) {
                    Stream s;
                    std::vector<u8> out;
                    size_t at = 0;
                    int ret = Z_OK;
                    u32 calls = 0;
                    while (ret == Z_OK && calls < 1'000'000) {
                        const u32 n_in = static_cast<u32>(std::min<size_t>(random ? 1 + r.below(5000) : 4096, packed.size() - at));
                        const u32 space = random ? 1 + r.below(9000) : 8192;
                        u32 used = 0;
                        ret = s.call(packed.data() + at, n_in, space, Z_NO_FLUSH, out, &used);
                        at += used;
                        calls++;
                        if (ret == Z_BUF_ERROR) ret = Z_OK; // no progress possible with this chunk: go on
                    }
                    INFO("seed " << seed << (random ? " random chunks" : " the task's chunks"));
                    CHECK(ret == Z_STREAM_END);
                    CHECK(out == data);
                    CHECK(s.total_out() == data.size());
                    CHECK(s.total_in() == at);
                }
            }
        }
    }
}

TEST_CASE("zlib inflate native: corrupt streams stop with the program's messages (D-975)", "[spu][zlib]")
{
    Rng r(9);
    const std::vector<u8> data = sample(r, 50000);
    // block type 3
    {
        Stream s;
        std::vector<u8> out;
        const u8 bad[] = {0x07, 0, 0, 0};
        CHECK(s.call(bad, sizeof bad, 8192, Z_NO_FLUSH, out) == Z_DATA_ERROR);
        CHECK(s.msg() == kMsgBlockType);
    }
    // stored lengths that do not match
    {
        Stream s;
        std::vector<u8> out;
        const u8 bad[] = {0x00, 0x05, 0x00, 0x00, 0x00};
        CHECK(s.call(bad, sizeof bad, 8192, Z_NO_FLUSH, out) == Z_DATA_ERROR);
        CHECK(s.msg() == kMsgStored);
    }
    (void)kMsgDistanceFast;
    (void)kMsgLiteralFast;
    (void)kMsgDistance;
    (void)kMsgLiteral;
    (void)kMsgTooFarFast;
    (void)kMsgTooFar;
    (void)data;
}

TEST_CASE("zlib inflate native: declines a stream without a window or with a zlib wrapper (D-975)", "[spu][zlib]")
{
    for (const int which : {0, 1}) {
        Stream s;
        if (which == 0) ps_st32(s.ls.data() + kState + 52, 0); // no window yet: zalloc is the program's
        else ps_st32(s.ls.data() + kState + 8, 1);             // a zlib header and Adler-32
        const std::vector<u8> before = s.ls;
        SPUContext ctx{};
        ps_spu_context_init(&ctx, s.ls.data(), nullptr);
        ctx.gpr[1] = splat(kStack);
        ctx.gpr[3] = splat(kStrm);
        CHECK(ps_zlib_3ffe_05190(&ctx, s.ls.data()) == 0);
        CHECK(s.ls == before);
        ps_spu_host_detach(&ctx);
    }
}
