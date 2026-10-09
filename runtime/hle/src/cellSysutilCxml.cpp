// cellSysutil's cxml, cxmlutil and sysutil_cxmlutil: libsysutil's binary XML documents, the helpers it reads and
// writes their attributes with, its fixed-memory allocator and its packet writer (D-534). Read from libsysutil.sprx
// (4.92) and compared with it call for call (tests/hle/test_sysutil_cxml_compare.cpp); RPCS3 has none of these (its
// cellSysutil registers them as stubs). The layout of the objects is in sysutil_cxml.hpp.
//
// The format: Document::Write writes the 0x38-byte header (with the tables' offsets laid out: the first at 0x40, each
// table 16-byte aligned) padded to 0x40, then each table padded to 16 bytes with zeros. A document either owns its
// tables (mode 0; they come from its allocator, called with the table's type 0..5 to get or grow one and 6..11 to free
// one) or reads a buffer it was made over (mode 2, CreateFromBuffer without a copy), which nothing may change.
//
// libsysutil's slips are kept: CreateElement grows the tree before it adds the element's name, so a failed name leaves
// the tree bigger; SetString marks the attribute a string before its value is added; Clear on a document made over a
// buffer leaves the table pointers as they were; the cxmlutil setters, when the element has no document, look for a
// free attribute and answer "no attribute" (0x8002e011), not "no document". Where libsysutil's code would read or
// write memory that is not there, pspocket logs the call site and answers CELL_EFAULT (a void function returns).

#include <bit>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "sysutil_cxml.hpp"

namespace pspocket::hle::sysutil {

namespace {

constexpr std::string_view kChan = "cellSysutil";

constexpr u32 kNidFixedAllocate = 0xB59872EF;
constexpr u32 kNidPacketWriterWrite = 0x7FC8F72C;
constexpr std::string_view kFixedAllocateName =
    "_ZN16sysutil_cxmlutil11FixedMemory8AllocateEN4cxml14AllocationTypeEPvS3_jPS3_Pj";
constexpr u32 CELL_EFAULT = 0x8001000D;

constexpr s32 err(u32 code) { return static_cast<s32>(code); }

// libcxml's padding of a size to 16 bytes, in its signed arithmetic.
s32 pad16(s32 x)
{
    if ((x & 0xF) == 0) return 0;
    return 16 - x % 16;
}
u32 align16(u32 x) { return x + static_cast<u32>(pad16(static_cast<s32>(x))); }

// A name or value: the game's string, read where libsysutil's code reads it, or one of pspocket's.
struct Text {
    u32 addr = 0;
    std::string_view host;
    bool is_host = false;

    static Text guest(u32 a) { return Text{a, {}, false}; }
    static Text of(std::string_view s) { return Text{0, s, true}; }
    u8 at(Mem& m, u32 i) const
    {
        if (is_host) return i < host.size() ? static_cast<u8>(host[i]) : 0;
        return m.r8(addr + i);
    }
    u32 length(Mem& m) const { return is_host ? static_cast<u32>(host.size()) : m.strlen(addr); }
    // memcpy(dst, text, length + 1).
    void copy_to(Mem& m, u32 dst, u32 len) const
    {
        if (!is_host) {
            m.copy(dst, addr, len + 1);
            return;
        }
        m.put(dst, host.data(), len);
        m.w8(dst + len, 0);
    }
};

// libcxml's comparison of a string of the document with a name: the first bytes, then strcmp.
bool same(Mem& m, u32 a, const Text& t)
{
    if (m.r8(a) != t.at(m, 0)) return false;
    for (u32 i = 0;; i++) {
        const u8 x = m.r8(a + i), y = t.at(m, i);
        if (x != y) return false;
        if (!x) return true;
    }
}

// ------------------------------------------------------------------------------------------------- process state

struct CxmlState {
    std::mutex mutex;
    u32 fixed_allocate = 0; // FixedMemory::Allocate's descriptor, once linked
    u32 packet_writer = 0;  // PacketWriter::Write's, once linked
};

CxmlState& state()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellSysutil's cxml called without an HLE process");
    return p->state<CxmlState>();
}

// The descriptor the game has for one of these functions, or 0 when it has none (then no document names it).
u32 linked_descriptor(u32 CxmlState::*slot, u32 nid)
{
    CxmlState& s = state();
    std::lock_guard lock(s.mutex);
    if (!(s.*slot)) s.*slot = HleProcess::current()->descriptor_of("cellSysutil", nid);
    return s.*slot;
}

// The descriptor libsysutil's own requests name as their documents' allocator (libsysutil's: its data's; pspocket's:
// the import's, linked here when the game has none).
u32 fixed_memory_allocator()
{
    CxmlState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.fixed_allocate) {
        HleProcess* p = HleProcess::current();
        s.fixed_allocate = p->descriptor_of("cellSysutil", kNidFixedAllocate);
        if (!s.fixed_allocate) s.fixed_allocate = p->link_function("cellSysutil", kNidFixedAllocate, kFixedAllocateName);
    }
    return s.fixed_allocate;
}

// ------------------------------------------------------------------------------------------------- FixedMemory

constexpr u32 kFixedBlock = 0x1000, kFixedInUse = 0x6000, kFixedTaken = 0x6001;

// sysutil_cxmlutil::FixedMemory::Allocate: the block of a table (types 0..5, at most 4 KB, each once), or the block
// given back (types 6..11). `old` is not looked at: a table never grows past its block.
void fixed_allocate(Mem& m, u32 type, u32 self, u32 size, u32 out, u32 out_size)
{
    if (type > 11) return;
    if (type >= 6) {
        m.w8(self + kFixedTaken + (type - 6), 0);
        return;
    }
    const u32 flag = self + kFixedTaken + type;
    if (!m.r8(flag) && size <= kFixedBlock) {
        if (type == 0) {
            m.w32(out_size, kFixedBlock);
            m.w8(flag, 1);
            m.w32(out, self);
        } else {
            m.w32(out, self + type * kFixedBlock);
            m.w32(out_size, kFixedBlock);
            m.w8(flag, 1);
        }
        return;
    }
    m.w32(out_size, 0);
    m.w32(out, 0);
}

// The document's allocator: FixedMemory::Allocate natively when the document names it, else the game's function.
void call_allocator(PPUContext& ctx, Mem& m, u32 d, u32 type, u32 old, u32 size, u32 out, u32 out_size)
{
    const u32 fn = m.r32(d + doc::alloc);
    const u32 arg = m.r32(d + doc::alloc_arg);
    if (fn && fn == linked_descriptor(&CxmlState::fixed_allocate, kNidFixedAllocate)) {
        fixed_allocate(m, type, arg, size, out, out_size);
        return;
    }
    m.r32(fn);
    m.r32(fn + 4);
    call_guest(ctx, fn, {type, arg, old, size, out, out_size});
}

// ------------------------------------------------------------------------------------------------- the document

// The header's fields reset: "CXML", version 0x110, every table empty.
void reset_header(Mem& m, u32 d)
{
    m.put(d + doc::magic, "CXML", 4);
    m.w32(d + doc::version, 0x110);
    for (u32 o = doc::tree_off; o < doc::header + doc::kHeaderSize; o += 4) m.w32(d + o, 0);
}

// A string of the string table (its offset in range), or 0.
u32 string_at(Mem& m, u32 d, u32 off)
{
    const s32 o = static_cast<s32>(off);
    if (o >= 0 && o < static_cast<s32>(m.r32(d + doc::size_of(2)))) return off + m.r32(d + doc::ptr_of(2));
    return 0;
}

// An ID of the ID table (past its 4-byte head), or 0.
u32 id_at(Mem& m, u32 d, u32 off)
{
    const s32 o = static_cast<s32>(off);
    if (o >= 0 && o < static_cast<s32>(m.r32(d + doc::size_of(1)))) return off + m.r32(d + doc::ptr_of(1)) + 4;
    return 0;
}

// Whether an element lies whole in the tree.
bool element_valid(Mem& m, u32 d, u32 off)
{
    const u32 tree_size = m.r32(d + doc::tree_size);
    if (static_cast<s32>(off) < 0 || off + 0x1C > tree_size) return false;
    const s32 count = static_cast<s32>(m.r32(off + m.r32(d + doc::tree) + 4));
    return count >= 0 && static_cast<u32>(count) * 16 + off + 0x1C <= tree_size;
}

// The string table's offset of `text`, added when it isn't there yet (`len` its length).
s32 add_string(PPUContext& ctx, Mem& m, u32 d, const Text& text, u32 len)
{
    if (m.r32(d + doc::mode)) return err(CXML_ERROR_READ_ONLY);
    if (!m.r32(d + doc::alloc)) return err(CXML_ERROR_NO_ALLOCATOR);
    const s32 size = static_cast<s32>(m.r32(d + doc::size_of(2)));
    for (s32 off = 0; off < size;) {
        const u32 at = static_cast<u32>(off) + m.r32(d + doc::ptr_of(2));
        if (same(m, at, text)) return off;
        off += static_cast<s32>(m.strlen(at)) + 1;
    }
    const u32 need = len + static_cast<u32>(size) + 1;
    if (m.r32(d + doc::cap_of(2)) < need) {
        StackVar out(ctx, m.base(), 8);
        call_allocator(ctx, m, d, 2, m.r32(d + doc::ptr_of(2)), need, out.addr(), out.addr() + 4);
        const u32 p = m.r32(out.addr());
        if (!p) return err(CXML_ERROR_NO_MEMORY);
        m.w32(d + doc::ptr_of(2), p);
        m.w32(d + doc::cap_of(2), m.r32(out.addr() + 4));
    }
    const u32 off = m.r32(d + doc::size_of(2));
    m.w32(d + doc::size_of(2), len + off + 1);
    text.copy_to(m, off + m.r32(d + doc::ptr_of(2)), len);
    return static_cast<s32>(off);
}

// The tree offset of an attribute's fields.
u32 attr_node(Mem& m, const Attribute& a) { return m.r32(a.doc + doc::tree) + a.off; }

s32 attr_set_name(PPUContext& ctx, Mem& m, const Attribute& a, const Text& name)
{
    if (!a.doc) return err(CXML_ERROR_NO_DOCUMENT);
    if (m.r32(a.doc + doc::mode)) return err(CXML_ERROR_READ_ONLY);
    const u32 len = name.length(m);
    const u32 tree = m.r32(a.doc + doc::tree);
    const s32 off = add_string(ctx, m, a.doc, name, len);
    if (off < 0) return off;
    m.w32(tree + a.off, static_cast<u32>(off));
    return 0;
}

s32 attr_set_int(Mem& m, const Attribute& a, u32 value)
{
    if (!a.doc) return err(CXML_ERROR_NO_DOCUMENT);
    if (m.r32(a.doc + doc::mode)) return err(CXML_ERROR_READ_ONLY);
    const u32 node = attr_node(m, a);
    m.w32(node + 8, value);
    m.w32(node + 4, 1);
    return 0;
}

s32 attr_set_float(Mem& m, const Attribute& a, float value)
{
    if (!a.doc) return err(CXML_ERROR_NO_DOCUMENT);
    if (m.r32(a.doc + doc::mode)) return err(CXML_ERROR_READ_ONLY);
    const u32 node = attr_node(m, a);
    m.w32(node + 8, std::bit_cast<u32>(value));
    m.w32(node + 4, 2);
    return 0;
}

s32 attr_set_string(PPUContext& ctx, Mem& m, const Attribute& a, const Text& value)
{
    if (!a.doc) return err(CXML_ERROR_NO_DOCUMENT);
    if (m.r32(a.doc + doc::mode)) return err(CXML_ERROR_READ_ONLY);
    const u32 node = attr_node(m, a);
    m.w32(node + 4, 3); // marked a string before its value is added (libsysutil's order)
    const u32 len = value.length(m);
    const s32 off = add_string(ctx, m, a.doc, value, len);
    if (off < 0) return off;
    m.w32(node + 12, len);
    m.w32(node + 8, static_cast<u32>(off));
    return 0;
}

// Attribute::SetFile: the file's offset and size (`get` reads them where libsysutil reads them: after its checks).
template <class Get>
s32 attr_set_file(Mem& m, const Attribute& a, Get&& get)
{
    if (!a.doc) return err(CXML_ERROR_NO_DOCUMENT);
    if (m.r32(a.doc + doc::mode)) return err(CXML_ERROR_READ_ONLY);
    const u32 node = attr_node(m, a);
    m.w32(node + 4, 6);
    const File f = get();
    m.w32(node + 8, f.off);
    m.w32(node + 12, f.size);
    return 0;
}

u32 attr_type(Mem& m, const Attribute& a) { return a.doc ? m.r32(attr_node(m, a) + 4) : 0; }

// An attribute's value of type `type`: its two words.
s32 attr_get(Mem& m, const Attribute& a, u32 type, u32& v1, u32& v2)
{
    if (!a.doc) return err(CXML_ERROR_NO_DOCUMENT);
    const u32 node = attr_node(m, a);
    if (m.r32(node + 4) != type) return err(CXML_ERROR_TYPE);
    v1 = m.r32(node + 8);
    v2 = m.r32(node + 12);
    return 0;
}

// Attribute::GetString's code (cxmlutil::GetString's): the string's address at *out_str, its length at *out_len.
s32 attr_get_string(Mem& m, const Attribute& a, u32 out_str, u32 out_len)
{
    if (!a.doc) return err(CXML_ERROR_NO_DOCUMENT);
    const u32 node = attr_node(m, a);
    if (m.r32(node + 4) != 3) return err(CXML_ERROR_TYPE);
    const u32 s = string_at(m, a.doc, m.r32(node + 8));
    m.w32(out_str, s);
    if (!s) return err(CXML_ERROR_NO_DOCUMENT);
    const u32 len = m.r32(node + 12);
    if (m.r8(s + len)) return err(CXML_ERROR_BAD_TABLE);
    m.w32(out_len, len);
    return 0;
}

// Attribute::GetID's code: the ID's string.
s32 attr_get_id(Mem& m, const Attribute& a, u32& out)
{
    if (!a.doc) return err(CXML_ERROR_NO_DOCUMENT);
    const u32 node = attr_node(m, a);
    if (m.r32(node + 4) != 7) return err(CXML_ERROR_TYPE);
    out = id_at(m, a.doc, m.r32(node + 8));
    return 0;
}

// The element's name (a string of the table), or 0.
u32 element_name(Mem& m, const Element& e)
{
    if (!e.doc) return 0;
    return string_at(m, e.doc, m.r32(m.r32(e.doc + doc::tree) + e.off));
}

u32 element_attr_count(Mem& m, const Element& e)
{
    if (!e.doc) return 0;
    return m.r32(m.r32(e.doc + doc::tree) + e.off + 4);
}

// The element's attribute `i`.
s32 element_attr_at(Mem& m, const Element& e, u32 i, Attribute& out)
{
    if (!e.doc) return err(CXML_ERROR_NO_DOCUMENT);
    const u32 count = m.r32(m.r32(e.doc + doc::tree) + e.off + 4);
    if (static_cast<s32>(i) >= static_cast<s32>(count)) return err(CXML_ERROR_NO_DOCUMENT);
    out = {e.doc, e.off, i * 16 + e.off + 0x1C};
    return 0;
}

s32 find_attribute(Mem& m, const Element& e, const Text& name, Attribute& out)
{
    if (!e.doc) return err(CXML_ERROR_NO_DOCUMENT);
    const u32 node = m.r32(e.doc + doc::tree) + e.off;
    const s32 count = static_cast<s32>(m.r32(node + 4));
    for (s32 i = 0; i < count; i++) {
        const u32 n = string_at(m, e.doc, m.r32(node + 0x1C + 16 * static_cast<u32>(i)));
        if (n && same(m, n, name)) {
            out = {e.doc, e.off, e.off + 0x1C + 16 * static_cast<u32>(i)};
            return 0;
        }
    }
    return err(CXML_ERROR_NO_ATTRIBUTE);
}

// The first attribute slot no value was given to (type 0).
s32 free_attribute(Mem& m, const Element& e, Attribute& out)
{
    Attribute a;
    for (u32 i = 0; static_cast<s32>(i) < static_cast<s32>(element_attr_count(m, e)); i++) {
        if (const s32 r = element_attr_at(m, e, i, a); r < 0) return r;
        if (attr_type(m, a) == 0) {
            out = a;
            return 0;
        }
    }
    return err(CXML_ERROR_NO_ATTRIBUTE);
}

// The attribute a cxmlutil setter writes: the one of that name, else a free one given the name.
s32 settable_attribute(PPUContext& ctx, Mem& m, const Element& e, const Text& name, Attribute& a)
{
    if (find_attribute(m, e, name, a) >= 0) return 0;
    if (const s32 r = free_attribute(m, e, a); r < 0) return r;
    return attr_set_name(ctx, m, a, name);
}

// libsysutil's way of answering a setter or getter: an error as it is, anything else 0.
s32 only_errors(s32 r) { return r < 0 ? r : 0; }

s32 set_int(PPUContext& ctx, Mem& m, const Element& e, const Text& name, s32 value)
{
    Attribute a;
    if (const s32 r = settable_attribute(ctx, m, e, name, a); r < 0) return r;
    return only_errors(attr_set_int(m, a, static_cast<u32>(value)));
}

s32 set_float(PPUContext& ctx, Mem& m, const Element& e, const Text& name, float value)
{
    Attribute a;
    if (const s32 r = settable_attribute(ctx, m, e, name, a); r < 0) return r;
    return only_errors(attr_set_float(m, a, value));
}

s32 set_string(PPUContext& ctx, Mem& m, const Element& e, const Text& name, const Text& value)
{
    Attribute a;
    if (const s32 r = settable_attribute(ctx, m, e, name, a); r < 0) return r;
    return only_errors(attr_set_string(ctx, m, a, value));
}

// cxmlutil's getters: the attribute, then its value through `get`.
template <class Get>
s32 get_value(Mem& m, const Element& e, const Text& name, Get&& get)
{
    Attribute a;
    if (const s32 r = find_attribute(m, e, name, a); r < 0) return r;
    return only_errors(get(a));
}

s32 create_element(PPUContext& ctx, Mem& m, u32 d, const Text& name, s32 count, Element& out)
{
    if (m.r32(d + doc::mode)) return err(CXML_ERROR_READ_ONLY);
    if (!m.r32(d + doc::alloc)) return err(CXML_ERROR_NO_ALLOCATOR);
    const u32 size = static_cast<u32>(count) * 16 + 0x1C;
    const u32 need = size + m.r32(d + doc::tree_size);
    if (m.r32(d + doc::tree_cap) < need) {
        StackVar outv(ctx, m.base(), 8);
        call_allocator(ctx, m, d, 0, m.r32(d + doc::tree), need, outv.addr(), outv.addr() + 4);
        const u32 p = m.r32(outv.addr());
        if (!p) return err(CXML_ERROR_NO_MEMORY);
        m.w32(d + doc::tree, p);
        m.w32(d + doc::tree_cap, m.r32(outv.addr() + 4));
    }
    const u32 off = m.r32(d + doc::tree_size);
    m.w32(d + doc::tree_size, off + size); // grown before the name is added (libsysutil's order)
    const s32 s = add_string(ctx, m, d, name, name.length(m));
    if (s < 0) return s;
    const u32 node = off + m.r32(d + doc::tree);
    m.w32(node + 24, 0xFFFFFFFF); // last child
    m.w32(node + 8, 0xFFFFFFFF);  // parent
    m.w32(node + 12, 0xFFFFFFFF); // previous sibling
    m.w32(node + 16, 0xFFFFFFFF); // next sibling
    m.w32(node + 20, 0xFFFFFFFF); // first child
    m.w32(node + 0, static_cast<u32>(s));
    m.w32(node + 4, static_cast<u32>(count));
    for (s32 i = 0; i < count; i++) {
        const u32 at = off + 16 * static_cast<u32>(i) + m.r32(d + doc::tree);
        m.w32(at + 0x28, 0);
        m.w32(at + 0x1C, 0xFFFFFFFF);
        m.w32(at + 0x20, 0);
        m.w32(at + 0x24, 0);
    }
    out = {d, off};
    return 0;
}

Element first_child(Mem& m, const Element& e)
{
    if (e.doc) {
        const u32 off = m.r32(m.r32(e.doc + doc::tree) + e.off + 20);
        if (element_valid(m, e.doc, off)) return {e.doc, off};
    }
    return {0, 0xFFFFFFFF};
}

Element next_sibling(Mem& m, const Element& e)
{
    if (e.doc) {
        const u32 off = m.r32(m.r32(e.doc + doc::tree) + e.off + 16);
        if (element_valid(m, e.doc, off)) return {e.doc, off};
    }
    return {0, 0xFFFFFFFF};
}

Element find_child(Mem& m, const Element& parent, const Text& name, const Text* attr, const Text* value)
{
    Element child = first_child(m, parent);
    while (child.doc && static_cast<s32>(child.off) >= 0) {
        const u32 n = element_name(m, child);
        if (same(m, n, name)) {
            if (!attr || !value) return child;
            Attribute a;
            u32 id = 0;
            if (find_attribute(m, child, *attr, a) == 0 && attr_type(m, a) == 7 && attr_get_id(m, a, id) == 0 &&
                same(m, id, *value))
                return child;
        }
        child = next_sibling(m, child);
    }
    return {0, 0xFFFFFFFF};
}

s32 append_child(Mem& m, const Element& parent, u32 child_off)
{
    const u32 d = parent.doc;
    if (!d) return err(CXML_ERROR_NO_DOCUMENT);
    if (m.r32(d + doc::mode)) return err(CXML_ERROR_READ_ONLY);
    const u32 tree = m.r32(d + doc::tree);
    const u32 cnode = tree + child_off;
    if (!child_off) return err(CXML_ERROR_NOT_APPENDABLE);
    const u32 pnode = tree + parent.off;
    if (static_cast<s32>(m.r32(cnode + 8)) >= 0) return err(CXML_ERROR_NOT_APPENDABLE);
    const u32 last = m.r32(pnode + 24);
    if (static_cast<s32>(m.r32(pnode + 20)) < 0) m.w32(pnode + 20, child_off);
    m.w32(pnode + 24, child_off);
    m.w32(cnode + 12, last);
    m.w32(cnode + 8, parent.off);
    if (element_valid(m, d, last)) m.w32(last + m.r32(d + doc::tree) + 16, child_off);
    return 0;
}

s32 register_file(PPUContext& ctx, Mem& m, u32 d, u32 data, u32 size, File& out)
{
    if (m.r32(d + doc::mode)) return err(CXML_ERROR_READ_ONLY);
    if (!m.r32(d + doc::alloc)) return err(CXML_ERROR_NO_ALLOCATOR);
    const u32 padded = align16(size);
    if (m.r32(d + doc::cap_of(5)) < padded + m.r32(d + doc::size_of(5))) {
        StackVar outv(ctx, m.base(), 8);
        call_allocator(ctx, m, d, 5, m.r32(d + doc::ptr_of(5)), padded + m.r32(d + doc::size_of(5)), outv.addr(),
            outv.addr() + 4);
        const u32 p = m.r32(outv.addr());
        if (!p) return err(CXML_ERROR_NO_MEMORY);
        m.w32(d + doc::ptr_of(5), p);
        m.w32(d + doc::cap_of(5), m.r32(outv.addr() + 4));
    }
    const u32 off = m.r32(d + doc::size_of(5));
    m.w32(d + doc::size_of(5), off + padded);
    m.copy(off + m.r32(d + doc::ptr_of(5)), data, size);
    m.fill(off + size + m.r32(d + doc::ptr_of(5)), 0, padded - size);
    out = {d, off, size};
    return 0;
}

// sysutil_cxmlutil::PacketWriter::Write on the game's writer object (channel, ID, bytes left): a packet's next bytes,
// the last of them ending it; their count, or the packet's error.
s32 writer_object_write(PPUContext& ctx, Mem& m, u32 data, u32 size, u32 writer)
{
    const u32 left = m.r32(writer + 8);
    if (!left) return 0;
    const s32 r = packet_write(ctx, m.r32(writer), static_cast<s32>(m.r32(writer + 4)), data, size, left == size ? 1 : 0);
    if (r != 0) return r;
    m.w32(writer + 8, m.r32(writer + 8) - size);
    return static_cast<s32>(size);
}

// ------------------------------------------------------------------------------------------------- writers

// The game's writer function (a descriptor), called as Document::Write calls it.
class GuestWriter final : public DocWriter {
public:
    GuestWriter(u32 fn, u32 arg) : fn_(fn), arg_(arg) {}
    s32 write(PPUContext& ctx, u32 data, u32 size) override
    {
        Mem m(ctx);
        if (fn_ && fn_ == linked_descriptor(&CxmlState::packet_writer, kNidPacketWriterWrite))
            return writer_object_write(ctx, m, data, size, arg_);
        m.r32(fn_);
        m.r32(fn_ + 4);
        return static_cast<s32>(static_cast<u32>(call_guest(ctx, fn_, {data, size, arg_})));
    }

private:
    u32 fn_, arg_;
};

// A function body; memory that is not there ends it (logged with the call site).
template <class R, class Body>
R guarded(PPUContext& ctx, std::string_view what, R on_fault, Body&& body)
{
    try {
        Mem m(ctx);
        return body(m);
    } catch (const Fault& f) {
        log_call_site(ctx, guest_base(ctx),
            std::format("{}: guest memory at {:#010x} is not there (libsysutil faults there)", what, f.addr));
        return on_fault;
    }
}

template <class Body>
void guarded_void(PPUContext& ctx, std::string_view what, Body&& body)
{
    guarded(ctx, what, 0, [&](Mem& m) {
        body(m);
        return 0;
    });
}

constexpr s32 kFault = static_cast<s32>(CELL_EFAULT);

Element read_element(Mem& m, u32 at) { return {m.r32(at), m.r32(at + 4)}; }

void write_element(Mem& m, u32 at, const Element& e)
{
    m.w32(at, e.doc);
    m.w32(at + 4, e.off);
}

Attribute read_attribute(Mem& m, u32 at) { return {m.r32(at), m.r32(at + 4), m.r32(at + 8)}; }

// ------------------------------------------------------------------------------------------------- exports

// cxml::Document::Document(). r3 is left as libsysutil leaves it: the header's address.
u32 cxml_Document_ctor(PPUContext& ctx, u32 d)
{
    guarded_void(ctx, "cxml::Document::Document", [&](Mem&) { document_construct(ctx, d); });
    return d + doc::header;
}

// cxml::Document::~Document(): Clear() (r3 is its answer).
s32 cxml_Document_dtor(PPUContext& ctx, u32 d)
{
    return guarded(ctx, "cxml::Document::~Document", kFault, [&](Mem&) { return document_clear(ctx, d); });
}

s32 cxml_Document_Clear(PPUContext& ctx, u32 d)
{
    return guarded(ctx, "cxml::Document::Clear", kFault, [&](Mem&) { return document_clear(ctx, d); });
}

void cxml_Document_SetHeaderMagic(PPUContext& ctx, u32 d, u32 magic)
{
    guarded_void(ctx, "cxml::Document::SetHeaderMagic", [&](Mem& m) {
        for (u32 i = 0; i < 4; i++) m.w8(d + doc::magic + i, m.r8(magic + i));
    });
}

s32 cxml_Document_CreateFromBuffer(PPUContext& ctx, u32 d, u32 buffer, u32 size, u32 copy)
{
    return guarded(ctx, "cxml::Document::CreateFromBuffer", kFault,
        [&](Mem&) { return document_create_from_buffer(ctx, d, buffer, size, copy); });
}

s32 cxml_Document_CreateElement(PPUContext& ctx, u32 d, u32 name, s32 count, u32 out)
{
    return guarded(ctx, "cxml::Document::CreateElement", kFault, [&](Mem& m) {
        Element e;
        const s32 r = create_element(ctx, m, d, Text::guest(name), count, e);
        if (r == 0) write_element(m, out, e);
        return r;
    });
}

s32 cxml_Document_RegisterFile(PPUContext& ctx, u32 d, u32 data, u32 size, u32 out)
{
    return guarded(ctx, "cxml::Document::RegisterFile", kFault, [&](Mem& m) {
        File f;
        const s32 r = register_file(ctx, m, d, data, size, f);
        if (r == 0) {
            m.w32(out, f.doc);
            m.w32(out + 8, f.size);
            m.w32(out + 4, f.off);
        }
        return r;
    });
}

s32 cxml_Document_Write(PPUContext& ctx, u32 d, u32 fn, u32 arg)
{
    return guarded(ctx, "cxml::Document::Write", kFault, [&](Mem&) {
        GuestWriter w(fn, arg);
        return document_write(ctx, d, w);
    });
}

// cxml::Element cxml::Document::GetDocumentElement() (the element returned through r3).
u32 cxml_Document_GetDocumentElement(PPUContext& ctx, u32 out, u32 d)
{
    guarded_void(ctx, "cxml::Document::GetDocumentElement",
        [&](Mem& m) { write_element(m, out, document_element(ctx, d)); });
    return out;
}

s32 cxml_Element_AppendChild(PPUContext& ctx, u32 parent, u32 child)
{
    return guarded(ctx, "cxml::Element::AppendChild", kFault, [&](Mem& m) {
        // libsysutil reads the child's offset only (its document is taken to be the parent's).
        const Element p = read_element(m, parent);
        if (!p.doc) return err(CXML_ERROR_NO_DOCUMENT);
        if (m.r32(p.doc + doc::mode)) return err(CXML_ERROR_READ_ONLY);
        return append_child(m, p, m.r32(child + 4));
    });
}

u32 cxml_Element_GetFirstChild(PPUContext& ctx, u32 out, u32 elem)
{
    guarded_void(ctx, "cxml::Element::GetFirstChild",
        [&](Mem& m) { write_element(m, out, first_child(m, read_element(m, elem))); });
    return out;
}

u32 cxml_Element_GetNextSibling(PPUContext& ctx, u32 out, u32 elem)
{
    guarded_void(ctx, "cxml::Element::GetNextSibling",
        [&](Mem& m) { write_element(m, out, next_sibling(m, read_element(m, elem))); });
    return out;
}

s32 cxml_Element_GetAttribute(PPUContext& ctx, u32 elem, u32 name, u32 out)
{
    return guarded(ctx, "cxml::Element::GetAttribute", kFault, [&](Mem& m) {
        Attribute a;
        const s32 r = find_attribute(m, read_element(m, elem), Text::guest(name), a);
        if (r == 0) {
            m.w32(out, a.doc);
            m.w32(out + 8, a.off);
            m.w32(out + 4, a.elem);
        }
        return r;
    });
}

s32 cxml_Attribute_GetInt(PPUContext& ctx, u32 attr, u32 out)
{
    return guarded(ctx, "cxml::Attribute::GetInt", kFault, [&](Mem& m) {
        u32 v1 = 0, v2 = 0;
        const s32 r = attr_get(m, read_attribute(m, attr), 1, v1, v2);
        if (r == 0) m.w32(out, v1);
        return r;
    });
}

s32 cxml_Attribute_GetFile(PPUContext& ctx, u32 attr, u32 out)
{
    return guarded(ctx, "cxml::Attribute::GetFile", kFault, [&](Mem& m) {
        const Attribute a = read_attribute(m, attr);
        u32 v1 = 0, v2 = 0;
        const s32 r = attr_get(m, a, 6, v1, v2);
        if (r == 0) {
            m.w32(out + 8, v2);
            m.w32(out + 4, v1);
            m.w32(out, a.doc);
        }
        return r;
    });
}

// const void* cxml::File::GetAddr(): the file's bytes, when the document holds them whole.
u32 cxml_File_GetAddr(PPUContext& ctx, u32 file)
{
    return static_cast<u32>(guarded(ctx, "cxml::File::GetAddr", 0, [&](Mem& m) -> s32 {
        const u32 d = m.r32(file);
        if (!d) return 0;
        const s32 off = static_cast<s32>(m.r32(file + 4)), size = static_cast<s32>(m.r32(file + 8));
        const u32 mode = m.r32(d + doc::mode);
        const u32 files = m.r32(d + doc::ptr_of(5));
        if ((mode != 0 && mode != 2) || !files) return 0;
        const s32 total = static_cast<s32>(m.r32(d + doc::size_of(5)));
        if (off < total && off + size <= total) return static_cast<s32>(files + static_cast<u32>(off));
        return 0;
    }));
}

// cxmlutil.

u32 cxmlutil_CheckElementName(PPUContext& ctx, u32 elem, u32 name)
{
    return static_cast<u32>(guarded(ctx, "cxmlutil::CheckElementName", 0, [&](Mem& m) -> s32 {
        const u32 n = element_name(m, read_element(m, elem));
        return same(m, n, Text::guest(name)) ? 1 : 0;
    }));
}

u32 cxmlutil_FindChildElement(PPUContext& ctx, u32 out, u32 parent, u32 name, u32 attr, u32 value)
{
    guarded_void(ctx, "cxmlutil::FindChildElement", [&](Mem& m) {
        const Text a = Text::guest(attr), v = Text::guest(value);
        write_element(m, out,
            find_child(m, read_element(m, parent), Text::guest(name), attr ? &a : nullptr, value ? &v : nullptr));
    });
    return out;
}

s32 cxmlutil_GetInt(PPUContext& ctx, u32 elem, u32 name, u32 out)
{
    return guarded(ctx, "cxmlutil::GetInt", kFault, [&](Mem& m) {
        return get_value(m, read_element(m, elem), Text::guest(name), [&](const Attribute& a) {
            u32 v1 = 0, v2 = 0;
            const s32 r = attr_get(m, a, 1, v1, v2);
            if (r == 0) m.w32(out, v1);
            return r;
        });
    });
}

s32 cxmlutil_GetFloat(PPUContext& ctx, u32 elem, u32 name, u32 out)
{
    return guarded(ctx, "cxmlutil::GetFloat", kFault, [&](Mem& m) {
        return get_value(m, read_element(m, elem), Text::guest(name), [&](const Attribute& a) {
            u32 v1 = 0, v2 = 0;
            const s32 r = attr_get(m, a, 2, v1, v2);
            if (r == 0) m.w32(out, v1);
            return r;
        });
    });
}

s32 cxmlutil_GetString(PPUContext& ctx, u32 elem, u32 name, u32 out_str, u32 out_len)
{
    return guarded(ctx, "cxmlutil::GetString", kFault, [&](Mem& m) {
        return get_value(m, read_element(m, elem), Text::guest(name),
            [&](const Attribute& a) { return attr_get_string(m, a, out_str, out_len); });
    });
}

s32 cxmlutil_GetFile(PPUContext& ctx, u32 elem, u32 name, u32 out)
{
    return guarded(ctx, "cxmlutil::GetFile", kFault, [&](Mem& m) {
        return get_value(m, read_element(m, elem), Text::guest(name), [&](const Attribute& a) {
            u32 v1 = 0, v2 = 0;
            const s32 r = attr_get(m, a, 6, v1, v2);
            if (r == 0) {
                m.w32(out + 8, v2);
                m.w32(out + 4, v1);
                m.w32(out, a.doc);
            }
            return r;
        });
    });
}

s32 cxmlutil_SetInt(PPUContext& ctx, u32 elem, u32 name, s32 value)
{
    return guarded(ctx, "cxmlutil::SetInt", kFault,
        [&](Mem& m) { return set_int(ctx, m, read_element(m, elem), Text::guest(name), value); });
}

s32 cxmlutil_SetFloat(PPUContext& ctx, u32 elem, u32 name, float value)
{
    return guarded(ctx, "cxmlutil::SetFloat", kFault,
        [&](Mem& m) { return set_float(ctx, m, read_element(m, elem), Text::guest(name), value); });
}

s32 cxmlutil_SetString(PPUContext& ctx, u32 elem, u32 name, u32 value)
{
    return guarded(ctx, "cxmlutil::SetString", kFault,
        [&](Mem& m) { return set_string(ctx, m, read_element(m, elem), Text::guest(name), Text::guest(value)); });
}

s32 cxmlutil_SetFile(PPUContext& ctx, u32 elem, u32 name, u32 file)
{
    return guarded(ctx, "cxmlutil::SetFile", kFault, [&](Mem& m) {
        // libsysutil reads the file's offset and size when it stores them, after the name.
        const Element e = read_element(m, elem);
        Attribute a;
        if (const s32 r = settable_attribute(ctx, m, e, Text::guest(name), a); r < 0) return r;
        return only_errors(attr_set_file(m, a, [&] { return File{0, m.r32(file + 4), m.r32(file + 8)}; }));
    });
}

// sysutil_cxmlutil.

u32 FixedMemory_Begin(PPUContext& ctx, s32 n)
{
    return static_cast<u32>(guarded(ctx, "sysutil_cxmlutil::FixedMemory::Begin", 0,
        [&](Mem&) { return static_cast<s32>(fixed_memory_begin(ctx, n)); }));
}

// r3 is left as libsysutil leaves it: the offset of the instance's in-use byte in its array.
u32 FixedMemory_End(PPUContext& ctx, s32 n)
{
    guarded_void(ctx, "sysutil_cxmlutil::FixedMemory::End", [&](Mem&) { fixed_memory_end(ctx, n); });
    return kFixedInUse + glob::kFixedSize * static_cast<u32>(n);
}

void FixedMemory_Allocate(PPUContext& ctx, u32 type, u32 self, u32, u32 size, u32 out, u32 out_size)
{
    guarded_void(ctx, "sysutil_cxmlutil::FixedMemory::Allocate",
        [&](Mem& m) { fixed_allocate(m, type, self, size, out, out_size); });
}

// PacketWriter::PacketWriter(int channel, int id, cxml::Document&): the channel, the ID, the document's size (r3 is left
// holding the size, as libsysutil leaves it).
u32 PacketWriter_ctor(PPUContext& ctx, u32 self, s32 channel, s32 id, u32 d)
{
    return static_cast<u32>(guarded(ctx, "sysutil_cxmlutil::PacketWriter::PacketWriter", 0, [&](Mem& m) {
        m.w32(self, static_cast<u32>(channel));
        m.w32(self + 4, static_cast<u32>(id));
        const u32 size = document_size(ctx, d);
        m.w32(self + 8, size);
        return static_cast<s32>(size);
    }));
}

s32 PacketWriter_Write(PPUContext& ctx, u32 data, u32 size, u32 self)
{
    return guarded(ctx, "sysutil_cxmlutil::PacketWriter::Write", kFault,
        [&](Mem& m) { return writer_object_write(ctx, m, data, size, self); });
}

} // namespace

// ------------------------------------------------------------------------------------------------- shared API

void document_construct(PPUContext& ctx, u32 d)
{
    Mem m(ctx);
    m.w32(d + doc::mode, 0);
    reset_header(m, d);
    m.w32(d + doc::alloc_arg, 0);
    for (u32 o = doc::tree; o <= doc::alloc; o += 4) m.w32(d + o, 0);
}

void document_use_fixed_memory(PPUContext& ctx, u32 d, u32 memory)
{
    Mem m(ctx);
    m.w32(d + doc::alloc, fixed_memory_allocator());
    m.w32(d + doc::alloc_arg, memory);
}

s32 document_clear(PPUContext& ctx, u32 d)
{
    Mem m(ctx);
    reset_header(m, d);
    if (m.r32(d + doc::mode) != 2) {
        if (!m.r32(d + doc::alloc)) return err(CXML_ERROR_NO_ALLOCATOR);
        for (u32 n = 0; n < 6; n++) {
            if (const u32 p = m.r32(d + doc::ptr_of(n))) call_allocator(ctx, m, d, 6 + n, p, 0, 0, 0);
            m.w32(d + doc::ptr_of(n), 0);
            m.w32(d + doc::cap_of(n), 0);
        }
    }
    m.w32(d + doc::mode, 0);
    return 0;
}

void document_set_magic(PPUContext& ctx, u32 d, std::string_view magic)
{
    Mem m(ctx);
    for (u32 i = 0; i < 4; i++) m.w8(d + doc::magic + i, i < magic.size() ? static_cast<u8>(magic[i]) : 0);
}

s32 document_create_element(PPUContext& ctx, u32 d, std::string_view name, s32 attributes, Element& out)
{
    Mem m(ctx);
    return create_element(ctx, m, d, Text::of(name), attributes, out);
}

s32 document_register_file(PPUContext& ctx, u32 d, u32 data, u32 size, File& out)
{
    Mem m(ctx);
    return register_file(ctx, m, d, data, size, out);
}

s32 document_create_from_buffer(PPUContext& ctx, u32 d, u32 buffer, u32 size, u32 copy)
{
    Mem m(ctx);
    document_clear(ctx, d);
    m.w32(d + doc::mode, (copy & 0xFF) ? 0 : 2);
    if (size <= 0x37) return err(CXML_ERROR_SHORT);
    m.copy(d + doc::header, buffer, doc::kHeaderSize);
    const u32 version = m.r32(d + doc::version);
    if (version != 0x110 && version != 0x100) return err(CXML_ERROR_VERSION);
    if (m.r32(d + doc::size_of(3)) & 3) return err(CXML_ERROR_BAD_TABLE);
    if (m.r32(d + doc::size_of(4)) & 3) return err(CXML_ERROR_BAD_TABLE);
    if (copy && !m.r32(d + doc::alloc)) return err(CXML_ERROR_NO_ALLOCATOR);
    for (u32 n = 0; n < 6; n++) {
        const u32 table_size = m.r32(d + doc::size_of(n));
        // The first four tables are taken when their size is positive, the float and file tables when it isn't 0.
        if (n < 4 ? static_cast<s32>(table_size) <= 0 : table_size == 0) continue;
        const u32 off = m.r32(d + doc::offset_of(n));
        if (static_cast<s32>(off) >= static_cast<s32>(size)) return err(CXML_ERROR_PAST_END);
        if (static_cast<s32>(off + table_size) > static_cast<s32>(size)) return err(CXML_ERROR_SHORT);
        if (!copy) {
            m.w32(d + doc::ptr_of(n), buffer + off);
        } else {
            StackVar outv(ctx, m.base(), 8);
            call_allocator(ctx, m, d, n, 0, table_size, outv.addr(), d + doc::cap_of(n));
            const u32 p = m.r32(outv.addr());
            if (!p) return err(CXML_ERROR_NO_MEMORY);
            m.w32(d + doc::ptr_of(n), p);
            m.copy(p, buffer + m.r32(d + doc::offset_of(n)), m.r32(d + doc::size_of(n)));
        }
        if (n == 2 && m.r8(m.r32(d + doc::ptr_of(2)) + m.r32(d + doc::size_of(2)) - 1))
            return err(CXML_ERROR_BAD_TABLE);
    }
    return 0;
}

s32 document_write(PPUContext& ctx, u32 d, DocWriter& writer)
{
    Mem m(ctx);
    const u32 mode = m.r32(d + doc::mode);
    if (mode != 0 && mode != 2) return err(CXML_ERROR_READ_ONLY);
    StackVar zeros(ctx, m.base(), 16);
    // The header's offsets laid out: the tree right after the padded header, each table 16-byte aligned.
    m.w32(d + doc::version, 0x110);
    u32 at = align16(doc::kHeaderSize);
    for (u32 n = 0; n < 6; n++) {
        m.w32(d + doc::offset_of(n), at);
        at += align16(m.r32(d + doc::size_of(n)));
    }
    s32 r = writer.write(ctx, d + doc::header, doc::kHeaderSize);
    if (r != static_cast<s32>(doc::kHeaderSize)) return r;
    const s32 pad = pad16(static_cast<s32>(doc::kHeaderSize));
    r = writer.write(ctx, zeros.addr(), static_cast<u32>(pad));
    if (r != pad) return r;
    for (u32 n = 0; n < 6; n++) {
        // Each size is read again after the writer's call, as libsysutil reads it.
        r = writer.write(ctx, m.r32(d + doc::ptr_of(n)), m.r32(d + doc::size_of(n)));
        if (r != static_cast<s32>(m.r32(d + doc::size_of(n)))) return r;
        r = writer.write(ctx, zeros.addr(), static_cast<u32>(pad16(r)));
        if (r != pad16(static_cast<s32>(m.r32(d + doc::size_of(n))))) return r;
    }
    return 0;
}

Element document_element(PPUContext& ctx, u32 d)
{
    Mem m(ctx);
    if (element_valid(m, d, 0)) return {d, 0};
    return {0, 0xFFFFFFFF};
}

u32 document_size(PPUContext& ctx, u32 d)
{
    Mem m(ctx);
    u32 size = align16(doc::kHeaderSize);
    for (u32 n = 0; n < 6; n++) size += align16(m.r32(d + doc::size_of(n)));
    return size;
}

s32 element_append_child(PPUContext& ctx, const Element& parent, const Element& child)
{
    Mem m(ctx);
    return append_child(m, parent, child.off);
}

s32 util_set_int(PPUContext& ctx, const Element& e, std::string_view name, s32 value)
{
    Mem m(ctx);
    return set_int(ctx, m, e, Text::of(name), value);
}

s32 util_set_float(PPUContext& ctx, const Element& e, std::string_view name, float value)
{
    Mem m(ctx);
    return set_float(ctx, m, e, Text::of(name), value);
}

s32 util_set_string(PPUContext& ctx, const Element& e, std::string_view name, u32 value)
{
    Mem m(ctx);
    return set_string(ctx, m, e, Text::of(name), Text::guest(value));
}

s32 util_set_string(PPUContext& ctx, const Element& e, std::string_view name, std::string_view value)
{
    Mem m(ctx);
    return set_string(ctx, m, e, Text::of(name), Text::of(value));
}

s32 util_set_file(PPUContext& ctx, const Element& e, std::string_view name, const File& file)
{
    Mem m(ctx);
    Attribute a;
    if (const s32 r = settable_attribute(ctx, m, e, Text::of(name), a); r < 0) return r;
    return only_errors(attr_set_file(m, a, [&] { return file; }));
}

s32 util_get_int(PPUContext& ctx, const Element& e, std::string_view name, s32& out)
{
    Mem m(ctx);
    return get_value(m, e, Text::of(name), [&](const Attribute& a) {
        u32 v1 = 0, v2 = 0;
        const s32 r = attr_get(m, a, 1, v1, v2);
        if (r == 0) out = static_cast<s32>(v1);
        return r;
    });
}

s32 util_get_string(PPUContext& ctx, const Element& e, std::string_view name, u32 out_str, u32 out_len)
{
    Mem m(ctx);
    return get_value(m, e, Text::of(name), [&](const Attribute& a) { return attr_get_string(m, a, out_str, out_len); });
}

s32 util_get_file(PPUContext& ctx, const Element& e, std::string_view name, File& out)
{
    Mem m(ctx);
    return get_value(m, e, Text::of(name), [&](const Attribute& a) {
        u32 v1 = 0, v2 = 0;
        const s32 r = attr_get(m, a, 6, v1, v2);
        if (r == 0) out = {a.doc, v1, v2};
        return r;
    });
}

u32 file_address(PPUContext& ctx, const File& f)
{
    Mem m(ctx);
    if (!f.doc) return 0;
    const u32 mode = m.r32(f.doc + doc::mode);
    const u32 files = m.r32(f.doc + doc::ptr_of(5));
    if ((mode != 0 && mode != 2) || !files) return 0;
    const s32 total = static_cast<s32>(m.r32(f.doc + doc::size_of(5)));
    const s32 off = static_cast<s32>(f.off), size = static_cast<s32>(f.size);
    if (off < total && off + size <= total) return files + f.off;
    return 0;
}

Element util_find_child(PPUContext& ctx, const Element& parent, std::string_view name)
{
    Mem m(ctx);
    return find_child(m, parent, Text::of(name), nullptr, nullptr);
}

u32 fixed_memory_begin(PPUContext& ctx, s32 n)
{
    if (n != 0 && n != 1) {
        log_call_site(ctx, guest_base(ctx),
            std::format("sysutil_cxmlutil::FixedMemory::Begin({}): libsysutil has two instances, 0 and 1; past them it "
                        "reads its other data; answered 0 (none free)",
                n));
        return 0;
    }
    Mem m(ctx);
    const u32 self = globals(ctx) + glob::fixed + glob::kFixedSize * static_cast<u32>(n);
    if (m.r8(self + kFixedInUse)) return 0;
    m.w8(self + kFixedInUse, 1);
    return self;
}

void fixed_memory_end(PPUContext& ctx, s32 n)
{
    if (n != 0 && n != 1) {
        log_call_site(ctx, guest_base(ctx),
            std::format("sysutil_cxmlutil::FixedMemory::End({}): libsysutil has two instances, 0 and 1", n));
        return;
    }
    Mem m(ctx);
    const u32 self = globals(ctx) + glob::fixed + glob::kFixedSize * static_cast<u32>(n);
    if (m.r8(self + kFixedInUse)) m.w8(self + kFixedInUse, 0);
}

PacketWriter::PacketWriter(PPUContext& ctx, s32 channel, s32 id, u32 doc)
    : channel_(channel), id_(id), left_(document_size(ctx, doc))
{
}

s32 PacketWriter::write(PPUContext& ctx, u32 data, u32 size)
{
    if (!left_) return 0;
    const s32 r = packet_write(ctx, static_cast<u32>(channel_), id_, data, size, left_ == size ? 1 : 0);
    if (r != 0) return r;
    left_ -= size;
    return static_cast<s32>(size);
}

void register_cxml(Registry& r)
{
#define CXML_FUNC(nid, name, fn) ::pspocket::hle::Function{{}, name, nid, &::pspocket::hle::detail::thunk<&fn>, false}
    r.add("cellSysutil", {
        CXML_FUNC(0xB47470E1, "_ZN16sysutil_cxmlutil11FixedMemory5BeginEi", FixedMemory_Begin),
        CXML_FUNC(0x40719C8C, "_ZN16sysutil_cxmlutil11FixedMemory3EndEi", FixedMemory_End),
        CXML_FUNC(kNidFixedAllocate, kFixedAllocateName, FixedMemory_Allocate),
        CXML_FUNC(0x20957CD4, "_ZN16sysutil_cxmlutil12PacketWriterC1EiiRN4cxml8DocumentE", PacketWriter_ctor),
        CXML_FUNC(kNidPacketWriterWrite, "_ZN16sysutil_cxmlutil12PacketWriter5WriteEPKvjPv", PacketWriter_Write),
        CXML_FUNC(0xCFAD36DE, "_ZN4cxml8DocumentC1Ev", cxml_Document_ctor),
        CXML_FUNC(0x5EC145E2, "_ZN4cxml8DocumentD1Ev", cxml_Document_dtor),
        CXML_FUNC(0x7F4E4139, "_ZN4cxml8Document5ClearEv", cxml_Document_Clear),
        CXML_FUNC(0x2CD62587, "_ZN4cxml8Document14SetHeaderMagicEPKc", cxml_Document_SetHeaderMagic),
        CXML_FUNC(0x6EEE1B61, "_ZN4cxml8Document16CreateFromBufferEPKvjb", cxml_Document_CreateFromBuffer),
        CXML_FUNC(0x6BA1D72F, "_ZN4cxml8Document13CreateElementEPKciPNS_7ElementE", cxml_Document_CreateElement),
        CXML_FUNC(0xD3CDD694, "_ZN4cxml8Document12RegisterFileEPKvjPNS_4FileE", cxml_Document_RegisterFile),
        CXML_FUNC(0x75AA7373, "_ZN4cxml8Document5WriteEPFiPKvjPvES3_", cxml_Document_Write),
        CXML_FUNC(0x8AD11D24, "_ZN4cxml8Document18GetDocumentElementEv", cxml_Document_GetDocumentElement),
        CXML_FUNC(0x58B963E5, "_ZN4cxml7Element11AppendChildERS0_", cxml_Element_AppendChild),
        CXML_FUNC(0x54CC1C3F, "_ZNK4cxml7Element13GetFirstChildEv", cxml_Element_GetFirstChild),
        CXML_FUNC(0xE7D9F074, "_ZNK4cxml7Element14GetNextSiblingEv", cxml_Element_GetNextSibling),
        CXML_FUNC(0x1C5F3492, "_ZNK4cxml7Element12GetAttributeEPKcPNS_9AttributeE", cxml_Element_GetAttribute),
        CXML_FUNC(0xD526FD70, "_ZNK4cxml9Attribute6GetIntEPi", cxml_Attribute_GetInt),
        CXML_FUNC(0x4D89A149, "_ZNK4cxml9Attribute7GetFileEPNS_4FileE", cxml_Attribute_GetFile),
        CXML_FUNC(0x79EFF338, "_ZNK4cxml4File7GetAddrEv", cxml_File_GetAddr),
        CXML_FUNC(0x4986187C, "_ZN8cxmlutil16CheckElementNameERKN4cxml7ElementEPKc", cxmlutil_CheckElementName),
        CXML_FUNC(0x6AF9FD89, "_ZN8cxmlutil16FindChildElementERKN4cxml7ElementEPKcS5_S5_", cxmlutil_FindChildElement),
        CXML_FUNC(0x134034CE, "_ZN8cxmlutil6GetIntERKN4cxml7ElementEPKcPi", cxmlutil_GetInt),
        CXML_FUNC(0xE77FAFB5, "_ZN8cxmlutil6SetIntERKN4cxml7ElementEPKci", cxmlutil_SetInt),
        CXML_FUNC(0xA971E5A3, "_ZN8cxmlutil8GetFloatERKN4cxml7ElementEPKcPf", cxmlutil_GetFloat),
        CXML_FUNC(0x933B103D, "_ZN8cxmlutil8SetFloatERKN4cxml7ElementEPKcf", cxmlutil_SetFloat),
        CXML_FUNC(0x0633EDC2, "_ZN8cxmlutil9GetStringERKN4cxml7ElementEPKcPS5_Pj", cxmlutil_GetString),
        CXML_FUNC(0xFB1E70A0, "_ZN8cxmlutil9SetStringERKN4cxml7ElementEPKcS5_", cxmlutil_SetString),
        CXML_FUNC(0x505FA917, "_ZN8cxmlutil7GetFileERKN4cxml7ElementEPKcPNS0_4FileE", cxmlutil_GetFile),
        CXML_FUNC(0x35F7ED00, "_ZN8cxmlutil7SetFileERKN4cxml7ElementEPKcRKNS0_4FileE", cxmlutil_SetFile),
    });
#undef CXML_FUNC
}

} // namespace pspocket::hle::sysutil
