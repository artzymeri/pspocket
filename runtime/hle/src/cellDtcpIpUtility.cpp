// cellDtcpIpUtility: the front end of the system software's DTCP-IP client (protected streams from a DLNA server:
// authentication, opening content, reading or buffering it, decrypted data, seeking, moving content) (D-930). Read from
// libsysutil_dtcp_ip.sprx (4.92) and compared with it call for call (tests/hle/test_sysutil_more.cpp). RPCS3 leaves the
// thirteen functions it names stubs (cellDtcpIpUtility.cpp) and does not know the module's other sixteen, in its
// libraries cellDtcpIpUtilityClearTextByte, cellDtcpIpUtilityMove and cellDtcpIpUtilityEx (their names here are the
// module's own diagnostics': cellDtcpIpOpenContentEx, cellDtcpIpMoveRead, ...).
//
// The module keeps its state in its data: the game's callback and argument, its memory container, its function flags
// (bit 0 buffering mode: the game writes the encrypted stream into a 1 MB ring; bit 62 the move mode), the system's
// shared memory (two 128 KB buffers of decrypted data, the ring at +256 KB), the ring's positions, three slots of
// decrypted data ready, the user's read buffer, an lwmutex ("_vppdrl") and the cancel and seek flags. Its calls are
// requests on libsysutil's channel 0, packet 0x140, magic "DTCP", each failure printed through liblv2's printf as the
// module prints it (its source file, function and line). Initialize registers a dispatcher for the system's event 0x14A
// (cellDtcpIpCallbackHandler), through which the system makes the shared memory, reports reads, data and progress, and
// calls the game's callback. pspocket's console has no system software: its channels are closed (D-534), so Initialize
// meets the closed channel at its write (0x8002b007), undoes what it began and answers it, and the other calls find no
// shared memory (0x80024E02). pspocket keeps the module's state in a block of guest memory laid out as the module's
// data (its lwmutex there), so lv2 sees the same calls.

#include <span>
#include <string_view>

#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"
#include "sysutil_sub.hpp"

namespace pspocket::hle::sysutil::sub {

namespace {

constexpr std::string_view kModule = "libsysutil_dtcp_ip";
constexpr std::string_view kFile = "sysutil_dtcp_ip.cpp";
constexpr s32 kPacket = 0x140;
constexpr u32 kEvent = 0x14A;
constexpr u64 kMemoryKey = 0x8006040000000A10ull;
constexpr std::string_view kHandler = "cellDtcpIpUtility_CallbackHandler";

constexpr u32 DTCP_ERROR_DISPATCHER = 0x80024E01;
constexpr u32 DTCP_ERROR_STATE = 0x80024E02;
constexpr u32 DTCP_ERROR_PARAM = 0x80024E03;
constexpr u32 DTCP_ERROR_CANCELLED = 0x80024E14;
constexpr u32 DTCP_ERROR_EMPTY = 0x80024E1E;
constexpr u32 DTCP_ERROR_BUSY = 0x8002B105;
constexpr u32 DTCP_ERROR_READ = 0x8002B106;
constexpr u32 DTCP_ERROR_CONTAINER = 0x8002B107;
constexpr u32 PACKET_TRUNCATED = 0x8002B006;

constexpr u64 kBuffering = 1;                     // CELL_DTCP_IP_FUNCTION_FLAG_BUFFERING_MODE
constexpr u64 kMove = 0x4000000000000000ull;      // the move mode
constexpr u32 kRing = 0x100000;

// The module's data (from 0x87E0 in its image), as pspocket lays it out.
namespace g {
constexpr u32 container = 0x008, used = 0x00C, slots = 0x010, word18 = 0x018, callback = 0x17C, userdata = 0x180,
              flags = 0x188, shm = 0x190, read_buffer = 0x194, lwmutex = 0x198, seeking = 0x1B0, read_dest = 0x1B4,
              read_size = 0x1B8, cancel = 0x1BC, ring = 0x1C0, ring_read = 0x1C4, ring_write = 0x1C8, slot_out = 0x1CC,
              slot_in = 0x1D0, buffers = 0x1D4, emi = 0x1DC, sizes = 0x1E4, kSize = 0x1EC;
static_assert(kSize <= 0x200);
}

struct DtcpState {
    std::mutex mutex;
    u32 data = 0;
};

DtcpState& state() { return HleProcess::current()->state<DtcpState>(); }

// The module's data block, made at first use with the module's initial values.
u32 data_block()
{
    DtcpState& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.data) {
        const u32 d = HleProcess::current()->main_memory().alloc(0x200, 0x80); // g::kSize bytes and more
        if (!d) PS_FATAL("cellDtcpIpUtility", "no main memory for the module's data");
        u8* b = HleProcess::current()->kernel().base();
        ps_write32(b, d + g::container, 0xFFFFFFFF);
        ps_write32(b, d + g::used, kRing);
        for (u32 i = 0; i < 3; i++) ps_write8(b, d + g::slots + i, 0xFF);
        ps_write32(b, d + g::word18, 0xFFFFFFFF);
        s.data = d;
    }
    return s.data;
}

u32 lv2_fn(PPUContext& ctx, std::string_view name, std::initializer_list<u64> args)
{
    return static_cast<u32>(call_sysutil(ctx, "sysPrxForUser", name, args));
}

// liblv2's memcpy, as the module copies (its own wrapper of _sys_memcpy).
void sys_memcpy_guest(PPUContext& ctx, u32 dst, u32 src, u32 n) { lv2_fn(ctx, "_sys_memcpy", {dst, src, n}); }

// CellDtcpIpHostInfo's 16-bit port (after the address string's pointer), read as the module reads it.
u32 host_port(Mem& m, u32 host) { return u32{m.r8(host + 4)} << 8 | m.r8(host + 5); }

// The module's state, through its data block.
struct Dtcp {
    PPUContext& ctx;
    Mem m;
    u32 d;
    explicit Dtcp(PPUContext& c) : ctx(c), m(c), d(data_block()) {}

    u32 r32(u32 off) { return m.r32(d + off); }
    void w32(u32 off, u32 v) { m.w32(d + off, v); }
    u8 r8(u32 off) { return m.r8(d + off); }
    void w8(u32 off, u8 v) { m.w8(d + off, v); }
    u64 flags() { return u64{r32(g::flags)} << 32 | r32(g::flags + 4); }
    void set_flags(u64 v)
    {
        w32(g::flags, static_cast<u32>(v >> 32));
        w32(g::flags + 4, static_cast<u32>(v));
    }
    u32 lw() const { return d + g::lwmutex; }
    void lock() { lv2_fn(ctx, "sys_lwmutex_lock", {lw(), 0}); }
    void unlock() { lv2_fn(ctx, "sys_lwmutex_unlock", {lw()}); }

    // liblv2's printf of the module's text and arguments (its strings put on the stack, as module_printf does).
    void print(std::string_view fmt, std::initializer_list<PrintArg> args) { module_printf(ctx, fmt, args); }
    // The module's "%s:%s(%d)\t..." diagnostics.
    void where(std::string_view fmt, std::string_view func, s32 line) { print(fmt, {kFile, func, line}); }

    // reset(): what Initialize sets, back as it was.
    void reset()
    {
        w8(g::seeking, 0);
        w32(g::read_size, 0);
        w32(g::container, 0xFFFFFFFF);
        w32(g::userdata, 0);
        w32(g::callback, 0);
        w32(g::read_dest, 0);
        set_flags(0);
    }

    // finalize(): the dispatcher unregistered, the shared memory given back, the buffers' state cleared, the lwmutex
    // destroyed.
    void finalize()
    {
        const s32 u = call_sysutil(ctx, "cellSysutil", "cellSysutilUnregisterCallbackDispatcher", {kEvent});
        if (u != 0) print("ERROR : cellSysutilUnregisterCallbackDispatcher() = 0x%x.\n", {u});
        if (const u32 shm = r32(g::shm)) {
            const s32 f = call_sysutil(ctx, "cellSysutil", "cellSysutilSharedMemoryFree", {kPacket, shm, 0x200});
            if (f != 0) {
                print("%s:%s(%d)\tcellSysutilSharedMemoryFree() is failed. ret(%#x)\n", {kFile, "finalize", 0x10B, f});
            }
        }
        w32(g::used, kRing);
        w32(g::read_buffer, 0);
        w32(g::ring, 0);
        w32(g::ring_read, 0);
        w32(g::ring_write, 0);
        w32(g::sizes + 4, 0);
        w32(g::buffers + 4, 0);
        w32(g::emi + 4, 0);
        w32(g::buffers, 0);
        w32(g::emi, 0);
        w32(g::sizes, 0);
        for (u32 i = 0; i < 3; i++) w8(g::slots + i, 0xFF);
        w32(g::shm, 0);
        w32(g::slot_out, 0);
        w32(g::slot_in, 0);
        lv2_fn(ctx, "sys_lwmutex_destroy", {lw()});
    }

    // The game's callback (callback(argument, event, status, value, value2)), when there is one.
    void notify(u32 event, u32 status, u32 value, u32 value2)
    {
        if (const u32 cb = r32(g::callback)) call_guest(ctx, cb, {r32(g::userdata), event, status, value, value2});
    }
};

// receiveResponsePacket(memory, kind, out): the reply read into FixedMemory, a document made over its whole 0x6000
// bytes, its "result" read: negative, the answer (printed); else 0, and for kind 1 the reply's "param" read into the
// game's word `out` first. Each failure printed as the module prints it.
s32 receive(Dtcp& s, u32 memory, u32 kind, u32 out)
{
    PPUContext& ctx = s.ctx;
    StackDocument d(ctx);
    const u32 doc = d.addr();
    document_construct(ctx, doc);
    StackVar done(ctx, guest_base(ctx), 16);
    const s32 n = packet_read(ctx, 0, kPacket, memory, 0x6000, done.addr());
    s32 r = 0;
    if (n < 0) {
        s.print("packet read error 0x%x.\n", {n});
        r = n;
    } else if (!ps_read8(guest_base(ctx), done.addr()) && n == 0x6000) {
        s.print("%s\n", {"not enough receive buffer."});
        r = err(PACKET_TRUNCATED);
    } else {
        as_export(ctx, "cxml::Document::CreateFromBuffer", err(CELL_EFAULT),
            [&] { return document_create_from_buffer(ctx, doc, memory, 0x6000, 0); });
        Element root{0, 0xFFFFFFFF};
        as_export(ctx, "cxml::Document::GetDocumentElement", 0, [&] {
            root = document_element(ctx, doc);
            return 0;
        });
        s32 result = 0;
        const s32 got = as_export(ctx, "cxmlutil::GetInt", err(CELL_EFAULT),
            [&] { return util_get_int(ctx, root, "result", result); });
        if (got != 0) {
            s.print("get result failed 0x%x\n", {got});
            r = got;
        } else if (result < 0) {
            s.print("error from server. (error=0x%x)\n", {result});
            r = result;
        } else if (kind == 1) {
            const s32 p = as_export(ctx, "cxmlutil::GetInt", err(CELL_EFAULT), [&] {
                s32 v = 0;
                const s32 e = util_get_int(ctx, root, "param", v);
                if (e == 0) Mem(ctx).w32(out, static_cast<u32>(v));
                return e;
            });
            if (p != 0) {
                s.print("get param failed 0x%x\n", {p});
                r = p;
            }
        }
    }
    document_clear(ctx, doc);
    return r;
}

// One attribute of a request: an int, a string of the game's (its address), or a file of bytes (a block of
// `size` bytes at `data`, registered and set).
struct Attr {
    enum Kind { Int, String, File } kind;
    std::string_view name;
    u32 value = 0, size = 0;
    static Attr i(std::string_view n, u32 v) { return {Int, n, v, 0}; }
    static Attr s(std::string_view n, u32 v) { return {String, n, v, 0}; }
    static Attr f(std::string_view n, u32 data, u32 size) { return {File, n, data, size}; }
};

// The request's outcome: its answer, and whether it got as far as receiveResponsePacket (`sent`).
struct Outcome {
    s32 r = 0;
    bool sent = false;
};

// The module's request: the packet begun, FixedMemory 0, a document "DTCP" with the element `name` (room for `count`
// attributes) and `attrs`, written, then the reply received (`kind`, `out`). Its failures printed as the module prints
// them, for `func` at `line` (the element, a file's registration or setting) or plainly (the write, the reply). The
// packet and FixedMemory are given back by the caller's `finish` after whatever it does in between (the module's
// order), which this calls.
template <class Finish>
s32 request(Dtcp& s, std::string_view func, s32 line, std::string_view name, s32 count, std::span<const Attr> attrs,
    u32 kind, u32 out, Finish&& finish)
{
    PPUContext& ctx = s.ctx;
    packet_begin(ctx, 0, kPacket);
    const u32 memory = fixed_memory_begin(ctx, 0);
    StackDocument d(ctx);
    const u32 doc = d.addr();
    document_construct(ctx, doc);
    document_use_fixed_memory(ctx, doc, memory);
    document_set_magic(ctx, doc, "DTCP");
    Outcome o;
    Element e;
    o.r = as_export(ctx, "cxml::Document::CreateElement", err(CELL_EFAULT),
        [&] { return document_create_element(ctx, doc, name, count, e); });
    if (o.r != 0) {
        s.print("%s:%s(%d)\tdoc.CreateElement(%s)\tgoto Exit\n", {kFile, func, line, name});
    } else {
        for (const Attr& a : attrs) {
            if (a.kind == Attr::Int) {
                as_export(ctx, "cxmlutil::SetInt", err(CELL_EFAULT),
                    [&] { return util_set_int(ctx, e, a.name, static_cast<s32>(a.value)); });
            } else if (a.kind == Attr::String) {
                as_export(ctx, "cxmlutil::SetString", err(CELL_EFAULT),
                    [&] { return util_set_string(ctx, e, a.name, a.value); });
            } else {
                File f;
                o.r = as_export(ctx, "cxml::Document::RegisterFile", err(CELL_EFAULT),
                    [&] { return document_register_file(ctx, doc, a.value, a.size, f); });
                if (o.r != 0) {
                    s.print("%s:%s(%d)\tdoc.RegisterFile() failed(0x%x)\n", {kFile, func, line, o.r});
                    break;
                }
                o.r = as_export(ctx, "cxmlutil::SetFile", err(CELL_EFAULT),
                    [&] { return util_set_file(ctx, e, a.name, f); });
                if (o.r != 0) {
                    s.print("%s:%s(%d)\tcxmlutil::SetFile() failed(0x%x)\n", {kFile, func, line, o.r});
                    break;
                }
            }
        }
        if (o.r == 0) {
            o.r = as_export(ctx, "cxml::Document::Write", err(CELL_EFAULT), [&] {
                PacketWriter w(ctx, 0, kPacket, doc);
                return document_write(ctx, doc, w);
            });
            if (o.r != 0) {
                log_handover(ctx, std::string(func), o.r);
                s.print("doc.Write failed 0x%x\n", {o.r});
            } else {
                document_clear(ctx, doc);
                o.sent = true;
                o.r = receive(s, memory, kind, out);
            }
        }
    }
    if (!o.sent) document_clear(ctx, doc);
    return finish(o, [&] {
        fixed_memory_end(ctx, 0);
        packet_end(ctx, 0, kPacket);
    });
}

// The common request: the reply's failure printed; the packet and FixedMemory given back.
s32 simple(Dtcp& s, std::string_view func, s32 line, std::string_view name, s32 count = 0,
    std::span<const Attr> attrs = {}, u32 kind = 0, u32 out = 0)
{
    return request(s, func, line, name, count, attrs, kind, out, [&](const Outcome& o, auto&& end) {
        if (o.sent && o.r < 0) s.print("ERROR : receiveResponsePacket() = 0x%x.\n", {o.r});
        end();
        return o.r;
    });
}

// The checks most calls start with.
bool no_memory(Dtcp& s, std::string_view func, s32 line)
{
    if (s.r32(g::shm)) return false;
    s.where("%s:%s(%d)\tdtcp_ip_client::shared_memory_buffer_ is NULL !!!\n", func, line);
    return true;
}
bool buffering(Dtcp& s, std::string_view func, s32 line)
{
    if (!(s.flags() & kBuffering)) return false;
    s.where("%s:%s(%d)\tCELL_DTCP_IP_FUNCTION_FLAG_BUFFERING_MODE !!!\n", func, line);
    return true;
}
bool cancelled(Dtcp& s, std::string_view func, s32 line)
{
    if (!s.r8(g::cancel)) return false;
    s.where("%s:%s(%d)\tcancel(stop sequence)\n", func, line);
    s.unlock();
    return true;
}

template <class Body>
s32 dtcp(PPUContext& ctx, std::string_view what, Body&& body)
{
    packets_start(ctx);
    return guarded(ctx, kModule, what, [&]() -> s32 {
        Dtcp s(ctx);
        return body(s);
    });
}

// ------------------------------------------------------------------------------------------------- initialization

// Initialize(container, callback, argument, priority, flags): once (not while a container or callback is set); the
// container checked through lv2 (sys_memory_container_get_size: at least 6 MB, 5 in the move mode, all of it
// available); the lwmutex made, the ring's state laid out, the dispatcher of the system's event 0x14A registered, then
// "dtcpIpInitialize" with the container, the priority and the flags as an 8-byte file. Any failure undoes what came
// before (the dispatcher, the lwmutex, the state set).
s32 initialize(PPUContext& ctx, std::string_view what, u32 container, u32 callback, u32 userdata, u32 priority,
    u64 flags)
{
    return dtcp(ctx, what, [&](Dtcp& s) -> s32 {
        if (s.r32(g::container) != 0xFFFFFFFF || s.r32(g::callback) != 0) {
            s.print("%s:%s(%d)\t memory_container_:0x%x system_callback_:0x%p\n",
                {kFile, "Initialize", 0x292, s.r32(g::container), s.r32(g::callback)});
            return err(DTCP_ERROR_STATE);
        }
        s.w32(g::container, container);
        s.w32(g::callback, callback);
        s.w32(g::userdata, userdata);
        s.set_flags(flags);
        if (!callback) {
            s.where("%s:%s(%d)\t(func == NULL)\n", "Initialize", 0x29E);
            s.reset();
            return err(DTCP_ERROR_PARAM);
        }
        const u32 required = (flags & kMove ? 0 : 0x100000) + 0x500000;
        s32 r = err(DTCP_ERROR_CONTAINER);
        const auto container_failed = [&] {
            s.print("%s:%s(%d)\t checkMemoryContainer failed(%x)\n", {kFile, "Initialize", 0x2B8, r});
            s.reset();
            return r;
        };
        if (static_cast<s32>(container) == -1) return container_failed();
        {
            StackVar info(ctx, s.m.base(), 16);
            r = static_cast<s32>(lv2_syscall(ctx, 343, {info.addr(), container}));
            if (r != 0) {
                s.print("%s:%s(%d)\t sys_memory_container_get_size failed(%x)\n",
                    {kFile, "checkMemoryContainer", 0xDA, r});
                return container_failed();
            }
            const u32 total = s.m.r32(info.addr()), available = s.m.r32(info.addr() + 4);
            if (total < required) {
                r = err(DTCP_ERROR_CONTAINER);
                s.print("%s:%s(%d)\t info.total_user_memory(%x) < required_size(%x)\n",
                    {kFile, "checkMemoryContainer", 0xE0, total, required});
                return container_failed();
            }
            if (available < total) {
                r = err(DTCP_ERROR_CONTAINER);
                s.print("%s:%s(%d)\t info.available_user_memory(%x) < info.total_user_memory(%x) \n",
                    {kFile, "checkMemoryContainer", 0xE6, available, total});
                return container_failed();
            }
        }
        {
            // sys_lwmutex_attribute_t: SYS_SYNC_PRIORITY, not recursive, "_vppdrl".
            StackVar attr(ctx, s.m.base(), 16);
            s.m.w32(attr.addr(), 2);
            s.m.w32(attr.addr() + 4, 0x20);
            s.m.put(attr.addr() + 8, "_vppdrl", 8);
            r = static_cast<s32>(lv2_fn(ctx, "sys_lwmutex_create", {s.lw(), attr.addr()}));
            if (r != 0) {
                s.reset();
                return r;
            }
        }
        s.w32(g::used, kRing);
        s.w32(g::ring_read, 0);
        s.w32(g::ring_write, 0);
        for (u32 i = 0; i < 3; i++) s.w8(g::slots + i, 0xFF);
        s.w32(g::sizes + 4, 0);
        s.w32(g::emi + 4, 0);
        s.w32(g::sizes, 0);
        s.w32(g::emi, 0);
        s.w32(g::slot_out, 0);
        s.w32(g::slot_in, 0);
        const s32 reg = call_sysutil(ctx, "cellSysutil", "cellSysutilRegisterCallbackDispatcher",
            {kEvent, internal_descriptor("cellDtcpIpUtility", kHandler)});
        if (reg != 0) {
            s.print("cellSysutilRegisterCallbackDispatcher: failed:0x%x\n", {reg});
            lv2_fn(ctx, "sys_lwmutex_destroy", {s.lw()});
            s.reset();
            return err(DTCP_ERROR_DISPATCHER);
        }
        StackVar file(ctx, s.m.base(), 8);
        s.m.w32(file.addr(), static_cast<u32>(flags >> 32));
        s.m.w32(file.addr() + 4, static_cast<u32>(flags));
        const Attr attrs[] = {Attr::i("dtcpIpMemContainer", container), Attr::i("dtcpIpPpuThreadPriority", priority),
            Attr::f("dtcpIpFunctionFlag", file.addr(), 8)};
        return request(s, "Initialize", 0x2F8, "dtcpIpInitialize", 3, attrs, 0, 0, [&](const Outcome& o, auto&& end) {
            if (o.sent && o.r == 0) {
                end();
                return 0;
            }
            if (o.sent) s.print("ERROR : receiveResponsePacket() = 0x%x.\n", {o.r});
            end();
            call_sysutil(ctx, "cellSysutil", "cellSysutilUnregisterCallbackDispatcher", {kEvent});
            lv2_fn(ctx, "sys_lwmutex_destroy", {s.lw()});
            s.reset();
            return o.r;
        });
    });
}

// cellDtcpIpInitialize: the move mode's bit cleared from the flags. The ClearTextByte library's 0x545E08CB: as given.
s32 cellDtcpIpInitialize(PPUContext& ctx, u32 container, u32 callback, u32 userdata, u32 priority, u64 flags)
{
    return initialize(ctx, "cellDtcpIpInitialize", container, callback, userdata, priority, flags & ~kMove);
}
s32 cellDtcpIpInitializeEx(PPUContext& ctx, u32 container, u32 callback, u32 userdata, u32 priority, u64 flags)
{
    return initialize(ctx, "cellDtcpIpUtilityClearTextByte_545E08CB", container, callback, userdata, priority, flags);
}

// cellDtcpIpFinalize(): "dtcpIpFinalize" (no callback: not initialized); a failure finalizes here (the system's
// event 2 does it otherwise).
s32 cellDtcpIpFinalize(PPUContext& ctx)
{
    return dtcp(ctx, "cellDtcpIpFinalize", [&](Dtcp& s) -> s32 {
        if (!s.r32(g::callback)) {
            s.where("%s:%s(%d)\t system_callback_ is NULL\n", "cellDtcpIpFinalize", 0x33E);
            return err(DTCP_ERROR_STATE);
        }
        return request(s, "cellDtcpIpFinalize", 0x348, "dtcpIpFinalize", 0, {}, 0, 0,
            [&](const Outcome& o, auto&& end) {
                if (o.sent && o.r >= 0) {
                    end();
                    return o.r;
                }
                if (o.sent) s.print("ERROR : receiveResponsePacket() = 0x%x.\n", {o.r});
                s.finalize();
                s.reset();
                end();
                return o.r;
            });
    });
}

s32 cellDtcpIpSuspendActivationForDebug(PPUContext&) { return 0; }

// ------------------------------------------------------------------------------------------------- requests

s32 cellDtcpIpActivate(PPUContext& ctx)
{
    return dtcp(ctx, "cellDtcpIpActivate", [&](Dtcp& s) -> s32 {
        if (no_memory(s, "cellDtcpIpActivate", 0x393)) return err(DTCP_ERROR_STATE);
        return simple(s, "cellDtcpIpActivate", 0x39D, "dtcpIpActivate");
    });
}

s32 cellDtcpIpCheckActivation(PPUContext& ctx)
{
    return dtcp(ctx, "cellDtcpIpCheckActivation", [&](Dtcp& s) -> s32 {
        if (no_memory(s, "cellDtcpIpCheckActivation", 0x36D)) return err(DTCP_ERROR_STATE);
        return simple(s, "cellDtcpIpCheckActivation", 0x377, "dtcpIpCheckActivation");
    });
}

s32 cellDtcpIpClose(PPUContext& ctx)
{
    return dtcp(ctx, "cellDtcpIpClose", [&](Dtcp& s) -> s32 {
        if (no_memory(s, "cellDtcpIpClose", 0x50E)) return err(DTCP_ERROR_STATE);
        return simple(s, "cellDtcpIpClose", 0x518, "dtcpIpClose");
    });
}

s32 cellDtcpIpStartSequence(PPUContext& ctx)
{
    return dtcp(ctx, "cellDtcpIpStartSequence", [&](Dtcp& s) -> s32 {
        if (no_memory(s, "cellDtcpIpStartSequence", 0x5B5)) return err(DTCP_ERROR_STATE);
        return simple(s, "cellDtcpIpStartSequence", 0x5BF, "dtcpIpStartSequence");
    });
}

// cellDtcpIpStopSequence(): the three slots marked stopped first; "dtcpIpStopSequence"; the cancel set when answered.
s32 cellDtcpIpStopSequence(PPUContext& ctx)
{
    return dtcp(ctx, "cellDtcpIpStopSequence", [&](Dtcp& s) -> s32 {
        if (no_memory(s, "cellDtcpIpStopSequence", 0x5DA)) return err(DTCP_ERROR_STATE);
        s.lock();
        for (const u32 i : {2u, 0u, 1u}) s.w8(g::slots + i, 0xFE);
        s.unlock();
        return request(s, "cellDtcpIpStopSequence", 0x5EB, "dtcpIpStopSequence", 0, {}, 0, 0,
            [&](const Outcome& o, auto&& end) {
                if (o.sent && o.r < 0) {
                    s.print("ERROR : receiveResponsePacket() = 0x%x.\n", {o.r});
                } else if (o.sent) {
                    s.lock();
                    s.w8(g::cancel, 1);
                    s.unlock();
                }
                end();
                return o.r;
            });
    });
}

// The ClearTextByte library's plain requests: not in the buffering mode.
s32 clear_text_request(PPUContext& ctx, std::string_view func, s32 line_memory, s32 line_mode, s32 line_element,
    std::string_view name)
{
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (no_memory(s, func, line_memory)) return err(DTCP_ERROR_STATE);
        if (buffering(s, func, line_mode)) return err(DTCP_ERROR_STATE);
        return simple(s, func, line_element, name);
    });
}

s32 cellDtcpIpCloseContent(PPUContext& ctx)
{
    return clear_text_request(ctx, "cellDtcpIpCloseContent", 0x58A, 0x590, 0x59A, "dtcpIpCloContent");
}
s32 cellDtcpIpStopAuthentication(PPUContext& ctx)
{
    return clear_text_request(ctx, "cellDtcpIpStopAuthentication", 0x55F, 0x565, 0x56F, "dtcpIpStopAuth");
}
s32 cellDtcpIpFinalizeAuthentication(PPUContext& ctx)
{
    return clear_text_request(ctx, "cellDtcpIpFinalizeAuthentication", 0x534, 0x53A, 0x544, "dtcpIpFinalAuth");
}
s32 cellDtcpIpInitializeAuthentication(PPUContext& ctx)
{
    return clear_text_request(ctx, "cellDtcpIpInitializeAuthentication", 0x4A9, 0x4AF, 0x4B9, "dtcpIpInitAuth");
}

// The host's address and port (CellDtcpIpHostInfo: the address string's pointer, then a 16-bit port).
bool host_info(Dtcp& s, std::string_view func, s32 line_null, s32 line_ip, u32 host)
{
    if (!host) {
        s.where("%s:%s(%d)\thostInfo is NULL !!!\n", func, line_null);
        return false;
    }
    if (!s.m.r32(host)) {
        s.where("%s:%s(%d)\thostInfo->hostIp is NULL !!!\n", func, line_ip);
        return false;
    }
    return true;
}

// cellDtcpIpStartAuthentication(host): "dtcpIpStartAuth" with the host's address and port.
s32 cellDtcpIpStartAuthentication(PPUContext& ctx, u32 host)
{
    constexpr std::string_view func = "cellDtcpIpStartAuthentication";
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (no_memory(s, func, 0x4D4)) return err(DTCP_ERROR_STATE);
        if (buffering(s, func, 0x4DA)) return err(DTCP_ERROR_STATE);
        if (!host_info(s, func, 0x4DF, 0x4E4, host)) return err(DTCP_ERROR_PARAM);
        const Attr attrs[] = {Attr::s("dtcpIpHostIp", s.m.r32(host)), Attr::i("dtcpIpHostPort", host_port(s.m, host))};
        return simple(s, func, 0x4F2, "dtcpIpStartAuth", 2, attrs);
    });
}

// cellDtcpIpGetHttpStatusCode(code): the code zeroed, then "dtcpIpGetHttpStatusCode", whose reply's "param" is it.
s32 cellDtcpIpGetHttpStatusCode(PPUContext& ctx, u32 code)
{
    constexpr std::string_view func = "cellDtcpIpGetHttpStatusCode";
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (buffering(s, func, 0xA83)) return err(DTCP_ERROR_STATE);
        if (no_memory(s, func, 0xA89)) return err(DTCP_ERROR_STATE);
        if (!code) {
            s.where("%s:%s(%d)\thttpStatusCode is NULL !!!\n", func, 0xA8E);
            return err(DTCP_ERROR_PARAM);
        }
        s.lock();
        if (cancelled(s, func, 0xA94)) return err(DTCP_ERROR_CANCELLED);
        s.unlock();
        s.m.w32(code, 0);
        return request(s, func, 0xAA2, "dtcpIpGetHttpStatusCode", 0, {}, 1, code, [&](const Outcome& o, auto&& end) {
            if (o.sent && o.r < 0) s.print("ERROR : receiveResponsePacket() = 0x%x.\n", {o.r});
            end();
            return o.r;
        });
    });
}

// ------------------------------------------------------------------------------------------------- opening

// The open option block (28 bytes): a byte (0 for Open, 1 for the others; the three after it never written, which the
// module leaves as its stack has them: 0 here), option 1, options 2 to 5 in KB, option 20000's 1 (else -1).
struct OpenOptions {
    u8 first = 1;
    u32 o1 = 0, o2 = 0, o3 = 0, o4 = 0, o5 = 0, o20000 = 0xFFFFFFFF;
    void put(Mem& m, u32 at) const
    {
        m.w8(at, first);
        for (u32 i = 1; i < 4; i++) m.w8(at + i, 0);
        m.w32(at + 4, o1);
        m.w32(at + 8, o2);
        m.w32(at + 12, o3);
        m.w32(at + 16, o4);
        m.w32(at + 20, o5);
        m.w32(at + 24, o20000);
    }
};

// The option lists' check (the game's arrays of `count` IDs and values): each ID the function takes with its limit and
// the line of its complaint. Answers 0 or the parameter error.
struct OptionRule {
    u32 id;
    u32 limit; // the largest value, or ~0 for any; 20000 takes only 1
    s32 line;
};
s32 read_options(Dtcp& s, std::string_view func, u32 count, u32 ids, u32 values, s32 line_ids, s32 line_values,
    s32 line_invalid, std::span<const OptionRule> rules, OpenOptions& o)
{
    if (!count) return 0;
    if (!ids) {
        s.where("%s:%s(%d)\toptionId is NULL !!!\n", func, line_ids);
        return err(DTCP_ERROR_PARAM);
    }
    if (!values) {
        s.where("%s:%s(%d)\toptionValue is NULL !!!\n", func, line_values);
        return err(DTCP_ERROR_PARAM);
    }
    for (u32 i = 0; i < count; i++) {
        const u32 id = s.m.r32(ids + 4 * i);
        const OptionRule* rule = nullptr;
        for (const OptionRule& r : rules)
            if (r.id == id) rule = &r;
        if (!rule) {
            s.print("%s:%s(%d)\toptionId:%d is invalid !!!\n", {kFile, func, line_invalid, static_cast<s32>(id)});
            return err(DTCP_ERROR_PARAM);
        }
        const u32 v = s.m.r32(values + 4 * i);
        const bool bad = id == 20000 ? v != 1 : rule->limit != ~0u && v > rule->limit;
        if (bad) {
            s.print("%s:%s(%d)\toptionId:%d, optionValue:%d is invalid !!!\n",
                {kFile, func, rule->line, static_cast<s32>(id), static_cast<s32>(v)});
            return err(DTCP_ERROR_PARAM);
        }
        switch (id) {
        case 1: o.o1 = v; break;
        case 2: o.o2 = v << 10; break;
        case 3: o.o3 = v << 10; break;
        case 4: o.o4 = v << 10; break;
        case 5: o.o5 = v << 10; break;
        case 20000: o.o20000 = 1; break;
        }
    }
    return 0;
}

// The open request ("dtcpIpOpen": the URL unless buffering, the host, the move mode, the options' file).
s32 open_request(Dtcp& s, std::string_view func, s32 line, u32 url, u32 host, u32 move, const OpenOptions& o,
    bool clears_cancel)
{
    StackVar block(s.ctx, s.m.base(), 32);
    o.put(s.m, block.addr());
    Attr attrs[5];
    usize n = 0;
    if (!(s.flags() & kBuffering)) attrs[n++] = Attr::s("dtcpIpContentUrl", url);
    attrs[n++] = Attr::s("dtcpIpHostIp", s.m.r32(host));
    attrs[n++] = Attr::i("dtcpIpHostPort", host_port(s.m, host));
    attrs[n++] = Attr::i("dtcpIpOpenMoveMode", move);
    attrs[n++] = Attr::f("dtcpIpOpenOption", block.addr(), 0x1C);
    return request(s, func, line, "dtcpIpOpen", 5, std::span<const Attr>(attrs, n), 0, 0,
        [&](const Outcome& o2, auto&& end) {
            if (o2.sent && o2.r < 0) s.print("ERROR : receiveResponsePacket() = 0x%x.\n", {o2.r});
            else if (o2.sent && clears_cancel) s.w8(g::cancel, 0);
            end();
            return o2.r;
        });
}

// cellDtcpIpOpen(url, host): not in the move mode; the URL needed unless buffering.
s32 cellDtcpIpOpen(PPUContext& ctx, u32 url, u32 host)
{
    constexpr std::string_view func = "cellDtcpIpOpen";
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (no_memory(s, func, 0x3C4)) return err(DTCP_ERROR_STATE);
        if (s.flags() & kMove) {
            s.where("%s:%s(%d)\tOpen is unavailable\n", func, 0x3CA);
            return err(DTCP_ERROR_STATE);
        }
        if (!(s.flags() & kBuffering) && !url) {
            s.where("%s:%s(%d)\turl is NULL !!!\n", func, 0x3D1);
            return err(DTCP_ERROR_PARAM);
        }
        if (!host_info(s, func, 0x3D7, 0x3DC, host)) return err(DTCP_ERROR_PARAM);
        OpenOptions o;
        o.first = 0;
        return open_request(s, func, 0x3FB, url, host, 0, o, false);
    });
}

// cellDtcpIpOpenEx(url, host, count, ids, values): the buffering mode only; options 3, 4 (at most 128) and 5.
s32 cellDtcpIpOpenEx(PPUContext& ctx, u32 url, u32 host, u32 count, u32 ids, u32 values)
{
    constexpr std::string_view func = "cellDtcpIpOpenEx";
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (no_memory(s, func, 0x421)) return err(DTCP_ERROR_STATE);
        if (!(s.flags() & kBuffering)) {
            s.where("%s:%s(%d)\tnot CELL_DTCP_IP_FUNCTION_FLAG_BUFFERING_MODE !!!\n", func, 0x427);
            return err(DTCP_ERROR_STATE);
        }
        if (!host_info(s, func, 0x435, 0x43A, host)) return err(DTCP_ERROR_PARAM);
        OpenOptions o;
        static constexpr OptionRule rules[] = {{4, 0x80, 0x460}, {5, ~0u, 0}, {3, 0x80, 0x453}};
        if (const s32 r = read_options(s, func, count, ids, values, 0x440, 0x444, 0x469, rules, o)) return r;
        return open_request(s, func, 0x48B, url, host, 0, o, true);
    });
}

// cellDtcpIpMoveOpenEx(url, host, count, ids, values): options 2 (at most 1024 KB), 3, 4 (at most 128) and 5; in the
// move mode the data connection's buffer must be set (1 to 256 KB).
s32 cellDtcpIpMoveOpenEx(PPUContext& ctx, u32 url, u32 host, u32 count, u32 ids, u32 values)
{
    constexpr std::string_view func = "cellDtcpIpMoveOpenEx";
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (no_memory(s, func, 0x864)) return err(DTCP_ERROR_STATE);
        if (!url) {
            s.where("%s:%s(%d)\turl is NULL !!!\n", func, 0x869);
            return err(DTCP_ERROR_PARAM);
        }
        if (!host) {
            s.where("%s:%s(%d)\thostInfo is NULL !!!\n", func, 0x86E);
            return err(DTCP_ERROR_PARAM);
        }
        if (!s.m.r32(host)) {
            s.where("%s:%s(%d)\thostInfo->hostIp is NULL !!!\n", func, 0x873);
            return err(DTCP_ERROR_PARAM);
        }
        OpenOptions o;
        static constexpr OptionRule rules[] = {{3, 0x80, 0x89A}, {2, 0x400, 0x88D}, {4, 0x80, 0x8A7}, {5, ~0u, 0}};
        if (const s32 r = read_options(s, func, count, ids, values, 0x879, 0x87D, 0x8B0, rules, o)) return r;
        if ((s.flags() & kMove) && o.o2 - 1 > 0x3FFFF) {
            s.where("%s:%s(%d)\tCELL_DTCP_IP_MOVE_OPEN_OPTION_ID_DATA_CONNECTION_BUFFER_SIZE error !!!\n", func, 0x8BE);
            return err(DTCP_ERROR_PARAM);
        }
        return open_request(s, func, 0x8E0, url, host, 1, o, true);
    });
}

// cellDtcpIpMoveOpen(url, host): MoveOpenEx with no options, unless in the move mode.
s32 cellDtcpIpMoveOpen(PPUContext& ctx, u32 url, u32 host)
{
    {
        Dtcp s(ctx);
        if (s.flags() & kMove) {
            s.print("%s:%s(%d)\tOpen is unavailable\n", {kFile, "cellDtcpIpMoveOpen", 0x84F, 0});
            return err(DTCP_ERROR_STATE);
        }
    }
    return cellDtcpIpMoveOpenEx(ctx, url, host, 0, 0, 0);
}

// cellDtcpIpOpenContentEx(url, count, ids, values): not in the buffering mode; "dtcpIpOpContent" with the URL and the
// options' file (1 at most 2, 2 at most 1024 KB, 3 and 4 at most 128, 5, 20000 only 1).
s32 cellDtcpIpOpenContentEx(PPUContext& ctx, u32 url, u32 count, u32 ids, u32 values)
{
    constexpr std::string_view func = "cellDtcpIpOpenContentEx";
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (no_memory(s, func, 0x9DD)) return err(DTCP_ERROR_STATE);
        if (buffering(s, func, 0x9E3)) return err(DTCP_ERROR_STATE);
        if (!url) {
            s.where("%s:%s(%d)\turl is NULL !!!\n", func, 0x9E8);
            return err(DTCP_ERROR_PARAM);
        }
        OpenOptions o;
        static constexpr OptionRule rules[] = {
            {3, 0x80, 0xA1C}, {1, 2, 0xA01}, {2, 0x400, 0xA0F}, {5, ~0u, 0}, {4, 0x80, 0xA29}, {20000, 1, 0xA39}};
        if (const s32 r = read_options(s, func, count, ids, values, 0x9EE, 0x9F2, 0xA3E, rules, o)) return r;
        if ((s.flags() & kMove) && o.o2 - 1 > 0x3FFFF) {
            s.where("%s:%s(%d)\tCELL_DTCP_IP_OPEN_OPTION_ID_DATA_CONNECTION_BUFFER_SIZE error !!!\n", func, 0xA4C);
            return err(DTCP_ERROR_PARAM);
        }
        StackVar block(ctx, s.m.base(), 32);
        o.put(s.m, block.addr());
        const Attr attrs[] = {Attr::s("dtcpIpContentUrl", url), Attr::f("dtcpIpOpenOption", block.addr(), 0x1C)};
        return simple(s, func, 0xA67, "dtcpIpOpContent", 2, attrs);
    });
}

// ------------------------------------------------------------------------------------------------- moving

s32 cellDtcpIpMoveClose(PPUContext& ctx)
{
    constexpr std::string_view func = "cellDtcpIpMoveClose";
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (no_memory(s, func, 0x97E)) return err(DTCP_ERROR_STATE);
        return request(s, func, 0x988, "dtcpIpClose", 0, {}, 0, 0, [&](const Outcome& o, auto&& end) {
            if (o.sent && o.r < 0) s.print("ERROR : receiveResponsePacket() = 0x%x.\n", {o.r});
            else if (o.sent) s.w8(g::cancel, 1);
            end();
            return o.r;
        });
    });
}

s32 cellDtcpIpMoveCommitment(PPUContext& ctx)
{
    constexpr std::string_view func = "cellDtcpIpMoveCommitment";
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (no_memory(s, func, 0x958)) return err(DTCP_ERROR_STATE);
        return simple(s, func, 0x962, "dtcpIpMoveCommitment");
    });
}

// ------------------------------------------------------------------------------------------------- reading

// Read and MoveRead(buffer, size): not in the buffering mode; at most 128 KB; one read at a time (the buffer kept for
// the system's event 9, which copies the data); "dtcpIpRead" with the size. A failure forgets the buffer.
s32 read(PPUContext& ctx, std::string_view func, bool checks_cancel, s32 l_mode, s32 l_buf, s32 l_zero, s32 l_big,
    s32 l_memory, s32 l_cancel, s32 l_busy, s32 l_element, u32 buf, u32 size)
{
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (buffering(s, func, l_mode)) return err(DTCP_ERROR_STATE);
        if (!buf) {
            s.where("%s:%s(%d)\tbuf is NULL !!!\n", func, l_buf);
            return err(DTCP_ERROR_PARAM);
        }
        if (!size) {
            s.where("%s:%s(%d)\tsizeToRead is 0 !!!\n", func, l_zero);
            return err(DTCP_ERROR_PARAM);
        }
        if (size > 0x20000) {
            s.print("%s:%s(%d)\tsizeToRead[0x%x] > 0x%x !!!\n", {kFile, func, l_big, size, 0x20000});
            return err(DTCP_ERROR_PARAM);
        }
        if (no_memory(s, func, l_memory)) return err(DTCP_ERROR_STATE);
        s.lock();
        if (checks_cancel && cancelled(s, func, l_cancel)) return err(DTCP_ERROR_CANCELLED);
        if (s.r32(g::read_dest)) {
            s.where("%s:%s(%d)\tbusy !!!\n", func, l_busy);
            s.unlock();
            return err(DTCP_ERROR_BUSY);
        }
        s.w32(g::read_dest, buf);
        s.w32(g::read_size, size);
        s.unlock();
        const Attr attrs[] = {Attr::i("dtcpIpSizeToRead", size)};
        return request(s, func, l_element, "dtcpIpRead", 1, attrs, 0, 0, [&](const Outcome& o, auto&& end) {
            if (o.sent && o.r >= 0) {
                end();
                return o.r;
            }
            if (o.sent) s.print("ERROR : receiveResponsePacket() = 0x%x.\n", {o.r});
            s.lock();
            s.w32(g::read_size, 0);
            s.w32(g::read_dest, 0);
            s.unlock();
            end();
            return o.r;
        });
    });
}

s32 cellDtcpIpRead(PPUContext& ctx, u32 buf, u32 size)
{
    return read(ctx, "cellDtcpIpRead", true, 0x6DA, 0x6DF, 0x6E4, 0x6EA, 0x6F0, 0x6F6, 0x6FC, 0x70D, buf, size);
}
s32 cellDtcpIpMoveRead(PPUContext& ctx, u32 buf, u32 size)
{
    return read(ctx, "cellDtcpIpMoveRead", false, 0x8FD, 0x902, 0x907, 0x90D, 0x913, 0, 0x921, 0x932, buf, size);
}

// The seek requests: one at a time (the flag cleared by the system's event 8 or 0x2000002, or by a failure here).
template <class Attrs>
s32 seek(Dtcp& s, std::string_view func, s32 l_cancel, s32 l_busy, s32 l_element, s32 count, Attrs&& attrs)
{
    s.lock();
    if (cancelled(s, func, l_cancel)) return err(DTCP_ERROR_CANCELLED);
    if (s.r8(g::seeking)) {
        s.where("%s:%s(%d)\tbusy !!!\n", func, l_busy);
        s.unlock();
        return err(DTCP_ERROR_BUSY);
    }
    s.w8(g::seeking, 1);
    s.unlock();
    return request(s, func, l_element, "dtcpIpSeek", count, attrs, 0, 0, [&](const Outcome& o, auto&& end) {
        if (o.sent && o.r >= 0) {
            end();
            return o.r;
        }
        if (o.sent) s.print("ERROR : receiveResponsePacket() = 0x%x.\n", {o.r});
        s.lock();
        s.w8(g::seeking, 0);
        s.unlock();
        end();
        return o.r;
    });
}

// cellDtcpIpSeek(time): "dtcpIpSeek" with the time as an 8-byte file and the kind 0.
s32 cellDtcpIpSeek(PPUContext& ctx, u64 time)
{
    constexpr std::string_view func = "cellDtcpIpSeek";
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (buffering(s, func, 0x60B)) return err(DTCP_ERROR_STATE);
        if (no_memory(s, func, 0x611)) return err(DTCP_ERROR_STATE);
        StackVar t(ctx, s.m.base(), 8);
        s.m.w32(t.addr(), static_cast<u32>(time >> 32));
        s.m.w32(t.addr() + 4, static_cast<u32>(time));
        const Attr attrs[] = {Attr::f("dtcpIpSeekTime", t.addr(), 8), Attr::i("dtcpIpSeekKind", 0)};
        return seek(s, func, 0x617, 0x61D, 0x63A, 2, std::span<const Attr>(attrs));
    });
}

// cellDtcpIpSetByteSeekRange(first, last): both below 2^47 - 1, the last 0 or not before the first; "dtcpIpSeek" with
// both as 8-byte files and the kind 1.
s32 cellDtcpIpSetByteSeekRange(PPUContext& ctx, u64 first, u64 last)
{
    constexpr std::string_view func = "cellDtcpIpSetByteSeekRange";
    constexpr u64 kMax = 0x7FFFFFFFFFFEull;
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (buffering(s, func, 0x660)) return err(DTCP_ERROR_STATE);
        if (no_memory(s, func, 0x666)) return err(DTCP_ERROR_STATE);
        if (first > kMax || last > kMax) {
            s.print("%s:%s(%d)\tparam error ? first[0x%llx]>%llx or last[0x%llx]>%llx\n",
                {kFile, func, 0x66E, first, kMax, last, kMax});
            return err(DTCP_ERROR_PARAM);
        }
        if (last != 0 && last < first) {
            s.print("%s:%s(%d)\tparam error ? first[0x%llx] > last[0x%llx]\n", {kFile, func, 0x67B, first, last});
            return err(DTCP_ERROR_PARAM);
        }
        StackVar t(ctx, s.m.base(), 16);
        s.m.w32(t.addr(), static_cast<u32>(first >> 32));
        s.m.w32(t.addr() + 4, static_cast<u32>(first));
        s.m.w32(t.addr() + 8, static_cast<u32>(last >> 32));
        s.m.w32(t.addr() + 12, static_cast<u32>(last));
        const Attr attrs[] = {Attr::f("dtcpIpSeekPos", t.addr(), 8), Attr::f("dtcpIpSeekLastPos", t.addr() + 8, 8),
            Attr::i("dtcpIpSeekKind", 1)};
        return seek(s, func, 0x684, 0x68A, 0x6B6, 3, std::span<const Attr>(attrs));
    });
}

// ------------------------------------------------------------------------------------------------- buffering

// The encrypted stream copied into the 1 MB ring from its write position: up to the ring's end (when the system has
// read past it) or up to the system's read position, at most what is free; the bytes copied added to `copied`.
void ring_copy(Dtcp& s, u32 src, u32 size, u32 copied)
{
    s.m.w32(copied, 0);
    u32 n = kRing - s.r32(g::used);
    if (n > size) n = size;
    if (!n) return;
    for (u32 pass = 0;;) {
        const u32 write = s.r32(g::ring_write), read_at = s.r32(g::ring_read);
        u32 chunk = 0;
        if (pass == 0) {
            if (read_at > write) {
                pass = 1;
                if (!n) return;
                continue;
            }
            chunk = kRing - write;
        } else if (read_at > write) {
            chunk = read_at - write;
        }
        if (chunk) {
            const u32 c = chunk > n ? n : chunk;
            sys_memcpy_guest(s.ctx, s.r32(g::ring) + write, src + s.m.r32(copied), c);
            const u32 w = s.r32(g::ring_write) + c;
            s.w32(g::ring_write, w);
            if (w > 0xFFFFF) s.w32(g::ring_write, 0);
            const u32 used = s.r32(g::used) + c;
            s.w32(g::used, used);
            if (used != kRing) s.w32(g::used, used & 0xFFFFF);
            n -= c;
            s.m.w32(copied, s.m.r32(copied) + c);
        }
        if (++pass > 1 || !n) return;
    }
}

// cellDtcpIpSetEncryptedDataEx(data, size, eof, copied, free, low bitrate): the buffering mode only; the data copied
// into the ring (what does not fit is not, and the end of the file not told then); the ring's free space given; then,
// when something was copied or the file ended, "dtcpIpSetEncryptedData" with the end of the file, the ring's write
// position, the size copied and the low bitrate.
s32 set_encrypted(PPUContext& ctx, std::string_view what, u32 data, u32 size, u32 eof, u32 copied, u32 free_out,
    u32 low_bitrate)
{
    constexpr std::string_view func = "cellDtcpIpSetEncryptedDataEx";
    return dtcp(ctx, what, [&](Dtcp& s) -> s32 {
        StackVar own(ctx, s.m.base(), 16);
        if (!copied) copied = own.addr();
        if (no_memory(s, func, 0x781)) return err(DTCP_ERROR_STATE);
        if (!(s.flags() & kBuffering)) {
            s.where("%s:%s(%d)\tNOT CELL_DTCP_IP_FUNCTION_FLAG_BUFFERING_MODE !!!\n", func, 0x786);
            return err(DTCP_ERROR_STATE);
        }
        s.lock();
        if (cancelled(s, func, 0x78D)) return err(DTCP_ERROR_CANCELLED);
        s.m.w32(copied, 0);
        if (free_out) s.m.w32(free_out, 0);
        if (size) {
            if (!data) {
                s.unlock();
                return err(DTCP_ERROR_PARAM);
            }
            ring_copy(s, data, size, copied);
            if (s.m.r32(copied) < size) eof = 0;
        }
        if (free_out) s.m.w32(free_out, kRing - s.r32(g::used));
        s.unlock();
        if (s.m.r32(copied) == 0 && eof != 1) return 0;
        const Attr attrs[] = {Attr::i("dtcpIpEncryptedFileEof", eof), Attr::i("dtcpIpEncryptedDataEnd", s.r32(g::ring_write)),
            Attr::i("dtcpIpSizeCopied", s.m.r32(copied)), Attr::i("dtcpIpIsLowBitrate", low_bitrate)};
        return simple(s, func, 0x7C6, "dtcpIpSetEncryptedData", 4, attrs);
    });
}

s32 cellDtcpIpSetEncryptedDataEx(PPUContext& ctx, u32 data, u32 size, u32 eof, u32 copied, u32 free_out, u32 low)
{
    return set_encrypted(ctx, "cellDtcpIpSetEncryptedDataEx", data, size, eof, copied, free_out, low);
}
s32 cellDtcpIpSetEncryptedData(PPUContext& ctx, u32 data, u32 size, u32 eof, u32 copied, u32 free_out)
{
    return set_encrypted(ctx, "cellDtcpIpSetEncryptedData", data, size, eof, copied, free_out, 0);
}

// cellDtcpIpGetDecryptedData(buf, size, got, emi): the buffering mode only; the next slot of decrypted data (stopped:
// cancelled; empty: 0x80024E1E) copied out with its EMI, the slot freed, then "dtcpIpGetDecryptedData" with its index.
s32 cellDtcpIpGetDecryptedData(PPUContext& ctx, u32 buf, u32 size, u32 got, u32 emi)
{
    constexpr std::string_view func = "cellDtcpIpGetDecryptedData";
    return dtcp(ctx, func, [&](Dtcp& s) -> s32 {
        if (!buf) return err(DTCP_ERROR_PARAM);
        if (!got) {
            s.where("%s:%s(%d)\t sizeGot is NULL !!!\n", func, 0x7EB);
            return err(DTCP_ERROR_PARAM);
        }
        if (!emi) {
            s.where("%s:%s(%d)\t emi is NULL !!!\n", func, 0x7F0);
            return err(DTCP_ERROR_PARAM);
        }
        if (no_memory(s, func, 0x7F6)) return err(DTCP_ERROR_STATE);
        if (!(s.flags() & kBuffering)) {
            s.where("%s:%s(%d)\tNOT CELL_DTCP_IP_FUNCTION_FLAG_BUFFERING_MODE !!!\n", func, 0x7FB);
            return err(DTCP_ERROR_STATE);
        }
        s.lock();
        if (cancelled(s, func, 0x802)) return err(DTCP_ERROR_CANCELLED);
        const u32 at = s.r32(g::slot_out);
        const s32 slot = static_cast<s8>(s.r8(g::slots + at));
        if (slot < 0) {
            s.unlock();
            return err(slot == -2 ? DTCP_ERROR_CANCELLED : DTCP_ERROR_EMPTY);
        }
        const u32 n = s.r32(g::sizes + 4 * static_cast<u32>(slot));
        if (size < n) {
            s.print("%s:%s(%d)\tbufSize[%d] is small (<%d) !!!\n",
                {kFile, func, 0x814, static_cast<s32>(size), static_cast<s32>(n)});
            s.unlock();
            return err(DTCP_ERROR_PARAM);
        }
        if (n) sys_memcpy_guest(ctx, buf, s.r32(g::buffers + 4 * static_cast<u32>(slot)), n);
        s.m.w32(got, n);
        const u32 next = at + 1;
        s.w32(g::slot_out, next);
        s.m.w32(emi, s.r32(g::emi + 4 * static_cast<u32>(slot)));
        s.w8(g::slots + at, 0xFF);
        if (next > 2) s.w32(g::slot_out, next % 3);
        s.unlock();
        const Attr attrs[] = {Attr::i("dtcpIpDecryptedDataIndex", static_cast<u32>(slot))};
        return simple(s, func, 0x833, "dtcpIpGetDecryptedData", 1, attrs);
    });
}

// ------------------------------------------------------------------------------------------------- the system's events

// cellDtcpIpCallbackHandler(event, value): the dispatcher of the system's event 0x14A (pspocket's internal function).
// The event's kind is its top word and its status the next; the value's halves carry what the kind needs. Most pass
// on to the game's callback as (argument, kind, status, ...); 1 makes the shared memory (status 0), 2 finalizes, 6
// resets the buffers, 8 and 0x2000002 end a seek, 9 ends a read (copying it into the user's buffer), 10 frees ring
// space, 11 fills a slot of decrypted data.
s32 cellDtcpIpUtility_CallbackHandler(PPUContext& ctx, u64 event, u64 value)
{
    packets_start(ctx);
    guarded(ctx, kModule, "cellDtcpIpCallbackHandler", [&]() -> s32 {
        Dtcp s(ctx);
        const u32 kind = static_cast<u32>(event >> 32), status = static_cast<u32>(event);
        const u32 hi = static_cast<u32>(value >> 32), lo = static_cast<u32>(value);
        switch (kind) {
        case 0xB: {
            s.lock();
            const u32 in = s.r32(g::slot_in);
            s.w32(g::sizes + 4 * status, hi);
            s.w32(g::emi + 4 * status, lo);
            if (static_cast<s8>(s.r8(g::slots + in)) != -2) s.w8(g::slots + in, static_cast<u8>(status));
            s.w32(g::slot_in, in + 1 > 2 ? (in + 1) % 3 : in + 1);
            s.unlock();
            s.notify(0xB, 0, hi, 0);
            break;
        }
        case 1:
            if (status == 0) {
                const u32 size = (s.flags() & kMove ? 0 : 0xB0000) + 0xD0000;
                const s32 r = call_sysutil(ctx, "cellSysutil", "cellSysutilSharedMemoryAlloc",
                    {kPacket, kMemoryKey, size, 0xFFFFFFFF, 0x200, s.d + g::shm, 0, 0});
                if (r == 0) {
                    const u32 shm = s.r32(g::shm);
                    s.w32(g::ring, shm + 0x40000);
                    s.w32(g::buffers + 4, shm + 0x20000);
                    s.w32(g::buffers, shm);
                    s.w32(g::read_buffer, shm);
                } else {
                    s.print("%s:%s(%d)\tinit failed. ret(%#x)\n", {kFile, "cellDtcpIpCallbackHandler", 0x14A, r});
                }
            }
            s.notify(1, status, hi, 0);
            break;
        case 2:
            s.finalize();
            s.notify(2, status, 0, 0);
            s.reset();
            break;
        case 3: case 4: case 5: case 7: case 0xD:
        case 0x1000001: case 0x1000002: case 0x1000003: case 0x1000004: case 0x1000005: case 0x2000001:
            s.notify(kind, status, 0, 0);
            break;
        case 6:
            s.lock();
            s.w8(g::cancel, 0);
            s.w32(g::ring_read, 0);
            s.w32(g::ring_write, 0);
            s.w32(g::used, 0);
            for (u32 i = 0; i < 3; i++) s.w8(g::slots + i, 0xFF);
            s.w32(g::sizes + 4, 0);
            s.w32(g::emi + 4, 0);
            s.w32(g::sizes, 0);
            s.w32(g::emi, 0);
            s.w32(g::slot_out, 0);
            s.w32(g::slot_in, 0);
            s.unlock();
            s.notify(6, status, 0, 0);
            break;
        case 8: case 0x2000002: {
            s.lock();
            u32 st = status;
            if (status == 0 && s.r8(g::cancel)) st = DTCP_ERROR_CANCELLED;
            s.w8(g::seeking, 0);
            s.unlock();
            s.notify(kind, st, 0, 0);
            break;
        }
        case 9: {
            s.lock();
            u32 st = status;
            if (status == 0) {
                st = DTCP_ERROR_CANCELLED;
                if (!s.r8(g::cancel)) {
                    if (!s.r32(g::read_dest)) {
                        st = DTCP_ERROR_READ;
                        s.where("%s:%s(%d)\t READ CANCEL (dtcp_ip_client::user_read_buf is NULL)\n",
                            "cellDtcpIpCallbackHandler", 0x211);
                    } else if (s.r32(g::read_size) < hi) {
                        st = DTCP_ERROR_READ;
                        s.print("%s:%s(%d)\t buffer size[0x%x] < readsize[0x%x]\n",
                            {kFile, "cellDtcpIpCallbackHandler", 0x215, s.r32(g::read_size), hi});
                    } else if (!s.r32(g::read_buffer)) {
                        st = DTCP_ERROR_READ;
                        s.where("%s:%s(%d)\t shared_memory_read_buffer_ is NULL\n", "cellDtcpIpCallbackHandler", 0x219);
                    } else {
                        st = 0;
                        if (hi) sys_memcpy_guest(ctx, s.r32(g::read_dest), s.r32(g::read_buffer), hi);
                    }
                }
            }
            s.w32(g::read_dest, 0);
            s.unlock();
            s.notify(9, st, hi, lo);
            break;
        }
        case 10: {
            s.lock();
            const bool c = s.r8(g::cancel) != 0;
            const u32 used = s.r32(g::used) - lo;
            s.w32(g::used, used);
            s.w32(g::ring_read, hi);
            s.unlock();
            if (!c) s.notify(10, status, kRing - used, 0);
            break;
        }
        case 0xC: case 0x3000001:
            s.notify(kind, status, hi, lo);
            break;
        default: break;
        }
        return 0;
    });
    return 0;
}

} // namespace

u32 dtcp_data() { return data_block(); }

void register_dtcp(Registry& r)
{
    r.add("cellDtcpIpUtility", {
        PS_HLE_FUNC(0x9ADE62E5, cellDtcpIpInitialize),
        PS_HLE_FUNC(0x4659CA92, cellDtcpIpFinalize),
        PS_HLE_FUNC(0x48FF0164, cellDtcpIpActivate),
        PS_HLE_FUNC(0x9413678F, cellDtcpIpCheckActivation),
        PS_HLE_FUNC(0x8BD01379, cellDtcpIpOpen),
        PS_HLE_FUNC(0xE31BD1CA, cellDtcpIpClose),
        PS_HLE_FUNC(0x0836F1F7, cellDtcpIpRead),
        PS_HLE_FUNC(0xB89105CA, cellDtcpIpSeek),
        PS_HLE_FUNC(0xCB1E777F, cellDtcpIpStartSequence),
        PS_HLE_FUNC(0xB2176B06, cellDtcpIpStopSequence),
        PS_HLE_FUNC(0xE0ACE309, cellDtcpIpSetEncryptedData),
        PS_HLE_FUNC(0xAC14B8E3, cellDtcpIpGetDecryptedData),
        PS_HLE_FUNC(0xFB12B7FC, cellDtcpIpSuspendActivationForDebug),
        PS_HLE_INTERNAL(recomp::nid_of(kHandler), cellDtcpIpUtility_CallbackHandler),
    });
    r.add("cellDtcpIpUtilityClearTextByte", {
        Function{{}, "cellDtcpIpUtilityClearTextByte_545E08CB", 0x545E08CB, &detail::thunk<&cellDtcpIpInitializeEx>, false},
        Function{{}, "cellDtcpIpUtilityClearTextByte_7F4A8B0E", 0x7F4A8B0E,
            &detail::thunk<&cellDtcpIpInitializeAuthentication>, false},
        Function{{}, "cellDtcpIpUtilityClearTextByte_0AA9D63F", 0x0AA9D63F,
            &detail::thunk<&cellDtcpIpStartAuthentication>, false},
        Function{{}, "cellDtcpIpUtilityClearTextByte_1EC068FB", 0x1EC068FB,
            &detail::thunk<&cellDtcpIpFinalizeAuthentication>, false},
        Function{{}, "cellDtcpIpUtilityClearTextByte_550D3527", 0x550D3527,
            &detail::thunk<&cellDtcpIpStopAuthentication>, false},
        Function{{}, "cellDtcpIpUtilityClearTextByte_97619DB5", 0x97619DB5, &detail::thunk<&cellDtcpIpCloseContent>,
            false},
        Function{{}, "cellDtcpIpUtilityClearTextByte_2F87926F", 0x2F87926F, &detail::thunk<&cellDtcpIpOpenContentEx>,
            false},
        Function{{}, "cellDtcpIpUtilityClearTextByte_2070C99E", 0x2070C99E,
            &detail::thunk<&cellDtcpIpSetByteSeekRange>, false},
        Function{{}, "cellDtcpIpUtilityClearTextByte_65A86A64", 0x65A86A64,
            &detail::thunk<&cellDtcpIpGetHttpStatusCode>, false},
    });
    r.add("cellDtcpIpUtilityMove", {
        Function{{}, "cellDtcpIpUtilityMove_269A634B", 0x269A634B, &detail::thunk<&cellDtcpIpMoveOpenEx>, false},
        Function{{}, "cellDtcpIpUtilityMove_F4EF1B28", 0xF4EF1B28, &detail::thunk<&cellDtcpIpMoveOpen>, false},
        Function{{}, "cellDtcpIpUtilityMove_A98DEADE", 0xA98DEADE, &detail::thunk<&cellDtcpIpMoveClose>, false},
        Function{{}, "cellDtcpIpUtilityMove_9A219C98", 0x9A219C98, &detail::thunk<&cellDtcpIpMoveCommitment>, false},
        Function{{}, "cellDtcpIpUtilityMove_CA87F652", 0xCA87F652, &detail::thunk<&cellDtcpIpMoveRead>, false},
    });
    r.add("cellDtcpIpUtilityEx", {
        Function{{}, "cellDtcpIpUtilityEx_B17436B5", 0xB17436B5, &detail::thunk<&cellDtcpIpSetEncryptedDataEx>, false},
        Function{{}, "cellDtcpIpUtilityEx_4654188A", 0x4654188A, &detail::thunk<&cellDtcpIpOpenEx>, false},
    });
}

} // namespace pspocket::hle::sysutil::sub
