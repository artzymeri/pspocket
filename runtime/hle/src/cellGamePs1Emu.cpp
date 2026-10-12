// cellGamePs1Emu: the PS1 emulator's pad rumble request (D-929). Read from libsysutil_game_ps1emu.sprx (4.92, in
// /dev_flash/sys/internal) and compared with it call for call (tests/hle/test_sysutil_more.cpp). RPCS3 does not know
// the module's one function (0x61CE2BCD, which nothing names: its request is "setPadRumble").
//
// The module's start and stop do nothing a game sees (pspocket loads no module). Its function is a request on
// libsysutil's channel 1, packet 0x129, magic "GPS1", whose write's answer is the answer when it fails: on pspocket's
// console, the closed channel's 0x8002b007 (D-534).

#include <string_view>

#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "sysutil_sub.hpp"

namespace pspocket::hle::sysutil::sub {

namespace {

constexpr std::string_view kModule = "libsysutil_game_ps1emu";
constexpr u32 kChannel = 1;
constexpr s32 kPacket = 0x129;
constexpr u32 CELL_GAME_PS1EMU_ERROR_PARAM = 0x8002CB07;

// cellGamePs1Emu_61CE2BCD(mode): "setPadRumble" with "elmMode" (0 or 1, else the parameter error before any request);
// the reply's "elmResult" is the answer, read through the request's element (the module keeps it and reads the reply's
// tree at its offset, the root's). A failed step's answer is the answer.
s32 cellGamePs1Emu_61CE2BCD(PPUContext& ctx, u32 mode)
{
    if (mode > 1) return err(CELL_GAME_PS1EMU_ERROR_PARAM);
    packets_start(ctx);
    Request q(ctx, kChannel, kPacket);
    return guarded(ctx, kModule, "cellGamePs1Emu_61CE2BCD", [&]() -> s32 {
        q.begin_packet();
        q.begin_memory();
        StackDocument d(ctx);
        const u32 doc = d.addr();
        q.prepare(doc, "GPS1");
        Element e;
        s32 r = as_export(ctx, "cxml::Document::CreateElement", err(CELL_EFAULT),
            [&] { return document_create_element(ctx, doc, "setPadRumble", 1, e); });
        if (r == 0) {
            r = as_export(ctx, "cxmlutil::SetInt", err(CELL_EFAULT),
                [&] { return util_set_int(ctx, e, "elmMode", static_cast<s32>(mode)); });
        }
        if (r == 0) r = q.send(doc, "cellGamePs1Emu setPadRumble");
        if (r == 0) {
            q.recycle(doc);
            r = q.read(0);
            if (r >= 0) {
                as_export(ctx, "cxml::Document::CreateFromBuffer", err(CELL_EFAULT),
                    [&] { return document_create_from_buffer(ctx, doc, q.memory(), 0x6000, 0); });
                as_export(ctx, "cxml::Document::GetDocumentElement", 0, [&] {
                    document_element(ctx, doc);
                    return 0;
                });
                s32 result = 0;
                r = as_export(ctx, "cxmlutil::GetInt", err(CELL_EFAULT),
                    [&] { return util_get_int(ctx, e, "elmResult", result); });
                if (r == 0) r = result;
            }
        }
        q.destroy(doc);
        q.end_memory();
        q.end_packet();
        return r;
    }, [&] { q.abandon(); });
}

} // namespace

void register_game_ps1emu(Registry& r)
{
    r.add("cellGamePs1Emu", {
        Function{{}, "cellGamePs1Emu_61CE2BCD", 0x61CE2BCD, &detail::thunk<&cellGamePs1Emu_61CE2BCD>, false},
    });
}

} // namespace pspocket::hle::sysutil::sub
