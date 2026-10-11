// cellKey2char: keyboard key codes to the characters they type (libkey2char), pspocket's own (D-565, D-914).
// Read from Sony's libkey2char.sprx (fw 4.92, the reference, D-046) and compared with it running on pspocket
// (tests/hle/test_key2char_compare.cpp; no longer firmware-first, D-914); RPCS3's rpcs3/Emu/Cell/Modules/cellKey2char.cpp has the five names, the error
// codes and the 128-byte handle, and converts nothing.
//
// The handle the game owns holds all of libkey2char's state:
//   +0  the arrangement keys are converted in (0xFF: none, no keyboard was found at Open)
//   +1  the arrangement the game set (106 kana counts as 106)        +2  a dead key's or the kana's state
//   +3  the mode (0 English, 1 the native mode, 2 the second one)      +4  the dead key waiting for its letter
//   +6  the kana typed last (16 bits)                                  +8  opened
//   +10 the characters GetChar hands back (five UTF-16 units; *charCode points here)
//   +0x14 the Hangul automaton (below), +0x6C its output (three units), +0x72 their count, +0x74 composing,
//   +0x75 a syllable shown that the next one replaces (it then starts with a backspace).
// A key becomes a character through cellKbCnvRawCode in the conversion arrangement, then per arrangement and mode:
// - dead keys (` ´ ^ ~ ¨ ¸, ~ as the Polish programmers' ogonek key) wait for a letter and compose with it, in the
//   arrangements that have them; the result, a byte of Windows code page 1252 (1250 Polish, 1254 Turkish, ISO
//   8859-5's upper half for Russian), becomes UTF-16;
// - Japanese (106, mode 1): the JIS kana layout, its shift layer, dakuten and handakuten on the kana typed last;
// - traditional Chinese (arrangement 16): mode 1 the Zhuyin (bopomofo) layout, mode 2 the Cangjie radicals;
// - Korean (arrangement 11, mode 1): the 2-set (Dubeolsik) layout through libkey2char's composing automaton: jamo
//   pushed on a stack of six in the handle, the syllable's state recomputed from it, a final consonant moving on to
//   start the next syllable, the syllable and the one completed handed back as compatibility jamo or precomposed
//   syllables (Unicode's arithmetic).
// The tables are public standards (the keyboard layouts, the code pages, Unicode's compositions and jamo mappings).
// Where libkey2char reads memory a game gave it that is not guest memory (a broken handle's stack addresses, a NULL
// charNum), pspocket logs the call and goes on without the access or refuses it, where the module would fault.

#include <array>
#include <format>
#include <string_view>
#include <utility>

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

constexpr u32 kHandleSize = 128;
// The handle's fields.
constexpr u32 kConv = 0, kArrangement = 1, kState = 2, kMode = 3, kDead = 4, kLastKana = 6, kOpened = 8, kChars = 10,
              kHangul = 0x14, kOut = 0x6C, kOutCount = 0x72, kComposing = 0x74, kPending = 0x75;
constexpr u8 kBroken = 0xFF;
// The automaton (at the handle's +0x14): a word set to 1, the state, the syllable's initial, medial and final, a word
// 0x10001, the jamo stack and its copy before the last jamo ({u32 data, u16 count, u16 capacity} each), their data.
constexpr u32 kAState = 4, kAL = 8, kAV = 0xC, kAT = 0x10, kAWord14 = 0x14, kAStack = 0x18, kASaved = 0x20,
              kAStackData = 0x28, kASavedData = 0x40;
constexpr u32 kFillerL = 0x115F, kFillerV = 0x1160, kNoFinal = 0x11A7;
constexpr u16 kStackSlots = 6;
// cellKbCnvRawCode's flag for a key that is no character; the modifier keys and LEDs GetChar looks at.
constexpr u16 kRawDat = 0x8000;
constexpr u32 kMkeyShift = 0x22, kMkeyRightAlt = 0x40, kLedNumLock = 0x01, kLedCapsLock = 0x02;

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

// Guest memory as libkey2char reaches it: an access outside guest memory is logged (once a call) and left out.
struct Mem {
    PPUContext& ctx;
    u8* b;
    bool logged = false;

    explicit Mem(PPUContext& c) : ctx(c), b(gb(c)) {}
    bool ok(u32 addr, u32 size)
    {
        if (guest_readable(thread_of(ctx), addr, size)) return true;
        if (!logged) {
            logged = true;
            log_call_site(ctx, b,
                std::format("cellKey2char: {:#x} ({} bytes) is not guest memory; the access is left out", addr, size));
        }
        return false;
    }
    u16 r16(u32 a) { return ok(a, 2) ? ps_read16(b, a) : 0; }
    u32 r32(u32 a) { return ok(a, 4) ? ps_read32(b, a) : 0; }
    void w16(u32 a, u16 v)
    {
        if (ok(a, 2)) ps_write16(b, a, v);
    }
    void w32(u32 a, u32 v)
    {
        if (ok(a, 4)) ps_write32(b, a, v);
    }
};

// ------------------------------------------------------------------------------------------- the Hangul automaton

// One of the automaton's stacks, through the data address it holds.
struct Stack {
    Mem& m;
    u32 s;
    u32 data() { return m.r32(s); }
    u16 count() { return m.r16(s + 4); }
    u16 cap() { return m.r16(s + 6); }
    bool push(u32 v)
    {
        const u32 d = data();
        if (!d) return false;
        const u16 n = count();
        if (n >= cap()) return false;
        m.w32(d + 4 * u32{n}, v);
        m.w16(s + 4, static_cast<u16>(n + 1));
        return true;
    }
    u32 pop()
    {
        const u32 d = data();
        if (!d) return 0;
        const u16 n = count();
        if (!n) return 0;
        const u16 k = static_cast<u16>(n - 1);
        const u32 v = m.r32(d + 4 * u32{k});
        m.w16(s + 4, k);
        m.w32(d + 4 * u32{k}, 0);
        return v;
    }
    void clear()
    {
        const u16 c = cap();
        const u32 d = data();
        for (u32 i = 0; i < c; i++) m.w32(d + 4 * i, 0);
        m.w16(s + 4, 0);
    }
    void setup(u16 capacity, u32 d)
    {
        m.w16(s + 6, capacity);
        m.w32(s, d);
        clear();
    }
    u32 get(u32 i) { return i < cap() ? m.r32(data() + 4 * i) : 0; }
    void forget()
    {
        m.w16(s + 6, 0);
        m.w16(s + 4, 0);
    }
};

// A conjoining jamo's kind: 1 initial, 2 medial, 3 final, 0 none.
u32 jamo_kind(u32 c)
{
    if (c - 0x1100u <= 0x5F) return 1;
    if (c - 0x1160u <= 0x47) return 2;
    if (c - 0x11A8u <= 0x57) return 3;
    return 0;
}

// Two medials typed one after the other, as one (0: they do not combine).
u32 combine_vowel(u32 a, u32 v)
{
    u32 r = 0;
    switch (a) {
    case 0x1161: r = v == 0x1175 ? 0x1162 : 0; break; // a i: ae
    case 0x1163: r = v == 0x1175 ? 0x1164 : 0; break; // ya i: yae
    case 0x1165: r = v == 0x1175 ? 0x1166 : 0; break; // eo i: e
    case 0x1167: r = v == 0x1175 ? 0x1168 : 0; break; // yeo i: ye
    case 0x1169: r = v == 0x1162 ? 0x116B : v == 0x1175 ? 0x116C : v == 0x1161 ? 0x116A : 0; break; // o: wae, oe, wa
    case 0x116A: r = v == 0x1175 ? 0x116B : 0; break;                                               // wa i: wae
    case 0x116E: r = v == 0x1166 ? 0x1170 : v == 0x1175 ? 0x1171 : v == 0x1165 ? 0x116F : 0; break; // u: we, wi, wo
    case 0x116F: r = v == 0x1175 ? 0x1170 : 0; break;                                               // wo i: we
    case 0x1173: r = v == 0x1175 ? 0x1174 : 0; break;                                               // eu i: ui
    default: break;
    }
    return r & 0x117F;
}

// An initial consonant as a final (Unicode's correspondence; 0 for the doubled ones that have no final).
u32 to_final(u32 c)
{
    static constexpr u16 kFinal[19] = {0x11A8, 0x11A9, 0x11AB, 0x11AE, 0, 0x11AF, 0x11B7, 0x11B8, 0, 0x11BA, 0x11BB,
        0x11BC, 0x11BD, 0, 0x11BE, 0x11BF, 0x11C0, 0x11C1, 0x11C2};
    return c - 0x1100u <= 18 ? kFinal[c - 0x1100u] & 0x11FFu : 0;
}

// A final followed by another consonant, as the final cluster they make (0: none). The first may be in its initial
// or final form, the second too, as libkey2char's table has them (ㄱ in its final form makes no cluster).
u32 combine_final(u32 t, u32 x)
{
    u32 r = 0;
    if (t == 0x1107 || t == 0x11B8) {
        if (x == 0x1109 || x == 0x11BA) r = 0x11B9; // b s
    } else if (t == 0x1100) {
        if (x == 0x1109 || x == 0x11BA) r = 0x11AA; // g s
    } else if (t == 0x1102 || t == 0x11AB) {
        if (x == 0x1112 || x == 0x11C2) r = 0x11AD;      // n h
        else if (x == 0x110C || x == 0x11BD) r = 0x11AC; // n j
    } else if (t == 0x1105 || t == 0x11AF) {
        switch (x) {
        case 0x1100: case 0x11A8: r = 0x11B0; break; // l g
        case 0x1106: case 0x11B7: r = 0x11B1; break; // l m
        case 0x1107: case 0x11B8: r = 0x11B2; break; // l b
        case 0x1109: case 0x11BA: r = 0x11B3; break; // l s
        case 0x1110: case 0x11C0: r = 0x11B4; break; // l t
        case 0x1111: case 0x11C1: r = 0x11B5; break; // l p
        case 0x1112: case 0x11C2: r = 0x11B6; break; // l h
        default: break;
        }
    }
    return r & 0x11BF;
}

// A final as the initial it becomes when it moves on to the next syllable (0: a cluster, which does not).
u32 final_to_initial(u32 t)
{
    static constexpr u16 kInitial[27] = {0x1100, 0x1101, 0, 0x1102, 0, 0, 0x1103, 0x1105, 0, 0, 0, 0, 0, 0, 0,
        0x1106, 0x1107, 0, 0x1109, 0x110A, 0x110B, 0x110C, 0x110E, 0x110F, 0x1110, 0x1111, 0x1112};
    return t - 0x11A8u <= 26 ? kInitial[t - 0x11A8u] : 0;
}

// A syllable from its initial, medial and final (Unicode's arithmetic; a jamo alone is itself, 0 for none).
u32 compose(u32 l, u32 v, u32 t)
{
    if (t - kNoFinal > 27) t = kNoFinal;
    if (l > 0x1112) {
        if (v == kFillerV) return t - kNoFinal <= 27 ? t : 0;
        return v - 0x1161 <= 20 ? v : 0;
    }
    if (v == kFillerV) return l;
    if (v - 0x1161 > 20) return 0;
    return t + (l * 21 + v) * 28 - 0x285843;
}

// A conjoining jamo as its compatibility jamo (Unicode's mapping), 0 for one outside the modern sets.
u32 to_compat(u32 c)
{
    static constexpr u16 kInitial[19] = {0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143,
        0x3145, 0x3146, 0x3147, 0x3148, 0x3149, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E};
    static constexpr u16 kFinal[27] = {0x3131, 0x3132, 0x3133, 0x3134, 0x3135, 0x3136, 0x3137, 0x3139, 0x313A, 0x313B,
        0x313C, 0x313D, 0x313E, 0x313F, 0x3140, 0x3141, 0x3142, 0x3144, 0x3145, 0x3146, 0x3147, 0x3148, 0x314A, 0x314B,
        0x314C, 0x314D, 0x314E};
    u32 r = c + 0x1FEE; // the medials: 0x314F on
    if (((c - 0x1100) & 0xFFFF) < 19) r = kInitial[(c & 0xFFFF) - 0x1100];
    else if (((c - 0x1161) & 0xFFFF) > 20) {
        if (((c - 0x11A8) & 0xFFFF) > 26) return 0;
        r = kFinal[(c & 0xFFFF) - 0x11A8];
    }
    return r & 0xFFFF;
}

// The syllable's state from the jamo on the stack: 1 none, 2 an initial, 3 initial and medial (or a medial alone),
// 4 a compound medial, 5 a final after a medial, 6 a final where none can be, 9 a medial past a compound.
void compute_state(Mem& m, u32 a)
{
    Stack s{m, a + kAStack};
    const u16 n = s.count();
    u32 st = 1;
    for (u32 i = 0; i < n; i++) {
        const u32 k = jamo_kind(s.get(i));
        if (k == 2) {
            if (i == 1) st = st == 2 ? 3 : 4;
            else if (i == 0) st = 3;
            else if (i == 2) st = st == 3 ? 4 : 9;
            else if (i == 3) st = 9;
            else st = 1;
        } else if (k == 3) {
            if (i > 5) st = 1;
            else if (i == 1) st = st == 2 ? 6 : st;
            else if (i == 2) st = 5;
            else if (i == 3 || i == 4) st = (st == 3 || st == 4 || st == 9) ? 5 : 6;
            else if (i == 5) st = 6;
        } else if (k == 1) {
            st = i == 0 ? 2 : 1;
        }
    }
    m.w32(a + kAState, st);
}

// The syllable's initial, medial and final from the stack (medials combined, finals made clusters). Returns what
// libkey2char's function leaves in r3, the answer of its last step (its export is void): the stack's count when it
// is empty, else the last jamo's kind, or the medial or cluster it made.
u32 recompute(Mem& m, u32 a)
{
    Stack s{m, a + kAStack};
    const u16 n = s.count();
    m.w32(a + kAL, kFillerL);
    m.w32(a + kAV, kFillerV);
    m.w32(a + kAT, kNoFinal);
    u32 vowels = 0, vowel = 0, r3 = n;
    for (u32 i = 0; i < n; i++) {
        const u32 j = s.get(i);
        const u32 k = jamo_kind(j);
        r3 = k;
        if (k == 2) {
            if (vowels == 0) {
                vowel = j;
                vowels = 1;
            } else {
                vowels++;
                r3 = vowel = combine_vowel(vowel, j);
            }
            m.w32(a + kAV, vowel);
        } else if (k == 3) {
            const u32 t = m.r32(a + kAT);
            if (t != kNoFinal) r3 = combine_final(t, j);
            m.w32(a + kAT, t != kNoFinal ? r3 : j);
        } else if (k == 1) {
            m.w32(a + kAL, j);
        }
    }
    return r3;
}

void init_automaton(Mem& m, u32 a)
{
    Stack{m, a + kAStack}.setup(kStackSlots, a + kAStackData);
    Stack{m, a + kASaved}.setup(kStackSlots, a + kASavedData);
    m.w32(a + kAWord14, 0x10001);
    m.w32(a + kAState, 1);
    m.w32(a + kAL, kFillerL);
    m.w32(a + kAV, kFillerV);
    m.w32(a + kAT, kNoFinal);
    m.w32(a, 1);
}

void reset_automaton(Mem& m, u32 a)
{
    Stack{m, a + kAStack}.clear();
    m.w32(a + kAL, kFillerL);
    m.w32(a + kAState, 1);
    m.w32(a + kAV, kFillerV);
    m.w32(a + kAT, kNoFinal);
}

// A jamo typed: true when it completes a syllable (whose initial, medial and final are then the automaton's, the
// stack holding the next one's jamo), false while the syllable goes on.
bool add_jamo(Mem& m, u32 a, u32 j)
{
    Stack s{m, a + kAStack}, saved{m, a + kASaved};
    const u16 n = s.count();
    const u32 last = n ? m.r32(s.data() + 4 * u32{n} - 4) : 0;
    // The stack as it was, for 291ADBA5 to put back.
    const u16 cap = s.cap();
    if (cap) {
        const u32 from = s.data(), to = saved.data();
        for (u32 i = 0; i < cap; i++) m.w32(to + 4 * i, m.r32(from + 4 * i));
    }
    m.w16(saved.s + 6, cap);
    m.w16(saved.s + 4, s.count());

    u32 kind = jamo_kind(j);
    const bool consonant = kind != 2;
    const bool has_final = consonant && j != 0x1104 && j != 0x1108 && j != 0x110D; // not the doubled d, b, j
    u32 push = j, value = j, joined = 0;
    const u32 state = m.r32(a + kAState);
    enum class Then { Push, Clear, MoveOn } then = Then::Push;
    switch (state & 0xFFFF) {
    case 1:
        m.w32(a + kAT, kNoFinal);
        m.w32(a + kAL, kFillerL);
        m.w32(a + kAV, kFillerV);
        break;
    case 2:
        joined = combine_final(last, j);
        if (!consonant) break;
        if (!joined) {
            then = Then::Clear;
            break;
        }
        m.w32(a + kAL, kFillerL);
        value = push = to_final(j);
        kind = jamo_kind(joined);
        break;
    case 3:
    case 4:
        if ((state & 0xFFFF) == 3) joined = combine_vowel(last, j);
        else joined = combine_vowel(combine_vowel(s.get(static_cast<u32>(s.count()) - 2), last), j);
        if (!consonant || !has_final) {
            if (!joined) then = Then::Clear;
            break;
        }
        if (m.r32(a + kAL) == kFillerL) {
            then = Then::Clear;
            break;
        }
        value = push = to_final(j);
        kind = jamo_kind(push);
        break;
    case 5:
        if (!consonant) {
            then = Then::MoveOn;
            break;
        }
        joined = combine_final(last, j);
        if (!joined) {
            then = Then::Clear;
            break;
        }
        value = push = to_final(j);
        kind = jamo_kind(joined);
        break;
    case 6: then = consonant ? Then::Clear : Then::MoveOn; break;
    case 9:
        if (!consonant || !has_final || m.r32(a + kAL) == kFillerL) {
            then = Then::Clear;
            break;
        }
        value = push = to_final(j);
        kind = jamo_kind(push);
        break;
    default: break; // 0, 7, 8 and past 9: by the state's whole word below
    }
    if (then == Then::Push && state == 7) then = Then::MoveOn;
    else if (then == Then::Push && state == 8) then = Then::Clear;

    if (then == Then::Clear) {
        // The syllable ends; the jamo starts the next.
        if (state != 8) m.w32(a + kAState, 8);
        s.clear();
        s.push(push);
        compute_state(m, a);
        return true;
    }
    if (then == Then::MoveOn) {
        // The syllable ends without its last final, which starts the next with the medial typed.
        if (state != 7) m.w32(a + kAState, 7);
        const u32 initial = final_to_initial(s.pop());
        recompute(m, a);
        s.clear();
        s.push(initial & 0x111F);
        s.push(push);
        compute_state(m, a);
        return true;
    }
    s.push(push);
    compute_state(m, a);
    if (joined) value = joined;
    m.w32(a + 4 + 4 * kind, value); // kind 0 (no jamo) lands on the state word, as in libkey2char
    return false;
}

// Backspace in a syllable: its last jamo off. False when there is none.
bool backspace(Mem& m, u32 a)
{
    if (m.r32(a + kAState) == 1) return false;
    Stack{m, a + kAStack}.pop();
    recompute(m, a);
    compute_state(m, a);
    return true;
}

// The stack as it was before the last jamo: true when a syllable is being composed.
bool restore(Mem& m, u32 a)
{
    Stack s{m, a + kAStack}, saved{m, a + kASaved};
    const u16 cap = saved.cap();
    if (cap) {
        const u32 from = saved.data(), to = s.data();
        for (u32 i = 0; i < cap; i++) m.w32(to + 4 * i, m.r32(from + 4 * i));
    }
    m.w16(s.s + 6, cap);
    m.w16(s.s + 4, saved.count());
    recompute(m, a);
    compute_state(m, a);
    return m.r32(a + kAState) != 1;
}

// ------------------------------------------------------------------------------------------------- the layouts

// A key code and what it gives.
struct Key {
    u16 key, value;
};

template <usize N>
bool find(const Key (&table)[N], u16 key, u16& value)
{
    for (const Key& k : table)
        if (k.key == key) {
            value = k.value;
            return true;
        }
    return false;
}

// Keys the Korean mode hands back as they are, not processed: Escape, Caps Lock, F1 to F12, the editing and cursor
// block, Num Lock, Application and the Japanese keys; with Num Lock off, the keypad's digits and its point too.
bool korean_passthrough(u16 key)
{
    return key == 0x29 || key == 0x39 || (key >= 0x3A && key <= 0x53) || key == 0x65 || (key >= 0x87 && key <= 0x8B);
}
bool keypad_without_num_lock(u16 key) { return key >= 0x59 && key <= 0x63; }

// The keypad's characters; Enter, Backspace and Tab.
constexpr Key kKeypad[] = {{0x59, '1'}, {0x5A, '2'}, {0x5B, '3'}, {0x5C, '4'}, {0x5D, '5'}, {0x5E, '6'}, {0x5F, '7'},
    {0x60, '8'}, {0x61, '9'}, {0x62, '0'}, {0x63, '.'}, {0x54, '/'}, {0x55, '*'}, {0x56, '-'}, {0x57, '+'},
    {0x58, 0x0A}};
constexpr Key kControls[] = {{0x28, 0x0A}, {0x2A, 0x08}, {0x2B, 0x09}};
// The US layout's keys that are not letters, unshifted and shifted.
constexpr Key kUsKeys[] = {{0x2C, ' '}, {0x1E, '1'}, {0x1F, '2'}, {0x20, '3'}, {0x21, '4'}, {0x22, '5'}, {0x23, '6'},
    {0x24, '7'}, {0x25, '8'}, {0x26, '9'}, {0x27, '0'}, {0x35, '`'}, {0x2D, '-'}, {0x2E, '='}, {0x32, '\\'},
    {0x31, '\\'}, {0x2F, '['}, {0x30, ']'}, {0x33, ';'}, {0x34, '\''}, {0x36, ','}, {0x37, '.'}, {0x38, '/'}};
constexpr Key kUsShifted[] = {{0x35, '~'}, {0x1E, '!'}, {0x1F, '@'}, {0x20, '#'}, {0x21, '$'}, {0x22, '%'},
    {0x23, '^'}, {0x24, '&'}, {0x25, '*'}, {0x26, '('}, {0x27, ')'}, {0x2D, '_'}, {0x2E, '+'}, {0x31, '|'},
    {0x2F, '{'}, {0x32, '|'}, {0x30, '}'}, {0x33, ':'}, {0x34, '"'}, {0x36, '<'}, {0x37, '>'}, {0x38, '?'}};

// Korean 2-set (Dubeolsik, KS X 5002): the letters' jamo, and the shifted ones that differ.
constexpr Key kKoreanLetters[] = {{0x04, 0x1106}, {0x05, 0x1172}, {0x06, 0x110E}, {0x07, 0x110B}, {0x08, 0x1103},
    {0x09, 0x1105}, {0x0A, 0x1112}, {0x0B, 0x1169}, {0x0C, 0x1163}, {0x0D, 0x1165}, {0x0E, 0x1161}, {0x0F, 0x1175},
    {0x10, 0x1173}, {0x11, 0x116E}, {0x12, 0x1162}, {0x13, 0x1166}, {0x14, 0x1107}, {0x15, 0x1100}, {0x16, 0x1102},
    {0x17, 0x1109}, {0x18, 0x1167}, {0x19, 0x1111}, {0x1A, 0x110C}, {0x1B, 0x1110}, {0x1C, 0x116D}, {0x1D, 0x110F}};
constexpr Key kKoreanShifted[] = {{0x14, 0x1108}, {0x1A, 0x110D}, {0x08, 0x1104}, {0x15, 0x1101}, {0x17, 0x110A},
    {0x12, 0x1164}, {0x13, 0x1168}};

// JIS kana (the 106 keyboard), and its shift layer's small kana and punctuation.
constexpr Key kKanaKeys[] = {{0x20, 0x3042}, {0x08, 0x3044}, {0x21, 0x3046}, {0x22, 0x3048}, {0x23, 0x304A},
    {0x17, 0x304B}, {0x0A, 0x304D}, {0x0B, 0x304F}, {0x34, 0x3051}, {0x05, 0x3053}, {0x1B, 0x3055}, {0x07, 0x3057},
    {0x15, 0x3059}, {0x13, 0x305B}, {0x06, 0x305D}, {0x14, 0x305F}, {0x04, 0x3061}, {0x1D, 0x3064}, {0x1A, 0x3066},
    {0x16, 0x3068}, {0x18, 0x306A}, {0x0C, 0x306B}, {0x1E, 0x306C}, {0x36, 0x306D}, {0x0E, 0x306E}, {0x09, 0x306F},
    {0x19, 0x3072}, {0x1F, 0x3075}, {0x2E, 0x3078}, {0x2D, 0x307B}, {0x0D, 0x307E}, {0x11, 0x307F}, {0x32, 0x3080},
    {0x38, 0x3081}, {0x10, 0x3082}, {0x24, 0x3084}, {0x25, 0x3086}, {0x26, 0x3088}, {0x12, 0x3089}, {0x0F, 0x308A},
    {0x37, 0x308B}, {0x33, 0x308C}, {0x87, 0x308D}, {0x27, 0x308F}, {0x1C, 0x3093}, {0x2F, 0x309B}, {0x30, 0x309C},
    {0x89, 0x30FC}, {0x2C, ' '}, {0x31, 0x3080}};
constexpr Key kKanaShifted[] = {{0x20, 0x3041}, {0x08, 0x3043}, {0x21, 0x3045}, {0x22, 0x3047}, {0x23, 0x3049},
    {0x1D, 0x3063}, {0x24, 0x3083}, {0x25, 0x3085}, {0x26, 0x3087}, {0x27, 0x3092}, {0x36, 0x3001}, {0x37, 0x3002},
    {0x38, 0x30FB}, {0x30, 0x300C}, {0x32, 0x300D}, {0x31, 0x300D}};
// The kana that take dakuten (each the next code point voiced) and handakuten (two on).
constexpr u16 kDakuten[] = {0x304B, 0x304D, 0x304F, 0x3051, 0x3053, 0x3055, 0x3057, 0x3059, 0x305B, 0x305D, 0x305F,
    0x3061, 0x3064, 0x3066, 0x3068, 0x306F, 0x3072, 0x3075, 0x3078, 0x307B};
constexpr u16 kHandakuten[] = {0x306F, 0x3072, 0x3075, 0x3078, 0x307B};
constexpr u16 kDakutenMark = 0x309B, kHandakutenMark = 0x309C, kWo = 0x3092;

// Zhuyin (bopomofo, the standard layout), its tones, and the US punctuation it keeps.
constexpr Key kZhuyin[] = {{0x1E, 0x3105}, {0x14, 0x3106}, {0x04, 0x3107}, {0x1D, 0x3108}, {0x1F, 0x3109},
    {0x1A, 0x310A}, {0x16, 0x310B}, {0x1B, 0x310C}, {0x08, 0x310D}, {0x07, 0x310E}, {0x06, 0x310F}, {0x15, 0x3110},
    {0x09, 0x3111}, {0x19, 0x3112}, {0x22, 0x3113}, {0x17, 0x3114}, {0x0A, 0x3115}, {0x05, 0x3116}, {0x1C, 0x3117},
    {0x0B, 0x3118}, {0x11, 0x3119}, {0x25, 0x311A}, {0x0C, 0x311B}, {0x0E, 0x311C}, {0x36, 0x311D}, {0x26, 0x311E},
    {0x12, 0x311F}, {0x0F, 0x3120}, {0x37, 0x3121}, {0x27, 0x3122}, {0x13, 0x3123}, {0x33, 0x3124}, {0x38, 0x3125},
    {0x2D, 0x3126}, {0x18, 0x3127}, {0x0D, 0x3128}, {0x10, 0x3129}, {0x35, '`'}, {0x2F, '['}, {0x30, ']'},
    {0x2E, '='}, {0x34, '\''}, {0x31, '\\'}, {0x23, 0x02CA}, {0x20, 0x02C7}, {0x21, 0x02CB}, {0x24, 0x02D9},
    {0x2C, ' '}};
// Cangjie's radicals on the letters (Z, the collision key, has none here), the digits and punctuation (no space),
// and the euro sign on the grave accent's key.
constexpr Key kCangjie[] = {{0x35, 0x20AC}, {0x1E, '1'}, {0x1F, '2'}, {0x20, '3'}, {0x21, '4'}, {0x22, '5'},
    {0x23, '6'}, {0x24, '7'}, {0x25, '8'}, {0x26, '9'}, {0x27, '0'}, {0x2D, '-'}, {0x2E, '='}, {0x14, 0x624B},
    {0x1A, 0x7530}, {0x08, 0x6C34}, {0x15, 0x53E3}, {0x17, 0x5EFF}, {0x1C, 0x535C}, {0x18, 0x5C71}, {0x0C, 0x6208},
    {0x12, 0x4EBA}, {0x13, 0x5FC3}, {0x2F, '['}, {0x30, ']'}, {0x04, 0x65E5}, {0x16, 0x5C38}, {0x07, 0x6728},
    {0x09, 0x706B}, {0x0A, 0x571F}, {0x0B, 0x7AF9}, {0x0D, 0x5341}, {0x0E, 0x5927}, {0x0F, 0x4E2D}, {0x33, ';'},
    {0x34, '\''}, {0x1B, 0x96E3}, {0x06, 0x91D1}, {0x19, 0x5973}, {0x05, 0x6708}, {0x11, 0x5F13}, {0x10, 0x4E00},
    {0x36, ','}, {0x37, '.'}, {0x38, '/'}, {0x31, '\\'}};

// What a key gives in a native layout: its own table, then the keypad, Enter, Backspace and Tab.
template <usize N>
bool native_key(const Key (&table)[N], u16 key, u16& value)
{
    return find(table, key, value) || find(kKeypad, key, value) || find(kControls, key, value);
}

// ------------------------------------------------------------------------------------- dead keys and code pages

// The upper halves of Windows code pages 1252, 1250 and 1254 (0: undefined).
constexpr std::array<u16, 128> kCp1252 = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030,
    0x0160, 0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122,
    0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB,
    0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE,
    0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1,
    0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4,
    0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
    0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};
constexpr std::array<u16, 128> kCp1250 = {0x20AC, 0, 0x201A, 0, 0x201E, 0x2026, 0x2020, 0x2021, 0, 0x2030, 0x0160,
    0x2039, 0x015A, 0x0164, 0x017D, 0x0179, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0, 0x2122,
    0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A, 0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B, 0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5,
    0x00B6, 0x00B7, 0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C, 0x0154, 0x00C1, 0x00C2, 0x0102,
    0x00C4, 0x0139, 0x0106, 0x00C7, 0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E, 0x0110, 0x0143,
    0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7, 0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7, 0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED,
    0x00EE, 0x010F, 0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7, 0x0159, 0x016F, 0x00FA, 0x0171,
    0x00FC, 0x00FD, 0x0163, 0x02D9};
constexpr std::array<u16, 128> kCp1254 = [] {
    std::array<u16, 128> t = kCp1252;
    t[0x8E - 0x80] = 0;
    t[0x9E - 0x80] = 0;
    t[0xD0 - 0x80] = 0x011E;
    t[0xDD - 0x80] = 0x0130;
    t[0xDE - 0x80] = 0x015E;
    t[0xF0 - 0x80] = 0x011F;
    t[0xFD - 0x80] = 0x0131;
    t[0xFE - 0x80] = 0x015F;
    return t;
}();

// A converted byte as UTF-16 in the conversion arrangement's code page.
u16 to_utf16(u8 conv, u8 c)
{
    if (c < 0x80) return c;
    if (conv == 9 && c >= 0xA0) { // Russian: ISO 8859-5's upper half, its 0xF0 the numero sign
        const u16 v = static_cast<u16>(c + 0x360);
        return v == 0x450 ? 0x2116 : v;
    }
    if (conv == 22) return kCp1250[c - 0x80];
    if (conv == 24) return kCp1254[c - 0x80];
    return kCp1252[c - 0x80];
}

// The letters a dead key composes with, and what they make, in code page 1252 (the Polish programmers' key: 1250).
struct Pair {
    u8 base, composed;
};
constexpr Pair kGrave[] = {{'a', 0xE0}, {'i', 0xEC}, {'u', 0xF9}, {'e', 0xE8}, {'o', 0xF2}, {'A', 0xC0}, {'I', 0xCC},
    {'U', 0xD9}, {'E', 0xC8}, {'O', 0xD2}};
constexpr Pair kAcute[] = {{'a', 0xE1}, {'i', 0xED}, {'u', 0xFA}, {'e', 0xE9}, {'o', 0xF3}, {'y', 0xFD}, {'A', 0xC1},
    {'I', 0xCD}, {'U', 0xDA}, {'E', 0xC9}, {'O', 0xD3}, {'Y', 0xDD}};
constexpr Pair kCircumflex[] = {{'a', 0xE2}, {'i', 0xEE}, {'u', 0xFB}, {'e', 0xEA}, {'o', 0xF4}, {'A', 0xC2},
    {'I', 0xCE}, {'U', 0xDB}, {'E', 0xCA}, {'O', 0xD4}};
constexpr Pair kTilde[] = {{'a', 0xE3}, {'o', 0xF5}, {'n', 0xF1}, {'A', 0xC3}, {'O', 0xD5}, {'N', 0xD1}};
constexpr Pair kDiaeresis[] = {{'a', 0xE4}, {'i', 0xEF}, {'u', 0xFC}, {'e', 0xEB}, {'o', 0xF6}, {'y', 0xFF},
    {'A', 0xC4}, {'I', 0xCF}, {'U', 0xDC}, {'E', 0xCB}, {'O', 0xD6}, {'Y', 0x9F}};
constexpr Pair kCedilla[] = {{'c', 0xE7}, {'C', 0xC7}};
constexpr Pair kPolish[] = {{'a', 0xB9}, {'c', 0xE6}, {'e', 0xEA}, {'l', 0xB3}, {'n', 0xF1}, {'o', 0xF3}, {'s', 0x9C},
    {'x', 0x9F}, {'z', 0xBF}, {'A', 0xA5}, {'C', 0xC6}, {'E', 0xCA}, {'L', 0xA3}, {'N', 0xD1}, {'O', 0xD3},
    {'S', 0x8C}, {'X', 0x8F}, {'Z', 0xAF}};

template <usize N>
constexpr std::pair<const Pair*, usize> pairs(const Pair (&t)[N])
{
    return {t, N};
}
// A dead key's composition table; none for a character that is no dead key.
std::pair<const Pair*, usize> dead_table(u8 dead, u8 conv)
{
    switch (dead) {
    case '`': return pairs(kGrave);
    case 0xB4: return pairs(kAcute);
    case '^': return pairs(kCircumflex);
    case '~': return conv == 22 ? pairs(kPolish) : pairs(kTilde);
    case 0xA8: return pairs(kDiaeresis);
    case 0xB8: return pairs(kCedilla);
    default: return {nullptr, 0};
    }
}

// Whether a character typed is a dead key in the conversion arrangement: the 101, 106, Italian, Russian, UK and both
// Chinese arrangements have none; ~ is plain in German and Swiss German, ^ with AltGr in French and Belgian, ^ and `
// in Polish.
bool is_dead_key(u8 conv, u16 c, u32 mkey)
{
    if (conv <= 1 || conv == 6 || conv == 9 || conv == 10 || conv == 16 || conv == 17) return false;
    switch (c) {
    case '~': return conv != 3 && conv != 20;
    case 0xB4:
    case 0xB8:
    case 0xA8: return true;
    case '^':
        if ((conv == 21 || conv == 5) && (mkey & kMkeyRightAlt)) return false;
        return conv != 22;
    case '`': return conv != 22;
    default: return false;
    }
}

// ------------------------------------------------------------------------------------------------------ the calls

bool handle_in_memory(PPUContext& ctx, u32 handle, const char* fn)
{
    if (guest_readable(thread_of(ctx), handle, kHandleSize)) return true;
    log_call_site(ctx, gb(ctx), std::format("{}: the handle at {:#x} is not guest memory", fn, handle));
    return false;
}

// The handle's checks shared by every call but Open: there, and opened.
s32 opened(PPUContext& ctx, u32 handle, const char* fn)
{
    if (!handle || !handle_in_memory(ctx, handle, fn)) return error(CELL_K2C_ERROR_INVALID_HANDLE);
    if (ps_read8(gb(ctx), handle + kOpened) == 0) return error(CELL_K2C_ERROR_UNINITIALIZED);
    return lv2::CELL_OK;
}

// The arrangement into the handle (libkey2char's setter): the 106 kana layout as 106; Russian and Korean convert in the
// US layout until their native mode; the conversion arrangement 16 set again becomes 0; the Korean automaton is set up
// afresh.
s32 set_arrangement(PPUContext& ctx, u32 handle, u32 arrangement)
{
    u8* b = gb(ctx);
    if (arrangement > 24) return error(CELL_K2C_ERROR_INVALID_PARAMETER);
    const u8 conv = arrangement == 2 ? 1 : static_cast<u8>(arrangement);
    ps_write8(b, handle + kArrangement, conv);
    if (arrangement == 9) {
        ps_write8(b, handle + kConv, 0);
        return lv2::CELL_OK;
    }
    if (arrangement == 11) {
        Mem m(ctx);
        init_automaton(m, handle + kHangul);
        reset_automaton(m, handle + kHangul);
        ps_write8(b, handle + kPending, 0);
        ps_write8(b, handle + kConv, 0);
        return lv2::CELL_OK;
    }
    ps_write8(b, handle + kConv, ps_read8(b, handle + kConv) != 16 ? conv : 0);
    return lv2::CELL_OK;
}

// The keyboard's arrangement (the first port cellKbGetConfiguration answers for) into the handle.
s32 query_arrangement(PPUContext& ctx, u32 handle)
{
    StackVar config(ctx, gb(ctx), 16);
    s32 r = 0;
    for (u32 port = 0; port < 127; port++) {
        r = static_cast<s32>(lib_call(ctx, "sys_io", "cellKbGetConfiguration", {port, config.addr()}));
        if (r == 0) return set_arrangement(ctx, handle, ps_read32(gb(ctx), config.addr()));
    }
    return r;
}

s32 cellKey2CharOpen(PPUContext& ctx, u32 handle)
{
    if (!handle || !handle_in_memory(ctx, handle, "cellKey2CharOpen")) return error(CELL_K2C_ERROR_INVALID_HANDLE);
    u8* b = gb(ctx);
    if (ps_read8(b, handle + kOpened) != 0) return error(CELL_K2C_ERROR_ALREADY_INITIALIZED);
    ps_write8(b, handle + kMode, 0);
    ps_write8(b, handle + kState, 0);
    if (query_arrangement(ctx, handle) != 0) ps_write8(b, handle + kConv, kBroken); // no keyboard: none to convert in
    ps_write8(b, handle + kDead, 0);
    ps_write8(b, handle + kOpened, 1);
    return lv2::CELL_OK;
}

s32 cellKey2CharClose(PPUContext& ctx, u32 handle)
{
    if (const s32 r = opened(ctx, handle, "cellKey2CharClose"); r != lv2::CELL_OK) return r;
    u8* b = gb(ctx);
    if (ps_read8(b, handle + kConv) == 11) {
        Mem m(ctx);
        Stack{m, handle + kHangul + kASaved}.forget();
        Stack{m, handle + kHangul + kAStack}.forget();
    }
    ps_write8(b, handle + kOpened, 0);
    return lv2::CELL_OK;
}

s32 cellKey2CharSetMode(PPUContext& ctx, u32 handle, u32 mode)
{
    if (const s32 r = opened(ctx, handle, "cellKey2CharSetMode"); r != lv2::CELL_OK) return r;
    u8* b = gb(ctx);
    if (mode > 2) return error(CELL_K2C_ERROR_INVALID_PARAMETER);
    if (ps_read8(b, handle + kConv) == kBroken) return error(CELL_K2C_ERROR_OTHER);
    const u8 arrangement = ps_read8(b, handle + kArrangement);
    if (arrangement != 16 && mode == 2) return error(CELL_K2C_ERROR_INVALID_PARAMETER); // traditional Chinese's only
    if (arrangement == 11 && ps_read8(b, handle + kMode) == 1 && mode == 0) { // leaving Korean: the syllable dropped
        Mem m(ctx);
        reset_automaton(m, handle + kHangul);
    }
    switch (arrangement) {
    case 9:
    case 11:
        if (mode == 0) ps_write8(b, handle + kConv, 0);
        else if (mode == 1) ps_write8(b, handle + kConv, arrangement);
        break;
    case 1:
    case 16: break;
    default: return lv2::CELL_OK; // no native mode: the mode is not kept
    }
    ps_write8(b, handle + kMode, static_cast<u8>(mode));
    return lv2::CELL_OK;
}

s32 cellKey2CharSetArrangement(PPUContext& ctx, u32 handle, u32 arrangement)
{
    if (const s32 r = opened(ctx, handle, "cellKey2CharSetArrangement"); r != lv2::CELL_OK) return r;
    const s32 r = set_arrangement(ctx, handle, arrangement);
    if (r == lv2::CELL_OK) {
        u8* b = gb(ctx);
        ps_write16(b, handle + kLastKana, 0);
        ps_write8(b, handle + kMode, 0);
        ps_write8(b, handle + kState, 0);
        ps_write8(b, handle + kDead, 0);
    }
    return r;
}

// GetChar's outputs.
struct Out {
    PPUContext& ctx;
    u8* b;
    u32 handle, char_code, char_num, processed;
    void num(u32 n) { ps_write32(b, char_num, n); }
    u32 num() { return ps_read32(b, char_num); }
    void proc(u8 v) { ps_write8(b, processed, v); }
    u8 proc() { return ps_read8(b, processed); }
    // The first `bytes` bytes of UTF-16 units to where *charCode points (libkey2char copies an odd count after a
    // syllable or a kana's mark).
    void chars(u32 to, const u16 (&units)[8], u32 bytes)
    {
        u8 raw[16] = {};
        for (u32 i = 0; i < 8; i++) ps_st16(raw + 2 * i, units[i]);
        for (u32 i = 0; i < bytes && i < sizeof raw; i++) ps_write8(b, to + i, raw[i]);
    }
};

// Korean mode: the key through the automaton.
s32 get_char_korean(Out& o, u16 key, u32 mkey, u32 led)
{
    u8* b = o.b;
    const u32 h = o.handle, a = h + kHangul;
    Mem m(o.ctx);
    o.proc(1);
    u16 v = 0;
    const bool passthrough = korean_passthrough(key);
    if (passthrough) {
        o.proc(0);
        v = key;
    }
    if (!(led & kLedNumLock) && keypad_without_num_lock(key)) {
        o.proc(0);
        v = key;
    } else if (!passthrough) {
        u16 x = 0;
        if (((mkey & kMkeyShift) && (find(kKoreanShifted, key, x) || find(kUsShifted, key, x))) ||
            find(kKoreanLetters, key, x) || find(kControls, key, x) || find(kUsKeys, key, x) ||
            find(kKeypad, key, x))
            v = x;
    }

    ps_write16(b, h + kOutCount, 0);
    const auto count = [&] { return ps_read16(b, h + kOutCount); };
    // The next output unit (a conjoining jamo as its compatibility jamo), then a 0 after it.
    const auto put = [&](u32 ch) {
        const u16 i = count();
        ps_write16(b, h + kOutCount, static_cast<u16>(i + 1));
        u16 c = static_cast<u16>(ch);
        if (static_cast<u16>(c - 0x1100) < 0xFA) c = static_cast<u16>(to_compat(c));
        ps_write16(b, h + kOut + 2 * u32{i}, c);
        ps_write16(b, h + kOut + 2 * u32{count()}, 0);
    };
    const auto syllable = [&] { return compose(m.r32(a + kAL), m.r32(a + kAV), m.r32(a + kAT)); };
    // The jamo the syllable has last.
    u32 last = 0;
    {
        Stack s{m, a + kAStack};
        const u16 cap = s.cap();
        const u32 d = s.data();
        for (u32 i = 0; i < cap; i++) {
            const u32 w = m.r32(d + 4 * i);
            if (!w) break;
            last = w;
        }
    }

    u8 pending = 0;
    bool replace = false; // the syllable shown goes: a backspace first
    const bool hangul = static_cast<u16>(v + 0x5400) <= 0x2BAF || static_cast<u16>(v - 0x1100) <= 0xFF ||
                        static_cast<u16>(v - 0x3130) <= 0x5F;
    if (hangul) {
        ps_write8(b, h + kComposing, 1);
        if (v == 0x1175 && (last == 0x1161 || last == 0x1163 || last == 0x1165 || last == 0x1167)) {
            // i after a, ya, eo, yeo: the syllable ends there and i starts the next.
            recompute(m, a);
            put(syllable());
            reset_automaton(m, a);
            add_jamo(m, a, 0x1175);
            put(syllable());
        } else {
            const bool done = add_jamo(m, a, v);
            put(syllable());
            if (done) {
                recompute(m, a);
                put(syllable());
            }
        }
        pending = 1;
        replace = ps_read8(b, h + kPending) != 0;
    } else if (v == 8) {
        if (backspace(m, a)) {
            const u32 s = syllable();
            ps_write16(b, h + kOutCount, 0);
            if (static_cast<u16>(s) == kNoFinal) { // nothing left of the syllable: the backspace goes to the game
                ps_write16(b, h + kOut, 8);
                ps_write16(b, h + kOutCount, 1);
                ps_write16(b, h + kOut + 2, 0);
                ps_write8(b, h + kComposing, 0);
            } else {
                put(s);
                pending = 1;
                replace = ps_read8(b, h + kPending) != 0;
            }
        } else {
            put(8);
        }
    } else {
        reset_automaton(m, a);
        ps_write16(b, h + kOut + 2 * u32{count()}, v);
        ps_write16(b, h + kOutCount, static_cast<u16>(count() + 1));
        ps_write16(b, h + kOut + 2 * u32{count()}, 0);
        ps_write8(b, h + kComposing, 0);
    }
    u32 n = count();
    u16 units[8] = {};
    if (replace) {
        units[0] = 8;
        units[1] = ps_read16(b, h + kOut);
        units[2] = ps_read16(b, h + kOut + 2);
        n++;
    } else {
        units[0] = ps_read16(b, h + kOut);
        units[1] = ps_read16(b, h + kOut + 2);
    }
    o.num(n);
    if (o.proc() == 0) o.num(0);
    ps_write8(b, h + kPending, pending);
    o.chars(h + kChars, units, 2 * n + 1);
    return lv2::CELL_OK;
}

// Japanese mode (106): kana, and dakuten or handakuten on the kana typed last.
s32 get_char_kana(Out& o, u16 key, u32 mkey, u32 led)
{
    u8* b = o.b;
    const u32 h = o.handle, to = ps_read32(b, o.char_code);
    u16 units[8] = {};
    const bool mark = key == 0x2F || (key == 0x30 && !(mkey & kMkeyShift));
    if (key == 0x2F || key == 0x30) ps_write8(b, h + kState, 3);
    if (!mark) {
        ps_write8(b, h + kState, 2);
        u16 x = 0;
        if (((mkey & kMkeyShift) && find(kKanaShifted, key, x)) || native_key(kKanaKeys, key, x)) {
            units[0] = x;
            o.num(1);
        }
        if (o.num() == 0) {
            units[0] = static_cast<u16>(
                lib_call(o.ctx, "sys_io", "cellKbCnvRawCode", {ps_read8(b, h + kConv), mkey, led, key}));
            o.num(1);
        }
        o.chars(to, units, 10);
        ps_write16(b, h + kLastKana, units[0]);
        return lv2::CELL_OK;
    }
    const u16 prev = ps_read16(b, h + kLastKana);
    const bool voiced = key == 0x2F;
    units[0] = voiced ? kDakutenMark : kHandakutenMark;
    o.num(1);
    if (prev) {
        bool takes = false;
        if (voiced) {
            for (const u16 k : kDakuten) takes = takes || k == prev;
        } else {
            for (const u16 k : kHandakuten) takes = takes || k == prev;
        }
        if (takes) { // the kana shown goes, its voiced form comes
            units[0] = 8;
            units[1] = static_cast<u16>(prev + (voiced ? 1 : 2));
            o.num(2);
        }
    }
    ps_write16(b, h + kLastKana, 0);
    o.chars(to, units, 2 * o.num() + 1);
    ps_write8(b, h + kState, 2);
    return lv2::CELL_OK;
}

// Traditional Chinese, Zhuyin (mode 1) and Cangjie (mode 2): the layout's character unshifted; shifted, the letter
// (Zhuyin: in the conversion arrangement, with shift only under Caps Lock; Cangjie: in the US layout, its case
// inverted unless Caps Lock is on).
s32 get_char_chinese(Out& o, u16 key, u32 mkey, u32 led, bool cangjie)
{
    u8* b = o.b;
    const u32 h = o.handle, to = ps_read32(b, o.char_code);
    u16 units[8] = {};
    if (cangjie && key == 0x1D && !(mkey & kMkeyShift)) { // Z: no radical, not processed
        o.proc(0);
        o.num(0);
    }
    if (!(mkey & kMkeyShift)) {
        u16 x = 0;
        if (cangjie ? native_key(kCangjie, key, x) : native_key(kZhuyin, key, x)) {
            units[0] = x;
            o.num(1);
        }
    } else if (cangjie) {
        const u16 c = static_cast<u16>(lib_call(o.ctx, "sys_io", "cellKbCnvRawCode", {0, mkey, 0, key}));
        o.num(1);
        units[0] = c;
        if (led & kLedCapsLock) {
            if (static_cast<u16>(c - 'a') <= 25) units[0] = static_cast<u16>(c - 32);
        } else if (static_cast<u16>(c - 'A') <= 25) {
            units[0] = static_cast<u16>(c + 32);
        }
    } else {
        const u16 c = static_cast<u16>(lib_call(o.ctx, "sys_io", "cellKbCnvRawCode",
            {ps_read8(b, h + kConv), (led & kLedCapsLock) ? mkey : 0u, 0, key}));
        units[0] = c;
        o.num(1);
    }
    o.chars(to, units, 10);
    return lv2::CELL_OK;
}

// cellKey2CharGetChar(handle, const CellKey2CharKeyData *kdata {led, mkey, keycode (u16)}, uint16_t **charCode,
// uint32_t *charNum, bool *processed).
s32 cellKey2CharGetChar(PPUContext& ctx, u32 handle, u32 kdata, u32 char_code, u32 char_num, u32 processed)
{
    if (const s32 r = opened(ctx, handle, "cellKey2CharGetChar"); r != lv2::CELL_OK) return r;
    u8* b = gb(ctx);
    lv2::PpuThread& t = thread_of(ctx);
    if (!char_code || !kdata) return error(CELL_K2C_ERROR_INVALID_PARAMETER);
    // libkey2char writes charNum and processed unchecked (a NULL one faults on a PS3): pspocket refuses what is not
    // guest memory instead.
    if (!guest_readable(t, kdata, 12) || !guest_readable(t, char_code, 4) || !guest_readable(t, char_num, 4) ||
        !guest_readable(t, processed, 1)) {
        log_call_site(ctx, b, "cellKey2CharGetChar: an argument is not guest memory");
        return error(CELL_K2C_ERROR_INVALID_PARAMETER);
    }
    if (ps_read16(b, kdata + 8) == 0) return error(CELL_K2C_ERROR_INVALID_PARAMETER);
    if (ps_read8(b, handle + kConv) == kBroken && query_arrangement(ctx, handle) != 0)
        return error(CELL_K2C_ERROR_OTHER); // still no keyboard
    for (u32 i = 0; i < 10; i++) ps_write8(b, handle + kChars + i, 0);
    const u16 key = ps_read16(b, kdata + 8);
    ps_write32(b, char_code, handle + kChars);
    Out o{ctx, b, handle, char_code, char_num, processed};
    if (key == 0) { // read again: a game's other thread may have changed it
        o.num(0);
        return error(CELL_K2C_ERROR_INVALID_PARAMETER);
    }
    const u8 conv = ps_read8(b, handle + kConv);
    if (conv == 11) return get_char_korean(o, key, ps_read32(b, kdata + 4), ps_read32(b, kdata));
    const bool kana = conv == 1 && ps_read8(b, handle + kMode) == 1;
    const u32 mkey = ps_read32(b, kdata + 4);
    if (kana && (mkey & kMkeyShift) && key == 0x27) { // shift 0: wo
        ps_write16(b, handle + kChars, kWo);
        o.proc(1);
        o.num(1);
        return lv2::CELL_OK;
    }
    const u32 led = ps_read32(b, kdata);
    const u16 c = static_cast<u16>(lib_call(ctx, "sys_io", "cellKbCnvRawCode", {conv, mkey, led, key}));
    if (c & kRawDat) { // no character: the key code itself, not processed
        o.proc(0);
        ps_write16(b, ps_read32(b, char_code), key);
        o.num(0);
        return lv2::CELL_OK;
    }
    if (c == 0) return error(CELL_K2C_ERROR_OTHER);
    o.proc(1);
    o.num(0);
    if (kana) return get_char_kana(o, key, mkey, led);
    const u8 arrangement = ps_read8(b, handle + kArrangement), mode = ps_read8(b, handle + kMode);
    if (arrangement == 16 && (mode == 1 || mode == 2)) return get_char_chinese(o, key, mkey, led, mode == 2);

    // The code page's characters, a dead key waiting for its letter.
    u8 bytes[5] = {};
    const auto length = [&] {
        u32 n = 0;
        while (n < 5 && bytes[n]) n++;
        return n;
    };
    const u8 state = ps_read8(b, handle + kState);
    if (state == 0 || state == 2) {
        if (state == 2) ps_write8(b, handle + kState, 0);
        if (is_dead_key(conv, c, mkey)) {
            ps_write8(b, handle + kState, 1);
            ps_write8(b, handle + kDead, static_cast<u8>(c));
        } else {
            ps_write8(b, handle + kState, 0);
            bytes[0] = static_cast<u8>(c);
            o.num(length());
        }
    } else if (state == 1) {
        const u8 dead = ps_read8(b, handle + kDead);
        const auto [table, size] = dead_table(dead, conv);
        bool both = key == 0x2A; // Backspace: the dead key and the backspace as they are
        if (!both && table) {
            both = true; // a letter it does not compose with: both, as they are
            for (usize i = 0; i < size && both; i++)
                if (table[i].base == static_cast<u8>(c)) {
                    bytes[0] = table[i].composed;
                    o.num(length());
                    both = false;
                }
        }
        if (both) {
            bytes[0] = dead;
            bytes[1] = static_cast<u8>(c);
            o.num(length());
        }
        ps_write8(b, handle + kDead, 0);
        ps_write8(b, handle + kState, 0);
    } else { // a state the kana left: plain characters, the state kept
        bytes[0] = static_cast<u8>(c);
        o.num(length());
    }
    u16 units[8] = {};
    for (u32 i = 0; i < 5; i++) units[i] = to_utf16(conv, bytes[i]);
    o.chars(ps_read32(b, char_code), units, 10);
    return lv2::CELL_OK;
}

// libkey2char's other exports: its Hangul automaton's functions (on a handle's +0x14), under their NIDs. Those the
// module has as void answer what its code leaves in r3 (a stack's address, or its last step's answer).
u32 cellKey2char_83E91458(PPUContext& ctx, u32 a)
{
    Mem m(ctx);
    init_automaton(m, a);
    return a + kASaved;
}
u32 cellKey2char_AE4B34E6(PPUContext& ctx, u32 a)
{
    if (!a) return kASaved;
    Mem m(ctx);
    Stack{m, a + kASaved}.forget();
    Stack{m, a + kAStack}.forget();
    return a + kAStack;
}
u32 cellKey2char_E5D1F9B0(PPUContext& ctx, u32 a)
{
    Mem m(ctx);
    return recompute(m, a);
}
u32 cellKey2char_291ADBA5(PPUContext& ctx, u32 a)
{
    Mem m(ctx);
    return restore(m, a) ? 1 : 0;
}
u32 cellKey2char_1B5F6EFC(PPUContext& ctx, u32 a)
{
    Mem m(ctx);
    return backspace(m, a) ? 1 : 0;
}
u32 cellKey2char_51667234(PPUContext& ctx, u32 a, u32 jamo)
{
    Mem m(ctx);
    return add_jamo(m, a, jamo) ? 1 : 0;
}
u32 cellKey2char_7F8E6F3E(PPUContext& ctx, u32 a)
{
    Mem m(ctx);
    reset_automaton(m, a);
    return a + kAStack;
}
u32 cellKey2char_28893D81(PPUContext&, u32 l, u32 v, u32 t) { return compose(l, v, t); }
u32 cellKey2char_EAE114F6(PPUContext&, u32 c) { return to_compat(c); }

} // namespace

void register_cellKey2char(Registry& r)
{
    r.add(kModule, {
        PS_HLE_FUNC(0xABF629C1, cellKey2CharOpen),
        PS_HLE_FUNC(0x14BF2DC1, cellKey2CharClose),
        PS_HLE_FUNC(0x56776C0D, cellKey2CharGetChar),
        PS_HLE_FUNC(0xBFC03768, cellKey2CharSetMode),
        PS_HLE_FUNC(0x0DFBADFA, cellKey2CharSetArrangement),
        PS_HLE_FUNC(0x83E91458, cellKey2char_83E91458),
        PS_HLE_FUNC(0xAE4B34E6, cellKey2char_AE4B34E6),
        PS_HLE_FUNC(0xE5D1F9B0, cellKey2char_E5D1F9B0),
        PS_HLE_FUNC(0x291ADBA5, cellKey2char_291ADBA5),
        PS_HLE_FUNC(0x1B5F6EFC, cellKey2char_1B5F6EFC),
        PS_HLE_FUNC(0x51667234, cellKey2char_51667234),
        PS_HLE_FUNC(0x7F8E6F3E, cellKey2char_7F8E6F3E),
        PS_HLE_FUNC(0x28893D81, cellKey2char_28893D81),
        PS_HLE_FUNC(0xEAE114F6, cellKey2char_EAE114F6),
    });
}

} // namespace pspocket::hle
