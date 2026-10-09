// cellSysutil's cxml, cxmlutil, sysutil_cxmlutil, packets and shared memories (D-534) against libsysutil, Sony's own
// module, from the user's installed system software (D-193's method): the same script runs in two fresh processes,
// both with libsysutil loaded (so their memory is laid out alike), one calling libsysutil's exports and one pspocket's.
// After every call the answer, the script's memory (documents, element, attribute and file objects, strings, the test
// allocator's arena and its log of calls, the test writer's output), libsysutil's globals and FixedMemory blocks (at
// libsysutil's data in one run, pspocket's block in the other: addresses into them are compared as offsets), the
// channels' shared memory and the lv2 calls the thread made, with their arguments, must be the same. Both runs start
// libsysutil's channels as pspocket's console has them (sysutil_cxml.hpp packets_start): Sony's module through its own
// code for the system side and for closing them. The allocator and the writer documents are given are PPC code built
// here. Skipped without the firmware (PSPOCKET_TEST_DEV_FLASH, firmware_compare.hpp); no game data.

#include <catch_amalgamated.hpp>

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/recomp/nid.hpp"
#include "sysutil_cxml.hpp" // runtime/hle/src

using namespace pspocket;
using namespace pspocket::hle;
using namespace pspocket::hle::test;

namespace {

// The script's memory.
constexpr u32 kDocs = kData + 0x0000;     // four documents, 0x80 apart
constexpr u32 kObj = kData + 0x0200;      // element, attribute and file objects, outputs (0x600)
constexpr u32 kStr = kData + 0x0800;      // strings (0x800)
constexpr u32 kFiles = kData + 0x1000;    // file data (0x1000)
constexpr u32 kSmall = kData + 0x2000;    // the region snapshotted whole after each call
constexpr u32 kAlloc = kData + 0x2000;    // the allocator's state: count, limit, arena; its log from +16 (16 bytes each)
constexpr u32 kWriter = kData + 0x7000;   // the writer's state: cursor, fail at, calls, fail value, short at
constexpr u32 kCode = kData + 0x7400;     // the allocator's code (+0), the writer's (+0x100), their descriptors
constexpr u32 kDescs = kData + 0x7800;    // descriptors: allocator, writer, libsysutil's internal functions
constexpr u32 kArena = kData + 0x8000;    // the allocator's arena: a block of 0x4000 per table type (0x18000)
constexpr u32 kOut = kData + 0x20000;     // the writer's output (0x8000)
constexpr u32 kBuf = kData + 0x28000;     // buffers documents are made from (0x8000)
constexpr u32 kRegion = 0x30000;          // [kData, kData + kRegion) is compared after each call

constexpr u32 kAllocDesc = kDescs, kWriterDesc = kDescs + 8, kInternal = kDescs + 0x10;

// ------------------------------------------------------------------------------------------------- PPC code

// The few instructions the test's guest functions need.
struct Asm {
    u32 base;
    std::vector<u32> w;

    u32 here() const { return base + 4 * static_cast<u32>(w.size()); }
    void emit(u32 x) { w.push_back(x); }
    static u32 d(u32 op, u32 rt, u32 ra, s32 disp) { return op << 26 | rt << 21 | ra << 16 | (static_cast<u32>(disp) & 0xFFFF); }
    void lwz(u32 rt, s32 disp, u32 ra) { emit(d(32, rt, ra, disp)); }
    void stw(u32 rs, s32 disp, u32 ra) { emit(d(36, rs, ra, disp)); }
    void lbzu(u32 rt, s32 disp, u32 ra) { emit(d(35, rt, ra, disp)); }
    void stbu(u32 rs, s32 disp, u32 ra) { emit(d(39, rs, ra, disp)); }
    void addi(u32 rt, u32 ra, s32 si) { emit(d(14, rt, ra, si)); }
    void li(u32 rt, s32 si) { addi(rt, 0, si); }
    void slwi(u32 ra, u32 rs, u32 n) { emit(21u << 26 | rs << 21 | ra << 16 | n << 11 | 0u << 6 | (31 - n) << 1); }
    void add(u32 rt, u32 ra, u32 rb) { emit(31u << 26 | rt << 21 | ra << 16 | rb << 11 | 266u << 1); }
    void mr(u32 ra, u32 rs) { emit(31u << 26 | rs << 21 | ra << 16 | rs << 11 | 444u << 1); }
    void mtctr(u32 rs) { emit(31u << 26 | rs << 21 | 9u << 16 | 467u << 1); }
    void cmplwi(u32 cr, u32 ra, u32 ui) { emit(10u << 26 | cr << 23 | ra << 16 | (ui & 0xFFFF)); }
    void cmplw(u32 cr, u32 ra, u32 rb) { emit(31u << 26 | cr << 23 | ra << 16 | rb << 11 | 32u << 1); }
    void cmpw(u32 cr, u32 ra, u32 rb) { emit(31u << 26 | cr << 23 | ra << 16 | rb << 11); }
    void cmpwi(u32 cr, u32 ra, s32 si) { emit(d(11, cr << 2, ra, si)); }
    void blr() { emit(0x4E800020); }
    // A conditional branch to a label fixed later (BO, BI).
    usize bc(u32 bo, u32 bi)
    {
        emit(16u << 26 | bo << 21 | bi << 16);
        return w.size() - 1;
    }
    void bc_to(u32 bo, u32 bi, u32 target)
    {
        const s32 off = static_cast<s32>(target - here());
        emit(16u << 26 | bo << 21 | bi << 16 | (static_cast<u32>(off) & 0xFFFC));
    }
    void fix(usize at)
    {
        const s32 off = static_cast<s32>(here() - (base + 4 * static_cast<u32>(at)));
        w[at] |= static_cast<u32>(off) & 0xFFFC;
    }
    void put(u8* b) const
    {
        for (usize i = 0; i < w.size(); i++) ps_write32(b, base + 4 * static_cast<u32>(i), w[i]);
    }
};

constexpr u32 BO_TRUE = 12, BO_FALSE = 4, LT = 0, GT = 1, EQ = 2;
constexpr u32 cr(u32 n, u32 bit) { return 4 * n + bit; }

// The documents' allocator: (type, state, old, size, out, out_capacity). It logs (type, old, size) and gives table type
// t the block arena + 0x4000 * t with the state's limit as its capacity, or nothing past the limit; types 6..11 free.
void build_allocator(u8* b)
{
    Asm a{kCode, {}};
    a.lwz(9, 0, 4);
    a.slwi(10, 9, 4);
    a.add(10, 10, 4);
    a.stw(3, 16, 10);
    a.stw(5, 20, 10);
    a.stw(6, 24, 10);
    a.addi(9, 9, 1);
    a.stw(9, 0, 4);
    a.cmplwi(7, 3, 6);
    const usize to_ret = a.bc(BO_FALSE, cr(7, LT)); // type >= 6: freed, nothing to give
    a.lwz(11, 4, 4);
    a.cmplw(7, 6, 11);
    const usize to_fail = a.bc(BO_TRUE, cr(7, GT));
    a.lwz(12, 8, 4);
    a.slwi(10, 3, 14);
    a.add(12, 12, 10);
    a.stw(12, 0, 7);
    a.stw(11, 0, 8);
    a.fix(to_ret);
    a.blr();
    a.fix(to_fail);
    a.li(0, 0);
    a.stw(0, 0, 7);
    a.stw(0, 0, 8);
    a.blr();
    a.put(b);
    ps_write32(b, kAllocDesc, kCode);
    ps_write32(b, kAllocDesc + 4, 0);
}

// Document::Write's writer: (data, size, state). It copies the data to the cursor and answers the size, or at the call
// numbered "fail at" answers the fail value, or at the call numbered "short at" one byte less.
void build_writer(u8* b)
{
    Asm a{kCode + 0x100, {}};
    a.lwz(9, 8, 5);
    a.addi(10, 9, 1);
    a.stw(10, 8, 5);
    a.lwz(11, 4, 5);
    a.cmpw(7, 9, 11);
    const usize to_fail = a.bc(BO_TRUE, cr(7, EQ));
    a.lwz(12, 0, 5);
    a.mr(6, 4);
    a.cmpwi(7, 6, 0);
    const usize to_copied = a.bc(BO_TRUE, cr(7, EQ));
    a.mtctr(6);
    a.addi(3, 3, -1);
    a.addi(12, 12, -1);
    const u32 loop = a.here();
    a.lbzu(0, 1, 3);
    a.stbu(0, 1, 12);
    a.bc_to(16, 0, loop); // bdnz
    a.addi(12, 12, 1);
    a.fix(to_copied);
    a.stw(12, 0, 5);
    a.mr(3, 4);
    a.lwz(11, 16, 5);
    a.cmpw(7, 9, 11);
    const usize to_done = a.bc(BO_FALSE, cr(7, EQ));
    a.addi(3, 3, -1);
    a.fix(to_done);
    a.blr();
    a.fix(to_fail);
    a.lwz(3, 12, 5);
    a.blr();
    a.put(b);
    ps_write32(b, kWriterDesc, kCode + 0x100);
    ps_write32(b, kWriterDesc + 4, 0);
}

// ------------------------------------------------------------------------------------------------- the runs

u64 fnv(const u8* p, usize n)
{
    u64 h = 0xcbf29ce484222325ull;
    for (usize i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

struct Step {
    std::string call;           // "name(args) -> answer"
    std::vector<u8> small;      // [kData, kData + 0x2000), normalized
    u64 region = 0;             // the rest of the script's memory
    std::vector<u8> fixed;      // FixedMemory's two instances
    std::vector<u8> globals;    // libsysutil's globals (0x54 bytes)
    u64 shm = 0;                // the channels' shared memory
    std::vector<std::string> lv2; // the lv2 calls the call made
};

struct Run {
    std::vector<Step> steps;
};

struct Trace {
    std::mutex m;
    u8* base = nullptr;
    std::function<u32(u32)> norm;
    std::vector<std::string> calls;
};

bool on_stack(u64 v) { return v >= kStacks && v < kStacks + 0x100000; }

std::string arg(const Trace& tr, u64 v) { return on_stack(v) ? std::string("stack") : std::format("{:#x}", tr.norm(static_cast<u32>(v))); }

std::string bytes(const u8* b, u32 at, u32 n)
{
    std::string s;
    for (u32 i = 0; i < n; i++) s += std::format("{:02x}", b[at + i]);
    return s;
}

void observe(void* user, u32 n, const u64* a)
{
    auto& tr = *static_cast<Trace*>(user);
    u8* b = tr.base;
    std::string s;
    switch (n) {
    case 100: s = std::format("mutex_create({}, {})", arg(tr, a[0]), bytes(b, static_cast<u32>(a[1]), 40)); break;
    case 101: s = std::format("mutex_destroy({:#x})", a[0]); break;
    case 102: s = std::format("mutex_lock({:#x}, {})", a[0], a[1]); break;
    case 104: s = std::format("mutex_unlock({:#x})", a[0]); break;
    case 105:
        s = std::format("cond_create({}, {:#x}, {})", arg(tr, a[0]), a[1], bytes(b, static_cast<u32>(a[2]), 24));
        break;
    case 107: s = std::format("cond_wait({:#x}, {})", a[0], a[1]); break;
    case 108: s = std::format("cond_signal({:#x})", a[0]); break;
    case 109: s = std::format("cond_signal_all({:#x})", a[0]); break;
    case 141: s = std::format("usleep({})", a[0]); break;
    case 328:
        s = std::format("shm_from_container_ext({:#x}, {:#x}, {:#x}, {:#x}, {}, {}, {})", a[0], a[1], a[2], a[3],
            arg(tr, a[4]), a[5], arg(tr, a[6]));
        break;
    case 329: s = std::format("free_shm({:#x})", a[0]); break;
    case 330: s = std::format("allocate_address({:#x}, {:#x}, {:#x}, {})", a[0], a[1], a[2], arg(tr, a[3])); break;
    case 331: s = std::format("free_address({:#x})", a[0]); break;
    case 332: s = std::format("allocate_shm({:#x}, {:#x}, {:#x}, {})", a[0], a[1], a[2], arg(tr, a[3])); break;
    case 335: s = std::format("unmap_shm({:#x}, {})", a[0], arg(tr, a[1])); break;
    case 337: s = std::format("search_and_map({:#x}, {:#x}, {:#x}, {})", a[0], a[1], a[2], arg(tr, a[3])); break;
    case 341: s = std::format("container_create({}, {:#x})", arg(tr, a[0]), a[1]); break;
    case 343: s = std::format("container_get_size({}, {:#x})", arg(tr, a[0]), a[1]); break;
    case 362:
        s = std::format("shm_from_container({:#x}, {:#x}, {:#x}, {:#x}, {})", a[0], a[1], a[2], a[3], arg(tr, a[4]));
        break;
    case 403: s = std::format("tty \"{}\"", std::string(reinterpret_cast<const char*>(b + a[1]), a[2])); break;
    default: s = std::format("syscall {}", n); break;
    }
    std::lock_guard lock(tr.m);
    tr.calls.push_back(std::move(s));
}

class Script;
using Body = std::function<void(Script&)>;

// One run of a script: Sony's module or pspocket's functions.
class Script {
public:
    Script(const std::string& flash, bool sony) : sony_(sony), t_(f_, 0, "ppu")
    {
        REQUIRE(f_.hle->initialize_tls({0, 0, 0x100}));
        lib_.emplace(f_, t_, flash, "cellSysutil", false); // in both runs: the same memory
        b_ = f_.base();
        const auto& segs = lib_->module->segments;
        REQUIRE(segs.size() >= 2);
        text_ = segs[0].first;
        sony_globals_ = segs[1].first + (0x1F898 - 0x1EE60);
        // What either run makes in the main area, made in both before anything else so their layouts agree.
        f_.hle->link_variable("sysPrxForUser", recomp::nid_of("s_shared_memory_area"));
        my_fixed_alloc_ = f_.hle->link_function("cellSysutil", 0xB59872EF,
            "_ZN16sysutil_cxmlutil11FixedMemory8AllocateEN4cxml14AllocationTypeEPvS3_jPS3_Pj");
        my_packet_writer_ = f_.hle->link_function("cellSysutil", 0x7FC8F72C,
            "_ZN16sysutil_cxmlutil12PacketWriter5WriteEPKvjPv");
        my_globals_ = sysutil::globals(t_.ctx);
        std::memset(b_ + kData, 0, kRegion);
        build_allocator(b_);
        build_writer(b_);
        ps_write32(b_, kAlloc + 4, 0x4000); // the allocator's limit
        ps_write32(b_, kAlloc + 8, kArena);
        ps_write32(b_, kWriter + 0, kOut);
        ps_write32(b_, kWriter + 4, 0xFFFFFFFF);
        ps_write32(b_, kWriter + 16, 0xFFFFFFFF);
        tr_.base = b_;
        tr_.norm = [this](u32 v) { return norm(v); };
        f_.kernel->set_syscall_observer(&observe, &tr_);
    }
    ~Script() { f_.kernel->set_syscall_observer(nullptr, nullptr); }

    bool sony() const { return sony_; }
    u8* base() const { return b_; }
    PPUContext& ctx() { return t_.ctx; }
    Run& run() { return run_; }
    u32 globals() const { return sony_ ? sony_globals_ : my_globals_; }
    u32 fixed() const { return globals() + sysutil::glob::fixed; }
    // The descriptors a game has for FixedMemory::Allocate and PacketWriter::Write.
    u32 fixed_allocate() const { return sony_ ? export_of(0xB59872EF) : my_fixed_alloc_; }
    u32 packet_writer_write() const { return sony_ ? export_of(0x7FC8F72C) : my_packet_writer_; }

    // An address of this run made comparable: into FixedMemory or the globals, or one of the descriptors.
    u32 norm(u32 v) const
    {
        const u32 g = globals();
        if (v >= g + sysutil::glob::fixed && v < g + sysutil::glob::kSize) return 0xFF000000 | (v - g - sysutil::glob::fixed);
        if (v >= g && v < g + 0x60) return 0xFE000000 | (v - g);
        if (v == fixed_allocate()) return 0xFD000001;
        if (v == packet_writer_write()) return 0xFD000002;
        return v;
    }

    // libsysutil's start for its channels as pspocket's console has them.
    void start_channels()
    {
        // libsysutil's code at 0x193E4, as Sony's run leaves its descriptor (in both runs: the same memory).
        ps_write32(b_, kInternal, text_ + 0x193E4);
        ps_write32(b_, kInternal + 4, lib_->module->toc);
        if (sony_) {
            // The system side's start (libsysutil's code at 0x18C28 with 0), its rings undone (pspocket's console has
            // none laid out), then libsysutil's closing (0x193E4).
            const u32 r = internal(0x18C28, {0});
            CHECK(r == 0);
            const u32 shm = ps_read32(b_, sony_globals_);
            for (u32 ch = 0; ch < 2; ch++) {
                ps_write32(b_, shm + 0x8000 * ch, 0);
                ps_write32(b_, shm + 0x8000 * ch + 4, 0);
            }
            CHECK(internal(0x193E4, {}) == 0);
        } else {
            CHECK(sysutil::packets_start(t_.ctx) == 0);
        }
        step("start");
    }

    // libsysutil's internal function at `offset` in its code (its TOC), as the module calls it.
    u32 internal(u32 offset, std::initializer_list<u64> args)
    {
        ps_write32(b_, kInternal, text_ + offset);
        ps_write32(b_, kInternal + 4, lib_->module->toc);
        return static_cast<u32>(call_guest(t_.ctx, kInternal, args));
    }

    // A call of the library's export `name` (with f1 = `f` for a float argument), recorded as a step.
    u32 call(std::string_view name, std::vector<u64> args, double f = 0)
    {
        t_.ctx.fpr[1] = f;
        if (std::getenv("PSPOCKET_TEST_VERBOSE")) std::fprintf(stderr, "%s %s\n", sony_ ? "sony" : "mine", std::string(name).c_str());
        u32 v = 0;
        if (sony_) {
            std::array<u64, 8> a{};
            std::copy(args.begin(), args.end(), a.begin());
            v = lib_->call(t_, recomp::nid_of(name), {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]});
        } else {
            const Function& fn = GuestThread::find(name);
            for (usize i = 0; i < args.size(); i++) t_.ctx.gpr[3 + i] = args[i];
            fn.fn(t_.ctx, b_);
            v = static_cast<u32>(t_.ctx.gpr[3]);
        }
        std::string text = std::format("{}(", name);
        for (usize i = 0; i < args.size(); i++)
            text += std::format("{}{:#x}", i ? ", " : "", args[i] >> 32 ? args[i] : norm(static_cast<u32>(args[i])));
        if (f != 0) text += std::format(", f1 {}", f);
        text += std::format(") -> {:#x}", norm(v));
        step(std::move(text));
        return v;
    }

    // The state after a step.
    void step(std::string text)
    {
        Step s;
        s.call = std::move(text);
        s.small.assign(b_ + kData, b_ + kSmall);
        for (u32 at = 0; at + 4 <= kSmall - kData; at += 4) {
            const u32 v = ps_read32(s.small.data(), at);
            ps_write32(s.small.data(), at, norm(v));
        }
        s.region = fnv(b_ + kSmall, kRegion - (kSmall - kData));
        const u32 g = globals();
        s.fixed.assign(b_ + g + sysutil::glob::fixed, b_ + g + sysutil::glob::kSize);
        s.globals.assign(b_ + g, b_ + g + 0x54);
        if (const u32 shm = ps_read32(b_, g)) s.shm = fnv(b_ + shm, 0x10000);
        {
            std::lock_guard lock(tr_.m);
            s.lv2 = std::move(tr_.calls);
            tr_.calls.clear();
        }
        run_.steps.push_back(std::move(s));
    }

    // An lv2 call of the script's own (recorded with the next step's).
    u32 syscall(u32 n, std::initializer_list<u64> args) { return static_cast<u32>(lv2_syscall(t_.ctx, n, args)); }

    void w32(u32 at, u32 v) { ps_write32(b_, at, v); }
    u32 r32(u32 at) const { return ps_read32(b_, at); }
    void str(u32 at, std::string_view s)
    {
        std::memcpy(b_ + at, s.data(), s.size());
        b_[at + s.size()] = 0;
    }

private:
    u32 export_of(u32 nid) const
    {
        const auto it = lib_->module->functions.find({"cellSysutil", nid});
        return it == lib_->module->functions.end() ? 0 : it->second;
    }

    bool sony_;
    Fixture f_;
    GuestThread t_;
    std::optional<FirmwareLibrary> lib_;
    u8* b_ = nullptr;
    u32 text_ = 0, sony_globals_ = 0, my_globals_ = 0, my_fixed_alloc_ = 0, my_packet_writer_ = 0;
    Trace tr_;
    Run run_;
};

void compare(const Run& mine, const Run& sony)
{
    const usize n = std::max(mine.steps.size(), sony.steps.size());
    usize differ = 0;
    for (usize i = 0; i < n && differ < 8; i++) {
        if (i >= mine.steps.size() || i >= sony.steps.size()) {
            UNSCOPED_INFO(std::format("step {}: only one run has it", i));
            differ++;
            continue;
        }
        const Step& o = mine.steps[i];
        const Step& s = sony.steps[i];
        std::string why;
        if (o.call != s.call) why += std::format(" answer: pspocket {} | libsysutil {};", o.call, s.call);
        if (o.small != s.small) {
            usize at = 0;
            while (at < o.small.size() && o.small[at] == s.small[at]) at++;
            why += std::format(" memory at {:#x}: pspocket {} | libsysutil {};", kData + at,
                bytes(o.small.data(), static_cast<u32>(at & ~3u), 16), bytes(s.small.data(), static_cast<u32>(at & ~3u), 16));
        }
        if (o.region != s.region) why += " the arena, writer output or buffers differ;";
        if (o.fixed != s.fixed) {
            usize at = 0;
            while (at < o.fixed.size() && o.fixed[at] == s.fixed[at]) at++;
            why += std::format(" FixedMemory at +{:#x};", at);
        }
        if (o.globals != s.globals) {
            usize at = 0;
            while (at < o.globals.size() && o.globals[at] == s.globals[at]) at++;
            why += std::format(" globals at +{:#x}: pspocket {} | libsysutil {};", at,
                bytes(o.globals.data(), static_cast<u32>(at & ~3u), 8), bytes(s.globals.data(), static_cast<u32>(at & ~3u), 8));
        }
        if (o.shm != s.shm) why += " the channels' shared memory;";
        if (o.lv2 != s.lv2) {
            const usize m = std::max(o.lv2.size(), s.lv2.size());
            for (usize k = 0; k < m; k++) {
                const std::string a = k < o.lv2.size() ? o.lv2[k] : "(none)", c = k < s.lv2.size() ? s.lv2[k] : "(none)";
                if (a != c) {
                    why += std::format(" lv2 call {}: pspocket {} | libsysutil {};", k, a, c);
                    break;
                }
            }
        }
        if (!why.empty()) {
            UNSCOPED_INFO(std::format("step {} ({}):{}", i, s.call, why));
            differ++;
        }
    }
    CHECK(differ == 0);
}

usize lv2_calls(const Run& r)
{
    usize n = 0;
    for (const Step& s : r.steps) n += s.lv2.size();
    return n;
}

void compare_script(const char* what, const Body& body)
{
    const std::optional<std::string> flash = test_dev_flash();
    if (!flash) SKIP("PSPOCKET_TEST_DEV_FLASH is not set: no firmware to compare with");
    Run sony, mine;
    {
        Script s(*flash, true);
        body(s);
        sony = std::move(s.run());
    }
    {
        Script s(*flash, false);
        body(s);
        mine = std::move(s.run());
    }
    INFO(std::format("{}: {} calls, {} lv2 calls", what, sony.steps.size(), lv2_calls(sony)));
    compare(mine, sony);
}

// ------------------------------------------------------------------------------------------------- the scripts

// Mangled names.
constexpr const char* kDocCtor = "_ZN4cxml8DocumentC1Ev";
constexpr const char* kDocDtor = "_ZN4cxml8DocumentD1Ev";
constexpr const char* kClear = "_ZN4cxml8Document5ClearEv";
constexpr const char* kMagic = "_ZN4cxml8Document14SetHeaderMagicEPKc";
constexpr const char* kFromBuffer = "_ZN4cxml8Document16CreateFromBufferEPKvjb";
constexpr const char* kCreate = "_ZN4cxml8Document13CreateElementEPKciPNS_7ElementE";
constexpr const char* kRegister = "_ZN4cxml8Document12RegisterFileEPKvjPNS_4FileE";
constexpr const char* kWrite = "_ZN4cxml8Document5WriteEPFiPKvjPvES3_";
constexpr const char* kRoot = "_ZN4cxml8Document18GetDocumentElementEv";
constexpr const char* kAppend = "_ZN4cxml7Element11AppendChildERS0_";
constexpr const char* kFirst = "_ZNK4cxml7Element13GetFirstChildEv";
constexpr const char* kNext = "_ZNK4cxml7Element14GetNextSiblingEv";
constexpr const char* kGetAttr = "_ZNK4cxml7Element12GetAttributeEPKcPNS_9AttributeE";
constexpr const char* kAttrInt = "_ZNK4cxml9Attribute6GetIntEPi";
constexpr const char* kAttrFile = "_ZNK4cxml9Attribute7GetFileEPNS_4FileE";
constexpr const char* kFileAddr = "_ZNK4cxml4File7GetAddrEv";
constexpr const char* kCheckName = "_ZN8cxmlutil16CheckElementNameERKN4cxml7ElementEPKc";
constexpr const char* kFind = "_ZN8cxmlutil16FindChildElementERKN4cxml7ElementEPKcS5_S5_";
constexpr const char* kGetInt = "_ZN8cxmlutil6GetIntERKN4cxml7ElementEPKcPi";
constexpr const char* kSetInt = "_ZN8cxmlutil6SetIntERKN4cxml7ElementEPKci";
constexpr const char* kGetFloat = "_ZN8cxmlutil8GetFloatERKN4cxml7ElementEPKcPf";
constexpr const char* kSetFloat = "_ZN8cxmlutil8SetFloatERKN4cxml7ElementEPKcf";
constexpr const char* kGetString = "_ZN8cxmlutil9GetStringERKN4cxml7ElementEPKcPS5_Pj";
constexpr const char* kSetString = "_ZN8cxmlutil9SetStringERKN4cxml7ElementEPKcS5_";
constexpr const char* kGetFile = "_ZN8cxmlutil7GetFileERKN4cxml7ElementEPKcPNS0_4FileE";
constexpr const char* kSetFile = "_ZN8cxmlutil7SetFileERKN4cxml7ElementEPKcRKNS0_4FileE";
constexpr const char* kFixedBegin = "_ZN16sysutil_cxmlutil11FixedMemory5BeginEi";
constexpr const char* kFixedEnd = "_ZN16sysutil_cxmlutil11FixedMemory3EndEi";
constexpr const char* kFixedAllocate = "_ZN16sysutil_cxmlutil11FixedMemory8AllocateEN4cxml14AllocationTypeEPvS3_jPS3_Pj";
constexpr const char* kWriterCtor = "_ZN16sysutil_cxmlutil12PacketWriterC1EiiRN4cxml8DocumentE";
constexpr const char* kWriterWrite = "_ZN16sysutil_cxmlutil12PacketWriter5WriteEPKvjPv";

constexpr u32 doc(u32 n) { return kDocs + 0x80 * n; }
constexpr u32 obj(u32 n) { return kObj + 0x10 * n; } // 0x60 objects of 16 bytes
// Strings by index.
constexpr u32 S(u32 n) { return kStr + 0x20 * n; }

void strings(Script& s)
{
    const char* names[] = {"root", "child", "a", "f", "s", "hello", "x", "file", "zz", "TEST", "id", "key", "value",
        "child2", "a-much-longer-element-name", "", "param", "result", "CXML", "x2", "y", "z", "w", "v"};
    for (u32 i = 0; i < std::size(names); i++) s.str(S(i), names[i]);
}

// Elements, attributes, files, the tree, the writer and documents made over buffers (the test's allocator).
void script_documents(Script& s)
{
    strings(s);
    for (u32 i = 0; i < 0x40; i++) s.base()[kFiles + i] = static_cast<u8>(0xA0 + i);
    for (u32 n = 0; n < 4; n++) s.call(kDocCtor, {doc(n)});
    // No allocator yet.
    s.call(kCreate, {doc(0), S(0), 3, obj(0)});
    s.call(kRegister, {doc(0), kFiles, 4, obj(1)});
    s.call(kClear, {doc(0)});
    s.call(kDocDtor, {doc(1)});
    // The test's allocator.
    for (u32 n = 0; n < 4; n++) {
        s.w32(doc(n) + 0x6C, kAllocDesc);
        s.w32(doc(n) + 0x70, kAlloc);
    }
    s.call(kMagic, {doc(0), S(9)});
    s.call(kCreate, {doc(0), S(0), 3, obj(0)});  // root
    s.call(kCreate, {doc(0), S(1), 2, obj(1)});  // child
    s.call(kCreate, {doc(0), S(1), 0, obj(2)});  // child (the same name: one string)
    s.call(kCreate, {doc(0), S(13), 1, obj(3)}); // child2
    s.call(kCreate, {doc(0), S(14), 4, obj(4)}); // a long name
    s.call(kCreate, {doc(0), S(15), 0, obj(5)}); // an empty name
    s.call(kCreate, {doc(0), S(6), static_cast<u64>(-1), obj(6)}); // a negative count
    s.call(kAppend, {obj(0), obj(1)});
    s.call(kAppend, {obj(0), obj(2)});
    s.call(kAppend, {obj(0), obj(3)});
    s.call(kAppend, {obj(1), obj(4)});
    s.call(kAppend, {obj(0), obj(0)}); // the root
    s.call(kAppend, {obj(3), obj(2)}); // a parent already
    s.call(kAppend, {obj(40), obj(2)}); // an element of no document
    s.call(kAppend, {obj(2), obj(5)});
    // Attributes.
    s.call(kSetInt, {obj(0), S(2), 5});
    s.call(kSetInt, {obj(0), S(2), 6}); // again: the same attribute
    s.call(kSetFloat, {obj(0), S(3)}, 1.25);
    s.call(kSetString, {obj(0), S(4), S(5)});
    s.call(kSetInt, {obj(0), S(6), 1}); // no free slot
    s.call(kSetString, {obj(1), S(4), S(1)}); // a value that is a name already
    s.call(kSetString, {obj(1), S(2), S(15)}); // an empty value
    s.call(kSetFloat, {obj(3), S(3)}, -0.5);
    s.call(kSetFloat, {obj(3), S(3)}, 3.0e38);
    s.call(kSetInt, {obj(40), S(2), 1}); // no document
    s.call(kSetString, {obj(40), S(2), S(1)});
    s.call(kSetInt, {obj(5), S(2), 1});  // no attributes
    s.call(kSetInt, {obj(6), S(2), 1});  // a negative count
    // Files.
    s.call(kRegister, {doc(0), kFiles, 5, obj(10)});
    s.call(kRegister, {doc(0), kFiles + 5, 0, obj(11)});
    s.call(kRegister, {doc(0), kFiles + 8, 17, obj(12)});
    s.call(kRegister, {doc(0), kFiles, 16, obj(13)});
    s.call(kSetFile, {obj(4), S(7), obj(12)});
    s.call(kSetFile, {obj(4), S(8), obj(10)});
    s.call(kFileAddr, {obj(10)});
    s.call(kFileAddr, {obj(12)});
    s.call(kFileAddr, {obj(40)});
    s.w32(obj(14), doc(0));
    s.w32(obj(14) + 4, 0x50);
    s.w32(obj(14) + 8, 0x20);
    s.call(kFileAddr, {obj(14)}); // past the table
    s.w32(obj(14) + 4, 0x10);
    s.w32(obj(14) + 8, 0x30);
    s.call(kFileAddr, {obj(14)}); // overrunning it
    // Getters.
    s.call(kGetInt, {obj(0), S(2), obj(20)});
    s.call(kGetInt, {obj(0), S(3), obj(20)}); // a float
    s.call(kGetInt, {obj(0), S(8), obj(20)}); // none
    s.call(kGetFloat, {obj(0), S(3), obj(20) + 4});
    s.call(kGetFloat, {obj(0), S(2), obj(20) + 4});
    s.call(kGetString, {obj(0), S(4), obj(21), obj(21) + 4});
    s.call(kGetString, {obj(1), S(2), obj(22), obj(22) + 4});
    s.call(kGetString, {obj(0), S(2), obj(23), obj(23) + 4});
    s.call(kGetFile, {obj(4), S(7), obj(24)});
    s.call(kGetFile, {obj(4), S(2), obj(25)});
    s.call(kGetInt, {obj(40), S(2), obj(20)});
    s.call(kGetAttr, {obj(0), S(2), obj(26)});
    s.call(kAttrInt, {obj(26), obj(27)});
    s.call(kAttrFile, {obj(26), obj(28)});
    s.call(kGetAttr, {obj(4), S(7), obj(29)});
    s.call(kAttrFile, {obj(29), obj(30)});
    s.call(kAttrInt, {obj(29), obj(27)});
    s.call(kAttrInt, {obj(40), obj(27)});
    s.call(kGetAttr, {obj(0), S(8), obj(31)});
    s.call(kGetAttr, {obj(40), S(2), obj(31)});
    // The tree.
    s.call(kRoot, {obj(32), doc(0)});
    s.call(kFirst, {obj(33), obj(32)});
    s.call(kNext, {obj(34), obj(33)});
    s.call(kNext, {obj(35), obj(34)});
    s.call(kNext, {obj(36), obj(35)});
    s.call(kFirst, {obj(37), obj(5)});
    s.call(kFirst, {obj(37), obj(40)});
    s.call(kNext, {obj(37), obj(40)});
    s.call(kCheckName, {obj(0), S(0)});
    s.call(kCheckName, {obj(1), S(0)});
    s.call(kCheckName, {obj(2), S(1)});
    s.call(kCheckName, {obj(5), S(15)});
    s.call(kFind, {obj(38), obj(0), S(1), 0, 0});
    s.call(kFind, {obj(38), obj(0), S(13), 0, 0});
    s.call(kFind, {obj(38), obj(0), S(8), 0, 0});
    s.call(kFind, {obj(38), obj(0), S(1), S(10), 0});
    s.call(kFind, {obj(38), obj(0), S(1), S(10), S(11)}); // no ID attributes
    s.call(kFind, {obj(38), obj(40), S(1), 0, 0});
    // Written out, whole and with the writer failing.
    s.call(kWrite, {doc(0), kWriterDesc, kWriter});
    const u32 size = s.r32(kWriter) - kOut;
    std::memcpy(s.base() + kBuf, s.base() + kOut, size);
    for (const u32 fail : {0u, 1u, 2u, 3u, 7u, 13u}) {
        s.w32(kWriter, kOut + 0x4000);
        s.w32(kWriter + 4, fail);
        s.w32(kWriter + 8, 0);
        s.w32(kWriter + 12, 0x80010099 + fail);
        s.call(kWrite, {doc(0), kWriterDesc, kWriter});
    }
    s.w32(kWriter + 4, 0xFFFFFFFF);
    for (const u32 at : {0u, 1u, 2u, 4u, 9u}) {
        s.w32(kWriter, kOut + 0x4000);
        s.w32(kWriter + 8, 0);
        s.w32(kWriter + 16, at);
        s.call(kWrite, {doc(0), kWriterDesc, kWriter});
    }
    s.w32(kWriter + 16, 0xFFFFFFFF);
    // Documents made from the buffer: over it and copied.
    s.call(kFromBuffer, {doc(1), kBuf, size, 0});
    s.call(kRoot, {obj(41), doc(1)});
    s.call(kFirst, {obj(42), obj(41)});
    s.call(kGetInt, {obj(41), S(2), obj(43)});
    s.call(kGetString, {obj(41), S(4), obj(44), obj(44) + 4});
    s.call(kSetInt, {obj(41), S(2), 9}); // read-only
    s.call(kCreate, {doc(1), S(0), 1, obj(45)});
    s.call(kRegister, {doc(1), kFiles, 4, obj(45)});
    s.call(kAppend, {obj(41), obj(42)});
    s.call(kGetFile, {obj(42), S(7), obj(46)});
    s.call(kFileAddr, {obj(46)});
    s.call(kWrite, {doc(1), kWriterDesc, kWriter}); // a document over a buffer writes out too
    s.call(kClear, {doc(1)});
    s.call(kFromBuffer, {doc(2), kBuf, size, 1});
    s.call(kRoot, {obj(47), doc(2)});
    s.call(kSetInt, {obj(47), S(2), 7});
    s.call(kSetString, {obj(47), S(19), S(20)}); // no free slot left
    s.call(kGetInt, {obj(47), S(2), obj(48)});
    s.call(kFromBuffer, {doc(2), kBuf, size, 0x100}); // the mode from the low byte, the copy from the word
    s.call(kFromBuffer, {doc(3), kBuf, size, 1}); // no room in the allocator for the copies
    // Malformed buffers.
    const auto buffer = [&](u32 field, u32 value, u32 len, u32 copy) {
        std::memcpy(s.base() + kBuf + 0x4000, s.base() + kBuf, size);
        if (field != 0xFFFFFFFF) s.w32(kBuf + 0x4000 + field, value);
        s.call(kFromBuffer, {doc(3), kBuf + 0x4000, len, copy});
    };
    buffer(0xFFFFFFFF, 0, 0x37, 0);
    buffer(0xFFFFFFFF, 0, 0x38, 0);
    buffer(4, 0x100, size, 0);
    buffer(4, 0x101, size, 0);
    buffer(0x24, 6, size, 0);   // int array size not a multiple of 4
    buffer(0x2C, 2, size, 0);   // float array size
    buffer(8, size, size, 0);   // the tree past the end
    buffer(0xC, 0x10000, size, 0); // the tree overrunning
    buffer(0x18, 0x7FFFFFF0, size, 0); // a string table past the buffer
    buffer(0x2C, 0x80000000, size, 0); // a negative float table
    buffer(0x34, 0xFFFFFFF0, size, 0); // a negative file table
    buffer(0xFFFFFFFF, 0, size - 1, 0);
    s.w32(doc(3) + 0x6C, 0);
    buffer(0xFFFFFFFF, 0, size, 1); // a copy without an allocator
    buffer(0xFFFFFFFF, 0, size, 0);
    s.w32(doc(3) + 0x6C, kAllocDesc);
    // An unterminated string table.
    {
        std::memcpy(s.base() + kBuf + 0x4000, s.base() + kBuf, size);
        const u32 strings_off = s.r32(kBuf + 0x4000 + 0x18), strings_size = s.r32(kBuf + 0x4000 + 0x1C);
        s.base()[kBuf + 0x4000 + strings_off + strings_size - 1] = 'x';
        s.call(kFromBuffer, {doc(3), kBuf + 0x4000, size, 0});
        s.call(kFromBuffer, {doc(3), kBuf + 0x4000, size, 1});
    }
    // A buffer with an ID table and ID attributes, for FindChildElement.
    {
        const u32 bb = kBuf + 0x6000;
        std::memset(s.base() + bb, 0, 0x400);
        std::memcpy(s.base() + bb, "CXML", 4);
        s.w32(bb + 4, 0x110);
        // tree at 0x40: root (attrs 0) with two children "c" each with one ID attribute "id".
        const u32 tree = 0x40;
        auto el = [&](u32 off, u32 name, u32 count, u32 parent, u32 prev, u32 next, u32 first, u32 last) {
            s.w32(bb + tree + off, name);
            s.w32(bb + tree + off + 4, count);
            s.w32(bb + tree + off + 8, parent);
            s.w32(bb + tree + off + 12, prev);
            s.w32(bb + tree + off + 16, next);
            s.w32(bb + tree + off + 20, first);
            s.w32(bb + tree + off + 24, last);
        };
        el(0, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x1C, 0x48);
        el(0x1C, 5, 1, 0, 0xFFFFFFFF, 0x48, 0xFFFFFFFF, 0xFFFFFFFF);
        s.w32(bb + tree + 0x38, 7); // attribute "id" (string 7), type 7, ID 0
        s.w32(bb + tree + 0x3C, 7);
        s.w32(bb + tree + 0x40, 0);
        el(0x48, 5, 1, 0, 0x1C, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
        s.w32(bb + tree + 0x64, 7);
        s.w32(bb + tree + 0x68, 7);
        s.w32(bb + tree + 0x6C, 8);
        const u32 tree_size = 0x74;
        // IDs at 0xC0: entries of a 4-byte head and the ID's text.
        const u32 ids = 0xC0;
        s.w32(bb + ids, 0);
        std::memcpy(s.base() + bb + ids + 4, "one", 4);
        s.w32(bb + ids + 8, 0);
        std::memcpy(s.base() + bb + ids + 12, "two", 4);
        const u32 ids_size = 16;
        const u32 str = 0xD0;
        std::memcpy(s.base() + bb + str, "root\0c\0id\0", 10);
        const u32 str_size = 10;
        // The header (offsets from the buffer's start): each table's offset and size.
        s.w32(bb + 0x08, tree);
        s.w32(bb + 0x0C, tree_size);
        s.w32(bb + 0x10, ids);
        s.w32(bb + 0x14, ids_size);
        s.w32(bb + 0x18, str);
        s.w32(bb + 0x1C, str_size);
        s.w32(bb + 0x20, 0xE0);
        s.w32(bb + 0x28, 0xE0);
        s.w32(bb + 0x30, 0xE0);
        s.str(S(22), "c");
        s.str(S(23), "two");
        s.call(kFromBuffer, {doc(3), bb, 0xE0, 0});
        s.call(kRoot, {obj(50), doc(3)});
        s.call(kFind, {obj(51), obj(50), S(22), S(10), S(23)});
        s.call(kFind, {obj(51), obj(50), S(22), S(10), S(0)});
        s.call(kFind, {obj(51), obj(50), S(22), S(2), S(23)});
        s.call(kFind, {obj(51), obj(50), S(22), 0, S(23)});
        s.call(kGetAttr, {obj(51), S(10), obj(52)});
        s.call(kAttrInt, {obj(52), obj(53)});
        s.call(kGetString, {obj(51), S(10), obj(53), obj(53) + 4});
        s.call(kCheckName, {obj(51), S(22)});
        s.call(kDocDtor, {doc(3)});
    }
    // A table growing past the allocator's limit.
    s.w32(kAlloc + 4, 0x60);
    s.call(kDocCtor, {doc(3)});
    s.call(kCreate, {doc(3), S(0), 2, obj(54)});
    s.call(kCreate, {doc(3), S(1), 2, obj(55)}); // the tree grows past it
    s.call(kSetString, {obj(54), S(4), S(14)});  // the string table too
    s.call(kRegister, {doc(3), kFiles, 0x61, obj(56)});
    s.call(kClear, {doc(3)});
    s.w32(kAlloc + 4, 0x4000);
    // Cleared and destroyed.
    s.call(kClear, {doc(0)});
    s.call(kRoot, {obj(57), doc(0)});
    s.call(kDocDtor, {doc(0)});
    s.call(kDocDtor, {doc(2)});
    s.call(kClear, {doc(1)});
}

// sysutil_cxmlutil's FixedMemory, and documents built in it as libsysutil's requests build theirs.
void script_fixed_memory(Script& s)
{
    strings(s);
    s.call(kFixedBegin, {0});
    s.call(kFixedBegin, {0});
    s.call(kFixedBegin, {1});
    s.call(kFixedEnd, {1});
    s.call(kFixedEnd, {1});
    s.call(kFixedBegin, {1});
    const u32 m0 = s.fixed();
    for (u32 type = 0; type < 13; type++) {
        s.call(kFixedAllocate, {type, m0, 0, 0x1000, obj(type), obj(type) + 4});
        s.call(kFixedAllocate, {type, m0, 0, 0x10, obj(16 + type), obj(16 + type) + 4});
    }
    for (u32 type = 0; type < 6; type++) s.call(kFixedAllocate, {6 + type, m0, 0, 0, 0, 0});
    s.call(kFixedAllocate, {0, m0, 0, 0x1001, obj(32), obj(32) + 4});
    s.call(kFixedAllocate, {3, m0, 0, 0x1001, obj(32), obj(32) + 4});
    s.call(kFixedAllocate, {0xFFFFFFFF, m0, 0, 0x10, obj(33), obj(33) + 4});
    // A document in FixedMemory 1.
    const u32 m1 = s.fixed() + sysutil::glob::kFixedSize;
    s.call(kDocCtor, {doc(0)});
    s.w32(doc(0) + 0x6C, s.fixed_allocate());
    s.w32(doc(0) + 0x70, m1);
    s.call(kMagic, {doc(0), S(18)});
    s.call(kCreate, {doc(0), S(16), 3, obj(40)});
    s.call(kSetInt, {obj(40), S(17), 42});
    s.call(kSetString, {obj(40), S(4), S(14)});
    s.call(kSetFloat, {obj(40), S(3)}, 0.75);
    s.call(kRegister, {doc(0), kFiles, 0x23, obj(41)});
    s.call(kCreate, {doc(0), S(1), 1, obj(42)});
    s.call(kAppend, {obj(40), obj(42)});
    s.call(kSetFile, {obj(42), S(7), obj(41)});
    for (u32 i = 0; i < 40; i++) s.call(kCreate, {doc(0), S(1 + i % 20), 6, obj(43)}); // past the tree's block
    s.call(kRegister, {doc(0), kFiles, 0xFF0, obj(44)});
    s.call(kRegister, {doc(0), kFiles, 0x10, obj(44)});
    s.call(kWrite, {doc(0), kWriterDesc, kWriter});
    s.call(kFromBuffer, {doc(1), kOut, s.r32(kWriter) - kOut, 0});
    s.call(kDocCtor, {doc(2)});
    s.w32(doc(2) + 0x6C, s.fixed_allocate());
    s.w32(doc(2) + 0x70, m1);
    s.call(kFromBuffer, {doc(2), kOut, s.r32(kWriter) - kOut, 1}); // the blocks are taken: no memory
    s.call(kDocDtor, {doc(0)});
    s.call(kFixedEnd, {1});
    s.call(kDocDtor, {doc(2)});
    s.call(kFixedBegin, {1});
    s.call(kFromBuffer, {doc(2), kOut, s.r32(kWriter) - kOut, 1});
    s.call(kRoot, {obj(45), doc(2)});
    s.call(kGetInt, {obj(45), S(17), obj(46)});
    s.call(kClear, {doc(2)});
    s.call(kFixedEnd, {1});
    s.call(kFixedEnd, {0});
}

// The packets: the channels as pspocket's console has them (closed, no rings), then with a ring laid out by the test.
void script_packets(Script& s)
{
    strings(s);
    s.start_channels();
    const u32 shm = s.r32(s.globals());
    for (u32 i = 0; i < 0x100; i++) s.base()[kFiles + i] = static_cast<u8>(i * 7 + 1);
    // Closed channels with no ring.
    s.call("cellSysutilPacketBegin", {2, 1});
    s.call("cellSysutilPacketBegin", {0, 0x10E});
    s.call("cellSysutilPacketBegin", {0, 0x10E}); // the side is taken and the channel closed
    s.call("cellSysutilPacketBegin", {0, 0});      // the system's side
    s.call("cellSysutilPacketWrite", {0, 0, kFiles, 0x10, 1}); // the system's packet ID now
    s.call("cellSysutilPacketWrite", {0, 0x10E, kFiles, 0x10, 1});
    s.call("cellSysutilPacketWrite", {0, 0x10E, kFiles, 0, 1});
    s.call("cellSysutilPacketWrite", {3, 0x10E, kFiles, 0x10, 1});
    s.call("cellSysutilPacketRead", {0, 0, kOut, 0x10, obj(0)});
    s.call("cellSysutilPacketRead", {0, 0, kOut, 0, obj(0)});
    s.call("cellSysutilPacketRead", {0, 0, kOut, 0, 0});
    s.call("cellSysutilPacketRead", {2, 0, kOut, 0x10, obj(0)});
    s.call("cellSysutilPacketEnd", {0, 0});
    s.call("cellSysutilPacketEnd", {0, 0x10E});
    s.call("cellSysutilPacketEnd", {7, 0x10E});
    s.call("cellSysutilPacketRead", {0, 0x10E, kOut, 0x10, obj(0)}); // ended: the ID is -1
    s.call("cellSysutilPacketBegin", {1, 1});
    s.call(kWriterCtor, {obj(2), 1, 1, doc(0)});
    s.call(kDocCtor, {doc(0)});
    s.call(kWriterCtor, {obj(2), 1, 1, doc(0)});
    s.call(kWriterWrite, {kFiles, 0x40, obj(2)});
    s.call(kWrite, {doc(0), s.packet_writer_write(), obj(2)});
    s.call("cellSysutilPacketEnd", {1, 1});
    // A ring laid out on channel 1 (0x30 bytes at 0x400), the channel still closed: what would wait answers CLOSED.
    s.w32(shm + 0x8000, 0x400);
    s.w32(shm + 0x8004, 0x30);
    s.call("cellSysutilPacketBegin", {1, 7});
    s.call("cellSysutilPacketWrite", {1, 7, kFiles, 0x10, 0});
    s.call("cellSysutilPacketWrite", {1, 7, kFiles + 0x10, 0x18, 1});
    s.call("cellSysutilPacketWrite", {1, 7, kFiles, 4, 1}); // a complete packet waits for its reader
    s.call("cellSysutilPacketRead", {1, 7, kOut, 0x10, obj(1)}); // its own packet is not a reply
    s.call("cellSysutilPacketEnd", {1, 7});
    s.call("cellSysutilPacketBegin", {1, 0});
    s.call("cellSysutilPacketRead", {1, 0, kOut, 8, obj(1)});
    s.call("cellSysutilPacketRead", {1, 0, kOut + 8, 0x100, obj(1)});
    s.call("cellSysutilPacketRead", {1, 0, kOut + 0x30, 0x10, obj(1)}); // read whole: nothing more
    // The system's reply, wrapping round the ring.
    s.call("cellSysutilPacketWrite", {1, 0, kFiles + 0x40, 0x2C, 0});
    s.call("cellSysutilPacketWrite", {1, 0, kFiles + 0x70, 0x10, 1}); // no room past 0x30
    s.call("cellSysutilPacketEnd", {1, 0});
    s.call("cellSysutilPacketBegin", {1, 9});
    s.call("cellSysutilPacketRead", {1, 9, kOut + 0x40, 0x10, 0}); // part of it, no `done`: truncated
    s.call("cellSysutilPacketRead", {1, 9, kOut + 0x50, 0x1C, obj(1)});
    s.call("cellSysutilPacketEnd", {1, 9});
    s.call("cellSysutilPacketBegin", {1, 0});
    s.call("cellSysutilPacketWrite", {1, 0, kFiles + 0x80, 0x20, 1});
    s.call("cellSysutilPacketEnd", {1, 0});
    s.call("cellSysutilPacketBegin", {1, 3});
    s.call("cellSysutilPacketRead", {1, 3, kOut + 0x80, 0x40, obj(1)});
    s.call("cellSysutilPacketRead", {1, 3, kOut + 0xC0, 0x40, 0});
    s.call("cellSysutilPacketWrite", {1, 3, kFiles, 0x2F, 0});
    s.call("cellSysutilPacketWrite", {1, 3, kFiles, 0x2, 1}); // the ring is full
    s.call("cellSysutilPacketEnd", {1, 3});
    s.call("cellSysutilPacketBegin", {1, 0});
    s.call("cellSysutilPacketRead", {1, 0, kOut + 0x100, 0x2F, 0}); // the packet isn't complete: truncated
    s.call("cellSysutilPacketEnd", {1, 0});
    // A document written into a packet and read back.
    s.w32(shm + 0x8004, 0x7C00);
    s.w32(shm + 0x8008, 0);
    s.w32(shm + 0x800C, 0);
    s.w32(shm + 0x8010, 0);
    s.w32(shm + 0x8014, 0);
    s.w32(shm + 0x8018, 0);
    s.w32(doc(0) + 0x6C, kAllocDesc);
    s.w32(doc(0) + 0x70, kAlloc);
    s.call(kMagic, {doc(0), S(9)});
    s.call(kCreate, {doc(0), S(16), 2, obj(4)});
    s.call(kSetInt, {obj(4), S(17), 0x1234});
    s.call(kSetString, {obj(4), S(4), S(14)});
    s.call("cellSysutilPacketBegin", {1, 0x101});
    s.call(kWriterCtor, {obj(5), 1, 0x101, doc(0)});
    s.call(kWrite, {doc(0), s.packet_writer_write(), obj(5)});
    s.call(kWriterWrite, {kFiles, 0x10, obj(5)}); // nothing left to write
    s.call("cellSysutilPacketEnd", {1, 0x101});
    s.call("cellSysutilPacketBegin", {1, 0});
    s.call("cellSysutilPacketRead", {1, 0, kBuf, 0x6000, obj(6)});
    s.call("cellSysutilPacketEnd", {1, 0});
    s.call(kFromBuffer, {doc(1), kBuf, 0x6000, 0});
    s.call(kRoot, {obj(7), doc(1)});
    s.call(kGetInt, {obj(7), S(17), obj(8)});
    s.call(kDocDtor, {doc(0)});
}

// libsysutil's shared memories. (A side other than the system's, or a container, waits while lv2 has no shared
// memory of the key or no such container, retrying every 100 ms: neither is asked for here.)
void script_shared_memory(Script& s)
{
    s.start_channels();
    const char* alloc = "cellSysutilSharedMemoryAlloc";
    const char* free = "cellSysutilSharedMemoryFree";
    s.syscall(341, {obj(30), 0x800000}); // a memory container
    const u32 container = s.r32(obj(30));
    s.w32(obj(20) + 16, 0); // an entry of the _ext form: type 0
    s.w32(obj(20) + 20, 0);
    s.call(alloc, {0, 0x8006040000000401ull, 0x10000, 0xFFFFFFFF, 0x200, obj(0), 0, 0});
    s.call(alloc, {0, 0x8006040000000401ull, 0x10000, 0xFFFFFFFF, 0x200, obj(1), 0, 0}); // it exists
    s.call(alloc, {0x10B, 0x8006040000000401ull, 0x10000, 0xFFFFFFFF, 0x200, obj(1), 0, 0});
    s.call(alloc, {0, 0x8006040000000402ull, 0x10000, 0xFFFFFFFF, 0x300, obj(1), 0, 0});
    s.call(alloc, {0, 0x8006040000000402ull, 0x8000, 0xFFFFFFFF, 0x200, obj(1), 0, 0});
    s.call(alloc, {0, 0x8006040000000403ull, 0x200000, 0xFFFFFFFF, 0x400, obj(2), 0, 0});
    s.call(alloc, {0, 0x8006040000000404ull, 0x100000, 0xFFFFFFFF, 0x400, obj(3), 0, 0});
    s.call(alloc, {0, 0x8006040000000405ull, 0x20000, 0xFFFFFFFF, 0x200, obj(4), 0, 0});
    s.call(alloc, {0, 0x8006040000000406ull, 0x10000, container, 0x200, obj(5), 0, 0});
    s.call(alloc, {0, 0x8006040000000407ull, 0x10000, container, 0x200, obj(6), obj(20), 1});
    s.call(alloc, {0, 0x8006040000000408ull, 0x10000, container, 0x200, obj(7), obj(20), 0}); // no entries
    s.call(alloc, {0, 0x8006040000000409ull, 0x100000, container, 0x400, obj(8), 0, 0});
    s.call(alloc, {0, 0x8006040000000406ull, 0x10000, container, 0x200, obj(9), 0, 0}); // it exists
    s.call(free, {0, s.r32(obj(4)), 0x200});
    s.call(free, {0, s.r32(obj(4)), 0x200}); // gone
    s.call(free, {0, s.r32(obj(0)), 0x300});
    s.call(free, {0, s.r32(obj(2)), 0x400});
    s.call(free, {0, s.r32(obj(3)), 0x400});
    s.call(free, {0, s.r32(obj(8)), 0x400}); // the last of the 1 MB area: its range goes
    s.call(free, {0, s.r32(obj(0)), 0x200});
    s.call(free, {0, s.r32(obj(5)), 0x200});
    s.call(free, {0, s.r32(obj(6)), 0x200});
    s.call(alloc, {0, 0x800604000000040Aull, 0x100000, 0xFFFFFFFF, 0x400, obj(10), 0, 0});
    s.call(free, {0, s.r32(obj(10)), 0x400});
}

} // namespace

TEST_CASE("cellSysutil cxml: documents and their elements against libsysutil", "[hle][lle][sysutil]")
{
    compare_script("documents", script_documents);
}

TEST_CASE("cellSysutil cxml: FixedMemory and documents in it against libsysutil", "[hle][lle][sysutil]")
{
    compare_script("FixedMemory", script_fixed_memory);
}

TEST_CASE("cellSysutil packets against libsysutil", "[hle][lle][sysutil]")
{
    compare_script("packets", script_packets);
}

TEST_CASE("cellSysutil shared memories against libsysutil", "[hle][lle][sysutil]")
{
    compare_script("shared memories", script_shared_memory);
}

// Without the firmware: pspocket's channels as its console has them, and a document written out and read back.
TEST_CASE("cellSysutil: closed channels and a document round trip", "[hle][sysutil]")
{
    Fixture f;
    GuestThread t(f, 0, "ppu");
    u8* b = f.base();
    std::memset(b + kData, 0, kRegion);
    build_allocator(b);
    build_writer(b);
    ps_write32(b, kAlloc + 4, 0x4000);
    ps_write32(b, kAlloc + 8, kArena);
    ps_write32(b, kWriter + 0, kOut);
    ps_write32(b, kWriter + 4, 0xFFFFFFFF);
    ps_write32(b, kWriter + 16, 0xFFFFFFFF);
    REQUIRE(sysutil::packets_start(t.ctx) == 0);
    const u32 g = sysutil::globals(t.ctx);
    const u32 shm = ps_read32(b, g);
    REQUIRE(shm != 0);
    CHECK(b[shm + 0x1E] == 1); // closed
    CHECK(b[shm + 0x801E] == 1);
    CHECK(ps_read32(b, shm + 4) == 0); // no ring
    CHECK(ps_read32(b, g + sysutil::glob::packet_id(0)) == 0xFFFFFFFF);
    // A request's send finds no room: CLOSED.
    CHECK(t.call("cellSysutilPacketBegin", 0, 0x10E) == 0);
    CHECK(static_cast<u32>(t.call("cellSysutilPacketWrite", 0, 0x10E, kFiles, 0x10, 1)) == 0x8002b007);
    CHECK(static_cast<u32>(t.call("cellSysutilPacketRead", 0, 0x10E, kOut, 0x10, kObj)) == 0x8002b007);
    CHECK(static_cast<u32>(t.call("cellSysutilPacketRead", 0, 0x10E, kOut, 0, 0)) == 0x8002b006);
    CHECK(t.call("cellSysutilPacketEnd", 0, 0x10E) == 0);
    CHECK(static_cast<u32>(t.call("cellSysutilPacketBegin", 2, 1)) == 0x8002b002);
    // A document: built, written out, made again from its bytes.
    std::memcpy(b + kStr, "root\0a\0s\0value\0", 15);
    CHECK(t.call(kDocCtor, kDocs) == kDocs + 4); // r3 is left holding the header's address, as libsysutil leaves it
    ps_write32(b, kDocs + 0x6C, kAllocDesc);
    ps_write32(b, kDocs + 0x70, kAlloc);
    CHECK(t.call(kCreate, kDocs, kStr, 2, kObj) == 0);
    CHECK(t.call(kSetInt, kObj, kStr + 5, 77) == 0);
    CHECK(t.call(kSetString, kObj, kStr + 7, kStr + 9) == 0);
    CHECK(t.call(kWrite, kDocs, kWriterDesc, kWriter) == 0);
    const u32 size = ps_read32(b, kWriter) - kOut;
    CHECK(size == 0x40 + 0x40 + 0x10); // the header, the tree (0x3C), the strings (15)
    CHECK(std::memcmp(b + kOut, "CXML", 4) == 0);
    CHECK(ps_read32(b, kOut + 4) == 0x110);
    CHECK(ps_read32(b, kOut + 8) == 0x40);
    CHECK(t.call(kFromBuffer, kDocs + 0x80, kOut, size, 0) == 0);
    CHECK(t.call(kRoot, kObj + 0x10, kDocs + 0x80) == kObj + 0x10);
    CHECK(t.call(kGetInt, kObj + 0x10, kStr + 5, kObj + 0x20) == 0);
    CHECK(ps_read32(b, kObj + 0x20) == 77);
    CHECK(t.call(kGetString, kObj + 0x10, kStr + 7, kObj + 0x24, kObj + 0x28) == 0);
    CHECK(ps_read32(b, kObj + 0x28) == 5);
    CHECK(std::strcmp(reinterpret_cast<const char*>(b + ps_read32(b, kObj + 0x24)), "value") == 0);
    CHECK(static_cast<u32>(t.call(kSetInt, kObj + 0x10, kStr + 5, 1)) == 0x8002e010); // read-only
    CHECK(t.call(kDocDtor, kDocs) == 0);
    // FixedMemory: two instances, each once.
    const u32 m0 = static_cast<u32>(t.call(kFixedBegin, 0));
    CHECK(m0 == g + sysutil::glob::fixed);
    CHECK(t.call(kFixedBegin, 0) == 0);
    t.call(kFixedEnd, 0);
    CHECK(t.call(kFixedBegin, 0) == m0);
}
