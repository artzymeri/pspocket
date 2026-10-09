// cellKey2char: keyboard key codes to the characters they type (libkey2char), pspocket's own (D-565).
// Reference: RPCS3 rpcs3/Emu/Cell/Modules/cellKey2char.cpp: the error codes, the modes, the 128-byte handle the game
// owns and what RPCS3 read of it (byte 8: opened; byte 0: 255 when broken; byte 1: the keyboard arrangement), and the
// checks of each call; RPCS3 converts nothing (its GetChar is a TODO). No Sony module was at hand to compare with: the
// library is firmware-first (D-563).
// pspocket's: a key becomes a character as cellKbCnvRawCode makes it in the handle's arrangement (sys_io, RPCS3's
// tables: the letters, digits and punctuation of the 101, 106 and German layouts, Enter, Backspace, Tab, Space, the
// keypad with num lock, shift and caps lock), as UTF-16: the few codes that table gives past ASCII (code page 850's
// ¥ ß ´ § °) become their Unicode characters. GetChar hands back a pointer to the character, kept in the handle (bytes
// 0x40 on), its count (one, or none for a key that types nothing, which is not processed). The native modes (kana,
// Hangul, Cyrillic, Bopomofo, Cangjie input) are accepted and type as the English one (logged once). Open takes the
// arrangement the system settings give (cellSysutil's keyboard type); byte 2 keeps the mode.

#include <mutex>
#include <string_view>

#include "guest.hpp"
#include "pspocket/core/be.h"
#include "pspocket/core/log.hpp"
#include "pspocket/hle/hle.hpp"
#include "pspocket/hle/process.hpp"
#include "pspocket/lv2/errors.hpp"
#include "pspocket/recomp/nid.hpp"

namespace pspocket::hle {
namespace {

constexpr std::string_view kModule = "cellKey2char";
constexpr const char* kChan = "cellKey2char";

enum : u32 {
    CELL_K2C_ERROR_FATAL = 0x80121301,
    CELL_K2C_ERROR_INVALID_HANDLE = 0x80121302,
    CELL_K2C_ERROR_INVALID_PARAMETER = 0x80121303,
    CELL_K2C_ERROR_ALREADY_INITIALIZED = 0x80121304,
    CELL_K2C_ERROR_UNINITIALIZED = 0x80121305,
    CELL_K2C_ERROR_OTHER = 0x80121306,
};
constexpr s32 error(u32 code) { return static_cast<s32>(code); }

constexpr s32 kModeEnglish = 0, kModeNative2 = 2;
constexpr s32 kMappingLast = 24; // CELL_KB_MAPPING_TURKISH_TURKEY
constexpr u32 kSystemKeyboardType = 0x0153; // CELL_SYSUTIL_SYSTEMPARAM_ID_KEYBOARD_TYPE

// The handle's bytes.
constexpr u32 kHandleSize = 128, kStatus = 0, kArrangement = 1, kMode = 2, kOpened = 8, kChars = 0x40;
constexpr u8 kBroken = 255;
// cellKbCnvRawCode's flags: a key that is no character, a keypad character.
constexpr u32 kRawDat = 0x8000, kKeypad = 0x4000;

struct State {
    std::mutex mutex;
    bool native_logged = false;
};

State& state() { return HleProcess::current()->state<State>(); }

u8* gb(PPUContext& ctx) { return guest_base(ctx); }

// A function of another library pspocket always serves, called as libkey2char calls it: r3.
u64 lib_call(PPUContext& ctx, std::string_view module, std::string_view name, std::initializer_list<u64> args)
{
    const Function* f = registry().find(module, recomp::nid_of(name));
    if (!f) PS_FATAL(kChan, "{}::{}, which libkey2char calls, is not pspocket's", module, name);
    u64 saved[8];
    for (u32 k = 0; k < 8; k++) saved[k] = ctx.gpr[3 + k];
    u32 i = 3;
    for (const u64 a : args) ctx.gpr[i++] = a;
    f->fn(ctx, gb(ctx));
    const u64 r = ctx.gpr[3];
    for (u32 k = 0; k < 8; k++) ctx.gpr[3 + k] = saved[k];
    return r;
}

// The handle's checks shared by every call but Open: there, and opened.
s32 opened(PPUContext& ctx, u32 handle)
{
    if (!handle || !guest_readable(thread_of(ctx), handle, kHandleSize)) return error(CELL_K2C_ERROR_INVALID_HANDLE);
    if (ps_read8(gb(ctx), handle + kOpened) == 0) return error(CELL_K2C_ERROR_UNINITIALIZED);
    return lv2::CELL_OK;
}

// Code page 850's characters cellKbCnvRawCode's layouts give, as Unicode.
u16 unicode_of(u32 c)
{
    switch (c) {
    case 190: return 0x00A5; // yen
    case 225: return 0x00DF; // sharp s
    case 239: return 0x00B4; // acute accent
    case 245: return 0x00A7; // section
    case 248: return 0x00B0; // degree
    default: return static_cast<u16>(c);
    }
}

s32 cellKey2CharOpen(PPUContext& ctx, u32 handle)
{
    u8* base = gb(ctx);
    if (!handle || !guest_readable(thread_of(ctx), handle, kHandleSize)) return error(CELL_K2C_ERROR_INVALID_HANDLE);
    if (ps_read8(base, handle + kOpened) != 0) return error(CELL_K2C_ERROR_ALREADY_INITIALIZED);
    StackVar value(ctx, base, 8);
    s32 arrangement = 0;
    if (static_cast<s32>(lib_call(ctx, "cellSysutil", "cellSysutilGetSystemParamInt", {kSystemKeyboardType,
                                                                                        value.addr()})) == 0)
        arrangement = static_cast<s32>(ps_read32(base, value.addr()));
    if (arrangement < 0 || arrangement > kMappingLast) arrangement = 0;
    for (u32 i = kChars; i < kHandleSize; i++) ps_write8(base, handle + i, 0);
    ps_write8(base, handle + kStatus, 0);
    ps_write8(base, handle + kArrangement, static_cast<u8>(arrangement));
    ps_write8(base, handle + kMode, kModeEnglish);
    ps_write8(base, handle + kOpened, 1);
    return lv2::CELL_OK;
}

s32 cellKey2CharClose(PPUContext& ctx, u32 handle)
{
    if (const s32 r = opened(ctx, handle); r != lv2::CELL_OK) return r;
    ps_write8(gb(ctx), handle + kOpened, 0);
    return lv2::CELL_OK;
}

// cellKey2CharGetChar(handle, const CellKey2CharKeyData *kdata {led, mkey, keycode (u16)}, uint16_t **charCode,
// uint32_t *charNum, bool *processed).
s32 cellKey2CharGetChar(PPUContext& ctx, u32 handle, u32 kdata, u32 char_code, u32 char_num, u32 processed)
{
    u8* base = gb(ctx);
    if (const s32 r = opened(ctx, handle); r != lv2::CELL_OK) return r;
    lv2::PpuThread& t = thread_of(ctx);
    if (!char_code || !kdata || !guest_readable(t, kdata, 10) || !guest_readable(t, char_code, 4))
        return error(CELL_K2C_ERROR_INVALID_PARAMETER);
    const u32 led = ps_read32(base, kdata), mkey = ps_read32(base, kdata + 4);
    const u16 keycode = ps_read16(base, kdata + 8);
    if (!keycode) return error(CELL_K2C_ERROR_INVALID_PARAMETER);
    if (ps_read8(base, handle + kStatus) == kBroken) return error(CELL_K2C_ERROR_OTHER);
    if (ps_read8(base, handle + kMode) != kModeEnglish) {
        State& s = state();
        std::lock_guard lock(s.mutex);
        if (!s.native_logged) {
            s.native_logged = true;
            PS_LOG_INFO(kChan, "cellKey2CharGetChar: native mode {} (arrangement {}) types as the English one",
                ps_read8(base, handle + kMode), ps_read8(base, handle + kArrangement));
        }
    }
    const u32 arrangement = ps_read8(base, handle + kArrangement);
    const u32 c = static_cast<u32>(lib_call(ctx, "sys_io", "cellKbCnvRawCode", {arrangement, mkey, led,
                                                                                 static_cast<u64>(keycode & 0xFF)}));
    const bool typed = c != 0 && !(c & kRawDat);
    if (typed) ps_write16(base, handle + kChars, unicode_of(c & ~kKeypad & 0xFFFF));
    ps_write32(base, char_code, handle + kChars);
    if (char_num && guest_readable(t, char_num, 4)) ps_write32(base, char_num, typed ? 1 : 0);
    if (processed && guest_readable(t, processed, 1)) ps_write8(base, processed, typed ? 1 : 0);
    return lv2::CELL_OK;
}

s32 cellKey2CharSetMode(PPUContext& ctx, u32 handle, s32 mode)
{
    if (const s32 r = opened(ctx, handle); r != lv2::CELL_OK) return r;
    if (mode < kModeEnglish || mode > kModeNative2) return error(CELL_K2C_ERROR_INVALID_PARAMETER);
    u8* base = gb(ctx);
    if (ps_read8(base, handle + kStatus) == kBroken) return error(CELL_K2C_ERROR_OTHER);
    ps_write8(base, handle + kMode, static_cast<u8>(mode));
    return lv2::CELL_OK;
}

s32 cellKey2CharSetArrangement(PPUContext& ctx, u32 handle, s32 arrange)
{
    if (const s32 r = opened(ctx, handle); r != lv2::CELL_OK) return r;
    if (arrange < 0 || arrange > kMappingLast) return error(CELL_K2C_ERROR_INVALID_PARAMETER);
    ps_write8(gb(ctx), handle + kArrangement, static_cast<u8>(arrange));
    return lv2::CELL_OK;
}

} // namespace

void register_cellKey2char(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0xABF629C1, cellKey2CharOpen),
        PS_HLE_FUNC(0x14BF2DC1, cellKey2CharClose),
        PS_HLE_FUNC(0x56776C0D, cellKey2CharGetChar),
        PS_HLE_FUNC(0xBFC03768, cellKey2CharSetMode),
        PS_HLE_FUNC(0x0DFBADFA, cellKey2CharSetArrangement),
    });
    r.firmware_first(kModule);
}

} // namespace pspocket::hle
