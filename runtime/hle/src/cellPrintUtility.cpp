// cellPrintUtility: printing through the system software (D-925). Read from libsysutil_print.sprx (4.92) and compared
// with it call for call (tests/hle/test_sysutil_more.cpp). RPCS3 leaves these ten stubs (cellPrint.cpp, whose error
// codes these follow).
//
// The module keeps one callback {function, userdata}, which its loads and configuration set (cellPrintLoadAsync and
// the others pspocket answers at once, as RPCS3: sysutil_misc.cpp, which keep none) and the system's event 0x121 calls
// once through the callback dispatcher cellSysutilPrintInit registers (the module's start calls it, its stop calls
// cellSysutilPrintShutdown; pspocket loads no module, so a game that calls neither has no dispatcher). Each other
// function is a request on libsysutil's channel 0, packet 0x11C (magic "PRIN"), whose packet must begin (its error is
// the answer): an element "p_..." with the function's arguments, the reply's "result" (negative: the answer, printed as
// "error from server") and what the function reads; a function that succeeds clears the callback. The failures are
// printed through liblv2's printf as the module prints them. cellPrintSendBand sends on channel 1: its document, then
// the band as the same packet's raw data, the reply's "p_send_size" the size sent. On pspocket's console every send
// answers 0x8002b007 (D-534), printed as "doc.Write failed".

#include <mutex>
#include <string_view>

#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/recomp/nid.hpp"
#include "sysutil_sub.hpp"

namespace pspocket::hle::sysutil::sub {

namespace {

constexpr std::string_view kModule = "libsysutil_print";
constexpr s32 kPacket = 0x11C;
constexpr u32 kEvent = 0x121; // the system's print events
constexpr u32 CELL_PRINT_ERROR_INVALID_PARAM = 0x8002c404;
constexpr u32 kStatusSize = 44; // CellPrintStatus
constexpr std::string_view kDispatcher = "cellPrintUtility_PrintEvent";

// The module's callback.
struct PrintState {
    std::mutex mutex;
    u32 callback = 0, userdata = 0;
};

PrintState& state() { return HleProcess::current()->state<PrintState>(); }

void clear_callback()
{
    PrintState& s = state();
    std::lock_guard lock(s.mutex);
    s.callback = 0;
    s.userdata = 0;
}

u64 sext(s32 v) { return static_cast<u64>(static_cast<s64>(v)); }

// The module's request shape: the packet begun (its error the answer), FixedMemory 0, the document "PRIN", an element
// `name` with the ints given, written, then the reply read and its "result" checked; `reply(root)` reads the rest. Each
// failure printed as the module prints it.
template <class Reply>
s32 print_request(PPUContext& ctx, std::string_view what, std::string_view name,
    std::initializer_list<std::pair<std::string_view, s32>> ints, Reply&& reply)
{
    packets_start(ctx);
    Request q(ctx, 0, kPacket);
    return guarded(ctx, kModule, what, [&]() -> s32 {
        if (const s32 r = q.try_begin_packet(); r != 0) return r;
        q.begin_memory();
        StackDocument d(ctx);
        const u32 doc = d.addr();
        q.prepare(doc, "PRIN");
        const s32 r = [&]() -> s32 {
            Element e;
            s32 res = document_create_element(ctx, doc, name, static_cast<s32>(ints.size()), e);
            if (res != 0) return res;
            for (const auto& [n, v] : ints) {
                res = util_set_int(ctx, e, n, v);
                if (res != 0) return res;
            }
            res = q.send(doc, what);
            if (res != 0) {
                sysutil_printf(ctx, "doc.Write failed 0x%x\n", {sext(res)});
                return res;
            }
            q.recycle(doc);
            StackVar done(ctx, guest_base(ctx), 16);
            res = q.read(done.addr());
            if (res < 0) {
                sysutil_printf(ctx, "packet read error 0x%x.\n", {sext(res)});
                return res;
            }
            if (!ps_read8(guest_base(ctx), done.addr()) && res == 0x6000) {
                sysutil_printf(ctx, "not enough receive buffer.\n", {});
                return err(PACKET_ERROR_TRUNCATED);
            }
            document_create_from_buffer(ctx, doc, q.memory(), 0x6000, 0); // its answer is not looked at
            const Element root = document_element(ctx, doc);
            s32 result = 0;
            res = util_get_int(ctx, root, "result", result);
            if (res != 0) {
                sysutil_printf(ctx, "get result failed 0x%x\n", {sext(res)});
                return res;
            }
            if (result < 0) {
                sysutil_printf(ctx, "error from server. (error=0x%x)\n", {sext(result)});
                return result;
            }
            return reply(root);
        }();
        q.destroy(doc);
        q.end_memory();
        q.end_packet();
        return r;
    }, [&] { q.abandon(); });
}

// A request whose success only clears the callback.
s32 simple(PPUContext& ctx, std::string_view what, std::string_view name,
    std::initializer_list<std::pair<std::string_view, s32>> ints = {})
{
    return print_request(ctx, what, name, ints, [](const Element&) {
        clear_callback();
        return 0;
    });
}

s32 cellPrintStartJob(PPUContext& ctx, s32 total_page, s32 color_format)
{
    if (total_page <= 0 || color_format != 0) return err(CELL_PRINT_ERROR_INVALID_PARAM);
    return simple(ctx, "cellPrintStartJob", "p_j_start", {{"p_total_page", total_page}, {"p_color_format", 0}});
}

s32 cellPrintEndJob(PPUContext& ctx) { return simple(ctx, "cellPrintEndJob", "p_j_end"); }
s32 cellPrintCancelJob(PPUContext& ctx) { return simple(ctx, "cellPrintCancelJob", "p_j_abort"); }
s32 cellPrintStartPage(PPUContext& ctx) { return simple(ctx, "cellPrintStartPage", "p_p_start"); }
s32 cellPrintEndPage(PPUContext& ctx) { return simple(ctx, "cellPrintEndPage", "p_p_end"); }

// cellPrintGetStatus(status): the reply's file "p_status" copied when it is the 44 bytes of a CellPrintStatus (any
// other size answers 0 with nothing copied and the callback kept).
s32 cellPrintGetStatus(PPUContext& ctx, u32 status)
{
    if (!status) return err(CELL_PRINT_ERROR_INVALID_PARAM);
    return print_request(ctx, "cellPrintGetStatus", "p_get_status", {}, [&](const Element& root) -> s32 {
        File f;
        if (const s32 r = util_get_file(ctx, root, "p_status", f); r != 0) return r;
        if (f.size != kStatusSize) return 0;
        Mem(ctx).copy(status, file_address(ctx, f), kStatusSize);
        clear_callback();
        return 0;
    });
}

// cellPrintGetPrintableArea(width, height): the reply's "p_pixel_width" and "p_pixel_height". (The module checks the
// width's pointer as a 64-bit value.)
s32 cellPrintGetPrintableArea(PPUContext& ctx, u64 width, u32 height)
{
    if (!width || !height) return err(CELL_PRINT_ERROR_INVALID_PARAM);
    return print_request(ctx, "cellPrintGetPrintableArea", "p_area", {}, [&](const Element& root) -> s32 {
        s32 w = 0, h = 0;
        if (const s32 r = util_get_int(ctx, root, "p_pixel_width", w); r != 0) return r;
        if (const s32 r = util_get_int(ctx, root, "p_pixel_height", h); r != 0) return r;
        Mem m(ctx);
        m.w32(static_cast<u32>(width), static_cast<u32>(w));
        m.w32(height, static_cast<u32>(h));
        clear_callback();
        return 0;
    });
}

// cellPrintSendBand(buffer, size, sent size): on channel 1, "p_send_band" with "p_buff_size", then the band itself
// as the packet's last data; the reply's "result" (negative: the answer, not printed) and "p_send_size" the size sent.
s32 cellPrintSendBand(PPUContext& ctx, u32 buff, s32 size, u32 sent)
{
    if (!buff || !sent || size <= 0) return err(CELL_PRINT_ERROR_INVALID_PARAM);
    packets_start(ctx);
    Request q(ctx, 1, kPacket);
    return guarded(ctx, kModule, "cellPrintSendBand", [&]() -> s32 {
        Mem(ctx).w32(sent, 0);
        if (const s32 r = q.try_begin_packet(); r != 0) return r;
        q.begin_memory();
        StackDocument d(ctx);
        const u32 doc = d.addr();
        q.prepare(doc, "PRIN");
        Element e;
        s32 r = document_create_element(ctx, doc, "p_send_band", 1, e);
        if (r == 0) r = util_set_int(ctx, e, "p_buff_size", size);
        if (r == 0) {
            r = q.send(doc, "cellPrintSendBand");
            if (r != 0) sysutil_printf(ctx, "doc.Write failed 0x%x\n", {sext(r)});
        }
        if (r != 0) {
            q.destroy(doc);
            q.end_memory();
            q.end_packet();
            return r;
        }
        q.destroy(doc);
        r = packet_write(ctx, 1, kPacket, buff, static_cast<u32>(size), 1);
        if (r != 0) {
            sysutil_printf(ctx, "cellSysutilPacketWrite() 0x%x\n", {sext(r)});
            q.end_memory();
            q.end_packet();
            return r;
        }
        document_construct(ctx, doc);
        r = [&]() -> s32 {
            StackVar done(ctx, guest_base(ctx), 16);
            s32 res = q.read(done.addr());
            if (res < 0) {
                sysutil_printf(ctx, "packet read error 0x%x.\n", {sext(res)});
                return res;
            }
            if (!ps_read8(guest_base(ctx), done.addr()) && res == 0x6000) {
                sysutil_printf(ctx, "not enough receive buffer.\n", {});
                return err(PACKET_ERROR_TRUNCATED);
            }
            document_create_from_buffer(ctx, doc, q.memory(), 0x6000, 0);
            const Element root = document_element(ctx, doc);
            s32 result = 0;
            res = util_get_int(ctx, root, "result", result);
            if (res != 0) {
                sysutil_printf(ctx, "get result failed 0x%x\n", {sext(res)});
                return res;
            }
            if (result < 0) return result;
            s32 n = 0;
            if ((res = util_get_int(ctx, root, "p_send_size", n)) != 0) return res;
            Mem(ctx).w32(sent, static_cast<u32>(n));
            clear_callback();
            return 0;
        }();
        q.destroy(doc);
        q.end_memory();
        q.end_packet();
        return r;
    }, [&] { q.abandon(); });
}

// cellSysutilPrintInit(): the callback cleared and the dispatcher of the system's print events registered with
// libsysutil (its answer the answer).
s32 cellSysutilPrintInit(PPUContext& ctx)
{
    clear_callback();
    return call_sysutil(ctx, "cellSysutil", "cellSysutilRegisterCallbackDispatcher",
        {kEvent, internal_descriptor("cellPrintUtility", kDispatcher)});
}

// cellSysutilPrintShutdown(): the dispatcher unregistered; when that succeeds, the callback cleared.
s32 cellSysutilPrintShutdown(PPUContext& ctx)
{
    const s32 r = call_sysutil(ctx, "cellSysutil", "cellSysutilUnregisterCallbackDispatcher", {kEvent});
    if (r == 0) clear_callback();
    return r;
}

// The dispatcher of the system's print events (the module's code at 0x214; pspocket's internal function): the
// callback, once, with the event's parameter.
s32 cellPrintUtility_PrintEvent(PPUContext& ctx, u32, s32 param)
{
    u32 cb = 0, ud = 0;
    {
        PrintState& s = state();
        std::lock_guard lock(s.mutex);
        cb = s.callback;
        ud = s.userdata;
        s.callback = 0;
        s.userdata = 0;
    }
    if (cb) call_guest(ctx, cb, {sext(param), ud});
    return 0;
}

} // namespace

void register_print(Registry& r)
{
    r.add("cellPrintUtility", {
        PS_HLE_FUNC(0xCF1AAEFA, cellSysutilPrintInit),
        PS_HLE_FUNC(0x6D996018, cellSysutilPrintShutdown),
        PS_HLE_FUNC(0x6802DFB5, cellPrintGetStatus),
        PS_HLE_FUNC(0x6E952645, cellPrintGetPrintableArea),
        PS_HLE_FUNC(0x795B12B3, cellPrintStartJob),
        PS_HLE_FUNC(0xC04A7D42, cellPrintEndJob),
        PS_HLE_FUNC(0x293D9E9C, cellPrintCancelJob),
        PS_HLE_FUNC(0x865ACF74, cellPrintStartPage),
        PS_HLE_FUNC(0x0D44F661, cellPrintEndPage),
        PS_HLE_FUNC(0x0A373522, cellPrintSendBand),
        PS_HLE_INTERNAL(recomp::nid_of(kDispatcher), cellPrintUtility_PrintEvent),
    });
}

} // namespace pspocket::hle::sysutil::sub
