#pragma once

// libsysutil's own C++ libraries and its channels to the system software (D-534), shared by cellSysutilCxml.cpp,
// cellSysutilPacket.cpp, cellSysutilWebBrowser.cpp and cellSysutilSystem.cpp:
//  - cxml: the binary XML ("CXML") documents libsysutil builds its requests in and reads the replies from. A document
//    is a 0x74-byte object in guest memory (the game's, or the stack): a mode word, the 0x38-byte header that is
//    written out first (magic, version 0x110, then the offset and size of six tables), each table's pointer and
//    capacity, and the allocator (a function descriptor and its argument) the tables come from. The tables: the element
//    tree (an element is 0x1C bytes and 16 per attribute), the ID table, the string table, int arrays, float arrays
//    and files. Elements ({document, offset}), attributes ({document, element, offset}) and files ({document, offset,
//    size}) are small value objects the game keeps where it likes; pspocket's own requests keep them on the host.
//  - sysutil_cxmlutil::FixedMemory: libsysutil's allocator for those documents, two instances of six 4 KB blocks (one
//    per table) in its data; sysutil_cxmlutil::PacketWriter: Document::Write's writer into a packet.
//  - packets: two channels to the system software in a 64 KB shared memory of lv2's, each a ring with an lv2 mutex and
//    four condition variables, one side (ID 0) the system's, the other a request's (its ID).
// Everything the game can see is laid out where libsysutil lays it, so its documents and blocks are byte for byte
// Sony's; libsysutil's own globals are a block of pspocket's with the same layout as libsysutil's data from 0x1F898.

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/types.hpp"
#include "pspocket/ppu/context.h"

namespace pspocket::hle {
class Registry;
}

namespace pspocket::hle::sysutil {

// libcxml's error codes (Sony's names are unknown; these say what each means in its code).
enum : u32 {
    CXML_ERROR_SHORT = 0x8002e002,          // the buffer ends before the header or a table
    CXML_ERROR_PAST_END = 0x8002e004,       // a table starts past the buffer
    CXML_ERROR_NO_MEMORY = 0x8002e005,      // the allocator gave nothing
    CXML_ERROR_BAD_TABLE = 0x8002e006,      // a misaligned array table, an unterminated string
    CXML_ERROR_VERSION = 0x8002e007,        // neither 0x110 nor 0x100
    CXML_ERROR_NO_DOCUMENT = 0x8002e008,    // an element or attribute of no document, an index past the attributes
    CXML_ERROR_READ_ONLY = 0x8002e010,      // a document made over a buffer it does not own
    CXML_ERROR_NO_ATTRIBUTE = 0x8002e011,   // no attribute of that name, or no free attribute slot
    CXML_ERROR_TYPE = 0x8002e012,           // the attribute holds another type
    CXML_ERROR_NOT_APPENDABLE = 0x8002e013, // the root element, or an element that has a parent already
    CXML_ERROR_NO_ALLOCATOR = 0x8002e014,
};

// libsysutil's packet errors.
enum : u32 {
    PACKET_ERROR_NOT_OPEN = 0x8002b001,  // the channel's packet belongs to another ID
    PACKET_ERROR_CHANNEL = 0x8002b002,   // a channel other than 0 and 1
    PACKET_ERROR_NO_PARAM = 0x8002b003,  // a reply without its "param" element
    PACKET_ERROR_TRUNCATED = 0x8002b006, // a reply that doesn't fit the buffer
    PACKET_ERROR_CLOSED = 0x8002b007,    // the channel is closed
};

// The document object's fields (byte offsets).
namespace doc {
enum : u32 {
    mode = 0x00, // 0: owns its tables, 2: made over a buffer it does not own
    header = 0x04,
    magic = 0x04,
    version = 0x08,
    tree_off = 0x0C, // then the five other tables' offset and size, 8 bytes apart
    tree_size = 0x10,
    tree = 0x3C, // then the five other tables' pointer and capacity
    tree_cap = 0x40,
    alloc = 0x6C, // the allocator's function descriptor
    alloc_arg = 0x70,
    kSize = 0x74,
    kHeaderSize = 0x38,
};
// Table n's offset, size, pointer and capacity (0 tree, 1 IDs, 2 strings, 3 ints, 4 floats, 5 files).
constexpr u32 offset_of(u32 n) { return tree_off + 8 * n; }
constexpr u32 size_of(u32 n) { return tree_size + 8 * n; }
constexpr u32 ptr_of(u32 n) { return tree + 8 * n; }
constexpr u32 cap_of(u32 n) { return tree_cap + 8 * n; }
} // namespace doc

// libsysutil's globals, from its data at 0x1F898 (byte offsets in pspocket's block).
namespace glob {
enum : u32 {
    shm = 0x00,     // the channels' shared memory, as mapped
    channel = 0x00, // channel n's record at 24 * n: mutex at +4, conditions at +8..+20, the packet's ID at +24
    area_64k = 0x44, // the shared memory area of 64 KB pages and the count of shared memories in it
    area_1m = 0x4C,  // the same for 1 MB pages
    savedata_state = 0xD4,  // libsysutil's save data state (0x1F96C)
    savedata_memory = 0xD8, // the FixedMemory its save data's packet took (0x1F970)
    audio_out_cb = 0xE0, // cellAudioOutRegisterCallback's function and userdata (0x1F978)
    video_out_cb = 0xE8, // cellVideoOutRegisterCallback's (0x1F980)
    // The web browser's (0x1FB50): the memory container, eleven hooks {callback, userdata} (1 unknown MIME type, 2
    // error, 3 status error, 4 notify, 5 request, 10 the usrdata on game exit), the system callback and its userdata,
    // whether a browser was made, whether the config's rect was set, whether the busy icon was asked for.
    web_container = 0x2B8,
    web_hooks = 0x2BC,
    web_system_cb = 0x314,
    web_system_ud = 0x318,
    web_created = 0x31C,
    web_rect = 0x31D,
    web_busy_icon = 0x31E,
    fixed = 0x328, // FixedMemory instance n at fixed + kFixedSize * n
    kFixedSize = 0x6010,
    kSize = fixed + 2 * kFixedSize,
    // libsysutil's initialised data before 0x1F898, kept past the rest: the title ID for local contents
    // (cellWebBrowserSetLocalContentsAdditionalTitleID's pointer, at first an empty string's), the empty string.
    hinban = kSize,
    empty = kSize + 4,
    kBlockSize = kSize + 8,
};
// Channel n's lv2 objects: the mutex, the conditions (a packet's data, room for one, the system's side free, the
// request side free) and the ID of the packet being sent.
constexpr u32 mutex(u32 n) { return 24 * n + 4; }
constexpr u32 cond_data(u32 n) { return 24 * n + 8; }
constexpr u32 cond_room(u32 n) { return 24 * n + 12; }
constexpr u32 cond_system(u32 n) { return 24 * n + 16; }
constexpr u32 cond_request(u32 n) { return 24 * n + 20; }
constexpr u32 packet_id(u32 n) { return 24 * n + 24; }
} // namespace glob

// A channel's header in the shared memory (32 bytes at 0x8000 * channel): the ring's offset and size, where the next
// write and read go, the bytes in it, the packet's state (0 none, 1 being written, 2 complete), its direction (1 to
// the system, 2 from it), whether each side has a packet open, and whether the channel is closed.
namespace ring {
enum : u32 {
    data = 0x00,
    size = 0x04,
    write = 0x08,
    read = 0x0C,
    used = 0x10,
    state = 0x14,
    direction = 0x18,
    system_busy = 0x1C,
    request_busy = 0x1D,
    closed = 0x1E,
    kChannel = 0x8000,
};
} // namespace ring

// Memory a library function touches that is not there: Sony's code faults there; pspocket's ends the call, logs the
// call site and answers an error (each exported function says which).
struct Fault {
    u32 addr;
};

// Guest memory with the checks of guest_readable, throwing Fault.
class Mem {
public:
    explicit Mem(PPUContext& ctx) : t_(thread_of(ctx)), base_(t_.kernel.base()) {}

    u8* base() const { return base_; }
    u8 r8(u32 a)
    {
        check(a);
        return ps_read8(base_, a);
    }
    u32 r32(u32 a)
    {
        range(a, 4);
        return ps_read32(base_, a);
    }
    void w8(u32 a, u8 v)
    {
        check(a);
        ps_write8(base_, a, v);
    }
    void w32(u32 a, u32 v)
    {
        range(a, 4);
        ps_write32(base_, a, v);
    }
    // memcpy and memset as liblv2's (overlap is left to the caller, as there).
    void copy(u32 dst, u32 src, u32 n)
    {
        if (!n) return;
        range(src, n);
        range(dst, n);
        std::memmove(base_ + dst, base_ + src, n);
    }
    void put(u32 dst, const void* src, u32 n)
    {
        if (!n) return;
        range(dst, n);
        std::memcpy(base_ + dst, src, n);
    }
    void fill(u32 dst, u8 v, u32 n)
    {
        if (!n) return;
        range(dst, n);
        std::memset(base_ + dst, v, n);
    }
    // A NUL-terminated string (faulting where memory ends before its NUL).
    std::string cstr(u32 a)
    {
        std::string s;
        for (;; a++) {
            const u8 c = r8(a);
            if (!c) return s;
            s += static_cast<char>(c);
        }
    }
    u32 strlen(u32 a)
    {
        u32 n = 0;
        while (r8(a + n)) n++;
        return n;
    }

private:
    void check(u32 a)
    {
        if ((a >> 12) == page_) return;
        if (!t_.kernel.memory().is_committed(a)) throw Fault{a};
        page_ = a >> 12;
    }
    void range(u32 a, u32 n)
    {
        if (u64{a} + n > GuestMemory::kGuestSize) throw Fault{a};
        for (u64 p = a & ~u64{0xFFF}; p < u64{a} + n; p += 0x1000) check(static_cast<u32>(std::max<u64>(p, a)));
    }

    lv2::PpuThread& t_;
    u8* base_;
    u32 page_ = 0xFFFFFFFF;
};

// The value objects of cxml.
struct Element {
    u32 doc = 0;
    u32 off = 0xFFFFFFFF;
};
struct Attribute {
    u32 doc = 0;
    u32 elem = 0xFFFFFFFF;
    u32 off = 0xFFFFFFFF; // the attribute's offset in the tree
};
struct File {
    u32 doc = 0;
    u32 off = 0;
    u32 size = 0;
};

// What Document::Write gives its data to: (data, size) -> the bytes taken, or an error (the writer's own).
class DocWriter {
public:
    virtual s32 write(PPUContext& ctx, u32 data, u32 size) = 0;

protected:
    ~DocWriter() = default;
};

// ---- cxml (cellSysutilCxml.cpp), for pspocket's own requests: names and values are host strings (the exported
// functions read the game's). Each may throw Fault.

void document_construct(PPUContext& ctx, u32 doc);
// The allocator libsysutil's requests set in a document they build: FixedMemory::Allocate on the instance `memory`.
void document_use_fixed_memory(PPUContext& ctx, u32 doc, u32 memory);
s32 document_clear(PPUContext& ctx, u32 doc); // also ~Document()
void document_set_magic(PPUContext& ctx, u32 doc, std::string_view magic);
s32 document_create_element(PPUContext& ctx, u32 doc, std::string_view name, s32 attributes, Element& out);
s32 document_register_file(PPUContext& ctx, u32 doc, u32 data, u32 size, File& out);
// `copy`: its low byte chooses the mode (0: read-only over the buffer), the whole word whether tables are copied.
s32 document_create_from_buffer(PPUContext& ctx, u32 doc, u32 buffer, u32 size, u32 copy);
s32 document_write(PPUContext& ctx, u32 doc, DocWriter& writer);
Element document_element(PPUContext& ctx, u32 doc);
// The bytes Document::Write writes (PacketWriter's packet size).
u32 document_size(PPUContext& ctx, u32 doc);
s32 element_append_child(PPUContext& ctx, const Element& parent, const Element& child);

// cxmlutil, on an element.
s32 util_set_int(PPUContext& ctx, const Element& e, std::string_view name, s32 value);
s32 util_set_float(PPUContext& ctx, const Element& e, std::string_view name, float value);
// SetString with the game's string (its address) or one of pspocket's.
s32 util_set_string(PPUContext& ctx, const Element& e, std::string_view name, u32 value);
s32 util_set_string(PPUContext& ctx, const Element& e, std::string_view name, std::string_view value);
s32 util_set_file(PPUContext& ctx, const Element& e, std::string_view name, const File& file);
s32 util_get_int(PPUContext& ctx, const Element& e, std::string_view name, s32& out);
// GetString: the string's address to the guest word `out_str`, its length to `out_len`.
s32 util_get_string(PPUContext& ctx, const Element& e, std::string_view name, u32 out_str, u32 out_len);
s32 util_get_file(PPUContext& ctx, const Element& e, std::string_view name, File& out);
// File::GetAddr: the file's bytes when its document holds them whole, else 0.
u32 file_address(PPUContext& ctx, const File& f);
// The first child named `name` (and, when `attr` is given, whose ID attribute `attr` is `value`); {0, -1} for none.
Element util_find_child(PPUContext& ctx, const Element& parent, std::string_view name);

// ---- sysutil_cxmlutil (cellSysutilCxml.cpp).

// FixedMemory instance `n` (libsysutil has two): its address when it was free (now taken), else 0.
u32 fixed_memory_begin(PPUContext& ctx, s32 n);
void fixed_memory_end(PPUContext& ctx, s32 n);

// Document::Write into a packet: libsysutil's PacketWriter, kept on the host.
class PacketWriter final : public DocWriter {
public:
    PacketWriter(PPUContext& ctx, s32 channel, s32 id, u32 doc);
    s32 write(PPUContext& ctx, u32 data, u32 size) override;

private:
    s32 channel_, id_;
    u32 left_;
};

// ---- packets (cellSysutilPacket.cpp).

// libsysutil's globals block (made at first use; zeroed, then laid out as libsysutil's start leaves its data).
u32 globals(PPUContext& ctx);
// libsysutil's start for its channels as pspocket's console has them (D-534): the shared memory and each channel's
// lv2 objects made as the system software makes them, no rings laid out in it (there is no system software to lay them
// out), and both channels closed as libsysutil closes them when it stops. Done once per process, before the first
// function that needs it; tests call it where they give Sony's module the same set-up. Returns libsysutil's answer.
s32 packets_start(PPUContext& ctx);

s32 packet_begin(PPUContext& ctx, u32 channel, s32 id);
s32 packet_end(PPUContext& ctx, u32 channel, s32 id);
s32 packet_read(PPUContext& ctx, u32 channel, s32 id, u32 buffer, u32 size, u32 done);
s32 packet_write(PPUContext& ctx, u32 channel, s32 id, u32 data, u64 size, u8 last);

// The handover of a request to the system software failed with `result` (the channel is closed: pspocket has no system
// software): logged once per kind of request, with the call site.
void log_handover(PPUContext& ctx, std::string_view what, s32 result);

// libsysutil's diagnostics, _sys_printf through liblv2 (its text and arguments: integers or guest addresses).
void sysutil_printf(PPUContext& ctx, std::string_view fmt, std::initializer_list<u64> args);
// libsysutil's "ERROR: %s : %s - %d\n" with two host strings and a line.
void sysutil_error(PPUContext& ctx, std::string_view what, std::string_view file, s32 line);

// Registration of the files' functions (called by register_cellSysutil).
void register_cxml(Registry& r);
void register_packets(Registry& r);
void register_web_browser(Registry& r);
void register_system(Registry& r);

} // namespace pspocket::hle::sysutil
