#include "pspocket/codesign/macho.hpp"

#include <array>
#include <cstring>
#include <format>

namespace pspocket::codesign {
namespace {

constexpr u32 kMagic64 = 0xFEEDFACF;
constexpr u32 kCpuArm64 = 0x0100000C;
constexpr u32 kDylib = 6;
constexpr u32 kNoUndefs = 0x1, kDyldLink = 0x4, kTwoLevel = 0x80, kNoReexportedDylibs = 0x100000;
constexpr u32 kSegment64 = 0x19, kIdDylib = 0xD, kDyldInfoOnly = 0x80000022, kSymtab = 0x2, kDysymtab = 0xB,
              kUuid = 0x1B, kBuildVersion = 0x32, kCodeSignature = 0x1D;
constexpr u64 kPage = 0x4000; // arm64 iOS and macOS segments are 16 KB aligned

struct Writer {
    std::vector<u8>& out;
    void u8_(u8 v) { out.push_back(v); }
    void u32_(u32 v)
    {
        for (int i = 0; i < 4; i++) out.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void u64_(u64 v)
    {
        for (int i = 0; i < 8; i++) out.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void name16(std::string_view s)
    {
        char n[16] = {};
        std::memcpy(n, s.data(), std::min<usize>(s.size(), 16));
        out.insert(out.end(), n, n + 16);
    }
    void pad_to(usize alignment)
    {
        while (out.size() % alignment) out.push_back(0);
    }
};

void uleb(std::vector<u8>& out, u64 v)
{
    do {
        u8 b = v & 0x7F;
        v >>= 7;
        if (v) b |= 0x80;
        out.push_back(b);
    } while (v);
}

u32 get32(const std::vector<u8>& b, usize at)
{
    return static_cast<u32>(b[at]) | static_cast<u32>(b[at + 1]) << 8 | static_cast<u32>(b[at + 2]) << 16 |
        static_cast<u32>(b[at + 3]) << 24;
}

u64 get64(const std::vector<u8>& b, usize at) { return get32(b, at) | static_cast<u64>(get32(b, at + 4)) << 32; }

} // namespace

std::vector<u8> answer_dylib(std::string_view symbol, u32 answer, Platform platform, u32 min_os,
    std::string_view install_name)
{
    const std::string sym = "_" + std::string(symbol);
    const u32 id_size = static_cast<u32>((24 + install_name.size() + 1 + 7) & ~usize{7});
    const u32 ncmds = 9;
    const u32 cmds_size = (72 + 80) + 72 + id_size + 48 + 24 + 80 + 24 + 24 + 16;
    const u32 text_off = (32 + cmds_size + 15) & ~u32{15};
    const std::array<u32, 2> code = {0x52800000u | ((answer & 0xFFFF) << 5), 0xD65F03C0u}; // mov w0, #answer; ret

    // __LINKEDIT, from 16 KB: the export trie, the symbol table, the string table, then the code signature.
    std::vector<u8> trie;
    {
        std::vector<u8> child;
        std::vector<u8> info;
        uleb(info, 0);        // flags: a regular symbol
        uleb(info, text_off); // its address from the mach header
        uleb(child, info.size());
        child.insert(child.end(), info.begin(), info.end());
        child.push_back(0); // no children
        trie.push_back(0);  // the root exports nothing itself
        trie.push_back(1);  // one edge
        trie.insert(trie.end(), sym.begin(), sym.end());
        trie.push_back(0);
        uleb(trie, sym.size() + 4); // the child's offset: the root's own size (one byte of offset, at most 127)
        trie.insert(trie.end(), child.begin(), child.end());
        while (trie.size() % 8) trie.push_back(0);
    }
    const u32 trie_off = static_cast<u32>(kPage);
    const u32 symtab_off = trie_off + static_cast<u32>(trie.size());
    const u32 strtab_off = symtab_off + 16;
    std::string strtab(" ");
    strtab.push_back('\0');
    const u32 sym_strx = static_cast<u32>(strtab.size());
    strtab += sym;
    strtab.push_back('\0');
    while (strtab.size() % 8) strtab.push_back('\0');
    const u32 signature_off = (strtab_off + static_cast<u32>(strtab.size()) + 15) & ~u32{15};

    std::vector<u8> out;
    Writer w{out};
    w.u32_(kMagic64);
    w.u32_(kCpuArm64);
    w.u32_(0); // CPU_SUBTYPE_ARM64_ALL
    w.u32_(kDylib);
    w.u32_(ncmds);
    w.u32_(cmds_size);
    w.u32_(kNoUndefs | kDyldLink | kTwoLevel | kNoReexportedDylibs);
    w.u32_(0); // reserved

    // __TEXT: the header, the load commands and the code, read and execute.
    w.u32_(kSegment64);
    w.u32_(72 + 80);
    w.name16("__TEXT");
    w.u64_(0);
    w.u64_(kPage);
    w.u64_(0);
    w.u64_(kPage);
    w.u32_(5); // r-x
    w.u32_(5);
    w.u32_(1); // one section
    w.u32_(0);
    w.name16("__text");
    w.name16("__TEXT");
    w.u64_(text_off);
    w.u64_(sizeof code);
    w.u32_(text_off);
    w.u32_(2); // 4-byte alignment
    w.u32_(0);
    w.u32_(0);
    w.u32_(0x80000400); // S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS
    w.u32_(0);
    w.u32_(0);
    w.u32_(0);

    // __LINKEDIT: sizes as for an empty signature; sign() sets them for the real one.
    w.u32_(kSegment64);
    w.u32_(72);
    w.name16("__LINKEDIT");
    w.u64_(kPage);
    w.u64_((signature_off - kPage + kPage - 1) & ~(kPage - 1));
    w.u64_(kPage);
    w.u64_(signature_off - kPage);
    w.u32_(1); // r--
    w.u32_(1);
    w.u32_(0);
    w.u32_(0);

    w.u32_(kIdDylib);
    w.u32_(id_size);
    w.u32_(24); // the name's offset in the command
    w.u32_(1);  // timestamp
    w.u32_(0x10000); // current version 1.0
    w.u32_(0x10000); // compatibility version 1.0
    for (const char c : install_name) w.u8_(static_cast<u8>(c));
    w.u8_(0);
    w.pad_to(8);

    w.u32_(kDyldInfoOnly);
    w.u32_(48);
    for (int i = 0; i < 8; i++) w.u32_(0); // no rebases, binds, weak or lazy binds
    w.u32_(trie_off);
    w.u32_(static_cast<u32>(trie.size()));

    w.u32_(kSymtab);
    w.u32_(24);
    w.u32_(symtab_off);
    w.u32_(1);
    w.u32_(strtab_off);
    w.u32_(static_cast<u32>(strtab.size()));

    w.u32_(kDysymtab);
    w.u32_(80);
    w.u32_(0); // no local symbols
    w.u32_(0);
    w.u32_(0); // one external defined symbol
    w.u32_(1);
    w.u32_(1); // no undefined ones
    w.u32_(0);
    for (int i = 0; i < 12; i++) w.u32_(0);

    w.u32_(kUuid);
    w.u32_(24);
    for (int i = 0; i < 16; i++) w.u8_(static_cast<u8>(0x50 + i + (answer & 0xFF))); // stable per answer

    w.u32_(kBuildVersion);
    w.u32_(24);
    w.u32_(static_cast<u32>(platform));
    w.u32_(min_os);
    w.u32_(min_os); // sdk
    w.u32_(0);      // no tools

    w.u32_(kCodeSignature);
    w.u32_(16);
    w.u32_(signature_off);
    w.u32_(0);

    w.pad_to(16);
    out.resize(text_off, 0);
    for (const u32 insn : code) w.u32_(insn);
    out.resize(kPage, 0);

    out.insert(out.end(), trie.begin(), trie.end());
    // nlist_64: the symbol in section 1, external.
    w.u32_(sym_strx);
    w.u8_(0x0F); // N_SECT | N_EXT
    w.u8_(1);
    w.u8_(0);
    w.u8_(0);
    w.u64_(text_off);
    out.insert(out.end(), strtab.begin(), strtab.end());
    out.resize(signature_off, 0);
    return out;
}

std::optional<MachOLayout> read_layout(const std::vector<u8>& b, std::string* error)
{
    const auto fail = [&](std::string why) -> std::optional<MachOLayout> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };
    if (b.size() < 32 || get32(b, 0) != kMagic64) return fail("not a 64-bit Mach-O");
    MachOLayout l;
    l.filetype = get32(b, 12);
    const u32 ncmds = get32(b, 16), cmds_size = get32(b, 20);
    if (32 + u64{cmds_size} > b.size()) return fail("load commands past the end of the file");
    usize at = 32;
    for (u32 i = 0; i < ncmds; i++) {
        if (at + 8 > 32 + u64{cmds_size}) return fail("load command past the commands' size");
        const u32 cmd = get32(b, at), size = get32(b, at + 4);
        if (size < 8 || at + size > 32 + u64{cmds_size}) return fail(std::format("load command {} has size {}", i, size));
        if (cmd == kSegment64 && size >= 72) {
            const std::string_view name(reinterpret_cast<const char*>(&b[at + 8]), strnlen(reinterpret_cast<const char*>(&b[at + 8]), 16));
            if (name == "__LINKEDIT") {
                l.linkedit_cmd = static_cast<u32>(at);
                l.linkedit_fileoff = get64(b, at + 40);
            } else if (name == "__TEXT") {
                l.text_fileoff = get64(b, at + 40);
                l.text_filesize = get64(b, at + 48);
            }
        } else if (cmd == kCodeSignature && size >= 16) {
            l.code_signature_cmd = static_cast<u32>(at);
            l.signature_offset = get32(b, at + 8);
            l.signature_size = get32(b, at + 12);
        }
        at += size;
    }
    if (!l.linkedit_cmd) return fail("no __LINKEDIT segment");
    if (!l.code_signature_cmd) return fail("no LC_CODE_SIGNATURE (link with a signature, or reserve one)");
    if (l.signature_offset < l.linkedit_fileoff || l.signature_offset > b.size())
        return fail("the code signature is not in __LINKEDIT");
    return l;
}

} // namespace pspocket::codesign
