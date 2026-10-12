// cellSysconfPs1emu: the PS1 emulator's settings requests (libsysutil_sysconf_ps1emu.sprx, fw 4.92: four exports
// known only by their NIDs), pspocket's own (D-918). Read from the module and compared with it running on pspocket
// (tests/hle/test_sysconf_ps1emu_compare.cpp). The module is a client of libsysutil's: each export builds a request
// as libsysutil's own requests are built (sysutil_cxml.hpp; D-534) and sends it to the system software on channel 0
// (packet 0x110, magic "SYSC", FixedMemory instance 0), then reads the system's reply document:
// - 0x83E79A23 (port, u8 out[2]): "getBattery" {portNo}: the reply's "result" (a negative one is the answer), "status"
//   to out[0] and "level" to out[1]; a level out of range for the status (status 0: 1-2, status 1: 1-5, 2 and 3 any,
//   above 3 none) is CELL_SYSUTIL_ERROR_VALUE.
// - 0x6A12D11F (port1, port2): "swapGamePad" {port1, port2}; the same port twice is refused, nothing else is checked.
// - 0x639ABBDE (port): "disconGamePad" {portNo}.
// - 0xEFDDAF6C (port, u16 out[2]): "getPower" {portNo}: "status" to out[0], "reserved" to out[1].
// Ports go up to 6 (the module's own check, error 0x8002BB01). pspocket's console has no system software: its channels
// are closed (D-534), so each request's send answers 0x8002b007, as Sony's module answers on pspocket; the replies are
// read as the module reads them all the same (the comparison answers them from a system side of its own).

#include <format>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "sysutil_cxml.hpp"

namespace pspocket::hle {
namespace {

using namespace sysutil;

constexpr std::string_view kChan = "cellSysconfPs1emu";

enum : u32 {
    // The module's argument error (Sony's name for it is unknown: in the sysconf range of cellSysutil's errors).
    kErrorParam = 0x8002BB01,
    CELL_SYSUTIL_ERROR_VALUE = 0x8002B102,
    CELL_EFAULT = 0x8001000D,
};

constexpr s32 err(u32 code) { return static_cast<s32>(code); }

template <class Body>
s32 guarded(PPUContext& ctx, std::string_view what, Body&& body)
{
    try {
        return body();
    } catch (const Fault& f) {
        log_call_site(ctx, guest_base(ctx),
            std::format("{}: guest memory at {:#010x} is not there (the module faults there): CELL_EFAULT", what,
                f.addr));
        return err(CELL_EFAULT);
    }
}

constexpr u32 kChannel = 0;
constexpr s32 kPacket = 0x110;

// One request of the module's: an element `name` with up to two int attributes, sent; the reply read into the same
// document, its root's "result" checked; `reply` reads the rest (given the root). Every way out ends the same way: the
// document cleared, FixedMemory 0 given back, the packet ended.
template <class Reply>
s32 request(PPUContext& ctx, std::string_view what, std::string_view name, std::string_view a1, s32 v1,
    std::string_view a2, s32 v2, Reply&& reply)
{
    packet_begin(ctx, kChannel, kPacket);
    const u32 memory = fixed_memory_begin(ctx, 0);
    StackVar frame(ctx, guest_base(ctx), 0x80);
    const u32 doc = frame.addr();
    document_construct(ctx, doc);
    document_use_fixed_memory(ctx, doc, memory);
    document_set_magic(ctx, doc, "SYSC");
    Element e;
    s32 r = document_create_element(ctx, doc, name, a2.empty() ? 1 : 2, e);
    if (r == 0) r = util_set_int(ctx, e, a1, v1);
    if (r == 0 && !a2.empty()) r = util_set_int(ctx, e, a2, v2);
    if (r == 0) {
        PacketWriter w(ctx, kChannel, kPacket, doc);
        r = document_write(ctx, doc, w);
        if (r != 0) log_handover(ctx, what, r);
    }
    if (r == 0) {
        document_clear(ctx, doc);
        document_construct(ctx, doc);
        StackVar done(ctx, guest_base(ctx), 16);
        r = packet_read(ctx, kChannel, kPacket, memory, 0x6000, done.addr());
        if (r >= 0) {
            if (!ps_read8(guest_base(ctx), done.addr()) && r == 0x6000) {
                r = err(PACKET_ERROR_TRUNCATED);
            } else {
                document_create_from_buffer(ctx, doc, memory, 0x6000, 0);
                const Element root = document_element(ctx, doc);
                s32 result = 0;
                r = util_get_int(ctx, root, "result", result);
                if (r == 0 && result < 0) r = result;
                else if (r == 0) r = reply(root);
            }
        }
    }
    document_clear(ctx, doc);
    fixed_memory_end(ctx, 0);
    packet_end(ctx, kChannel, kPacket);
    return r;
}

// getBattery(port, u8 out[2]).
s32 cellSysconfPs1emu_83E79A23(PPUContext& ctx, u32 port, u32 out)
{
    if (port > 6 || !out) return err(kErrorParam);
    packets_start(ctx);
    return guarded(ctx, "cellSysconfPs1emu getBattery", [&] {
        return request(ctx, "cellSysconfPs1emu getBattery", "getBattery", "portNo", static_cast<s32>(port), {}, 0,
            [&](const Element& root) {
                Mem m(ctx);
                s32 v = 0;
                s32 r = util_get_int(ctx, root, "status", v);
                if (r != 0) return r;
                m.w8(out, static_cast<u8>(v));
                r = util_get_int(ctx, root, "level", v);
                if (r != 0) return r;
                m.w8(out + 1, static_cast<u8>(v));
                const u8 status = m.r8(out), level1 = static_cast<u8>(static_cast<u8>(v) - 1);
                if (status == 1) return level1 <= 4 ? 0 : err(CELL_SYSUTIL_ERROR_VALUE);
                if (status == 0) return level1 <= 1 ? 0 : err(CELL_SYSUTIL_ERROR_VALUE);
                return status <= 3 ? 0 : err(CELL_SYSUTIL_ERROR_VALUE);
            });
    });
}

// swapGamePad(port1, port2).
s32 cellSysconfPs1emu_6A12D11F(PPUContext& ctx, s32 port1, s32 port2)
{
    if (port1 == port2) return err(kErrorParam);
    packets_start(ctx);
    return guarded(ctx, "cellSysconfPs1emu swapGamePad", [&] {
        return request(ctx, "cellSysconfPs1emu swapGamePad", "swapGamePad", "port1", port1, "port2", port2,
            [](const Element&) { return 0; });
    });
}

// disconGamePad(port).
s32 cellSysconfPs1emu_639ABBDE(PPUContext& ctx, u32 port)
{
    if (port > 6) return err(kErrorParam);
    packets_start(ctx);
    return guarded(ctx, "cellSysconfPs1emu disconGamePad", [&] {
        return request(ctx, "cellSysconfPs1emu disconGamePad", "disconGamePad", "portNo", static_cast<s32>(port), {},
            0, [](const Element&) { return 0; });
    });
}

// getPower(port, u16 out[2]).
s32 cellSysconfPs1emu_EFDDAF6C(PPUContext& ctx, u32 port, u32 out)
{
    if (port > 6 || !out) return err(kErrorParam);
    packets_start(ctx);
    return guarded(ctx, "cellSysconfPs1emu getPower", [&] {
        return request(ctx, "cellSysconfPs1emu getPower", "getPower", "portNo", static_cast<s32>(port), {}, 0,
            [&](const Element& root) {
                Mem m(ctx);
                s32 v = 0;
                s32 r = util_get_int(ctx, root, "status", v);
                if (r != 0) return r;
                m.w8(out, static_cast<u8>(static_cast<u32>(v) >> 8));
                m.w8(out + 1, static_cast<u8>(v));
                r = util_get_int(ctx, root, "reserved", v);
                if (r != 0) return r;
                m.w8(out + 2, static_cast<u8>(static_cast<u32>(v) >> 8));
                m.w8(out + 3, static_cast<u8>(v));
                return 0;
            });
    });
}

} // namespace

void register_cellSysconfPs1emu(Registry& r)
{
    r.add(kChan, {
        PS_HLE_FUNC(0x639ABBDE, cellSysconfPs1emu_639ABBDE),
        PS_HLE_FUNC(0x6A12D11F, cellSysconfPs1emu_6A12D11F),
        PS_HLE_FUNC(0x83E79A23, cellSysconfPs1emu_83E79A23),
        PS_HLE_FUNC(0xEFDDAF6C, cellSysconfPs1emu_EFDDAF6C),
    });
}

} // namespace pspocket::hle
