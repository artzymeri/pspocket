#pragma once

// The harness of the font libraries' comparisons (D-525, D-526): a script of calls runs in two fresh processes, one
// on Sony's firmware modules and one on pspocket's, and what each observes is kept as text lines to compare. Objects
// are described by what they hold (their fields, the strings, arrays and pixels they point to), not by their
// addresses, which differ by construction (pspocket's mirrors are not Sony's allocations). Guest code for the
// callbacks the libraries call (memory, logging callbacks, a face requester, a stream reader) is written here with the
// PPU assembler. The fonts are the firmware's own (dev_flash/data/font), read at test time; nothing of the firmware
// is kept.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "firmware_compare.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/recomp/nid.hpp"
#include "pspocket/recomp/ppu/asm.hpp"

namespace pspocket::hle::test::ft {

namespace as = pspocket::recomp::ppu::as;

// Guest memory of a run.
inline constexpr u32 kCode = kData + 0x0000;      // callbacks' code (16 KB)
inline constexpr u32 kOpds = kData + 0x4000;      // their descriptors
inline constexpr u32 kMemory = kData + 0x4800;    // FT_MemoryRec {user, alloc, free, realloc}
inline constexpr u32 kHeapCtl = kData + 0x5000;   // {next, count}, then entries {kind, size, addr, old} from +0x10
inline constexpr u32 kHeapLogMax = 0x2000;
inline constexpr u32 kLogCtl = kData + 0x30000;   // callbacks' log: {count}, entries of 64 bytes from +0x40
inline constexpr u32 kLogMax = 0x800;
inline constexpr u32 kRet = kData + 0x58000;      // what the logging callbacks answer, by call number
inline constexpr u32 kScratch = kData + 0x60000;  // the scripts' own memory, to kData + 0x300000
inline constexpr u32 kHeap = 0x01000000, kHeapSize = 0x03000000; // the FT_Memory's blocks
inline constexpr u32 kBlobs = 0x04000000, kBlobsSize = 0x02000000; // font files in memory
inline constexpr u32 kFontHeap = 0x06000000, kFontHeapSize = 0x02000000; // a CellFontMemoryInterface's blocks
inline constexpr u32 kFontHeapCtl = kData + 0x2C000; // its {next, mallocs, frees}; the interface at +0x10

inline std::string hex(const u8* p, usize n)
{
    std::string s;
    s.reserve(n * 2);
    for (usize i = 0; i < n; i++) s += std::format("{:02x}", p[i]);
    return s;
}

// A file of the firmware's dev_flash.
inline std::vector<u8> read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

class Run {
public:
    Run(const std::string& flash, bool sony_, const std::vector<std::string>& libraries)
        : Run(flash, sony_ ? libraries : std::vector<std::string>{}, sony_ ? std::vector<std::string>{} : libraries)
    {
    }

    // A run with the libraries `firmware` as Sony's modules and `host` as pspocket's (a host module's imports from the
    // firmware's linked as the loader links them).
    Run(const std::string& flash, const std::vector<std::string>& firmware, const std::vector<std::string>& host)
        : sony(host.empty()), t(f, 0, "ppu")
    {
        REQUIRE(f.hle->main_memory().reserve(kHeap, kHeapSize));
        REQUIRE(f.mem.commit(kHeap, kHeapSize));
        REQUIRE(f.hle->main_memory().reserve(kBlobs, kBlobsSize));
        REQUIRE(f.mem.commit(kBlobs, kBlobsSize));
        REQUIRE(f.hle->main_memory().reserve(kFontHeap, kFontHeapSize));
        REQUIRE(f.mem.commit(kFontHeap, kFontHeapSize));
        REQUIRE(f.kernel->vfs_mount("/dev_flash", flash, true));
        REQUIRE(f.hle->initialize_tls({0, 0, 0x100}));
        b = f.base();
        if (!firmware.empty()) {
            lle = std::make_unique<loader::LleModules>(flash);
            REQUIRE(lle->index());
            for (const std::string& l : firmware) lle->force(l);
            for (const std::string& l : firmware) {
                std::string why;
                const loader::PrxModule* m = lle->load(l, *f.hle, &why);
                INFO(why);
                REQUIRE(m);
                modules[l] = m;
            }
        }
        if (!host.empty()) {
            REQUIRE(f.hle->link_internal_functions(host));
            for (const HostImport& i : registry().imports()) {
                if (std::find(host.begin(), host.end(), i.module) == host.end() || !modules.contains(std::string(i.library)))
                    continue;
                const loader::PrxModule* m = modules.at(std::string(i.library));
                const auto& exports = i.variable ? m->variables : m->functions;
                const auto it = exports.find({std::string(i.library), i.nid});
                REQUIRE(it != exports.end());
                f.hle->link_import(i.library, i.nid, it->second);
            }
        }
        if (lle)
            for (const u32 s : lle->take_starts()) call_guest(t.ctx, s, {0, 0});
        write_code();
        std::memset(b + kHeap, 0xCD, 0x200000);
        scratch_ = kScratch;
    }

    const bool sony;
    Fixture f;
    GuestThread t;
    std::unique_ptr<loader::LleModules> lle;
    std::map<std::string, const loader::PrxModule*> modules;
    u8* b = nullptr;
    std::vector<std::string> out;

    // The descriptor of library function `name` (Sony's export, or pspocket's linked stub). A name ending in _XXXXXXXX
    // (an export with no known name) is that NID.
    static u32 nid_of_name(const char* name)
    {
        const std::string_view n(name);
        if (n.size() > 9 && n[n.size() - 9] == '_') {
            const std::string tail(n.substr(n.size() - 8));
            if (tail.find_first_not_of("0123456789ABCDEF") == std::string::npos) return static_cast<u32>(std::stoul(tail, nullptr, 16));
        }
        return recomp::nid_of(name);
    }
    bool firmware(const std::string& library) const { return modules.contains(library); }
    u32 opd(const std::string& library, const char* name)
    {
        const u32 nid = nid_of_name(name);
        if (firmware(library)) {
            const loader::PrxModule* m = modules.at(library);
            const auto it = m->functions.find({library, nid});
            REQUIRE(it != m->functions.end());
            return it->second;
        }
        return f.hle->link_function(library, nid, name);
    }

    // Calls `name` of `library`: r3.
    u64 call64(const std::string& library, const char* name, std::initializer_list<u64> args)
    {
        if (std::getenv("PSPOCKET_FT_TRACE")) std::fprintf(stderr, "CALL %s %s\n", sony ? "sony" : "ours", name);
        if (firmware(library)) return call_guest(t.ctx, opd(library, name), args);
        const Function& fn = GuestThread::find(name);
        u32 i = 0;
        for (const u64 a : args) {
            if (i < 8) t.ctx.gpr[3 + i] = a;
            i++;
        }
        const u64 sp = t.ctx.gpr[1];
        fn.fn(t.ctx, b);
        CHECK(t.ctx.gpr[1] == sp);
        return t.ctx.gpr[3];
    }
    u32 call(const std::string& library, const char* name, std::initializer_list<u64> args)
    {
        return static_cast<u32>(call64(library, name, args));
    }
    // A call with floating-point arguments too (f1.. in order), through the function's descriptor either way.
    u32 callf(const std::string& library, const char* name, std::initializer_list<u64> args,
        std::initializer_list<double> fargs)
    {
        if (std::getenv("PSPOCKET_FT_TRACE")) std::fprintf(stderr, "CALL %s %s\n", sony ? "sony" : "ours", name);
        u32 i = 1;
        for (const double v : fargs) t.ctx.fpr[i++] = v;
        return static_cast<u32>(call_guest(t.ctx, opd(library, name), args));
    }
    // A call to cell_FreeType2 whose answer is kept.
    u32 ft(const char* name, std::initializer_list<u64> args)
    {
        const u32 r = call("cell_FreeType2", name, args);
        rec(std::format("{} -> {:#x}", name, r));
        return r;
    }
    u32 ftq(const char* name, std::initializer_list<u64> args) { return call("cell_FreeType2", name, args); }

    void rec(std::string s) { out.push_back(std::move(s)); }

    // The scripts' own memory.
    u32 alloc(u32 size, u32 align = 16)
    {
        scratch_ = (scratch_ + align - 1) & ~(align - 1);
        const u32 a = scratch_;
        scratch_ += size;
        REQUIRE(scratch_ < kData + 0x300000);
        std::memset(b + a, 0, size);
        return a;
    }
    u32 str(std::string_view s)
    {
        const u32 a = alloc(static_cast<u32>(s.size()) + 1);
        std::memcpy(b + a, s.data(), s.size());
        return a;
    }
    // A blob (a font file) in guest memory.
    u32 blob(const std::vector<u8>& data)
    {
        blobs_ = (blobs_ + 0xFFF) & ~0xFFFu;
        const u32 a = kBlobs + blobs_;
        REQUIRE(blobs_ + data.size() <= kBlobsSize);
        std::memcpy(b + a, data.data(), data.size());
        blobs_ += static_cast<u32>(data.size());
        return a;
    }

    u32 r32(u32 a) const { return ps_read32(b, a); }
    s32 s32at(u32 a) const { return static_cast<s32>(ps_read32(b, a)); }
    u16 r16(u32 a) const { return ps_read16(b, a); }
    s16 s16at(u32 a) const { return static_cast<s16>(ps_read16(b, a)); }
    u8 r8(u32 a) const { return b[a]; }
    void w32(u32 a, u32 v) { ps_write32(b, a, v); }
    void w16(u32 a, u16 v) { ps_write16(b, a, v); }
    std::string cstr(u32 a, usize max = 4096) const
    {
        if (!a) return "(null)";
        std::string s;
        while (b[a + s.size()] && s.size() < max) s += static_cast<char>(b[a + s.size()]);
        return s;
    }

    // ---- the FT_Memory's log: what a section allocated, freed and reallocated, blocks named by their allocation
    u32 heap_mark() const { return r32(kHeapCtl + 4); }
    std::string heap_ops(u32 mark)
    {
        std::string s;
        const u32 n = r32(kHeapCtl + 4);
        for (u32 i = mark; i < n; i++) {
            const u32 e = kHeapCtl + 0x10 + 16 * i;
            const u32 kind = r32(e), size = r32(e + 4), addr = r32(e + 8), old = r32(e + 12);
            if (kind == 1) {
                blocks_[addr] = static_cast<u32>(blocks_.size());
                s += std::format(" A{}", size);
            } else if (kind == 2) {
                auto it = blocks_.find(addr);
                s += it == blocks_.end() ? std::format(" F?") : std::format(" F#{}", it->second);
            } else if (kind == 3) {
                auto it = blocks_.find(old);
                s += std::format(" R{}<-#{}", size, it == blocks_.end() ? -1 : static_cast<int>(it->second));
                blocks_[addr] = static_cast<u32>(blocks_.size());
            }
        }
        return s;
    }
    u32 heap_count() const { return r32(kHeapCtl + 4); }

    // ---- logging callbacks: each call appends {tag, its words} at kLogCtl and answers kRet[call number]
    // A callback taking `ptrs` pointers to vectors (logged as the two words each points to) and then `scalars` values.
    u32 logger(u32 tag, u32 ptrs, u32 scalars) { return make_logger(tag, ptrs, scalars, false); }
    // FT_SpanFunc(y, count, spans, user): y, count, user and the first 8 spans' bytes.
    u32 span_logger(u32 tag) { return make_logger(tag, 0, 0, true); }
    u32 log_count() const { return r32(kLogCtl); }
    std::vector<u32> log_entry(u32 i) const
    {
        std::vector<u32> v(16);
        for (u32 k = 0; k < 16; k++) v[k] = r32(kLogCtl + 0x40 + 64 * i + 4 * k);
        return v;
    }
    void set_answer(u32 call_number, u32 value) { w32(kRet + 4 * call_number, value); }
    void clear_log()
    {
        std::memset(b + kLogCtl, 0, 0x40 + 64 * kLogMax);
        std::memset(b + kRet, 0, 4 * kLogMax);
    }

    // A face requester for FTC_Manager_New: face ID i opens the path at req_data[i] with FT_New_Face of the library
    // under test (and counts its calls at req_data[63]).
    u32 requester() { return requester_; }
    // An FT_StreamRec read function over memory: the stream's descriptor is the data's address; logs nothing.
    u32 stream_read() { return stream_read_; }

    // A CellFontMemoryInterface {arg, malloc, free, realloc, calloc}: a bump allocator over kFontHeap (each block after
    // a 16-byte header holding its size; frees counted, nothing reused). arg = {next, mallocs, frees}.
    u32 font_memory();
    u32 font_mallocs() const { return r32(kFontHeapCtl + 4); }
    u32 font_frees() const { return r32(kFontHeapCtl + 8); }

private:
    u32 make_logger(u32 tag, u32 ptrs, u32 scalars, bool spans);
    void write_code();

    u32 scratch_ = 0, blobs_ = 0;
    u32 code_ = kCode, opd_next_ = kOpds;
    u32 requester_ = 0, stream_read_ = 0;
    std::map<u32, u32> blocks_;

    u32 put_code(as::Assembler& a)
    {
        const std::vector<u8> bytes = a.finish();
        std::memcpy(b + a.base(), bytes.data(), bytes.size());
        code_ = a.base() + static_cast<u32>(bytes.size());
        return a.base();
    }
    u32 put_opd(u32 entry)
    {
        const u32 o = opd_next_;
        opd_next_ += 8;
        w32(o, entry);
        w32(o + 4, 0);
        return o;
    }
};

inline u32 hi(u32 v) { return v >> 16; }
inline u32 lo(u32 v) { return v & 0xFFFF; }

inline void Run::write_code()
{
    // FT_Memory: a bump allocator over kHeap (16-byte aligned, never reused) logging every call.
    as::Assembler a(code_);
    auto log_entry_code = [&](u32 kind, u32 size_reg, u32 addr_reg, u32 old_reg) {
        // r5 = control block
        a.emit(as::lwz(8, 4, 5));
        a.emit(as::slwi(9, 8, 4));
        a.emit(as::add(9, 9, 5));
        a.emit(as::li(10, static_cast<s32>(kind)));
        a.emit(as::stw(10, 0x10, 9));
        a.emit(as::stw(size_reg, 0x14, 9));
        a.emit(as::stw(addr_reg, 0x18, 9));
        a.emit(as::stw(old_reg, 0x1C, 9));
        a.emit(as::addi(8, 8, 1));
        a.emit(as::stw(8, 4, 5));
    };
    const u32 alloc_at = a.pc();
    a.emit(as::lwz(5, 0, 3));
    a.emit(as::lwz(6, 0, 5));
    a.emit(as::add(7, 6, 4));
    a.emit(as::addi(7, 7, 15));
    a.emit(as::rlwinm(7, 7, 0, 0, 27));
    a.emit(as::stw(7, 0, 5));
    log_entry_code(1, 4, 6, 0);
    a.emit(as::mr(3, 6));
    a.emit(as::blr());
    const u32 free_at = a.pc();
    a.emit(as::lwz(5, 0, 3));
    log_entry_code(2, 0, 4, 0);
    a.emit(as::blr());
    const u32 realloc_at = a.pc();
    {
        // realloc(memory r3, cur r4, new r5, block r6): a new block, min(cur, new) bytes copied
        a.emit(as::mr(12, 5));
        a.emit(as::lwz(5, 0, 3));
        a.emit(as::lwz(11, 0, 5));
        a.emit(as::add(7, 11, 12));
        a.emit(as::addi(7, 7, 15));
        a.emit(as::rlwinm(7, 7, 0, 0, 27));
        a.emit(as::stw(7, 0, 5));
        a.emit(as::mr(7, 4));
        a.emit(as::cmplw(0, 12, 4));
        auto keep = a.new_label();
        a.bge(0, keep);
        a.emit(as::mr(7, 12));
        a.bind(keep);
        a.emit(as::li(8, 0));
        auto loop = a.here();
        auto done = a.new_label();
        a.emit(as::cmplw(0, 8, 7));
        a.bge(0, done);
        a.emit(as::lbzx(9, 6, 8));
        a.emit(as::stbx(9, 11, 8));
        a.emit(as::addi(8, 8, 1));
        a.b(loop);
        a.bind(done);
        log_entry_code(3, 12, 11, 6);
        a.emit(as::mr(3, 11));
        a.emit(as::blr());
    }
    // the requester: (face_id r3, library r4, req_data r5, aface r6) -> FT_New_Face(library, req_data[face_id], 0, aface)
    const u32 requester_at = a.pc();
    const u32 new_face = opd("cell_FreeType2", "FT_New_Face");
    a.emit(as::mflr(0));
    a.emit(as::std_(0, 16, 1));
    a.emit(as::stdu(1, -128, 1));
    a.emit(as::std_(2, 40, 1));
    a.emit(as::lwz(7, 252, 5)); // calls counted at req_data[63]
    a.emit(as::addi(7, 7, 1));
    a.emit(as::stw(7, 252, 5));
    a.emit(as::slwi(7, 3, 2));
    a.emit(as::mr(3, 4));
    a.emit(as::lwzx(4, 5, 7));
    a.emit(as::li(5, 0));
    a.emit(as::lis(12, static_cast<s32>(hi(new_face))));
    a.emit(as::ori(12, 12, lo(new_face)));
    a.emit(as::lwz(0, 0, 12));
    a.emit(as::lwz(2, 4, 12));
    a.emit(as::mtctr(0));
    a.emit(as::bcctr(20, 0, true));
    a.emit(as::ld(2, 40, 1));
    a.emit(as::addi(1, 1, 128));
    a.emit(as::ld(0, 16, 1));
    a.emit(as::mtlr(0));
    a.emit(as::blr());
    // a memory stream's read: (stream r3, offset r4, buffer r5, count r6) -> count, from descriptor + offset
    const u32 read_at = a.pc();
    {
        auto done = a.new_label();
        a.emit(as::cmpwi(0, 6, 0));
        a.beq(0, done);
        a.emit(as::lwz(7, 12, 3));
        a.emit(as::add(7, 7, 4));
        a.emit(as::li(8, 0));
        auto loop = a.here();
        a.emit(as::cmplw(0, 8, 6));
        a.bge(0, done);
        a.emit(as::lbzx(9, 7, 8));
        a.emit(as::stbx(9, 5, 8));
        a.emit(as::addi(8, 8, 1));
        a.b(loop);
        a.bind(done);
        a.emit(as::mr(3, 6));
        a.emit(as::blr());
    }
    put_code(a);
    w32(kMemory, kHeapCtl);
    w32(kMemory + 4, put_opd(alloc_at));
    w32(kMemory + 8, put_opd(free_at));
    w32(kMemory + 12, put_opd(realloc_at));
    std::memset(b + kHeapCtl, 0, 0x10 + 16 * kHeapLogMax);
    w32(kHeapCtl, kHeap);
    requester_ = put_opd(requester_at);
    stream_read_ = put_opd(read_at);
    clear_log();
}

inline u32 Run::font_memory()
{
    const u32 iface = kFontHeapCtl + 0x10;
    if (r32(iface + 4)) return iface;
    as::Assembler a(code_);
    // malloc(arg r3, size r4): r5 = next, block r6 = r5 + 16, next = (r6 + size + 15) & ~15
    const auto bump = [&] {
        a.emit(as::lwz(5, 0, 3));
        a.emit(as::addi(6, 5, 16));
        a.emit(as::add(7, 6, 4));
        a.emit(as::addi(7, 7, 15));
        a.emit(as::rlwinm(7, 7, 0, 0, 27));
        a.emit(as::stw(7, 0, 3));
        a.emit(as::stw(4, 0, 5));
        a.emit(as::lwz(8, 4, 3));
        a.emit(as::addi(8, 8, 1));
        a.emit(as::stw(8, 4, 3));
    };
    const u32 malloc_at = a.pc();
    bump();
    a.emit(as::mr(3, 6));
    a.emit(as::blr());
    const u32 free_at = a.pc();
    a.emit(as::lwz(8, 8, 3));
    a.emit(as::addi(8, 8, 1));
    a.emit(as::stw(8, 8, 3));
    a.emit(as::blr());
    const u32 realloc_at = a.pc();
    {
        // realloc(arg r3, p r4, size r5): a new block, min(old, size) bytes copied
        a.emit(as::mr(12, 4));
        a.emit(as::mr(4, 5));
        bump();
        auto copied = a.new_label();
        a.emit(as::cmpwi(0, 12, 0));
        a.beq(0, copied);
        a.emit(as::lwz(7, -16, 12));
        a.emit(as::cmplw(0, 7, 4));
        auto keep = a.new_label();
        a.ble(0, keep);
        a.emit(as::mr(7, 4));
        a.bind(keep);
        a.emit(as::li(8, 0));
        auto loop = a.here();
        a.emit(as::cmplw(0, 8, 7));
        a.bge(0, copied);
        a.emit(as::lbzx(9, 12, 8));
        a.emit(as::stbx(9, 6, 8));
        a.emit(as::addi(8, 8, 1));
        a.b(loop);
        a.bind(copied);
        a.emit(as::mr(3, 6));
        a.emit(as::blr());
    }
    const u32 calloc_at = a.pc();
    {
        // calloc(arg r3, n r4, size r5): n * size zeroed bytes
        a.emit(as::mullw(4, 4, 5));
        bump();
        a.emit(as::li(8, 0));
        a.emit(as::li(9, 0));
        auto loop = a.here();
        auto done = a.new_label();
        a.emit(as::cmplw(0, 8, 4));
        a.bge(0, done);
        a.emit(as::stbx(9, 6, 8));
        a.emit(as::addi(8, 8, 1));
        a.b(loop);
        a.bind(done);
        a.emit(as::mr(3, 6));
        a.emit(as::blr());
    }
    put_code(a);
    w32(kFontHeapCtl, kFontHeap);
    w32(kFontHeapCtl + 4, 0);
    w32(kFontHeapCtl + 8, 0);
    w32(iface, kFontHeapCtl);
    w32(iface + 4, put_opd(malloc_at));
    w32(iface + 8, put_opd(free_at));
    w32(iface + 12, put_opd(realloc_at));
    w32(iface + 16, put_opd(calloc_at));
    return iface;
}

inline u32 Run::make_logger(u32 tag, u32 ptrs, u32 scalars, bool spans)
{
    as::Assembler a(code_);
    const u32 at = a.pc();
    // r11 = log control, r12 = count, r0 = entry
    a.emit(as::lis(11, static_cast<s32>(hi(kLogCtl))));
    a.emit(as::ori(11, 11, lo(kLogCtl)));
    a.emit(as::lwz(12, 0, 11));
    a.emit(as::slwi(9, 12, 6));
    a.emit(as::add(9, 9, 11));
    a.emit(as::addi(9, 9, 0x40));
    // entry in r11 from now on; the count kept in r10 for the answer
    a.emit(as::mr(10, 12));
    a.emit(as::addi(12, 12, 1));
    a.emit(as::stw(12, 0, 11));
    a.emit(as::mr(11, 9));
    a.emit(as::li(12, static_cast<s32>(tag)));
    a.emit(as::stw(12, 0, 11));
    if (spans) {
        // y, count, user, then up to 48 bytes of spans
        a.emit(as::stw(3, 4, 11));
        a.emit(as::stw(4, 8, 11));
        a.emit(as::stw(6, 12, 11));
        a.emit(as::mulli(7, 4, 6));
        a.emit(as::cmplwi(0, 7, 48));
        auto ok = a.new_label();
        a.ble(0, ok);
        a.emit(as::li(7, 48));
        a.bind(ok);
        a.emit(as::li(8, 0));
        a.emit(as::addi(9, 11, 16));
        auto loop = a.here();
        auto done = a.new_label();
        a.emit(as::cmplw(0, 8, 7));
        a.bge(0, done);
        a.emit(as::lbzx(12, 5, 8));
        a.emit(as::stbx(12, 9, 8));
        a.emit(as::addi(8, 8, 1));
        a.b(loop);
        a.bind(done);
    } else {
        u32 off = 4;
        for (u32 i = 0; i < ptrs; i++) {
            a.emit(as::lwz(12, 0, 3 + i));
            a.emit(as::stw(12, static_cast<s32>(off), 11));
            a.emit(as::lwz(12, 4, 3 + i));
            a.emit(as::stw(12, static_cast<s32>(off + 4), 11));
            off += 8;
        }
        for (u32 i = 0; i < scalars; i++) {
            a.emit(as::stw(3 + ptrs + i, static_cast<s32>(off), 11));
            off += 4;
        }
    }
    // the answer: kRet[count]
    a.emit(as::slwi(10, 10, 2));
    a.emit(as::lis(12, static_cast<s32>(hi(kRet))));
    a.emit(as::ori(12, 12, lo(kRet)));
    a.emit(as::lwzx(3, 12, 10));
    a.emit(as::blr());
    put_code(a);
    return put_opd(at);
}

// Runs `script` on Sony's modules (`firmware`) and on pspocket's (`host`, over `under`, Sony's modules the host's run
// on: libfont under pspocket's cellFontFT) and compares what each kept.
inline void compare(const std::string& flash, const std::vector<std::string>& under,
    const std::vector<std::string>& host, const std::function<void(Run&)>& script, const std::string& what)
{
    std::vector<std::string> sonys, mine;
    {
        std::vector<std::string> all = under;
        all.insert(all.end(), host.begin(), host.end());
        Run r(flash, all, {});
        script(r);
        sonys = std::move(r.out);
    }
    {
        Run r(flash, under, host);
        script(r);
        mine = std::move(r.out);
    }
    if (std::getenv("PSPOCKET_FT_DUMP")) {
        for (usize i = 0; i < std::max(sonys.size(), mine.size()); i++) {
            const std::string s = i < sonys.size() ? sonys[i] : "(none)";
            const std::string m = i < mine.size() ? mine[i] : "(none)";
            std::fprintf(stderr, "%s %zu S: %s\n", s == m ? "  " : "!!", i, s.c_str());
            if (s != m) std::fprintf(stderr, "!! %zu O: %s\n", i, m.c_str());
        }
    }
    usize differ = 0;
    for (usize i = 0; i < std::max(sonys.size(), mine.size()); i++) {
        const std::string s = i < sonys.size() ? sonys[i] : "(none)";
        const std::string m = i < mine.size() ? mine[i] : "(none)";
        if (s == m) continue;
        if (++differ > 20) continue;
        INFO(std::format("{}: observation {}:\n  Sony's: {}\n  ours:   {}", what, i, s, m));
        CHECK(m == s);
    }
    INFO(std::format("{}: {} observations, {} differ", what, sonys.size(), differ));
    CHECK(differ == 0);
}

// Runs `script` on Sony's modules and on pspocket's and compares what each kept.
inline void compare(const std::string& flash, const std::vector<std::string>& libraries,
    const std::function<void(Run&)>& script, const std::string& what)
{
    compare(flash, {}, libraries, script, what);
}

} // namespace pspocket::hle::test::ft
