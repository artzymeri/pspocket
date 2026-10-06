// cellImeJp: the PS3's Japanese input method, which a game drives with its own text entry: characters in, a cursor,
// a focus (the part being converted), conversion candidates, and the confirmed string out. Reference: RPCS3
// rpcs3/Emu/Cell/Modules/cellImeJp.cpp/.h (ime_jp_manager).
//
// As RPCS3: one IME per process, opened with a non-zero memory container and closed again; the input string (UTF-16,
// at most 127 characters), its cursor and its focus move as RPCS3's do; converting offers one candidate, the focus
// string itself (RPCS3 has no dictionary: a TODO), and confirming takes the input as typed. The states and their
// checks are RPCS3's (BEFORE_INPUT, BEFORE_CONVERT, CONVERTING, CANDIDATES...). Extension characters allowed by
// default depend on the game's SDK version (before 3.6: two sets, after: three). The handle is RPCS3's 0xF0000000;
// as in RPCS3, no call checks it. cellImeJpOpenExt is an RPCS3 stub (rpcs3_stubs.inc).
//
// Difference from RPCS3: pointers are checked before use (D-042): memory that isn't guest memory is logged with the call
// site and refused with CELL_IMEJP_ERROR_PARAM before anything changes (RPCS3 faults).

#include <algorithm>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "guest.hpp"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kChan = "cellImeJp";

enum : u32 {
    CELL_IMEJP_ERROR_ERR = 0x8002BF01,
    CELL_IMEJP_ERROR_CONTEXT = 0x8002BF11,
    CELL_IMEJP_ERROR_ALREADY_OPEN = 0x8002BF21,
    CELL_IMEJP_ERROR_PARAM = 0x8002BF41,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

s32 finish(s32 r, const std::string& call)
{
    log::print(r < 0 ? log::Level::Warn : log::Level::Info, kChan, "{}: {}", call,
        r < 0 ? std::format("{:#010x}", static_cast<u32>(r)) : std::string("CELL_OK"));
    return r;
}

// Input states.
constexpr u32 CELL_IMEJP_BEFORE_INPUT = 0, CELL_IMEJP_BEFORE_CONVERT = 1, CELL_IMEJP_CONVERTING = 2,
              CELL_IMEJP_CANDIDATES = 7, CELL_IMEJP_MOVE_CLAUSE_GAP = 8;
constexpr s16 CELL_IMEJP_RET_CONFIRMED = 2;
constexpr s16 CELL_IMEJP_FOCUS_NEXT = 0, CELL_IMEJP_FOCUS_BEFORE = 1, CELL_IMEJP_FOCUS_TOP = 2,
              CELL_IMEJP_FOCUS_END = 3;
constexpr s16 CELL_IMEJP_FIXINMODE_OFF = 0, CELL_IMEJP_DSPCHAR_HIRA = 1, CELL_IMEJP_ROMAN_INPUT = 0;
constexpr s16 CELL_IMEJP_EXTENSIONCH_UD09TO15 = 0x4, CELL_IMEJP_EXTENSIONCH_UD85TO94 = 0x8,
              CELL_IMEJP_EXTENSIONCH_OUTJIS = 0x10;
constexpr usize kStringMax = 128;           // CELL_IMEJP_STRING_MAXLENGTH, the terminator included
constexpr u32 kDicPathMax = 256;          // CELL_IMEJP_DIC_PATH_MAXLENGTH (CellImeJpAddDic)
constexpr u32 kHandle = 0xF0000000;       // RPCS3 ime_jp_address

// RPCS3 ime_jp_manager.
struct ImeJp {
    std::mutex mutex;
    bool extensions_set = false; // RPCS3 sets allowed_extensions when its manager is made
    bool initialized = false;
    u32 input_state = CELL_IMEJP_BEFORE_INPUT;
    std::vector<std::string> dictionary_paths;
    std::u16string confirmed, converted, input;
    usize cursor = 0, focus_begin = 0, focus_length = 0;
    s16 fix_input_mode = CELL_IMEJP_FIXINMODE_OFF, input_char_type = CELL_IMEJP_DSPCHAR_HIRA,
        kana_input_mode = CELL_IMEJP_ROMAN_INPUT, allowed_extensions = 0;

    void move_cursor(s32 amount)
    {
        cursor = static_cast<usize>(std::max(0, std::min(static_cast<s32>(cursor) + amount, static_cast<s32>(input.size()))));
    }

    // RPCS3 move_focus_end: the focus' end moves by `amount` (wrapping between 1 and the rest of the input when asked),
    // its length kept within 1 and the rest of the input; none past the end.
    void move_focus_end(s32 amount, bool wrap_around)
    {
        if (focus_begin >= input.size()) {
            focus_length = 0;
            return;
        }
        constexpr usize min_length = 1;
        const usize max_length = input.size() - focus_begin;
        if (amount > 0) {
            if (wrap_around && focus_length >= max_length) focus_length = min_length;
            else focus_length += static_cast<usize>(amount);
        } else if (amount < 0) {
            if (wrap_around && focus_length <= min_length) focus_length = max_length;
            else focus_length = static_cast<usize>(std::max(0, static_cast<s32>(focus_length) + amount));
        }
        focus_length = std::max(min_length, std::min(max_length, focus_length));
    }

    // RPCS3 move_focus: the focus' start moves within the input, and its end by as much.
    void move_focus(s32 amount)
    {
        focus_begin =
            static_cast<usize>(std::max(0, std::min(static_cast<s32>(focus_begin) + amount, static_cast<s32>(input.size()))));
        move_focus_end(amount, false);
    }

    // RPCS3 addChar: a character at the cursor (none past the 127th); the focus grows when the cursor was in it, else
    // it becomes the new character.
    bool add_char(u16 c)
    {
        if (!c || cursor >= kStringMax - 1 || cursor > input.size()) return false;
        input.insert(cursor, 1, static_cast<char16_t>(c));
        const usize cursor_old = cursor;
        const bool in_focus = cursor >= focus_begin && cursor <= focus_begin + focus_length;
        move_cursor(1);
        if (in_focus) {
            move_focus_end(1, false);
        } else {
            focus_begin = cursor_old;
            focus_length = 1;
            move_focus(0);
        }
        input_state = CELL_IMEJP_BEFORE_CONVERT;
        return true;
    }

    // RPCS3 remove_character: before the cursor (backspace) or at it (delete).
    bool remove_char(bool forward)
    {
        if (!forward && !cursor) return false;
        const usize pos = forward ? cursor : cursor - 1;
        if (pos >= kStringMax - 1 || pos >= input.size()) return false;
        input.erase(pos, 1);
        const bool part_of_focus = pos > focus_begin && pos <= focus_begin + focus_length;
        if (!forward) move_cursor(-1);
        if (part_of_focus) move_focus_end(-1, false);
        else if (focus_begin > pos) move_focus(-1);
        if (input.empty()) input_state = CELL_IMEJP_BEFORE_INPUT;
        return true;
    }

    void clear_input()
    {
        cursor = focus_begin = focus_length = 0;
        input.clear();
        converted.clear();
    }

    std::u16string focus_string() const
    {
        if (input.empty() || focus_length == 0 || focus_begin >= input.size()) return {};
        return input.substr(focus_begin, focus_length);
    }

    // RPCS3 get_candidate_list: one candidate, the focus string (RPCS3 has no dictionary).
    std::vector<std::u16string> candidates() const
    {
        if (input.empty() || focus_length == 0 || focus_begin >= input.size()) return {};
        return {focus_string()};
    }
};

// The process's IME, its default extension characters set from the game's SDK version on first use (RPCS3's
// ime_jp_manager constructor).
ImeJp& ime(PPUContext& ctx, std::unique_lock<std::mutex>& lock)
{
    HleProcess* p = HleProcess::current();
    if (!p) PS_FATAL(kChan, "cellImeJp called without an HLE process");
    ImeJp& m = p->state<ImeJp>();
    lock = std::unique_lock(m.mutex);
    if (!m.extensions_set) {
        const bool old_sdk = static_cast<s32>(thread_of(ctx).kernel.config().sdk_version) < 0x360000;
        m.allowed_extensions = static_cast<s16>((old_sdk ? 0 : CELL_IMEJP_EXTENSIONCH_UD09TO15) |
                                                CELL_IMEJP_EXTENSIONCH_UD85TO94 | CELL_IMEJP_EXTENSIONCH_OUTJIS);
        m.extensions_set = true;
    }
    return m;
}

// Whether [addr, addr + size) is guest memory; if not, the call site is logged (D-042: RPCS3 would fault).
bool usable(PPUContext& ctx, u32 addr, u32 size, std::string_view what)
{
    if (guest_readable(thread_of(ctx), addr, size)) return true;
    log_call_site(ctx, guest_base(ctx), std::format("{} ({:#x}, {} bytes) is not guest memory", what, addr, size));
    return false;
}

// A dictionary path (CellImeJpAddDic: char path[256]), when it isn't empty.
void add_dictionary(PPUContext& ctx, ImeJp& m, u32 dic)
{
    if (!dic || !usable(ctx, dic, kDicPathMax, "cellImeJpOpen: addDicPath")) return;
    std::string path = read_cstr(thread_of(ctx), dic, kDicPathMax).text;
    if (path.empty()) return;
    PS_LOG_INFO(kChan, "cellImeJpOpen: dictionary \"{}\" (not used)", path);
    m.dictionary_paths.push_back(std::move(path));
}

// RPCS3 cellImeJpOpen, Open2, Open3: a container and a handle pointer; one IME at a time; dictionaries noted.
s32 open(PPUContext& ctx, std::string_view name, u32 container, u32 handle, u32 dic, bool dic_list)
{
    const std::string call = std::format("{}(container {:#x}, hImeJpHandle {:#x}, addDicPath {:#x})", name, container,
        handle, dic);
    if (!container || !handle || !usable(ctx, handle, 4, name)) return finish(error(CELL_IMEJP_ERROR_PARAM), call);
    std::unique_lock<std::mutex> lock;
    ImeJp& m = ime(ctx, lock);
    if (m.initialized) return finish(error(CELL_IMEJP_ERROR_ALREADY_OPEN), call);
    if (dic_list) {
        if (dic && usable(ctx, dic, 16, name)) {
            for (u32 i = 0; i < 4; i++) add_dictionary(ctx, m, ps_read32(guest_base(ctx), dic + 4 * i));
        }
    } else {
        add_dictionary(ctx, m, dic);
    }
    ps_write32(guest_base(ctx), handle, kHandle);
    m.initialized = true;
    return finish(lv2::CELL_OK, call);
}

s32 cellImeJpOpen(PPUContext& ctx, u32 container, u32 handle, u32 dic)
{
    return open(ctx, "cellImeJpOpen", container, handle, dic, false);
}

s32 cellImeJpOpen2(PPUContext& ctx, u32 container, u32 handle, u32 dic)
{
    return open(ctx, "cellImeJpOpen2", container, handle, dic, false);
}

// Open3 takes four CellImeJpAddDic pointers.
s32 cellImeJpOpen3(PPUContext& ctx, u32 container, u32 handle, u32 dic)
{
    return open(ctx, "cellImeJpOpen3", container, handle, dic, true);
}

// The calls on an open IME whose state must satisfy `allowed`: CONTEXT when closed, ERR when the state doesn't fit.
template <class Fn>
s32 with_ime(PPUContext& ctx, const std::string& call, bool (*allowed)(u32), Fn&& fn)
{
    std::unique_lock<std::mutex> lock;
    ImeJp& m = ime(ctx, lock);
    if (!m.initialized) return finish(error(CELL_IMEJP_ERROR_CONTEXT), call);
    if (allowed && !allowed(m.input_state)) return finish(error(CELL_IMEJP_ERROR_ERR), call);
    fn(m);
    return finish(lv2::CELL_OK, call);
}

bool before_input(u32 s) { return s == CELL_IMEJP_BEFORE_INPUT; }
bool before_convert(u32 s) { return s == CELL_IMEJP_BEFORE_CONVERT; }
bool after_input(u32 s) { return s != CELL_IMEJP_BEFORE_INPUT; }
bool converting(u32 s) { return s != CELL_IMEJP_BEFORE_INPUT && s != CELL_IMEJP_BEFORE_CONVERT; }
bool candidates(u32 s) { return s == CELL_IMEJP_CANDIDATES; }
bool not_clause_gap(u32 s) { return s != CELL_IMEJP_MOVE_CLAUSE_GAP; }

s32 cellImeJpClose(PPUContext& ctx, u32 handle)
{
    return with_ime(ctx, std::format("cellImeJpClose({:#x})", handle), nullptr, [](ImeJp& m) {
        m.input_state = CELL_IMEJP_BEFORE_INPUT;
        m.clear_input();
        m.confirmed.clear();
        m.initialized = false;
    });
}

s32 cellImeJpReset(PPUContext& ctx, u32 handle)
{
    return with_ime(ctx, std::format("cellImeJpReset({:#x})", handle), nullptr, [](ImeJp& m) {
        m.input_state = CELL_IMEJP_BEFORE_INPUT;
        m.clear_input();
        m.confirmed.clear();
    });
}

// The input settings: the kana mode and the extension characters only before input.
s32 cellImeJpSetKanaInputMode(PPUContext& ctx, u32 handle, s16 mode)
{
    return with_ime(ctx, std::format("cellImeJpSetKanaInputMode({:#x}, {})", handle, mode), before_input,
        [mode](ImeJp& m) { m.kana_input_mode = mode; });
}

s32 cellImeJpSetInputCharType(PPUContext& ctx, u32 handle, s16 type)
{
    return with_ime(ctx, std::format("cellImeJpSetInputCharType({:#x}, {})", handle, type), nullptr,
        [type](ImeJp& m) { m.input_char_type = type; });
}

s32 cellImeJpSetFixInputMode(PPUContext& ctx, u32 handle, s16 mode)
{
    return with_ime(ctx, std::format("cellImeJpSetFixInputMode({:#x}, {})", handle, mode), nullptr,
        [mode](ImeJp& m) { m.fix_input_mode = mode; });
}

s32 cellImeJpAllowExtensionCharacters(PPUContext& ctx, u32 handle, s16 extensions)
{
    return with_ime(ctx, std::format("cellImeJpAllowExtensionCharacters({:#x}, {:#x})", handle, extensions),
        before_input, [extensions](ImeJp& m) { m.allowed_extensions = extensions; });
}

// RPCS3 cellImeJpGetStatus(handle, s16* status): the pointer first.
s32 cellImeJpGetStatus(PPUContext& ctx, u32 handle, u32 status)
{
    const std::string call = std::format("cellImeJpGetStatus({:#x}, {:#x})", handle, status);
    if (!status || !usable(ctx, status, 2, call)) return finish(error(CELL_IMEJP_ERROR_PARAM), call);
    return with_ime(ctx, call, nullptr,
        [&](ImeJp& m) { ps_write16(guest_base(ctx), status, static_cast<u16>(m.input_state)); });
}

// RPCS3 cellImeJpEnterChar(handle, char, s16* outputStatus): the character added (when it fits), CONFIRMED out.
s32 enter_char(PPUContext& ctx, std::string_view name, u32 handle, u16 c, u32 out)
{
    const std::string call = std::format("{}({:#x}, {:#06x}, {:#x})", name, handle, c, out);
    if (!out || !usable(ctx, out, 2, call)) return finish(error(CELL_IMEJP_ERROR_PARAM), call);
    return with_ime(ctx, call, not_clause_gap, [&](ImeJp& m) {
        m.add_char(c);
        ps_write16(guest_base(ctx), out, static_cast<u16>(CELL_IMEJP_RET_CONFIRMED));
    });
}

s32 cellImeJpEnterChar(PPUContext& ctx, u32 handle, u16 c, u32 out)
{
    return enter_char(ctx, "cellImeJpEnterChar", handle, c, out);
}

s32 cellImeJpEnterCharExt(PPUContext& ctx, u32 handle, u16 c, u32 out)
{
    return enter_char(ctx, "cellImeJpEnterCharExt", handle, c, out);
}

// RPCS3 cellImeJpEnterString(handle, const u16* string, s16* outputStatus): characters added up to the NUL (or as
// many as fit, 128 at most), CONFIRMED out.
s32 enter_string(PPUContext& ctx, std::string_view name, u32 handle, u32 str, u32 out)
{
    const std::string call = std::format("{}({:#x}, {:#x}, {:#x})", name, handle, str, out);
    if (!out || !usable(ctx, out, 2, call)) return finish(error(CELL_IMEJP_ERROR_PARAM), call);
    return with_ime(ctx, call, not_clause_gap, [&](ImeJp& m) {
        lv2::PpuThread& t = thread_of(ctx);
        for (u32 i = 0; str && i < kStringMax; i++) {
            if (!guest_readable(t, str + 2 * i, 2)) {
                log_call_site(ctx, guest_base(ctx), call + ": the string runs out of guest memory");
                break;
            }
            if (!m.add_char(ps_read16(guest_base(ctx), str + 2 * i))) break;
        }
        ps_write16(guest_base(ctx), out, static_cast<u16>(CELL_IMEJP_RET_CONFIRMED));
    });
}

s32 cellImeJpEnterString(PPUContext& ctx, u32 handle, u32 str, u32 out)
{
    return enter_string(ctx, "cellImeJpEnterString", handle, str, out);
}

s32 cellImeJpEnterStringExt(PPUContext& ctx, u32 handle, u32 str, u32 out)
{
    return enter_string(ctx, "cellImeJpEnterStringExt", handle, str, out);
}

// The cursor and editing before conversion.
s32 cellImeJpModeCaretRight(PPUContext& ctx, u32 handle)
{
    return with_ime(ctx, std::format("cellImeJpModeCaretRight({:#x})", handle), before_convert,
        [](ImeJp& m) { m.move_cursor(1); });
}

s32 cellImeJpModeCaretLeft(PPUContext& ctx, u32 handle)
{
    return with_ime(ctx, std::format("cellImeJpModeCaretLeft({:#x})", handle), before_convert,
        [](ImeJp& m) { m.move_cursor(-1); });
}

s32 cellImeJpBackspaceWord(PPUContext& ctx, u32 handle)
{
    return with_ime(ctx, std::format("cellImeJpBackspaceWord({:#x})", handle), before_convert,
        [](ImeJp& m) { m.remove_char(false); });
}

s32 cellImeJpDeleteWord(PPUContext& ctx, u32 handle)
{
    return with_ime(ctx, std::format("cellImeJpDeleteWord({:#x})", handle), before_convert,
        [](ImeJp& m) { m.remove_char(true); });
}

s32 cellImeJpAllDeleteConvertString(PPUContext& ctx, u32 handle)
{
    return with_ime(ctx, std::format("cellImeJpAllDeleteConvertString({:#x})", handle), after_input, [](ImeJp& m) {
        m.clear_input();
        m.input_state = CELL_IMEJP_BEFORE_INPUT;
    });
}

// Converting: the candidates are offered (CANDIDATES).
s32 cellImeJpConvertForward(PPUContext& ctx, u32 handle)
{
    return with_ime(ctx, std::format("cellImeJpConvertForward({:#x})", handle), after_input,
        [](ImeJp& m) { m.input_state = CELL_IMEJP_CANDIDATES; });
}

s32 cellImeJpConvertBackward(PPUContext& ctx, u32 handle)
{
    return with_ime(ctx, std::format("cellImeJpConvertBackward({:#x})", handle), after_input,
        [](ImeJp& m) { m.input_state = CELL_IMEJP_CANDIDATES; });
}

// RPCS3's TODOs after their state checks.
s32 cellImeJpCurrentPartConfirm(PPUContext& ctx, u32 handle, s16 item)
{
    return with_ime(ctx, std::format("cellImeJpCurrentPartConfirm({:#x}, {})", handle, item), after_input,
        [](ImeJp&) {});
}

s32 cellImeJpTemporalConfirm(PPUContext& ctx, u32 handle, s16 index)
{
    return with_ime(ctx, std::format("cellImeJpTemporalConfirm({:#x}, {})", handle, index), candidates,
        [](ImeJp&) {});
}

s32 cellImeJpPostConvert(PPUContext& ctx, u32 handle, s16 type)
{
    return with_ime(ctx, std::format("cellImeJpPostConvert({:#x}, {})", handle, type), after_input, [](ImeJp&) {});
}

// RPCS3 cellImeJpAllConfirm: the input as typed becomes the confirmed string.
s32 cellImeJpAllConfirm(PPUContext& ctx, u32 handle)
{
    return with_ime(ctx, std::format("cellImeJpAllConfirm({:#x})", handle), after_input, [](ImeJp& m) {
        m.confirmed = m.input;
        m.clear_input();
        m.input_state = CELL_IMEJP_BEFORE_INPUT;
    });
}

s32 cancel_convert(PPUContext& ctx, const std::string& call)
{
    return with_ime(ctx, call, converting, [](ImeJp& m) {
        m.converted.clear();
        m.input_state = CELL_IMEJP_BEFORE_CONVERT;
    });
}

s32 cellImeJpAllConvertCancel(PPUContext& ctx, u32 handle)
{
    return cancel_convert(ctx, std::format("cellImeJpAllConvertCancel({:#x})", handle));
}

s32 cellImeJpConvertCancel(PPUContext& ctx, u32 handle)
{
    return cancel_convert(ctx, std::format("cellImeJpConvertCancel({:#x})", handle));
}

// The focus' end moves by one, wrapping around.
s32 cellImeJpExtendConvertArea(PPUContext& ctx, u32 handle)
{
    return with_ime(ctx, std::format("cellImeJpExtendConvertArea({:#x})", handle), converting,
        [](ImeJp& m) { m.move_focus_end(1, true); });
}

s32 cellImeJpShortenConvertArea(PPUContext& ctx, u32 handle)
{
    return with_ime(ctx, std::format("cellImeJpShortenConvertArea({:#x})", handle), converting,
        [](ImeJp& m) { m.move_focus_end(-1, true); });
}

// RPCS3 cellImeJpMoveFocusClause(handle, moveType): next, before, top or end; then CONVERTING.
s32 cellImeJpMoveFocusClause(PPUContext& ctx, u32 handle, s16 type)
{
    const auto allowed = [](u32 s) { return converting(s) && s != CELL_IMEJP_MOVE_CLAUSE_GAP; };
    return with_ime(ctx, std::format("cellImeJpMoveFocusClause({:#x}, {})", handle, type), +allowed, [type](ImeJp& m) {
        const s32 length = static_cast<s32>(m.input.size());
        switch (type) {
        case CELL_IMEJP_FOCUS_NEXT: m.move_focus(1); break;
        case CELL_IMEJP_FOCUS_BEFORE: m.move_focus(-1); break;
        case CELL_IMEJP_FOCUS_TOP: m.move_focus(-length); break;
        case CELL_IMEJP_FOCUS_END:
            m.move_focus(length);
            m.move_focus(-1);
            break;
        default: break;
        }
        m.input_state = CELL_IMEJP_CONVERTING;
    });
}

// An s16 out of the open IME: the pointer first.
s32 get_s16(PPUContext& ctx, const std::string& call, u32 out, bool (*allowed)(u32), s16 (*value)(const ImeJp&))
{
    if (!out || !usable(ctx, out, 2, call)) return finish(error(CELL_IMEJP_ERROR_PARAM), call);
    return with_ime(ctx, call, allowed,
        [&](ImeJp& m) { ps_write16(guest_base(ctx), out, static_cast<u16>(value(m))); });
}

// The focus in bytes.
s32 cellImeJpGetFocusTop(PPUContext& ctx, u32 handle, u32 top)
{
    return get_s16(ctx, std::format("cellImeJpGetFocusTop({:#x}, {:#x})", handle, top), top, nullptr,
        [](const ImeJp& m) { return static_cast<s16>(m.focus_begin * 2); });
}

s32 cellImeJpGetFocusLength(PPUContext& ctx, u32 handle, u32 length)
{
    return get_s16(ctx, std::format("cellImeJpGetFocusLength({:#x}, {:#x})", handle, length), length, nullptr,
        [](const ImeJp& m) { return static_cast<s16>(m.focus_length * 2); });
}

// A string out (u16[128]): up to 127 characters, the rest zero.
s32 get_string(PPUContext& ctx, std::string_view name, u32 handle, u32 out, bool confirmed)
{
    const std::string call = std::format("{}({:#x}, {:#x})", name, handle, out);
    if (!out || !usable(ctx, out, 2 * kStringMax, call)) return finish(error(CELL_IMEJP_ERROR_PARAM), call);
    return with_ime(ctx, call, nullptr, [&](ImeJp& m) {
        const std::u16string& s = confirmed ? m.confirmed : m.input;
        const usize n = std::min(kStringMax - 1, s.size());
        u8* base = guest_base(ctx);
        for (usize i = 0; i < kStringMax; i++) {
            ps_write16(base, out + 2 * static_cast<u32>(i), i < n ? static_cast<u16>(s[i]) : 0);
        }
    });
}

s32 cellImeJpGetConfirmYomiString(PPUContext& ctx, u32 handle, u32 out)
{
    return get_string(ctx, "cellImeJpGetConfirmYomiString", handle, out, true);
}

s32 cellImeJpGetConfirmString(PPUContext& ctx, u32 handle, u32 out)
{
    return get_string(ctx, "cellImeJpGetConfirmString", handle, out, true);
}

s32 cellImeJpGetConvertYomiString(PPUContext& ctx, u32 handle, u32 out)
{
    return get_string(ctx, "cellImeJpGetConvertYomiString", handle, out, false);
}

s32 cellImeJpGetConvertString(PPUContext& ctx, u32 handle, u32 out)
{
    return get_string(ctx, "cellImeJpGetConvertString", handle, out, false);
}

// RPCS3 cellImeJpGetCandidateListSize: the bytes of the focus string and of each candidate with its offset, each
// NUL-terminated.
s32 cellImeJpGetCandidateListSize(PPUContext& ctx, u32 handle, u32 size)
{
    return get_s16(ctx, std::format("cellImeJpGetCandidateListSize({:#x}, {:#x})", handle, size), size, candidates,
        [](const ImeJp& m) {
            usize bytes = 2 * (m.focus_string().size() + 1);
            for (const std::u16string& c : m.candidates()) bytes += 2 + 2 * (c.size() + 1);
            return static_cast<s16>(bytes);
        });
}

// RPCS3 cellImeJpGetCandidateList(handle, s16* listNum, u16* candidates): the focus string, NUL, then each candidate,
// NUL and its offset (0); the count out.
s32 cellImeJpGetCandidateList(PPUContext& ctx, u32 handle, u32 list_num, u32 out)
{
    const std::string call = std::format("cellImeJpGetCandidateList({:#x}, {:#x}, {:#x})", handle, list_num, out);
    if (!list_num || !out || !usable(ctx, list_num, 2, call)) return finish(error(CELL_IMEJP_ERROR_PARAM), call);
    std::u16string data; // what goes out, as u16 values
    s16 count = 0;
    {
        std::unique_lock<std::mutex> lock;
        const ImeJp& m = ime(ctx, lock);
        if (m.initialized && m.input_state == CELL_IMEJP_CANDIDATES) {
            data = m.focus_string();
            data += u'\0';
            for (const std::u16string& c : m.candidates()) {
                data += c;
                data += u'\0';
                data += u'\0'; // the offset
                count++;
            }
        }
    }
    if (!data.empty() && !usable(ctx, out, 2 * static_cast<u32>(data.size()), call)) {
        return finish(error(CELL_IMEJP_ERROR_PARAM), call);
    }
    return with_ime(ctx, call, candidates, [&](ImeJp&) {
        u8* base = guest_base(ctx);
        for (usize i = 0; i < data.size(); i++) ps_write16(base, out + 2 * static_cast<u32>(i), static_cast<u16>(data[i]));
        ps_write16(base, list_num, static_cast<u16>(count));
    });
}

s32 cellImeJpGetCandidateSelect(PPUContext& ctx, u32 handle, u32 index)
{
    return get_s16(ctx, std::format("cellImeJpGetCandidateSelect({:#x}, {:#x})", handle, index), index, candidates,
        [](const ImeJp&) { return s16{0}; });
}

// RPCS3 cellImeJpGetPredictList(handle, yomi, itemNum, s32* listCount, CellImeJpPredictItem*): no predictions.
s32 cellImeJpGetPredictList(PPUContext& ctx, u32 handle, u32 yomi, s32 item_num, u32 list_count, u32 items)
{
    const std::string call = std::format("cellImeJpGetPredictList({:#x}, {:#x}, {}, {:#x}, {:#x})", handle, yomi,
        item_num, list_count, items);
    if (!items || !list_count || !usable(ctx, list_count, 4, call)) {
        return finish(error(CELL_IMEJP_ERROR_PARAM), call);
    }
    return with_ime(ctx, call, nullptr, [&](ImeJp&) { ps_write32(guest_base(ctx), list_count, 0); });
}

s32 cellImeJpConfirmPrediction(PPUContext& ctx, u32 handle, u32 item)
{
    const std::string call = std::format("cellImeJpConfirmPrediction({:#x}, {:#x})", handle, item);
    if (!item) return finish(error(CELL_IMEJP_ERROR_PARAM), call);
    return with_ime(ctx, call, nullptr, [](ImeJp&) {});
}

} // namespace

void register_cellImeJp(Registry& r)
{
    r.add("cellImeJpUtility", {
        PS_HLE_FUNC(0x44608862, cellImeJpOpen),
        PS_HLE_FUNC(0x47B43DD4, cellImeJpOpen2),
        PS_HLE_FUNC(0x1B119958, cellImeJpOpen3),
        PS_HLE_FUNC(0x46D1234A, cellImeJpClose),
        PS_HLE_FUNC(0x24E9D8FC, cellImeJpSetKanaInputMode),
        PS_HLE_FUNC(0xF5992EC8, cellImeJpSetInputCharType),
        PS_HLE_FUNC(0xC1786C81, cellImeJpSetFixInputMode),
        PS_HLE_FUNC(0x8FF02267, cellImeJpAllowExtensionCharacters),
        PS_HLE_FUNC(0x36D38701, cellImeJpReset),
        PS_HLE_FUNC(0x66C6CC78, cellImeJpGetStatus),
        PS_HLE_FUNC(0x6CCBE3D6, cellImeJpEnterChar),
        PS_HLE_FUNC(0x5B6ADA55, cellImeJpEnterCharExt),
        PS_HLE_FUNC(0x441A1C2B, cellImeJpEnterString),
        PS_HLE_FUNC(0x6298B55A, cellImeJpEnterStringExt),
        PS_HLE_FUNC(0xAC6693D8, cellImeJpModeCaretRight),
        PS_HLE_FUNC(0xE76C9700, cellImeJpModeCaretLeft),
        PS_HLE_FUNC(0xAA1D1F57, cellImeJpBackspaceWord),
        PS_HLE_FUNC(0x72257652, cellImeJpDeleteWord),
        PS_HLE_FUNC(0x6319EDA3, cellImeJpAllDeleteConvertString),
        PS_HLE_FUNC(0x1E29103B, cellImeJpConvertForward),
        PS_HLE_FUNC(0xC2BB48BC, cellImeJpConvertBackward),
        PS_HLE_FUNC(0x7A18C2B9, cellImeJpCurrentPartConfirm),
        PS_HLE_FUNC(0x7189430B, cellImeJpAllConfirm),
        PS_HLE_FUNC(0xEAE879DC, cellImeJpConvertCancel),
        PS_HLE_FUNC(0xCBBC20B7, cellImeJpAllConvertCancel),
        PS_HLE_FUNC(0x37961CC1, cellImeJpExtendConvertArea),
        PS_HLE_FUNC(0xAA2A3287, cellImeJpShortenConvertArea),
        PS_HLE_FUNC(0xBD679CC1, cellImeJpTemporalConfirm),
        PS_HLE_FUNC(0x8BB41F47, cellImeJpPostConvert),
        PS_HLE_FUNC(0x1E411261, cellImeJpMoveFocusClause),
        PS_HLE_FUNC(0x0E363AE7, cellImeJpGetFocusTop),
        PS_HLE_FUNC(0x5F5B3227, cellImeJpGetFocusLength),
        PS_HLE_FUNC(0x89F8A567, cellImeJpGetConfirmYomiString),
        PS_HLE_FUNC(0xD3FC3606, cellImeJpGetConfirmString),
        PS_HLE_FUNC(0xEA2D4881, cellImeJpGetConvertYomiString),
        PS_HLE_FUNC(0xF91ABDA3, cellImeJpGetConvertString),
        PS_HLE_FUNC(0xC4796A45, cellImeJpGetCandidateListSize),
        PS_HLE_FUNC(0xE4CC15BA, cellImeJpGetCandidateList),
        PS_HLE_FUNC(0x177BD218, cellImeJpGetCandidateSelect),
        PS_HLE_FUNC(0x1986F2CD, cellImeJpGetPredictList),
        PS_HLE_FUNC(0xEEDE898C, cellImeJpConfirmPrediction),
    });
}

} // namespace pspocket::hle
