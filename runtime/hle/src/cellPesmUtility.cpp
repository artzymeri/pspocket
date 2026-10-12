// cellPesmUtility: the front end of the system software's PSP movie recorder (D-926). Read from libsysutil_pesm.sprx
// (4.92) and compared with it call for call (tests/hle/test_sysutil_more.cpp). RPCS3 leaves its fifteen functions
// stubs (cellPesmUtility.cpp) and does not know its other two.
//
// The module keeps one object, made at the first call (operator new): the request in flight, a state (0 at first, 1
// while initializing, 2 initialized, 3 in a call, 4 while finalizing, 5 finalized) and a list of the shared memories it
// opened, each {side 0x13D, key, address}. Every function but the version and the two loads is a request to the system
// software on libsysutil's channel 0, packet 0x13D, magic "PESM", made the module's way. A request object (operator new)
// begins the packet (its answer not looked at), takes FixedMemory 0, builds an element named after the command with
// the function's fields (the module's table: ints and strings) and writes it (its answer not looked at either). Then
// the reply of the request in flight is read into FixedMemory; a failed read leaves the request's own document, so on
// pspocket's console, whose channels are closed (D-534), each function reads its own request back as the reply. When
// the root is named after the command, the reply's fields are read over the values the function put there (a field
// missing keeps its value), the request is deleted and "PesmResult" answered; a root of another name answers
// 0x80861001 and keeps the request, whose reply the next call reads instead of sending its own.
//
// Initialize opens the system's shared memory for the recorder (key 0x8086000000000200, 2 MB of 64 KB pages, through
// libsysutil's cellSysutilSharedMemoryAlloc), into which PrepareRec and the encryptions copy the game's data and from
// which StartMovieRec and GetSinf answer; Finalize gives it back. The system software makes it: libsysutil waits for it,
// trying again every 100 ms, so Sony's module would wait for ever on pspocket's console, which has no system software.
// pspocket's opens it only when lv2 has it (a test's system side makes it, Kernel::create_system_shared_memory) and
// otherwise goes on as the module goes on when the opening fails, logged: the calls that need it answer 0x80862002.

#include <mutex>
#include <span>
#include <string_view>

#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/kernel.hpp"
#include "pspocket/recomp/nid.hpp"
#include "sysutil_sub.hpp"

namespace pspocket::hle::sysutil::sub {

namespace {

constexpr std::string_view kModule = "libsysutil_pesm";
constexpr s32 kPacket = 0x13D;
constexpr u64 kMemoryKey = 0x8086000000000200ull; // the recorder's shared memory (its node's key: the same 64 bits)
constexpr u32 kMemorySize = 0x200000;
constexpr u32 kPage64K = 0x200;

constexpr u32 PESM_ERROR_NOT_ANSWERED = 0x80861001; // the reply's root is not the command's: the request kept
constexpr u32 PESM_ERROR_TOO_LARGE = 0x80861002;    // a sample past 1 MB
constexpr u32 PESM_ERROR_STATE = 0x80862001;        // not initialized, in a call, or finalized
constexpr u32 PESM_ERROR_NO_MEMORY = 0x80862002;    // the system's shared memory not opened

// The module's fields: name and kind (0 an int, 1 a string's address; its kind 2, an int read through a local, has no
// field).
struct FieldDef {
    std::string_view name;
    u32 kind;
};
constexpr FieldDef kFields[] = {
    {"PesmTargetFullName", 1}, {"PesmTargetFSize", 0}, {"PesmWrittenSize", 0}, {"PesmMemoryContainer", 0},
    {"PesmMemoryContainer2", 0}, {"PesmResult", 0}, {"PesmReqId", 0}, {"PesmDevice", 0}, {"PesmDeviceName", 1},
    {"PesmEncryptTrack", 0}, {"PesmEncryptSeed", 0}, {"PesmEncryptSize", 0}, {"PesmEncryptSrcoffset", 0},
    {"PesmEncryptDstoffset", 0}, {"PesmPrepareIidsize", 0}, {"PesmInitentryMntname", 1}, {"PesmInitentryRetname", 1},
    {"PesmEndmovieMovie", 0}, {"PesmSinfSize", 0}, {"PesmSinfTrack", 0}, {"PesmInitentry2Profile", 0},
    {"PesmFinalize2Poweroff", 0},
};
enum Field : u32 {
    MemoryContainer = 3,
    MemoryContainer2 = 4,
    Result = 5,
    ReqId = 6,
    Device = 7,
    DeviceName = 8,
    EncryptTrack = 9,
    EncryptSeed = 10,
    EncryptSize = 11,
    EncryptSrcoffset = 12,
    EncryptDstoffset = 13,
    PrepareIidsize = 14,
    InitentryMntname = 15,
    InitentryRetname = 16,
    EndmovieMovie = 17,
    SinfSize = 18,
    SinfTrack = 19,
    Initentry2Profile = 20,
    Finalize2Poweroff = 21,
};

// The module's commands: the request's element and the attributes it makes room for.
struct CommandDef {
    std::string_view name;
    s32 attributes;
};
constexpr CommandDef kCommands[] = {
    {"PesmCmdLoadAsync", 2}, {"PesmCmdUnloadAsync", 1}, {"PesmCmdCopyToMs", 2}, {"PesmCmdInitialize", 2},
    {"PesmCmdOpenDevice", 2}, {"PesmCmdInitEntry", 2}, {"PesmCmdPrepareRec", 1}, {"PesmCmdGetSinf", 2},
    {"PesmCmdStartMovieRec", 1}, {"PesmCmdEncryptSample", 5}, {"PesmCmdEndMovieRec", 2}, {"PesmCmdCloseDevice", 1},
    {"PesmCmdFinalize", 1}, {"PesmCmdGetModuleVersion", 0}, {"PesmCmdInitEntry2", 2}, {"PesmCmdFinalize2", 1},
    {"PesmCmdEncryptSample2", 2},
};
enum Command : u32 {
    Initialize = 3,
    OpenDevice = 4,
    InitEntry = 5,
    PrepareRec = 6,
    GetSinf = 7,
    StartMovieRec = 8,
    EncryptSample = 9,
    EndMovieRec = 10,
    CloseDevice = 11,
    Finalize = 12,
    InitEntry2 = 14,
    Finalize2 = 15,
    EncryptSample2 = 16,
};

// A field of a request or a reply with its value.
struct Value {
    u32 field;
    u32 value;
};

// The module's object (0x1C bytes), a node of its list (0x20, 8-aligned; the list's head is one) and a request
// (0x8C: its class's vtable, FixedMemory, the document, the root element, FixedMemory::Allocate, the packet).
namespace obj {
constexpr u32 request = 0x00, state = 0x04, list = 0x14, count = 0x18, size = 0x1C;
}
namespace node {
constexpr u32 next = 0x00, prev = 0x04, side = 0x08, key = 0x10, addr = 0x18, size = 0x20;
}
namespace req {
constexpr u32 memory = 0x04, doc = 0x08, root = 0x7C, packet = 0x88, size = 0x8C;
}

// The module's global: its object's address, 0 until the first call.
struct PesmState {
    std::mutex mutex;
    u32 object = 0;
    bool memory_logged = false;
};

PesmState& state() { return HleProcess::current()->state<PesmState>(); }

u32 lib(PPUContext& ctx, std::string_view module, std::string_view name, std::initializer_list<u64> args)
{
    return static_cast<u32>(call_sysutil(ctx, module, name, args));
}
u32 op_new(PPUContext& ctx, u32 size) { return lib(ctx, "sys_libstdcxx", "_Znwj", {size}); }
u32 op_new_aligned(PPUContext& ctx, u32 size, u32 align) { return lib(ctx, "sys_libstdcxx", "_Znwjj", {size, align}); }
void op_delete(PPUContext& ctx, u32 p) { lib(ctx, "sys_libstdcxx", "_ZdlPv", {p}); }
void sys_memcpy(PPUContext& ctx, u32 dst, u32 src, u32 size) { lib(ctx, "sysPrxForUser", "_sys_memcpy", {dst, src, size}); }
void sys_strcpy(PPUContext& ctx, u32 dst, u32 src) { lib(ctx, "sysPrxForUser", "_sys_strcpy", {dst, src}); }

// The module's object, made at the first call as its code makes it: the object, then its list's head.
u32 the_object(PPUContext& ctx)
{
    PesmState& s = state();
    {
        std::lock_guard lock(s.mutex);
        if (s.object) return s.object;
    }
    Mem m(ctx);
    const u32 o = op_new(ctx, obj::size);
    m.w32(o + obj::request, 0);
    m.w32(o + obj::state, 0);
    const u32 head = op_new_aligned(ctx, node::size, 8);
    m.w32(o + 0x0C, 0);
    m.w32(o + obj::count, 0);
    m.w32(o + 0x08, 0);
    m.w32(head + node::prev, head);
    m.w32(head + node::next, head);
    m.w32(o + obj::list, head);
    std::lock_guard lock(s.mutex);
    s.object = o;
    return o;
}

u32 get_state(Mem& m, u32 o) { return m.r32(o + obj::state); }
void set_state(Mem& m, u32 o, u32 v) { m.w32(o + obj::state, v); }

Element get_root(Mem& m, u32 r) { return {m.r32(r + req::root), m.r32(r + req::root + 4)}; }
void set_root(Mem& m, u32 r, const Element& e)
{
    m.w32(r + req::root, e.doc);
    m.w32(r + req::root + 4, e.off);
}

// The request object made, its document built and written: the class's constructor, prepare (FixedMemory as the
// allocator, the magic, the command's element) and the fields' setters, then its send. Each call into libsysutil as its
// export answers it (as_export): a game's string that is not there leaves its attribute out, and the request goes on.
void send_request(PPUContext& ctx, u32 o, Command cmd, std::span<const Value> fields)
{
    Mem m(ctx);
    const u32 r = op_new(ctx, req::size);
    m.w32(r, 0); // the module's vtable: pspocket's object has none
    const u32 doc = r + req::doc;
    document_construct(ctx, doc);
    set_root(m, r, {0, 0xFFFFFFFF});
    m.w32(r + 0x84, 0); // the module's FixedMemory::Allocate (document_use_fixed_memory sets the document's)
    m.w32(r + req::packet, kPacket);
    packet_begin(ctx, 0, kPacket);
    const u32 memory = fixed_memory_begin(ctx, 0);
    m.w32(o + obj::request, r);
    m.w32(r + req::memory, memory);
    document_use_fixed_memory(ctx, doc, memory);
    document_set_magic(ctx, doc, "PESM");
    Element root;
    if (as_export(ctx, "cxml::Document::CreateElement", err(CELL_EFAULT),
            [&] { return document_create_element(ctx, doc, kCommands[cmd].name, kCommands[cmd].attributes, root); })
        == 0)
        set_root(m, r, root);
    root = get_root(m, r);
    for (const Value& v : fields) {
        const FieldDef& f = kFields[v.field];
        if (f.kind == 0) {
            as_export(ctx, "cxmlutil::SetInt", err(CELL_EFAULT),
                [&] { return util_set_int(ctx, root, f.name, static_cast<s32>(v.value)); });
        } else {
            as_export(ctx, "cxmlutil::SetString", err(CELL_EFAULT),
                [&] { return util_set_string(ctx, root, f.name, v.value); });
        }
    }
    const s32 res = as_export(ctx, "cxml::Document::Write", err(CELL_EFAULT), [&] {
        PacketWriter w(ctx, 0, kPacket, doc);
        return document_write(ctx, doc, w);
    });
    if (res != 0) log_handover(ctx, kCommands[cmd].name, res);
}

// The reply read into FixedMemory (no `done`) and a document made over what was read, when it was; the root then the
// document's, which a failed read leaves the request's own. (When the document is broken, libsysutil's
// GetDocumentElement faults and leaves the module's local as its stack had it; pspocket's starts empty: no element.)
void read_reply(PPUContext& ctx, u32 r)
{
    Mem m(ctx);
    const u32 memory = m.r32(r + req::memory);
    const u32 doc = r + req::doc;
    const s32 n = packet_read(ctx, 0, kPacket, memory, 0x6000, 0);
    if (n >= 0) {
        as_export(ctx, "cxml::Document::CreateFromBuffer", err(CELL_EFAULT),
            [&] { return document_create_from_buffer(ctx, doc, memory, static_cast<u32>(n), 0); });
    }
    Element root{0, 0xFFFFFFFF};
    as_export(ctx, "cxml::Document::GetDocumentElement", 0, [&] {
        root = document_element(ctx, doc);
        return 0;
    });
    set_root(m, r, root);
}

// The class's deleting destructor: FixedMemory and the packet given back, the document destroyed, the object freed.
void delete_request(PPUContext& ctx, u32 o, u32 r)
{
    fixed_memory_end(ctx, 0);
    packet_end(ctx, 0, kPacket);
    document_clear(ctx, r + req::doc);
    op_delete(ctx, r);
    Mem(ctx).w32(o + obj::request, 0);
}

// The module's exchange for `cmd`: the request sent when none is in flight; the reply of the one in flight read; when
// its root is the command's, `reads` read over their values (each kept when its attribute is not there), the request
// deleted and true. Else false, the request kept.
bool exchange(PPUContext& ctx, u32 o, Command cmd, std::span<const Value> sends, std::span<Value> reads)
{
    Mem m(ctx);
    if (!m.r32(o + obj::request)) send_request(ctx, o, cmd, sends);
    const u32 r = m.r32(o + obj::request);
    read_reply(ctx, r);
    const Element root = get_root(m, r);
    if (!as_export(ctx, "cxmlutil::CheckElementName", false,
            [&] { return util_check_name(ctx, root, kCommands[cmd].name); }))
        return false;
    for (Value& v : reads) {
        const FieldDef& f = kFields[v.field];
        if (f.kind == 0) {
            s32 x = static_cast<s32>(v.value);
            as_export(ctx, "cxmlutil::GetInt", err(CELL_EFAULT), [&] { return util_get_int(ctx, root, f.name, x); });
            v.value = static_cast<u32>(x);
        } else {
            u32 len = 0;
            as_export(ctx, "cxmlutil::GetString", err(CELL_EFAULT),
                [&] { return util_get_string_host(ctx, root, f.name, v.value, len); });
        }
    }
    delete_request(ctx, o, r);
    return true;
}

// The node of the recorder's shared memory in the module's list, or 0.
u32 find_memory(Mem& m, u32 o)
{
    const u32 head = m.r32(o + obj::list);
    for (u32 n = m.r32(head + node::next); n != head; n = m.r32(n + node::next))
        if ((u64{m.r32(n + node::key)} << 32 | m.r32(n + node::key + 4)) == kMemoryKey) return n;
    return 0;
}

// Initialize's: the recorder's shared memory opened through libsysutil and put at the list's end. When lv2 has none
// under its key, libsysutil would wait for the system software to make it, for ever on pspocket's console: not opened,
// as when the opening fails.
void open_memory(PPUContext& ctx, u32 o)
{
    if (!HleProcess::current()->kernel().shared_memory_key_exists(kMemoryKey)) {
        PesmState& s = state();
        {
            std::lock_guard lock(s.mutex);
            if (s.memory_logged) return;
            s.memory_logged = true;
        }
        log_call_site(ctx, guest_base(ctx),
            "cellPesmInitialize: the recorder's shared memory (key 0x8086000000000200) is the system software's to "
            "make, and pspocket's console has none: not opened (Sony's module would wait for it for ever); the calls "
            "that need it answer 0x80862002");
        return;
    }
    Mem m(ctx);
    StackVar out(ctx, m.base(), 16);
    if (call_sysutil(ctx, "cellSysutil", "cellSysutilSharedMemoryAlloc",
            {kPacket, kMemoryKey, kMemorySize, 0xFFFFFFFF, kPage64K, out.addr(), 0, 0}) != 0)
        return;
    const u32 head = m.r32(o + obj::list);
    const u32 last = m.r32(head + node::prev);
    const u32 n = op_new_aligned(ctx, node::size, 8);
    m.w32(n + node::side, kPacket);
    m.w32(n + node::addr, m.r32(out.addr()));
    m.w32(n + node::next, head);
    m.w32(n + node::prev, last);
    m.w32(n + node::key, static_cast<u32>(kMemoryKey >> 32));
    m.w32(n + node::key + 4, static_cast<u32>(kMemoryKey));
    m.w32(head + node::prev, n);
    m.w32(o + obj::count, m.r32(o + obj::count) + 1);
    m.w32(last + node::next, n);
}

void free_memory(PPUContext& ctx, Mem& m, u32 n)
{
    call_sysutil(ctx, "cellSysutil", "cellSysutilSharedMemoryFree", {m.r32(n + node::side), m.r32(n + node::addr),
        kPage64K});
}

// The finalizations': the recorder's shared memory given back and its node taken out, then every other node's memory
// given back (their nodes left in the list, as the module leaves them).
void close_memories(PPUContext& ctx, u32 o)
{
    Mem m(ctx);
    if (const u32 n = find_memory(m, o)) {
        free_memory(ctx, m, n);
        const u32 prev = m.r32(n + node::prev), next = m.r32(n + node::next);
        m.w32(prev + node::next, next);
        m.w32(next + node::prev, prev);
        op_delete(ctx, n);
        m.w32(o + obj::count, m.r32(o + obj::count) - 1);
    }
    if (m.r32(o + obj::count)) {
        const u32 head = m.r32(o + obj::list);
        for (u32 n = m.r32(head + node::next); n != head; n = m.r32(n + node::next)) free_memory(ctx, m, n);
    }
}

// A function's body on the module's object: libsysutil's channels started first (D-534), a fault logged and answered
// CELL_EFAULT (Sony's module faults there).
template <class Body>
s32 pesm(PPUContext& ctx, std::string_view what, Body&& body)
{
    packets_start(ctx);
    return guarded(ctx, kModule, what, [&]() -> s32 {
        Mem m(ctx);
        return body(m, the_object(ctx));
    });
}

// cellPesmInitialize(container, container2): not while initializing or initialized; the two containers' sizes asked
// of lv2 (sys_memory_container_get_size, the answers not looked at), then "PesmCmdInitialize" with them. The reply's
// result (container2 when it has none) is answered once the shared memory is opened (or not); a reply of another root
// leaves the module initializing.
s32 cellPesmInitialize(PPUContext& ctx, u32 container, u32 container2)
{
    return pesm(ctx, "cellPesmInitialize", [&](Mem& m, u32 o) -> s32 {
        const u32 st = get_state(m, o);
        if (st == 2 || st == 1) return err(PESM_ERROR_STATE);
        set_state(m, o, 1);
        {
            StackVar info(ctx, m.base(), 16);
            lv2_syscall(ctx, 343, {info.addr(), container});
            lv2_syscall(ctx, 343, {info.addr(), container2});
        }
        const Value sends[] = {{MemoryContainer, container}, {MemoryContainer2, container2}};
        Value reads[] = {{Result, container2}};
        if (!exchange(ctx, o, Initialize, sends, reads)) return err(PESM_ERROR_NOT_ANSWERED);
        open_memory(ctx, o);
        set_state(m, o, 2);
        return static_cast<s32>(reads[0].value);
    });
}

// The finalizations: not while finalizing or finalized; the request (its result, or 0x80861001), then the shared
// memories given back and the module finalized whatever the reply.
s32 finalize(PPUContext& ctx, std::string_view what, Command cmd, Value send, u32 result)
{
    return pesm(ctx, what, [&](Mem& m, u32 o) -> s32 {
        const u32 st = get_state(m, o);
        if (st == 5 || st == 4) return err(PESM_ERROR_STATE);
        set_state(m, o, 4);
        const Value sends[] = {send};
        Value reads[] = {{Result, result}};
        const s32 r =
            exchange(ctx, o, cmd, sends, reads) ? static_cast<s32>(reads[0].value) : err(PESM_ERROR_NOT_ANSWERED);
        close_memories(ctx, o);
        set_state(m, o, 5);
        return r;
    });
}

// cellPesmFinalize(): "PesmCmdFinalize" with the request ID 100 (its result 100 when the reply has none).
s32 cellPesmFinalize(PPUContext& ctx) { return finalize(ctx, "cellPesmFinalize", Finalize, {ReqId, 100}, 100); }

// cellPesmFinalize2(poweroff): "PesmCmdFinalize2" with it (also the result when the reply has none).
s32 cellPesmFinalize2(PPUContext& ctx, u32 poweroff)
{
    return finalize(ctx, "cellPesmFinalize2", Finalize2, {Finalize2Poweroff, poweroff}, poweroff);
}

// A call of an initialized module: state 3 during it, 2 after it, whatever it answers once it has begun. `busy` is
// what it answers when the module is not initialized.
template <class Body>
s32 in_call(PPUContext& ctx, std::string_view what, u32 busy, Body&& body)
{
    return pesm(ctx, what, [&](Mem& m, u32 o) -> s32 {
        if (get_state(m, o) != 2) return static_cast<s32>(busy);
        set_state(m, o, 3);
        bool done = false;
        const s32 r = body(m, o, done);
        if (done) set_state(m, o, 2);
        return r;
    });
}

// cellPesmOpenDevice(name, device): "PesmCmdOpenDevice" with the device's name; the device the reply's (the name's
// address when it has none).
s32 cellPesmOpenDevice(PPUContext& ctx, u32 name, u32 device)
{
    return in_call(ctx, "cellPesmOpenDevice", PESM_ERROR_STATE, [&](Mem& m, u32 o, bool& done) -> s32 {
        const Value sends[] = {{DeviceName, name}};
        Value reads[] = {{Result, name}, {Device, name}};
        s32 r = err(PESM_ERROR_NOT_ANSWERED);
        if (exchange(ctx, o, OpenDevice, sends, reads)) {
            r = static_cast<s32>(reads[0].value);
            m.w32(device, reads[1].value);
        }
        done = true;
        return r;
    });
}

// cellPesmCloseDevice(device): "PesmCmdCloseDevice" with it.
s32 cellPesmCloseDevice(PPUContext& ctx, u32 device)
{
    return in_call(ctx, "cellPesmCloseDevice", PESM_ERROR_STATE, [&](Mem&, u32 o, bool& done) -> s32 {
        const Value sends[] = {{Device, device}};
        Value reads[] = {{Result, device}};
        done = true;
        return exchange(ctx, o, CloseDevice, sends, reads) ? static_cast<s32>(reads[0].value)
                                                           : err(PESM_ERROR_NOT_ANSWERED);
    });
}

// The recorder's shared memory's address, or 0 (the call then answers 0x80862002, the module left in it).
u32 memory_address(Mem& m, u32 o)
{
    const u32 n = find_memory(m, o);
    return n ? m.r32(n + node::addr) : 0;
}

// cellPesmInitEntry(_, mount name, returned name) and cellPesmInitEntry2(_, mount name, profile, returned name):
// "PesmCmdInitEntry" with the mount name ("PesmCmdInitEntry2" with the profile too); the returned name copied from the
// reply's (liblv2's strcpy), or from the value the function put there when it has none: the mount name (the profile's
// value as an address for InitEntry2). Not initialized: 1.
s32 init_entry(PPUContext& ctx, std::string_view what, Command cmd, std::span<const Value> sends, u32 dflt, u32 retname)
{
    return in_call(ctx, what, 1, [&](Mem& m, u32 o, bool& done) -> s32 {
        if (!find_memory(m, o)) return err(PESM_ERROR_NO_MEMORY);
        Value reads[] = {{Result, dflt}, {InitentryRetname, dflt}};
        s32 r = err(PESM_ERROR_NOT_ANSWERED);
        if (exchange(ctx, o, cmd, sends, reads)) {
            r = static_cast<s32>(reads[0].value);
            sys_strcpy(ctx, retname, reads[1].value);
        }
        done = true;
        return r;
    });
}

s32 cellPesmInitEntry(PPUContext& ctx, u32, u32 mntname, u32 retname)
{
    const Value sends[] = {{InitentryMntname, mntname}};
    return init_entry(ctx, "cellPesmInitEntry", InitEntry, sends, mntname, retname);
}

s32 cellPesmInitEntry2(PPUContext& ctx, u32, u32 mntname, u32 profile, u32 retname)
{
    const Value sends[] = {{InitentryMntname, mntname}, {Initentry2Profile, profile}};
    return init_entry(ctx, "cellPesmInitEntry2", InitEntry2, sends, profile, retname);
}

// cellPesmPrepareRec(_, info): the info's 48 bytes copied into the shared memory (no info: -19, the module left in
// the call), then "PesmCmdPrepareRec" with the size 24.
s32 cellPesmPrepareRec(PPUContext& ctx, u32, u32 info)
{
    return in_call(ctx, "cellPesmPrepareRec", PESM_ERROR_STATE, [&](Mem& m, u32 o, bool& done) -> s32 {
        const u32 shm = memory_address(m, o);
        if (!shm) return err(PESM_ERROR_NO_MEMORY);
        if (!info) return -19;
        sys_memcpy(ctx, shm, info, 0x30);
        const Value sends[] = {{PrepareIidsize, 0x18}};
        Value reads[] = {{Result, 0x18}};
        done = true;
        return exchange(ctx, o, PrepareRec, sends, reads) ? static_cast<s32>(reads[0].value)
                                                          : err(PESM_ERROR_NOT_ANSWERED);
    });
}

// cellPesmStartMovieRec(_, info): "PesmCmdStartMovieRec" with the info's first word as the device; on a result of 0,
// 20 bytes of the shared memory copied over the info.
s32 cellPesmStartMovieRec(PPUContext& ctx, u32, u32 info)
{
    return in_call(ctx, "cellPesmStartMovieRec", PESM_ERROR_STATE, [&](Mem& m, u32 o, bool& done) -> s32 {
        const u32 shm = memory_address(m, o);
        if (!shm) return err(PESM_ERROR_NO_MEMORY);
        const u32 device = m.r32(info);
        const Value sends[] = {{Device, device}};
        Value reads[] = {{Result, device}};
        s32 r = err(PESM_ERROR_NOT_ANSWERED);
        if (exchange(ctx, o, StartMovieRec, sends, reads)) {
            r = static_cast<s32>(reads[0].value);
            if (r == 0) sys_memcpy(ctx, info, shm, 0x14);
        }
        done = true;
        return r;
    });
}

// cellPesmEndMovieRec(_, info): any time but while finalizing or finalized; "PesmCmdEndMovieRec" with the info's
// second word, read before anything else.
s32 cellPesmEndMovieRec(PPUContext& ctx, u32, u32 info)
{
    return pesm(ctx, "cellPesmEndMovieRec", [&](Mem& m, u32 o) -> s32 {
        const u32 st = get_state(m, o);
        if (st == 5 || st == 4) return err(PESM_ERROR_STATE);
        const u32 movie = m.r32(info + 4);
        set_state(m, o, 3);
        const Value sends[] = {{EndmovieMovie, movie}};
        Value reads[] = {{Result, movie}};
        const s32 r = exchange(ctx, o, EndMovieRec, sends, reads) ? static_cast<s32>(reads[0].value)
                                                                  : err(PESM_ERROR_NOT_ANSWERED);
        set_state(m, o, 2);
        return r;
    });
}

// cellPesmEncryptSample(_, sample, encrypted, info, size): the sample (the info's size, at most 1 MB) copied into the
// shared memory, then "PesmCmdEncryptSample" with the info's track, seed and size and a source offset 0; the encrypted
// sample's address the reply's destination offset into the shared memory, its size the reply's (0 when it has none).
s32 cellPesmEncryptSample(PPUContext& ctx, u32, u32 sample, u32 encrypted, u32 info, u32 size)
{
    return in_call(ctx, "cellPesmEncryptSample", PESM_ERROR_STATE, [&](Mem& m, u32 o, bool& done) -> s32 {
        const u32 shm = memory_address(m, o);
        if (!shm) return err(PESM_ERROR_NO_MEMORY);
        if (m.r32(info + 12) > 0x100000) return err(PESM_ERROR_TOO_LARGE);
        sys_memcpy(ctx, shm, sample, m.r32(info + 12));
        const Value sends[] = {{EncryptTrack, m.r32(info + 4)}, {EncryptSeed, m.r32(info + 8)},
            {EncryptSize, m.r32(info + 12)}, {EncryptSrcoffset, 0}};
        Value reads[] = {{Result, 0}, {EncryptDstoffset, 0}, {EncryptSize, 0}};
        s32 r = err(PESM_ERROR_NOT_ANSWERED);
        if (exchange(ctx, o, EncryptSample, sends, reads)) {
            r = static_cast<s32>(reads[0].value);
            m.w32(encrypted, reads[1].value + shm);
            m.w32(size, reads[2].value);
        }
        done = true;
        return r;
    });
}

// cellPesmEncryptSample2(_, sample, size, encrypted): the sample and its 128-byte header (at most 1 MB together)
// copied into the shared memory, then "PesmCmdEncryptSample2" with no field; the encrypted sample's address as
// EncryptSample's. (The module starts the reply's two values from a word its stack happens to hold; pspocket's from 0.)
s32 cellPesmEncryptSample2(PPUContext& ctx, u32, u32 sample, u32 size, u32 encrypted)
{
    return in_call(ctx, "cellPesmEncryptSample2", PESM_ERROR_STATE, [&](Mem& m, u32 o, bool& done) -> s32 {
        const u32 shm = memory_address(m, o);
        if (!shm) return err(PESM_ERROR_NO_MEMORY);
        if (size + 0x80 > 0x100000) return err(PESM_ERROR_TOO_LARGE);
        sys_memcpy(ctx, shm, sample, size + 0x80);
        Value reads[] = {{Result, 0}, {EncryptDstoffset, 0}};
        s32 r = err(PESM_ERROR_NOT_ANSWERED);
        if (exchange(ctx, o, EncryptSample2, {}, reads)) {
            r = static_cast<s32>(reads[0].value);
            m.w32(encrypted, reads[1].value + shm);
        }
        done = true;
        return r;
    });
}

// cellPesmGetSinf(_, track, sinf, size): "PesmCmdGetSinf" with the track; the size the reply's (the track when it has
// none) and, when the result is 0 and the size not, the sinf at the shared memory's start. It answers 0 whatever the
// reply.
s32 cellPesmGetSinf(PPUContext& ctx, u32, u32 track, u32 sinf, u32 size)
{
    return in_call(ctx, "cellPesmGetSinf", PESM_ERROR_STATE, [&](Mem& m, u32 o, bool& done) -> s32 {
        const u32 shm = memory_address(m, o);
        if (!shm) return err(PESM_ERROR_NO_MEMORY);
        const Value sends[] = {{SinfTrack, track}};
        Value reads[] = {{Result, track}, {SinfSize, track}};
        if (exchange(ctx, o, GetSinf, sends, reads)) {
            m.w32(size, reads[1].value);
            if (reads[0].value == 0 && m.r32(size) != 0) m.w32(sinf, shm);
        }
        done = true;
        return 0;
    });
}

// cellPesmLoadAsync, cellPesmUnloadAsync and the module's export 0x8AB3156B: -1, nothing done.
s32 cellPesmLoadAsync(PPUContext&) { return -1; }
s32 cellPesmUnloadAsync(PPUContext&) { return -1; }
s32 cellPesmUtility_8AB3156B(PPUContext&) { return -1; }

// The module's export 0x6312A6CC: its version (0x30001) and 1 written where given.
s32 cellPesmUtility_6312A6CC(PPUContext& ctx, u32 version, u32 one)
{
    return guarded(ctx, kModule, "cellPesmUtility_6312A6CC", [&]() -> s32 {
        Mem m(ctx);
        if (version) m.w32(version, 0x30001);
        if (one) m.w32(one, 1);
        return 0;
    });
}

} // namespace

void register_pesm(Registry& r)
{
    r.add("cellPesmUtility", {
        PS_HLE_FUNC(0x05C4EE8D, cellPesmInitialize),
        PS_HLE_FUNC(0x195716DB, cellPesmFinalize),
        PS_HLE_FUNC(0xCBC06973, cellPesmFinalize2),
        PS_HLE_FUNC(0x2C43AB76, cellPesmOpenDevice),
        PS_HLE_FUNC(0xD425AC7D, cellPesmCloseDevice),
        PS_HLE_FUNC(0xAF41D6C8, cellPesmInitEntry),
        PS_HLE_FUNC(0xD45C3481, cellPesmInitEntry2),
        PS_HLE_FUNC(0xEC2E26B6, cellPesmPrepareRec),
        PS_HLE_FUNC(0xA985A03B, cellPesmStartMovieRec),
        PS_HLE_FUNC(0xBA39AB02, cellPesmEndMovieRec),
        PS_HLE_FUNC(0x65F7338B, cellPesmEncryptSample),
        PS_HLE_FUNC(0xC54AAD2A, cellPesmEncryptSample2),
        PS_HLE_FUNC(0xA84FBC36, cellPesmGetSinf),
        PS_HLE_FUNC(0x2821CBAA, cellPesmLoadAsync),
        PS_HLE_FUNC(0x941F9C4E, cellPesmUnloadAsync),
        Function{{}, "cellPesmUtility_6312A6CC", 0x6312A6CC, &detail::thunk<&cellPesmUtility_6312A6CC>, false},
        Function{{}, "cellPesmUtility_8AB3156B", 0x8AB3156B, &detail::thunk<&cellPesmUtility_8AB3156B>, false},
    });
}

} // namespace pspocket::hle::sysutil::sub
