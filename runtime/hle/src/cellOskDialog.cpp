// cellSysutil's on-screen keyboard dialog (cellOskDialog). Reference: RPCS3
// rpcs3/Emu/Cell/Modules/cellOskDialog.cpp/.h, and for what the user's answer does, its keyboard overlay
// (rpcs3/Emu/RSX/Overlays/overlay_osk.cpp).
//
// Implemented: what GTA V imports: cellOskDialogLoadAsync, cellOskDialogUnloadAsync, cellOskDialogAbort and four
// settings (device mask, key layout option, support language, layout mode), with RPCS3's argument checks, results,
// states and system events. A load opens the dialog and sends CELL_SYSUTIL_OSKDIALOG_LOADED; the dialog closing (the
// user confirms or cancels, or the game aborts it) sends FINISHED; the unload hands the result to the game, ends the
// dialog, resets the settings and sends UNLOADED. The events reach the game's sysutil callbacks at its
// cellSysutilCheckCallback.
//
// There is no keyboard UI yet, so the dialog acts as if the user cancelled it as soon as it appeared: the first
// cellSysutilCheckCallback after the load delivers LOADED, then closes the dialog with CELL_OSKDIALOG_CLOSE_CANCEL
// (logged as a warning), so FINISHED reaches the callbacks at the check after that, and the unload reports
// CELL_OSKDIALOG_INPUT_FIELD_RESULT_CANCELED. The keyboard's text is then empty; as in RPCS3, a cancelled dialog's text
// is not copied to the game's result buffer (only with the result OK, that of a dialog confirmed or still open, is it
// copied). A keyboard UI plugs in at show_keyboard(): it shows the request there, and calls keyboard_closed() with the
// user's answer and text.
//
// Every function RPCS3 registers is here (D-183): the rest of the settings and the separate-window option are kept for
// a keyboard UI (nothing draws them), cellOskDialogGetInputText serves the text the unload or
// cellOskDialogExtUpdateInputText last copied, and cellOskExtUtility's callbacks, colours and switches are kept as
// RPCS3 keeps them. cellOskDialogExtSendFinishMessage closes the dialog as the user would.
//
// Deviation from RPCS3: pointers are checked before use (D-042). One that is not guest memory, or a message or initial
// text that runs into such memory, is logged with the call site and refused with CELL_OSKDIALOG_ERROR_PARAM (RPCS3
// faults).

#include <algorithm>
#include <array>
#include <functional>
#include <bit>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "sysutil.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellOskDialog";

enum : u32 {
    CELL_SYSUTIL_ERROR_BUSY = 0x8002b105,
    CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED = 0x8002b302, // RPCS3 answers with the message dialog's code
    CELL_OSKDIALOG_ERROR_PARAM = 0x8002b504,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

std::string result_name(s32 r)
{
    switch (static_cast<u32>(r)) {
    case 0: return "CELL_OK";
    case CELL_SYSUTIL_ERROR_BUSY: return "CELL_SYSUTIL_ERROR_BUSY";
    case CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED: return "CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED";
    case CELL_OSKDIALOG_ERROR_PARAM: return "CELL_OSKDIALOG_ERROR_PARAM";
    }
    return std::format("{:#010x}", static_cast<u32>(r));
}

// Logs a call, "name(arguments)" and `detail`, with its result, and returns the result: at info, or as a warning for
// an error (the game passed something wrong, or asked at the wrong time). Keyboard dialogs are rare, so every call is
// logged.
s32 logged(s32 r, std::string_view call, std::string_view detail = {})
{
    log::print(r < 0 ? log::Level::Warn : log::Level::Info, kChan, "{}{}: {}", call, detail, result_name(r));
    return r;
}

// The dialog's system events (RPCS3 cellOskDialog.h), the statuses the game's sysutil callbacks get.
constexpr u64 CELL_SYSUTIL_OSKDIALOG_LOADED = 0x0502;
constexpr u64 CELL_SYSUTIL_OSKDIALOG_FINISHED = 0x0503;
constexpr u64 CELL_SYSUTIL_OSKDIALOG_UNLOADED = 0x0504;

// CellOskDialogInputFieldResult: how the dialog ended, what the unload reports.
constexpr s32 CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK = 0;
constexpr s32 CELL_OSKDIALOG_INPUT_FIELD_RESULT_CANCELED = 1;
constexpr s32 CELL_OSKDIALOG_INPUT_FIELD_RESULT_ABORT = 2;
constexpr s32 CELL_OSKDIALOG_INPUT_FIELD_RESULT_NO_INPUT_TEXT = 3;

// CellOskDialogFinishReason: how the user closed the dialog.
constexpr s32 CELL_OSKDIALOG_CLOSE_CONFIRM = 0, CELL_OSKDIALOG_CLOSE_CANCEL = 1;

constexpr u32 CELL_OSKDIALOG_STRING_SIZE = 512; // UTF-16 units in a dialog string, its terminator included
constexpr u32 CELL_OSKDIALOG_10KEY_PANEL = 1, CELL_OSKDIALOG_FULLKEY_PANEL = 2;
constexpr u32 CELL_OSKDIALOG_DEVICE_MASK_PAD = 0xFF;
constexpr u32 CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_LEFT = 0x200, CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_CENTER = 0x400,
              CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_RIGHT = 0x800, CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_TOP = 0x1000,
              CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_CENTER = 0x2000, CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_BOTTOM = 0x4000;

// CellOskDialogParam (20 bytes): be32 allowOskPanelFlg, firstViewPanel; CellOskDialogPoint controlPoint {be f32 x, y};
// be s32 prohibitFlgs.
constexpr u32 kParamSize = 20;
// CellOskDialogInputFieldInfo (12 bytes): be32 message, init_text (UTF-16 strings), limit_length.
constexpr u32 kFieldInfoSize = 12;
// CellOskDialogCallbackReturnParam (12 bytes): be s32 result, numCharsResultString; be32 pResultString (UTF-16).
constexpr u32 kReturnParamSize = 12;

// RPCS3 osk_info::get_aligned_x/y: a layout mode may name several alignments; centred wins, then left (top).
u32 aligned_x(u32 mode)
{
    if (mode & CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_CENTER) return CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_CENTER;
    if (mode & CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_LEFT) return CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_LEFT;
    return CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_RIGHT;
}

u32 aligned_y(u32 mode)
{
    if (mode & CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_CENTER) return CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_CENTER;
    if (mode & CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_TOP) return CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_TOP;
    return CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_BOTTOM;
}

// RPCS3 osk_window_layout: the dialog's alignment, and its offset (the load's control point).
struct Layout {
    u32 mode = CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_LEFT | CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_TOP;
    u32 x_align = CELL_OSKDIALOG_LAYOUTMODE_X_ALIGN_LEFT;
    u32 y_align = CELL_OSKDIALOG_LAYOUTMODE_Y_ALIGN_TOP;
    f32 x_offset = 0.0f, y_offset = 0.0f;
};

// What the game sets up for its dialogs, for a keyboard UI to follow (nothing reads it yet). The unload resets it to
// these values (RPCS3 osk_info::reset).
struct Settings {
    u32 device_mask = 0;                                 // the input devices the dialog ignores (separate windows)
    u32 key_layout_options = CELL_OSKDIALOG_10KEY_PANEL; // the panels it offers: 10-key, full keyboard, or both
    u32 supported_languages = 0;                         // CELL_OSKDIALOG_PANELMODE_* it may offer besides the defaults
    Layout layout{};
    // The rest of RPCS3's osk_info, reset with these (D-183).
    bool dimmer = true;            // the screen dims behind the dialog
    u32 initial_input_device = 0;  // CELL_OSKDIALOG_INPUT_DEVICE_PAD
    u32 initial_key_layout = 0;    // CELL_OSKDIALOG_INITIAL_PANEL_LAYOUT_SYSTEM
    bool separate_windows = false; // cellOskDialogSetSeparateWindowOption
    u32 continuous_mode = 0;       // CELL_OSKDIALOG_CONTINUOUS_MODE_NONE
    s32 field_window_width = 0;
    f32 field_transparency = 1.0f;
    Layout field_layout{}, panel_layout{};
    bool lock_input_device = false; // cellOskDialogExtInputDeviceLock
    bool clipboard = false, half_byte_kana = false, pointer = false;
    f32 pointer_x = 0.0f, pointer_y = 0.0f;
    std::array<f32, 4> base_color{0.2f, 0.2f, 0.2f, 1.0f};
    f32 initial_scale = 1.0f;
    u32 confirm_callback = 0, force_finish_callback = 0, keyboard_hook = 0; // descriptors, kept (no keyboard UI)
    u16 hook_event_mode = 0;
};

// What a keyboard UI shows (RPCS3 OskDialogBase::osk_params, the parts the load sets).
struct Request {
    std::u16string message; // shown above the input field
    std::u16string init_text;
    u32 max_length = 0;       // UTF-16 units the user may enter
    u32 panels = 0;           // allowOskPanelFlg: the CELL_OSKDIALOG_PANELMODE_* panels the user may switch to
    u32 first_view_panel = 0; // the one shown first
    u32 prohibit_flags = 0;   // CELL_OSKDIALOG_NO_SPACE, NO_RETURN, NO_INPUT_ANALOG, NO_STARTUP_EFFECT
};

// RPCS3 osk_info and its dialog (OskDialogBase): one dialog at a time, from its load to its unload.
struct OskState {
    std::mutex mutex;
    bool loaded = false; // RPCS3 osk_info::init
    u64 generation = 0;  // loads so far: the keyboard's answer for one dialog can't close the next
    bool open = false;   // shown, waiting for the user (RPCS3 OskDialogState::Open); false once it closed
    s32 input_result = CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK;
    std::u16string text; // the keyboard's text (RPCS3 osk_text): the initial text, then the user's
    std::u16string valid_text; // the text served to the game (RPCS3 valid_text): copied at the unload or ExtUpdate
    Settings settings;
};

OskState& osk()
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellOskDialog called without an HLE process");
    return p->state<OskState>();
}

// UTF-16 as UTF-8, for logs (an unpaired surrogate becomes U+FFFD).
std::string utf8(std::u16string_view s)
{
    std::string out;
    for (usize i = 0; i < s.size(); i++) {
        u32 c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u32{s[i + 1]} - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Reads the guest UTF-16 string at `addr` into `out`: up to its terminator, `max` units at most. False if it runs into
// memory that is not guest memory first.
bool read_utf16(lv2::PpuThread& t, u32 addr, u32 max, std::u16string& out)
{
    out.clear();
    for (u32 i = 0; i < max; i++) {
        const u64 a = u64{addr} + 2 * u64{i};
        if (a + 2 > GuestMemory::kGuestSize || !guest_readable(t, static_cast<u32>(a), 2)) return false;
        const char16_t c = static_cast<char16_t>(ps_read16(t.kernel.base(), static_cast<u32>(a)));
        if (!c) break;
        out += c;
    }
    return true;
}

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// ---------------------------------------------------------------------------------------------- the keyboard

void keyboard_closed(u64 generation, s32 status, std::u16string text);

// Shows the dialog (RPCS3 OskDialogBase::Create). There is no keyboard UI yet, so the user cancels at once: the next
// cellSysutilCheckCallback, after it delivered the LOADED the load queued, closes the dialog, its text empty. A
// keyboard UI shows `request` here instead, and later calls keyboard_closed() with this generation, the user's answer
// and the text (from any thread). While it shows, it keeps the pads' input from the game (RPCS3 input::SetIntercepted,
// until the dialog closes). Called with the state locked.
void show_keyboard(u64 generation, const Request& request)
{
    PS_LOG_WARN(kChan, "no keyboard UI: the dialog (\"{}\", initial text \"{}\") closes as if the user cancelled it",
        utf8(request.message), utf8(request.init_text));
    sysutil_queue_call([generation](PPUContext&) -> s32 {
        keyboard_closed(generation, CELL_OSKDIALOG_CLOSE_CANCEL, u"");
        return lv2::CELL_OK;
    });
}

// The user closed the dialog (RPCS3 on_osk_close, without separate windows): confirmed with the text entered
// (CELL_OSKDIALOG_CLOSE_CONFIRM: the result is OK), or cancelled (CANCELED). FINISHED goes to the game's callbacks.
// The answer is dropped when its dialog isn't open any more: the game aborted or unloaded it meanwhile.
void keyboard_closed(u64 generation, s32 status, std::u16string text)
{
    OskState& s = osk();
    std::lock_guard lock(s.mutex);
    if (!s.loaded || s.generation != generation || !s.open) {
        PS_LOG_INFO(kChan, "the keyboard answered for a dialog the game has aborted or unloaded since: ignored");
        return;
    }
    const bool confirmed = status == CELL_OSKDIALOG_CLOSE_CONFIRM;
    s.open = false;
    s.text = std::move(text);
    s.input_result = confirmed ? CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK : CELL_OSKDIALOG_INPUT_FIELD_RESULT_CANCELED;
    PS_LOG_INFO(kChan, "the dialog closed, {} (\"{}\"); FINISHED goes to the game's callbacks",
        confirmed ? "confirmed" : "cancelled", utf8(s.text));
    sysutil_send_event(CELL_SYSUTIL_OSKDIALOG_FINISHED, 0);
}

// ---------------------------------------------------------------------------------------------- the dialog

// cellOskDialogLoadAsync(container, const CellOskDialogParam* dialogParam, const CellOskDialogInputFieldInfo*
// inputFieldInfo). RPCS3's checks, in its order: both pointers, the message and initial text pointers, limit_length up
// to 512 (CELL_OSKDIALOG_ERROR_PARAM); then one dialog at a time (CELL_SYSUTIL_ERROR_BUSY until the unload). The
// dialog opens with the initial text, limit_length units at most (511 for 512), and LOADED is sent. The memory
// container isn't used (RPCS3). RPCS3 shows the dialog just before it sends LOADED; here the keyboard gets it just
// after, so that its answer can't reach the game before LOADED.
s32 cellOskDialogLoadAsync(PPUContext& ctx, u32 container, u32 dialog_param, u32 field_info)
{
    const std::string call = std::format("cellOskDialogLoadAsync(container {:#x}, dialogParam {:#x}, inputFieldInfo "
                                         "{:#x})",
        container, dialog_param, field_info);
    if (!dialog_param || !field_info) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    if (!usable(ctx, dialog_param, kParamSize, "cellOskDialogLoadAsync: dialogParam") ||
        !usable(ctx, field_info, kFieldInfoSize, "cellOskDialogLoadAsync: inputFieldInfo")) {
        return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    }
    lv2::PpuThread& t = thread_of(ctx);
    u8* base = t.kernel.base();
    const u32 message = ps_read32(base, field_info + 0);
    const u32 init_text = ps_read32(base, field_info + 4);
    const u32 limit_length = ps_read32(base, field_info + 8);
    if (!message || !init_text || limit_length > CELL_OSKDIALOG_STRING_SIZE) {
        return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call,
            std::format(" {{message {:#x}, init_text {:#x}, limit_length {}}}", message, init_text, limit_length));
    }
    Request request;
    request.panels = ps_read32(base, dialog_param + 0);
    request.first_view_panel = ps_read32(base, dialog_param + 4);
    const f32 x = std::bit_cast<f32>(ps_read32(base, dialog_param + 8));
    const f32 y = std::bit_cast<f32>(ps_read32(base, dialog_param + 12));
    request.prohibit_flags = ps_read32(base, dialog_param + 16);
    request.max_length = limit_length >= CELL_OSKDIALOG_STRING_SIZE ? CELL_OSKDIALOG_STRING_SIZE - 1 : limit_length;

    OskState& s = osk();
    std::lock_guard lock(s.mutex);
    if (s.loaded) return logged(error(CELL_SYSUTIL_ERROR_BUSY), call);
    if (!read_utf16(t, init_text, request.max_length, request.init_text) ||
        !read_utf16(t, message, CELL_OSKDIALOG_STRING_SIZE, request.message)) {
        log_call_site(ctx, base,
            std::format("cellOskDialogLoadAsync: the initial text ({:#x}) or the message ({:#x}) runs into memory that "
                        "is not guest memory",
                init_text, message));
        return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    }
    s.loaded = true;
    s.generation++;
    s.open = true;
    s.input_result = CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK;
    s.text = request.init_text;
    s.settings.layout.x_offset = x;
    s.settings.layout.y_offset = y;
    sysutil_send_event(CELL_SYSUTIL_OSKDIALOG_LOADED, 0);
    show_keyboard(s.generation, request);
    return logged(lv2::CELL_OK, call,
        std::format(" {{message \"{}\", initial text \"{}\", {} units at most, panels {:#x}, first panel {:#x}, "
                    "prohibit {:#x}, control point ({}, {})}}",
            utf8(request.message), utf8(request.init_text), request.max_length, request.panels,
            request.first_view_panel, request.prohibit_flags, x, y));
}

// cellOskDialogUnloadAsync(CellOskDialogCallbackReturnParam* OutputInfo), RPCS3's getText for the unload. Checked:
// the pointer and numCharsResultString, not negative (CELL_OSKDIALOG_ERROR_PARAM), then that a dialog is loaded
// (CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED). The result is the dialog's: OK while it is still open, CANCELED, ABORT.
// Only with OK (or NO_INPUT_TEXT) is the text copied to pResultString, when that isn't null: up to its terminator or
// numCharsResultString units, then a terminator, 511 units at most (RPCS3's loop, which can write one unit past
// numCharsResultString). The dialog then ends (a keyboard UI would hide one still shown), the settings are reset and
// UNLOADED is sent.
s32 cellOskDialogUnloadAsync(PPUContext& ctx, u32 output_info)
{
    const std::string call = std::format("cellOskDialogUnloadAsync(OutputInfo {:#x})", output_info);
    if (!output_info) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    if (!usable(ctx, output_info, kReturnParamSize, "cellOskDialogUnloadAsync: OutputInfo")) {
        return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    }
    u8* base = guest_base(ctx);
    const s32 num_chars = static_cast<s32>(ps_read32(base, output_info + 4));
    const u32 result_string = ps_read32(base, output_info + 8);
    if (num_chars < 0) {
        return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call, std::format(" {{numCharsResultString {}}}", num_chars));
    }
    OskState& s = osk();
    std::lock_guard lock(s.mutex);
    if (!s.loaded) return logged(error(CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED), call);

    // The text served to the game (RPCS3 valid_text): the keyboard's, 511 units at most.
    const std::u16string text = s.text.substr(0, CELL_OSKDIALOG_STRING_SIZE - 1);
    const s32 result = s.input_result;
    const bool copy = result_string && (result == CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK ||
                                           result == CELL_OSKDIALOG_INPUT_FIELD_RESULT_NO_INPUT_TEXT);
    // RPCS3's copy, unit by unit: the text until its terminator or numCharsResultString units, whichever comes first,
    // then a terminator; 511 units at most.
    const u32 limit = static_cast<u32>(num_chars);
    const auto unit = [&](u32 i) -> char16_t { return i < limit && i < text.size() ? text[i] : u'\0'; };
    u32 units = 0; // written, the terminator included
    std::u16string copied;
    if (copy) {
        while (units < CELL_OSKDIALOG_STRING_SIZE - 1) {
            if (!unit(units++)) break;
        }
        if (!usable(ctx, result_string, 2 * units, "cellOskDialogUnloadAsync: pResultString")) {
            return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
        }
        for (u32 i = 0; i < units; i++) {
            ps_write16(base, result_string + 2 * i, unit(i));
            if (unit(i)) copied += unit(i);
        }
    }
    ps_write32(base, output_info + 0, static_cast<u32>(result));

    s.loaded = false;
    s.open = false;
    s.input_result = CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK;
    s.text.clear();
    s.valid_text.clear();
    s.settings = {};
    sysutil_send_event(CELL_SYSUTIL_OSKDIALOG_UNLOADED, 0);
    return logged(lv2::CELL_OK, call,
        copy ? std::format(" {{result {}, \"{}\" copied ({} units)}}", result, utf8(copied), units)
             : std::format(" {{result {}, no text copied}}", result));
}

// cellOskDialogAbort(): RPCS3 ends a loaded dialog with the result ABORT and sends FINISHED, whether the dialog was
// still open or had closed already (every abort sends one). Without a loaded dialog:
// CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED. (A keyboard UI would hide a dialog still shown.)
s32 cellOskDialogAbort(PPUContext&)
{
    constexpr std::string_view call = "cellOskDialogAbort()";
    OskState& s = osk();
    std::lock_guard lock(s.mutex);
    if (!s.loaded) return logged(error(CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED), call);
    const bool was_open = s.open;
    s.open = false;
    s.input_result = CELL_OSKDIALOG_INPUT_FIELD_RESULT_ABORT;
    sysutil_send_event(CELL_SYSUTIL_OSKDIALOG_FINISHED, 0);
    return logged(lv2::CELL_OK, call, was_open ? " (the dialog was open)" : " (the dialog had closed already)");
}

// ---------------------------------------------------------------------------------------------- settings
// For the dialogs loaded after them, until an unload resets them; RPCS3 accepts them with or without a dialog loaded.

// cellOskDialogSetDeviceMask(deviceMask): the input devices the dialog ignores, up to CELL_OSKDIALOG_DEVICE_MASK_PAD
// (above: CELL_OSKDIALOG_ERROR_PARAM). RPCS3 applies it to separate windows only, which aren't implemented.
s32 cellOskDialogSetDeviceMask(PPUContext&, u32 device_mask)
{
    const std::string call = std::format("cellOskDialogSetDeviceMask({:#x})", device_mask);
    if (device_mask > CELL_OSKDIALOG_DEVICE_MASK_PAD) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    OskState& s = osk();
    std::lock_guard lock(s.mutex);
    s.settings.device_mask = device_mask;
    return logged(lv2::CELL_OK, call);
}

// cellOskDialogSetKeyLayoutOption(option): the panels the dialog offers, CELL_OSKDIALOG_10KEY_PANEL,
// CELL_OSKDIALOG_FULLKEY_PANEL or both (anything else: CELL_OSKDIALOG_ERROR_PARAM).
s32 cellOskDialogSetKeyLayoutOption(PPUContext&, u32 option)
{
    const std::string call = std::format("cellOskDialogSetKeyLayoutOption({:#x})", option);
    if (option == 0 || option > (CELL_OSKDIALOG_10KEY_PANEL | CELL_OSKDIALOG_FULLKEY_PANEL)) {
        return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    }
    OskState& s = osk();
    std::lock_guard lock(s.mutex);
    s.settings.key_layout_options = option;
    return logged(lv2::CELL_OK, call);
}

// cellOskDialogAddSupportLanguage(supportLanguage): the CELL_OSKDIALOG_PANELMODE_* languages the dialog may offer
// besides the defaults. Despite the name RPCS3 replaces the value rather than adding to it, and checks nothing.
s32 cellOskDialogAddSupportLanguage(PPUContext&, u32 support_language)
{
    OskState& s = osk();
    std::lock_guard lock(s.mutex);
    s.settings.supported_languages = support_language;
    return logged(lv2::CELL_OK, std::format("cellOskDialogAddSupportLanguage({:#x})", support_language));
}

// cellOskDialogSetLayoutMode(layoutMode): the dialog's alignment (CELL_OSKDIALOG_LAYOUTMODE_*; with several, centred
// wins, then left or top). RPCS3 checks nothing.
s32 cellOskDialogSetLayoutMode(PPUContext&, s32 layout_mode)
{
    const u32 mode = static_cast<u32>(layout_mode);
    OskState& s = osk();
    std::lock_guard lock(s.mutex);
    s.settings.layout.mode = mode;
    s.settings.layout.x_align = aligned_x(mode);
    s.settings.layout.y_align = aligned_y(mode);
    return logged(lv2::CELL_OK, std::format("cellOskDialogSetLayoutMode({:#x})", mode),
        std::format(" {{x align {:#x}, y align {:#x}}}", s.settings.layout.x_align, s.settings.layout.y_align));
}

// cellOskDialogGetInputText(OutputInfo): RPCS3's getText outside the unload. The text served is the one last copied
// (by an unload or cellOskDialogExtUpdateInputText): OK when there is one, else NO_INPUT_TEXT; copied as the unload
// copies it, only with OK.
s32 cellOskDialogGetInputText(PPUContext& ctx, u32 output_info)
{
    const std::string call = std::format("cellOskDialogGetInputText(OutputInfo {:#x})", output_info);
    if (!output_info || !usable(ctx, output_info, kReturnParamSize, "cellOskDialogGetInputText: OutputInfo"))
        return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    u8* base = guest_base(ctx);
    const s32 num_chars = static_cast<s32>(ps_read32(base, output_info + 4));
    const u32 result_string = ps_read32(base, output_info + 8);
    if (num_chars < 0) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    OskState& s = osk();
    std::lock_guard lock(s.mutex);
    if (!s.loaded) return logged(error(CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED), call);
    const std::u16string& text = s.valid_text;
    const s32 result = text.empty() ? CELL_OSKDIALOG_INPUT_FIELD_RESULT_NO_INPUT_TEXT : CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK;
    ps_write32(base, output_info + 0, static_cast<u32>(result));
    if (result_string && result == CELL_OSKDIALOG_INPUT_FIELD_RESULT_OK) {
        const u32 limit = static_cast<u32>(num_chars);
        const auto unit = [&](u32 i) -> char16_t { return i < limit && i < text.size() ? text[i] : u'\0'; };
        u32 units = 0;
        while (units < CELL_OSKDIALOG_STRING_SIZE - 1)
            if (!unit(units++)) break;
        if (!usable(ctx, result_string, 2 * units, "cellOskDialogGetInputText: pResultString"))
            return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
        for (u32 i = 0; i < units; i++) ps_write16(base, result_string + 2 * i, unit(i));
    }
    return logged(lv2::CELL_OK, call, std::format(" {{result {}, \"{}\"}}", result, utf8(text)));
}

// cellOskDialogGetSize(width, height, dialogType): RPCS3's 1 x 1, the width 0 for the separate window types (4 up).
s32 cellOskDialogGetSize(PPUContext& ctx, u32 width, u32 height, u32 dialog_type)
{
    const std::string call = std::format("cellOskDialogGetSize({:#x}, {:#x}, type {})", width, height, dialog_type);
    if (!width || !height) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    ps_write16(guest_base(ctx), width, dialog_type >= 4 ? 0 : 1);
    ps_write16(guest_base(ctx), height, 1);
    return logged(lv2::CELL_OK, call);
}

// A setting changed under the state's lock, logged.
s32 set(std::string_view call, const std::function<void(Settings&)>& change)
{
    OskState& s = osk();
    std::lock_guard lock(s.mutex);
    change(s.settings);
    return logged(lv2::CELL_OK, call);
}

s32 cellOskDialogDisableDimmer(PPUContext&)
{
    return set("cellOskDialogDisableDimmer()", [](Settings& st) { st.dimmer = false; });
}

// 0 the pad, 1 the keyboard (CELL_OSKDIALOG_INPUT_DEVICE_*).
s32 cellOskDialogSetInitialInputDevice(PPUContext&, u32 device)
{
    const std::string call = std::format("cellOskDialogSetInitialInputDevice({})", device);
    if (device > 1) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    return set(call, [&](Settings& st) { st.initial_input_device = device; });
}

// The panel shown first: SYSTEM, 10KEY or FULLKEY; kept only when the key layout option offers it (RPCS3).
s32 cellOskDialogSetInitialKeyLayout(PPUContext&, u32 layout)
{
    const std::string call = std::format("cellOskDialogSetInitialKeyLayout({})", layout);
    if (layout > 2) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    return set(call, [&](Settings& st) {
        if (st.key_layout_options & layout) st.initial_key_layout = layout;
    });
}

// CellOskDialogSeparateWindowOption (28 bytes): be32 continuousMode; be s32 deviceMask, inputFieldWindowWidth; be f32
// inputFieldBackgroundTrans; be32 inputFieldLayoutInfo, inputPanelLayoutInfo, reserved. A CellOskDialogLayoutInfo is
// be s32 layoutMode and a CellOskDialogPoint {be f32 x, y}. The panel aligns with the field when it has no layout.
s32 cellOskDialogSetSeparateWindowOption(PPUContext& ctx, u32 option)
{
    const std::string call = std::format("cellOskDialogSetSeparateWindowOption({:#x})", option);
    if (!option || !usable(ctx, option, 28, "cellOskDialogSetSeparateWindowOption: windowOption"))
        return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    u8* base = guest_base(ctx);
    const u32 continuous = ps_read32(base, option + 0);
    const s32 mask = static_cast<s32>(ps_read32(base, option + 4));
    const u32 field = ps_read32(base, option + 16), panel = ps_read32(base, option + 20);
    if (!field || ps_read32(base, option + 24) || continuous > 3 || mask > static_cast<s32>(CELL_OSKDIALOG_DEVICE_MASK_PAD) ||
        !usable(ctx, field, 12, "cellOskDialogSetSeparateWindowOption: inputFieldLayoutInfo") ||
        (panel && !usable(ctx, panel, 12, "cellOskDialogSetSeparateWindowOption: inputPanelLayoutInfo"))) {
        return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    }
    const auto layout_at = [&](u32 at) {
        Layout l;
        l.mode = ps_read32(base, at);
        l.x_align = aligned_x(l.mode);
        l.y_align = aligned_y(l.mode);
        l.x_offset = std::bit_cast<f32>(ps_read32(base, at + 4));
        l.y_offset = std::bit_cast<f32>(ps_read32(base, at + 8));
        return l;
    };
    return set(call, [&](Settings& st) {
        st.separate_windows = true;
        st.continuous_mode = continuous;
        st.device_mask = static_cast<u32>(mask);
        st.field_window_width = static_cast<s32>(ps_read32(base, option + 8));
        st.field_transparency = std::clamp(std::bit_cast<f32>(ps_read32(base, option + 12)), 0.0f, 1.0f);
        st.field_layout = layout_at(field);
        st.panel_layout = panel ? layout_at(panel) : st.field_layout;
    });
}

// ------------------------------------------------------------------------------------- cellOskExtUtility (RPCS3)

s32 cellOskDialogExtInputDeviceLock(PPUContext&)
{
    return set("cellOskDialogExtInputDeviceLock()", [](Settings& st) { st.lock_input_device = true; });
}

s32 cellOskDialogExtInputDeviceUnlock(PPUContext&)
{
    return set("cellOskDialogExtInputDeviceUnlock()", [](Settings& st) { st.lock_input_device = false; });
}

// The callbacks a keyboard UI would call; kept (there is none).
s32 register_callback(std::string_view name, u32 callback, u32 Settings::*slot)
{
    const std::string call = std::format("{}({:#x})", name, callback);
    if (!callback) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    return set(call, [&](Settings& st) { st.*slot = callback; });
}

s32 cellOskDialogExtRegisterConfirmWordFilterCallback(PPUContext&, u32 callback)
{
    return register_callback("cellOskDialogExtRegisterConfirmWordFilterCallback", callback, &Settings::confirm_callback);
}

s32 cellOskDialogExtRegisterForceFinishCallback(PPUContext&, u32 callback)
{
    return register_callback("cellOskDialogExtRegisterForceFinishCallback", callback, &Settings::force_finish_callback);
}

// The hardware keyboard hook: function keys (1), ASCII keys (2), and for the Ex one modifiers alone (4).
s32 keyboard_hook(std::string_view name, u16 mode, u16 allowed, u32 callback)
{
    const std::string call = std::format("{}(hookEventMode {:#x}, {:#x})", name, mode, callback);
    if (mode == 0 || mode > allowed || !callback) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    return set(call, [&](Settings& st) {
        st.keyboard_hook = callback;
        st.hook_event_mode = mode;
    });
}

s32 cellOskDialogExtRegisterKeyboardEventHookCallback(PPUContext&, u16 mode, u32 callback)
{
    return keyboard_hook("cellOskDialogExtRegisterKeyboardEventHookCallback", mode, 3, callback);
}

s32 cellOskDialogExtRegisterKeyboardEventHookCallbackEx(PPUContext&, u16 mode, u32 callback)
{
    return keyboard_hook("cellOskDialogExtRegisterKeyboardEventHookCallbackEx", mode, 7, callback);
}

// Option dictionaries for the Japanese input method: logged, as RPCS3 (nothing uses them).
s32 cellOskDialogExtAddJapaneseOptionDictionary(PPUContext&, u32 paths)
{
    return logged(lv2::CELL_OK, std::format("cellOskDialogExtAddJapaneseOptionDictionary({:#x})", paths));
}

s32 cellOskDialogExtAddOptionDictionary(PPUContext&, u32 info)
{
    const std::string call = std::format("cellOskDialogExtAddOptionDictionary({:#x})", info);
    if (!info) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    return logged(lv2::CELL_OK, call);
}

s32 cellOskDialogExtEnableClipboard(PPUContext&)
{
    return set("cellOskDialogExtEnableClipboard()", [](Settings& st) { st.clipboard = true; });
}

s32 cellOskDialogExtEnableHalfByteKana(PPUContext&)
{
    return set("cellOskDialogExtEnableHalfByteKana()", [](Settings& st) { st.half_byte_kana = true; });
}

s32 cellOskDialogExtDisableHalfByteKana(PPUContext&)
{
    return set("cellOskDialogExtDisableHalfByteKana()", [](Settings& st) { st.half_byte_kana = false; });
}

s32 cellOskDialogExtSetPointerEnable(PPUContext&, bool enable)
{
    return set(std::format("cellOskDialogExtSetPointerEnable({})", enable), [&](Settings& st) { st.pointer = enable; });
}

// CellOskDialogPoint {be f32 x, y}; a null one changes nothing (RPCS3).
s32 cellOskDialogExtUpdatePointerDisplayPos(PPUContext& ctx, u32 pos)
{
    const std::string call = std::format("cellOskDialogExtUpdatePointerDisplayPos({:#x})", pos);
    if (!pos) return logged(lv2::CELL_OK, call);
    if (!usable(ctx, pos, 8, "cellOskDialogExtUpdatePointerDisplayPos: pos")) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    const f32 x = std::bit_cast<f32>(ps_read32(guest_base(ctx), pos)), y = std::bit_cast<f32>(ps_read32(guest_base(ctx), pos + 4));
    return set(call, [&](Settings& st) {
        st.pointer_x = x;
        st.pointer_y = y;
    });
}

// cellOskDialogExtSetBaseColor(red, green, blue, alpha): floats in f1 to f4, each 0 to 1.
s32 cellOskDialogExtSetBaseColor(PPUContext& ctx)
{
    const std::array<f32, 4> c{static_cast<f32>(ctx.fpr[1]), static_cast<f32>(ctx.fpr[2]), static_cast<f32>(ctx.fpr[3]),
        static_cast<f32>(ctx.fpr[4])};
    const std::string call = std::format("cellOskDialogExtSetBaseColor({}, {}, {}, {})", c[0], c[1], c[2], c[3]);
    for (const f32 v : c)
        if (v < 0.0f || v > 1.0f) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    return set(call, [&](Settings& st) { st.base_color = c; });
}

// cellOskDialogExtSetInitialScale(scale): a float in f1, 0.80 to 1.05.
s32 cellOskDialogExtSetInitialScale(PPUContext& ctx)
{
    const f32 scale = static_cast<f32>(ctx.fpr[1]);
    const std::string call = std::format("cellOskDialogExtSetInitialScale({})", scale);
    if (scale < 0.80f || scale > 1.05f) return logged(error(CELL_OSKDIALOG_ERROR_PARAM), call);
    return set(call, [&](Settings& st) { st.initial_scale = scale; });
}

// The current text becomes the text cellOskDialogGetInputText serves, while the dialog is still in use.
s32 cellOskDialogExtUpdateInputText(PPUContext&)
{
    OskState& s = osk();
    std::lock_guard lock(s.mutex);
    if (s.loaded) s.valid_text = s.text.substr(0, CELL_OSKDIALOG_STRING_SIZE - 1);
    return logged(lv2::CELL_OK, "cellOskDialogExtUpdateInputText()", std::format(" {{\"{}\"}}", utf8(s.valid_text)));
}

// The game closes the dialog as the user would (CellOskDialogFinishReason: CONFIRM keeps the text, else cancelled).
// No dialog loaded: CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED (RPCS3).
s32 cellOskDialogExtSendFinishMessage(PPUContext&, u32 reason)
{
    const std::string call = std::format("cellOskDialogExtSendFinishMessage({})", reason);
    u64 generation = 0;
    std::u16string text;
    {
        OskState& s = osk();
        std::lock_guard lock(s.mutex);
        if (!s.loaded) return logged(error(CELL_MSGDIALOG_ERROR_DIALOG_NOT_OPENED), call);
        generation = s.generation;
        text = s.text;
    }
    keyboard_closed(generation, reason == CELL_OSKDIALOG_CLOSE_CONFIRM ? CELL_OSKDIALOG_CLOSE_CONFIRM
                                                                       : CELL_OSKDIALOG_CLOSE_CANCEL,
        std::move(text));
    return logged(lv2::CELL_OK, call);
}

} // namespace

void register_cellOskDialog(Registry& r)
{
    r.add("cellSysutil", {
        PS_HLE_FUNC(0x7FCFC915, cellOskDialogLoadAsync),
        PS_HLE_FUNC(0x3D1E1931, cellOskDialogUnloadAsync),
        PS_HLE_FUNC(0xB6D84526, cellOskDialogAbort),
        PS_HLE_FUNC(0xD3790A86, cellOskDialogSetDeviceMask),
        PS_HLE_FUNC(0xB53C54FA, cellOskDialogSetKeyLayoutOption),
        PS_HLE_FUNC(0x7F21C918, cellOskDialogAddSupportLanguage),
        PS_HLE_FUNC(0xF0EC3CCC, cellOskDialogSetLayoutMode),
        PS_HLE_FUNC(0x1D99C3EE, cellOskDialogGetInputText),
        PS_HLE_FUNC(0x35BEADE0, cellOskDialogGetSize),
        PS_HLE_FUNC(0x41F20828, cellOskDialogDisableDimmer),
        PS_HLE_FUNC(0xF3B4B43E, cellOskDialogSetInitialInputDevice),
        PS_HLE_FUNC(0xC427890C, cellOskDialogSetInitialKeyLayout),
        PS_HLE_FUNC(0x53E39DF3, cellOskDialogSetSeparateWindowOption),
    });
    r.add("cellOskExtUtility", {
        PS_HLE_FUNC(0x09565B21, cellOskDialogExtInputDeviceUnlock),
        PS_HLE_FUNC(0x1E1B4C97, cellOskDialogExtRegisterKeyboardEventHookCallback),
        PS_HLE_FUNC(0xBDCF8383, cellOskDialogExtRegisterKeyboardEventHookCallbackEx),
        PS_HLE_FUNC(0x23A2EDE6, cellOskDialogExtAddJapaneseOptionDictionary),
        PS_HLE_FUNC(0x40187C25, cellOskDialogExtEnableClipboard),
        PS_HLE_FUNC(0x42439DB5, cellOskDialogExtSendFinishMessage),
        PS_HLE_FUNC(0x4FE14D09, cellOskDialogExtAddOptionDictionary),
        PS_HLE_FUNC(0x75370397, cellOskDialogExtSetInitialScale),
        PS_HLE_FUNC(0x7C95FEB8, cellOskDialogExtInputDeviceLock),
        PS_HLE_FUNC(0x7EB292CD, cellOskDialogExtSetBaseColor),
        PS_HLE_FUNC(0x8692FCD2, cellOskDialogExtRegisterConfirmWordFilterCallback),
        PS_HLE_FUNC(0x8B60C469, cellOskDialogExtUpdateInputText),
        PS_HLE_FUNC(0x9CB12C89, cellOskDialogExtDisableHalfByteKana),
        PS_HLE_FUNC(0xC5447A90, cellOskDialogExtSetPointerEnable),
        PS_HLE_FUNC(0xD3EA496B, cellOskDialogExtUpdatePointerDisplayPos),
        PS_HLE_FUNC(0xE6C43C58, cellOskDialogExtEnableHalfByteKana),
        PS_HLE_FUNC(0xE800F586, cellOskDialogExtRegisterForceFinishCallback),
    });
}

} // namespace pspocket::hle
