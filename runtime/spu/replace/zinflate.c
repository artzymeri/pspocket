/*
 * zlib 1.2.3's inflate() natively, on a stream and state in SPU local store (D-975).
 *
 * Games link zlib into their SPU programs (GTA V's "inflateServer" task: RAGE streams its packed resources through it,
 * a 4 KB input chunk and an 8 KB output chunk a call). A native inflate must leave local store as the program's does:
 * the z_stream and its inflate_state stay where the program keeps them, in zlib 1.2.3's layout for a 32-bit target
 * (every field four bytes; codes four bytes: op, bits, big-endian val), with the program's own fixed tables and message
 * strings referred to by their addresses, the window in local store. The SPURS task may yield between calls and come
 * back on another SPU from a saved local store: nothing of the stream lives on the host between calls. So this is
 * zlib 1.2.3's inflate.c, inffast.c and inftrees.c (Jean-loup Gailly and Mark Adler, the zlib licence: the reference
 * for every step, kept statement for statement so the state, the tables, the window and the output are the same bytes)
 * over local store, built for the decoding speed of a host core. A host zlib (libz, libcompression) gives the same
 * output bytes but keeps its own state: a stream could not move between it and the program's code, nor be checked bit
 * for bit against it.
 *
 * Declined (nothing changed, the program's inflate runs): a stream with a zlib or gzip wrapper (its check values), a
 * state without its window yet (inflate allocates it through the stream's zalloc, the program's code), and buffers
 * that would wrap local store.
 */
#include "native.h"

#include <string.h>

#include "pspocket/core/be.h"
#include "pspocket/spu/context.h"

#define LS_MASK PS_SPU_LS_MASK

/* z_stream (32-bit) */
enum { ZS_NEXT_IN = 0, ZS_AVAIL_IN = 4, ZS_TOTAL_IN = 8, ZS_NEXT_OUT = 12, ZS_AVAIL_OUT = 16, ZS_TOTAL_OUT = 20,
       ZS_MSG = 24, ZS_STATE = 28, ZS_DATA_TYPE = 44, ZS_SIZE = 56 };
/* struct inflate_state (32-bit): its 28 words, then lens[320], work[288], codes[2048] */
enum { ST_WORDS = 28, ST_LENS = 112, ST_WORK = 752, ST_CODES = 1328, ST_SIZE = 1328 + 4 * 2048 };
enum { W_MODE, W_LAST, W_WRAP, W_HAVEDICT, W_FLAGS, W_DMAX, W_CHECK, W_TOTAL, W_HEAD, W_WBITS, W_WSIZE, W_WHAVE, W_WRITE,
       W_WINDOW, W_HOLD, W_BITS, W_LENGTH, W_OFFSET, W_EXTRA, W_LENCODE, W_DISTCODE, W_LENBITS, W_DISTBITS, W_NCODE,
       W_NLEN, W_NDIST, W_HAVE, W_NEXT };

/* inflate_mode */
enum { HEAD, FLAGS, TIME, OS, EXLEN, EXTRA, NAME, COMMENT, HCRC, DICTID, DICT, TYPE, TYPEDO, STORED, COPY, TABLE,
       LENLENS, CODELENS, LEN, LENEXT, DIST, DISTEXT, MATCH, LIT, CHECK, LENGTH, DONE, BAD, MEM, SYNC };

enum { Z_OK = 0, Z_STREAM_END = 1, Z_NEED_DICT = 2, Z_STREAM_ERROR = -2, Z_DATA_ERROR = -3, Z_MEM_ERROR = -4,
       Z_BUF_ERROR = -5, Z_FINISH = 4, Z_BLOCK = 5 };

#define FALLTHROUGH __attribute__((fallthrough))
#define MAXBITS 15
#define ENOUGH 2048
#define MAXD 592

typedef struct {
    uint8_t op, bits;
    uint16_t val;
} code;

/* The program's constants: where its fixed code tables and its message strings are (inflate.c's and inffast.c's). */
typedef struct {
    uint32_t lenfix, distfix;
    uint32_t msg_header_check, msg_compression_method, msg_window_size, msg_block_type, msg_stored_lengths,
        msg_too_many_symbols, msg_code_lengths_set, msg_bit_length_repeat, msg_literal_lengths_set, msg_distances_set,
        msg_literal_length_code, msg_distance_code, msg_too_far_back;
    uint32_t fast_literal_length_code, fast_distance_code, fast_too_far_back;
} zbuild;

typedef struct {
    uint8_t* ls;
    uint32_t strm, st; /* LS addresses */
    uint32_t w[ST_WORDS];
    uint32_t next_in, avail_in, total_in, next_out, avail_out, total_out, msg, data_type;
    const zbuild* b;
} zctx;

static inline code ld_code(const uint8_t* ls, uint32_t a)
{
    const uint8_t* p = ls + (a & LS_MASK);
    code c = {p[0], p[1], (uint16_t)(p[2] << 8 | p[3])};
    return c;
}
static inline void st_code(uint8_t* ls, uint32_t a, code c)
{
    uint8_t* p = ls + (a & LS_MASK);
    p[0] = c.op;
    p[1] = c.bits;
    p[2] = (uint8_t)(c.val >> 8);
    p[3] = (uint8_t)c.val;
}
static inline unsigned lens_at(const uint8_t* ls, uint32_t a, unsigned i) { return ps_ld16(ls + ((a + 2 * i) & LS_MASK)); }
static inline void lens_set(uint8_t* ls, uint32_t a, unsigned i, unsigned v) { ps_st16(ls + ((a + 2 * i) & LS_MASK), (uint16_t)v); }

/* ---- inftrees.c: inflate_table ---- */

typedef enum { CODES, LENS, DISTS } codetype;

static int inflate_table(uint8_t* ls, codetype type, uint32_t lens, unsigned codes, uint32_t* table, unsigned* bits,
    uint32_t work)
{
    unsigned len, sym, min, max, root, curr, drop;
    int left;
    unsigned used, huff, incr, fill, low, mask;
    code this;
    uint32_t next;
    const unsigned short *base, *extra;
    int end;
    unsigned short count[MAXBITS + 1], offs[MAXBITS + 1];
    static const unsigned short lbase[31] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67,
        83, 99, 115, 131, 163, 195, 227, 258, 0, 0};
    static const unsigned short lext[31] = {16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 18, 18, 18, 18, 19, 19, 19,
        19, 20, 20, 20, 20, 21, 21, 21, 21, 16, 201, 196};
    static const unsigned short dbase[32] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
        769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 0, 0};
    static const unsigned short dext[32] = {16, 16, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 23, 24,
        24, 25, 25, 26, 26, 27, 27, 28, 28, 29, 29, 64, 64};

    for (len = 0; len <= MAXBITS; len++) count[len] = 0;
    for (sym = 0; sym < codes; sym++) count[lens_at(ls, lens, sym)]++;

    root = *bits;
    for (max = MAXBITS; max >= 1; max--)
        if (count[max] != 0) break;
    if (root > max) root = max;
    if (max == 0) {
        this.op = 64;
        this.bits = 1;
        this.val = 0;
        st_code(ls, *table, this);
        *table += 4;
        st_code(ls, *table, this);
        *table += 4;
        *bits = 1;
        return 0;
    }
    for (min = 1; min <= MAXBITS; min++)
        if (count[min] != 0) break;
    if (root < min) root = min;

    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= count[len];
        if (left < 0) return -1;
    }
    if (left > 0 && (type == CODES || max != 1)) return -1;

    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) offs[len + 1] = (unsigned short)(offs[len] + count[len]);

    for (sym = 0; sym < codes; sym++) {
        const unsigned l = lens_at(ls, lens, sym);
        if (l != 0) lens_set(ls, work, offs[l]++, sym);
    }

    switch (type) {
    case CODES:
        base = extra = NULL; /* not used: every symbol is below end */
        end = 19;
        break;
    case LENS:
        base = lbase - 257;
        extra = lext - 257;
        end = 256;
        break;
    default:
        base = dbase;
        extra = dext;
        end = -1;
    }

    huff = 0;
    sym = 0;
    len = min;
    next = *table;
    curr = root;
    drop = 0;
    low = (unsigned)(-1);
    used = 1U << root;
    mask = used - 1;

    if (type == LENS && used >= ENOUGH - MAXD) return 1;

    for (;;) {
        const unsigned w = lens_at(ls, work, sym);
        this.bits = (uint8_t)(len - drop);
        if ((int)w < end) {
            this.op = 0;
            this.val = (uint16_t)w;
        } else if ((int)w > end) {
            this.op = (uint8_t)extra[w];
            this.val = base[w];
        } else {
            this.op = 32 + 64;
            this.val = 0;
        }

        incr = 1U << (len - drop);
        fill = 1U << curr;
        min = fill;
        do {
            fill -= incr;
            st_code(ls, next + 4 * ((huff >> drop) + fill), this);
        } while (fill != 0);

        incr = 1U << (len - 1);
        while (huff & incr) incr >>= 1;
        if (incr != 0) {
            huff &= incr - 1;
            huff += incr;
        } else {
            huff = 0;
        }

        sym++;
        if (--(count[len]) == 0) {
            if (len == max) break;
            len = lens_at(ls, lens, lens_at(ls, work, sym));
        }

        if (len > root && (huff & mask) != low) {
            if (drop == 0) drop = root;
            next += 4 * min;
            curr = len - drop;
            left = (int)(1 << curr);
            while (curr + drop < max) {
                left -= count[curr + drop];
                if (left <= 0) break;
                curr++;
                left <<= 1;
            }
            used += 1U << curr;
            if (type == LENS && used >= ENOUGH - MAXD) return 1;
            low = huff & mask;
            code link = {(uint8_t)curr, (uint8_t)root, (uint16_t)((next - *table) / 4)};
            st_code(ls, *table + 4 * low, link);
        }
    }

    this.op = 64;
    this.bits = (uint8_t)(len - drop);
    this.val = 0;
    while (huff != 0) {
        if (drop != 0 && (huff & mask) != low) {
            drop = 0;
            len = root;
            next = *table;
            this.bits = (uint8_t)len;
        }
        st_code(ls, next + 4 * (huff >> drop), this);
        incr = 1U << (len - 1);
        while (huff & incr) incr >>= 1;
        if (incr != 0) {
            huff &= incr - 1;
            huff += incr;
        } else {
            huff = 0;
        }
    }

    *table += 4 * used;
    *bits = root;
    return 0;
}

/* ---- inffast.c: inflate_fast ---- */

static void inflate_fast(zctx* z, unsigned start)
{
    uint8_t* ls = z->ls;
    uint32_t* w = z->w;
    const uint8_t* in = ls + z->next_in;
    const uint8_t* last = in + (z->avail_in - 5);
    uint8_t* out = ls + z->next_out;
    uint8_t* const beg = out - (start - z->avail_out);
    uint8_t* const end = out + (z->avail_out - 257);
    const unsigned wsize = w[W_WSIZE], whave = w[W_WHAVE], write = w[W_WRITE];
    const uint8_t* const window = ls + w[W_WINDOW];
    uint32_t hold = w[W_HOLD];
    unsigned bits = w[W_BITS];
    const uint8_t* const lcode = ls + w[W_LENCODE];
    const uint8_t* const dcode = ls + w[W_DISTCODE];
    const unsigned lmask = (1U << w[W_LENBITS]) - 1, dmask = (1U << w[W_DISTBITS]) - 1;
    unsigned op, len, dist;
    const uint8_t* from;
    code this;
#define LCODE(i) (this = (code){lcode[4 * (i)], lcode[4 * (i) + 1], (uint16_t)(lcode[4 * (i) + 2] << 8 | lcode[4 * (i) + 3])})
#define DCODE(i) (this = (code){dcode[4 * (i)], dcode[4 * (i) + 1], (uint16_t)(dcode[4 * (i) + 2] << 8 | dcode[4 * (i) + 3])})

    do {
        if (bits < 15) {
            hold += (uint32_t)(*in++) << bits;
            bits += 8;
            hold += (uint32_t)(*in++) << bits;
            bits += 8;
        }
        LCODE(hold & lmask);
    dolen:
        op = this.bits;
        hold >>= op;
        bits -= op;
        op = this.op;
        if (op == 0) {
            *out++ = (uint8_t)this.val;
        } else if (op & 16) {
            len = this.val;
            op &= 15;
            if (op) {
                if (bits < op) {
                    hold += (uint32_t)(*in++) << bits;
                    bits += 8;
                }
                len += (unsigned)hold & ((1U << op) - 1);
                hold >>= op;
                bits -= op;
            }
            if (bits < 15) {
                hold += (uint32_t)(*in++) << bits;
                bits += 8;
                hold += (uint32_t)(*in++) << bits;
                bits += 8;
            }
            DCODE(hold & dmask);
        dodist:
            op = this.bits;
            hold >>= op;
            bits -= op;
            op = this.op;
            if (op & 16) {
                dist = this.val;
                op &= 15;
                if (bits < op) {
                    hold += (uint32_t)(*in++) << bits;
                    bits += 8;
                    if (bits < op) {
                        hold += (uint32_t)(*in++) << bits;
                        bits += 8;
                    }
                }
                dist += (unsigned)hold & ((1U << op) - 1);
                hold >>= op;
                bits -= op;
                op = (unsigned)(out - beg);
                if (dist > op) {
                    op = dist - op;
                    if (op > whave) {
                        z->msg = z->b->fast_too_far_back;
                        w[W_MODE] = BAD;
                        break;
                    }
                    from = window;
                    if (write == 0) {
                        from += wsize - op;
                        if (op < len) {
                            len -= op;
                            do {
                                *out++ = *from++;
                            } while (--op);
                            from = out - dist;
                        }
                    } else if (write < op) {
                        from += wsize + write - op;
                        op -= write;
                        if (op < len) {
                            len -= op;
                            do {
                                *out++ = *from++;
                            } while (--op);
                            from = window;
                            if (write < len) {
                                op = write;
                                len -= op;
                                do {
                                    *out++ = *from++;
                                } while (--op);
                                from = out - dist;
                            }
                        }
                    } else {
                        from += write - op;
                        if (op < len) {
                            len -= op;
                            do {
                                *out++ = *from++;
                            } while (--op);
                            from = out - dist;
                        }
                    }
                    while (len > 2) {
                        *out++ = *from++;
                        *out++ = *from++;
                        *out++ = *from++;
                        len -= 3;
                    }
                    if (len) {
                        *out++ = *from++;
                        if (len > 1) *out++ = *from++;
                    }
                } else {
                    from = out - dist;
                    do {
                        *out++ = *from++;
                        *out++ = *from++;
                        *out++ = *from++;
                        len -= 3;
                    } while (len > 2);
                    if (len) {
                        *out++ = *from++;
                        if (len > 1) *out++ = *from++;
                    }
                }
            } else if ((op & 64) == 0) {
                DCODE(this.val + (hold & ((1U << op) - 1)));
                goto dodist;
            } else {
                z->msg = z->b->fast_distance_code;
                w[W_MODE] = BAD;
                break;
            }
        } else if ((op & 64) == 0) {
            LCODE(this.val + (hold & ((1U << op) - 1)));
            goto dolen;
        } else if (op & 32) {
            w[W_MODE] = TYPE;
            break;
        } else {
            z->msg = z->b->fast_literal_length_code;
            w[W_MODE] = BAD;
            break;
        }
    } while (in < last && out < end);
#undef LCODE
#undef DCODE

    len = bits >> 3;
    in -= len;
    bits -= len << 3;
    hold &= (1U << bits) - 1;

    z->next_in = (uint32_t)(in - ls);
    z->next_out = (uint32_t)(out - ls);
    z->avail_in = (unsigned)(in < last ? 5 + (last - in) : 5 - (in - last));
    z->avail_out = (unsigned)(out < end ? 257 + (end - out) : 257 - (out - end));
    w[W_HOLD] = hold;
    w[W_BITS] = bits;
}

/* ---- inflate.c: updatewindow, inflate (NO_GZIP: the program's build has none of the gzip messages) ---- */

static void updatewindow(zctx* z, unsigned out)
{
    uint8_t* ls = z->ls;
    uint32_t* w = z->w;
    unsigned copy, dist;
    if (w[W_WSIZE] == 0) {
        w[W_WSIZE] = 1U << w[W_WBITS];
        w[W_WRITE] = 0;
        w[W_WHAVE] = 0;
    }
    copy = out - z->avail_out;
    if (copy >= w[W_WSIZE]) {
        memmove(ls + w[W_WINDOW], ls + z->next_out - w[W_WSIZE], w[W_WSIZE]);
        w[W_WRITE] = 0;
        w[W_WHAVE] = w[W_WSIZE];
    } else {
        dist = w[W_WSIZE] - w[W_WRITE];
        if (dist > copy) dist = copy;
        memmove(ls + w[W_WINDOW] + w[W_WRITE], ls + z->next_out - copy, dist);
        copy -= dist;
        if (copy) {
            memmove(ls + w[W_WINDOW], ls + z->next_out - copy, copy);
            w[W_WRITE] = copy;
            w[W_WHAVE] = w[W_WSIZE];
        } else {
            w[W_WRITE] += dist;
            if (w[W_WRITE] == w[W_WSIZE]) w[W_WRITE] = 0;
            if (w[W_WHAVE] < w[W_WSIZE]) w[W_WHAVE] += dist;
        }
    }
}

static int in_ls(uint32_t a, uint32_t n) { return a <= PS_SPU_LS_SIZE && n <= PS_SPU_LS_SIZE - a; }

/* Returns inflate()'s result, or 1000 to decline. */
static int inflate_ls(zctx* z, int flush)
{
    uint8_t* ls = z->ls;
    uint32_t* w = z->w;
    uint32_t next, put;
    unsigned have, left;
    uint32_t hold;
    unsigned bits, in, out, copy, len;
    uint32_t from;
    code this, last;
    int ret;
    static const unsigned short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const uint32_t lens = z->st + ST_LENS, work = z->st + ST_WORK, codes = z->st + ST_CODES;

#define LOAD()                                                                                                         \
    do {                                                                                                               \
        put = z->next_out;                                                                                             \
        left = z->avail_out;                                                                                           \
        next = z->next_in;                                                                                             \
        have = z->avail_in;                                                                                            \
        hold = w[W_HOLD];                                                                                              \
        bits = w[W_BITS];                                                                                              \
    } while (0)
#define RESTORE()                                                                                                      \
    do {                                                                                                               \
        z->next_out = put;                                                                                             \
        z->avail_out = left;                                                                                           \
        z->next_in = next;                                                                                             \
        z->avail_in = have;                                                                                            \
        w[W_HOLD] = hold;                                                                                              \
        w[W_BITS] = bits;                                                                                              \
    } while (0)
#define INITBITS()                                                                                                     \
    do {                                                                                                               \
        hold = 0;                                                                                                      \
        bits = 0;                                                                                                      \
    } while (0)
#define PULLBYTE()                                                                                                     \
    do {                                                                                                               \
        if (have == 0) goto inf_leave;                                                                                 \
        have--;                                                                                                        \
        hold += (uint32_t)ls[next++] << bits;                                                                          \
        bits += 8;                                                                                                     \
    } while (0)
#define NEEDBITS(n)                                                                                                    \
    do {                                                                                                               \
        while (bits < (unsigned)(n)) PULLBYTE();                                                                       \
    } while (0)
#define BITS(n) ((unsigned)hold & ((1U << (n)) - 1))
#define DROPBITS(n)                                                                                                    \
    do {                                                                                                               \
        hold >>= (n);                                                                                                  \
        bits -= (unsigned)(n);                                                                                         \
    } while (0)
#define BYTEBITS()                                                                                                     \
    do {                                                                                                               \
        hold >>= bits & 7;                                                                                             \
        bits -= bits & 7;                                                                                              \
    } while (0)

    if (w[W_MODE] == TYPE) w[W_MODE] = TYPEDO;
    LOAD();
    in = have;
    out = left;
    ret = Z_OK;
    for (;;) switch (w[W_MODE]) {
        case HEAD:
            if (w[W_WRAP] == 0) {
                w[W_MODE] = TYPEDO;
                break;
            }
            return 1000; /* a wrapper: declined before anything (checked at entry) */
        case TYPE:
            if (flush == Z_BLOCK) goto inf_leave;
            FALLTHROUGH;
        case TYPEDO:
            if (w[W_LAST]) {
                BYTEBITS();
                w[W_MODE] = CHECK;
                break;
            }
            NEEDBITS(3);
            w[W_LAST] = BITS(1);
            DROPBITS(1);
            switch (BITS(2)) {
            case 0:
                w[W_MODE] = STORED;
                break;
            case 1:
                w[W_LENCODE] = z->b->lenfix;
                w[W_LENBITS] = 9;
                w[W_DISTCODE] = z->b->distfix;
                w[W_DISTBITS] = 5;
                w[W_MODE] = LEN;
                break;
            case 2:
                w[W_MODE] = TABLE;
                break;
            case 3:
                z->msg = z->b->msg_block_type;
                w[W_MODE] = BAD;
            }
            DROPBITS(2);
            break;
        case STORED:
            BYTEBITS();
            NEEDBITS(32);
            if ((hold & 0xffff) != ((hold >> 16) ^ 0xffff)) {
                z->msg = z->b->msg_stored_lengths;
                w[W_MODE] = BAD;
                break;
            }
            w[W_LENGTH] = (unsigned)hold & 0xffff;
            INITBITS();
            w[W_MODE] = COPY;
            FALLTHROUGH;
        case COPY:
            copy = w[W_LENGTH];
            if (copy) {
                if (copy > have) copy = have;
                if (copy > left) copy = left;
                if (copy == 0) goto inf_leave;
                memmove(ls + put, ls + next, copy);
                have -= copy;
                next += copy;
                left -= copy;
                put += copy;
                w[W_LENGTH] -= copy;
                break;
            }
            w[W_MODE] = TYPE;
            break;
        case TABLE:
            NEEDBITS(14);
            w[W_NLEN] = BITS(5) + 257;
            DROPBITS(5);
            w[W_NDIST] = BITS(5) + 1;
            DROPBITS(5);
            w[W_NCODE] = BITS(4) + 4;
            DROPBITS(4);
            if (w[W_NLEN] > 286 || w[W_NDIST] > 30) {
                z->msg = z->b->msg_too_many_symbols;
                w[W_MODE] = BAD;
                break;
            }
            w[W_HAVE] = 0;
            w[W_MODE] = LENLENS;
            FALLTHROUGH;
        case LENLENS:
            while (w[W_HAVE] < w[W_NCODE]) {
                NEEDBITS(3);
                lens_set(ls, lens, order[w[W_HAVE]++], BITS(3));
                DROPBITS(3);
            }
            while (w[W_HAVE] < 19) lens_set(ls, lens, order[w[W_HAVE]++], 0);
            w[W_NEXT] = codes;
            w[W_LENCODE] = w[W_NEXT];
            w[W_LENBITS] = 7;
            ret = inflate_table(ls, CODES, lens, 19, &w[W_NEXT], &w[W_LENBITS], work);
            if (ret) {
                z->msg = z->b->msg_code_lengths_set;
                w[W_MODE] = BAD;
                break;
            }
            w[W_HAVE] = 0;
            w[W_MODE] = CODELENS;
            FALLTHROUGH;
        case CODELENS:
            while (w[W_HAVE] < w[W_NLEN] + w[W_NDIST]) {
                for (;;) {
                    this = ld_code(ls, w[W_LENCODE] + 4 * BITS(w[W_LENBITS]));
                    if ((unsigned)this.bits <= bits) break;
                    PULLBYTE();
                }
                if (this.val < 16) {
                    NEEDBITS(this.bits);
                    DROPBITS(this.bits);
                    lens_set(ls, lens, w[W_HAVE]++, this.val);
                } else {
                    if (this.val == 16) {
                        NEEDBITS(this.bits + 2);
                        DROPBITS(this.bits);
                        if (w[W_HAVE] == 0) {
                            z->msg = z->b->msg_bit_length_repeat;
                            w[W_MODE] = BAD;
                            break;
                        }
                        len = lens_at(ls, lens, w[W_HAVE] - 1);
                        copy = 3 + BITS(2);
                        DROPBITS(2);
                    } else if (this.val == 17) {
                        NEEDBITS(this.bits + 3);
                        DROPBITS(this.bits);
                        len = 0;
                        copy = 3 + BITS(3);
                        DROPBITS(3);
                    } else {
                        NEEDBITS(this.bits + 7);
                        DROPBITS(this.bits);
                        len = 0;
                        copy = 11 + BITS(7);
                        DROPBITS(7);
                    }
                    if (w[W_HAVE] + copy > w[W_NLEN] + w[W_NDIST]) {
                        z->msg = z->b->msg_bit_length_repeat;
                        w[W_MODE] = BAD;
                        break;
                    }
                    while (copy--) lens_set(ls, lens, w[W_HAVE]++, len);
                }
            }
            if (w[W_MODE] == BAD) break;
            w[W_NEXT] = codes;
            w[W_LENCODE] = w[W_NEXT];
            w[W_LENBITS] = 9;
            ret = inflate_table(ls, LENS, lens, w[W_NLEN], &w[W_NEXT], &w[W_LENBITS], work);
            if (ret) {
                z->msg = z->b->msg_literal_lengths_set;
                w[W_MODE] = BAD;
                break;
            }
            w[W_DISTCODE] = w[W_NEXT];
            w[W_DISTBITS] = 6;
            ret = inflate_table(ls, DISTS, lens + 2 * w[W_NLEN], w[W_NDIST], &w[W_NEXT], &w[W_DISTBITS], work);
            if (ret) {
                z->msg = z->b->msg_distances_set;
                w[W_MODE] = BAD;
                break;
            }
            w[W_MODE] = LEN;
            FALLTHROUGH;
        case LEN:
            if (have >= 6 && left >= 258) {
                RESTORE();
                inflate_fast(z, out);
                LOAD();
                break;
            }
            for (;;) {
                this = ld_code(ls, w[W_LENCODE] + 4 * BITS(w[W_LENBITS]));
                if ((unsigned)this.bits <= bits) break;
                PULLBYTE();
            }
            if (this.op && (this.op & 0xf0) == 0) {
                last = this;
                for (;;) {
                    this = ld_code(ls, w[W_LENCODE] + 4 * (last.val + (BITS(last.bits + last.op) >> last.bits)));
                    if ((unsigned)(last.bits + this.bits) <= bits) break;
                    PULLBYTE();
                }
                DROPBITS(last.bits);
            }
            DROPBITS(this.bits);
            w[W_LENGTH] = this.val;
            if ((int)this.op == 0) {
                w[W_MODE] = LIT;
                break;
            }
            if (this.op & 32) {
                w[W_MODE] = TYPE;
                break;
            }
            if (this.op & 64) {
                z->msg = z->b->msg_literal_length_code;
                w[W_MODE] = BAD;
                break;
            }
            w[W_EXTRA] = (unsigned)this.op & 15;
            w[W_MODE] = LENEXT;
            FALLTHROUGH;
        case LENEXT:
            if (w[W_EXTRA]) {
                NEEDBITS(w[W_EXTRA]);
                w[W_LENGTH] += BITS(w[W_EXTRA]);
                DROPBITS(w[W_EXTRA]);
            }
            w[W_MODE] = DIST;
            FALLTHROUGH;
        case DIST:
            for (;;) {
                this = ld_code(ls, w[W_DISTCODE] + 4 * BITS(w[W_DISTBITS]));
                if ((unsigned)this.bits <= bits) break;
                PULLBYTE();
            }
            if ((this.op & 0xf0) == 0) {
                last = this;
                for (;;) {
                    this = ld_code(ls, w[W_DISTCODE] + 4 * (last.val + (BITS(last.bits + last.op) >> last.bits)));
                    if ((unsigned)(last.bits + this.bits) <= bits) break;
                    PULLBYTE();
                }
                DROPBITS(last.bits);
            }
            DROPBITS(this.bits);
            if (this.op & 64) {
                z->msg = z->b->msg_distance_code;
                w[W_MODE] = BAD;
                break;
            }
            w[W_OFFSET] = this.val;
            w[W_EXTRA] = (unsigned)this.op & 15;
            w[W_MODE] = DISTEXT;
            FALLTHROUGH;
        case DISTEXT:
            if (w[W_EXTRA]) {
                NEEDBITS(w[W_EXTRA]);
                w[W_OFFSET] += BITS(w[W_EXTRA]);
                DROPBITS(w[W_EXTRA]);
            }
            if (w[W_OFFSET] > w[W_WHAVE] + out - left) {
                z->msg = z->b->msg_too_far_back;
                w[W_MODE] = BAD;
                break;
            }
            w[W_MODE] = MATCH;
            FALLTHROUGH;
        case MATCH:
            if (left == 0) goto inf_leave;
            copy = out - left;
            if (w[W_OFFSET] > copy) {
                copy = w[W_OFFSET] - copy;
                if (copy > w[W_WRITE]) {
                    copy -= w[W_WRITE];
                    from = w[W_WINDOW] + (w[W_WSIZE] - copy);
                } else {
                    from = w[W_WINDOW] + (w[W_WRITE] - copy);
                }
                if (copy > w[W_LENGTH]) copy = w[W_LENGTH];
            } else {
                from = put - w[W_OFFSET];
                copy = w[W_LENGTH];
            }
            if (copy > left) copy = left;
            left -= copy;
            w[W_LENGTH] -= copy;
            do {
                ls[put++] = ls[from++];
            } while (--copy);
            if (w[W_LENGTH] == 0) w[W_MODE] = LEN;
            break;
        case LIT:
            if (left == 0) goto inf_leave;
            ls[put++] = (uint8_t)w[W_LENGTH];
            left--;
            w[W_MODE] = LEN;
            break;
        case CHECK:
            if (w[W_WRAP]) return 1000;
            w[W_MODE] = DONE;
            FALLTHROUGH;
        case DONE:
            ret = Z_STREAM_END;
            goto inf_leave;
        case BAD:
            ret = Z_DATA_ERROR;
            goto inf_leave;
        case MEM:
            return Z_MEM_ERROR;
        case SYNC:
        default:
            return Z_STREAM_ERROR;
        }

inf_leave:
    RESTORE();
    if (w[W_WSIZE] || (w[W_MODE] < CHECK && out != z->avail_out)) updatewindow(z, out);
    in -= z->avail_in;
    out -= z->avail_out;
    z->total_in += in;
    z->total_out += out;
    w[W_TOTAL] += out;
    z->data_type = w[W_BITS] + (w[W_LAST] ? 64 : 0) + (w[W_MODE] == TYPE ? 128 : 0);
    if (((in == 0 && out == 0) || flush == Z_FINISH) && ret == Z_OK) ret = Z_BUF_ERROR;
    return ret;
#undef LOAD
#undef RESTORE
#undef INITBITS
#undef PULLBYTE
#undef NEEDBITS
#undef BITS
#undef DROPBITS
#undef BYTEBITS
}

/* ---- the entry: inflate(strm $3, flush $4) as the program calls it ---- */

static int zlib_inflate_native(SPUContext* ctx, uint8_t* ls, const zbuild* b)
{
    zctx z;
    z.ls = ls;
    z.b = b;
    z.strm = ctx->gpr[3].u32[3];
    const int flush = (int)ctx->gpr[4].u32[3];
    if (!z.strm || !in_ls(z.strm, ZS_SIZE) || (z.strm & 3)) return 0;
    const uint8_t* s = ls + z.strm;
    z.next_in = ps_ld32(s + ZS_NEXT_IN);
    z.avail_in = ps_ld32(s + ZS_AVAIL_IN);
    z.total_in = ps_ld32(s + ZS_TOTAL_IN);
    z.next_out = ps_ld32(s + ZS_NEXT_OUT);
    z.avail_out = ps_ld32(s + ZS_AVAIL_OUT);
    z.total_out = ps_ld32(s + ZS_TOTAL_OUT);
    z.msg = ps_ld32(s + ZS_MSG);
    z.st = ps_ld32(s + ZS_STATE);
    z.data_type = ps_ld32(s + ZS_DATA_TYPE);
    /* What zlib refuses (Z_STREAM_ERROR) and what is not done here: the program's own code for those. */
    if (!z.st || !in_ls(z.st, ST_SIZE) || (z.st & 3) || !z.next_out || (!z.next_in && z.avail_in)) return 0;
    for (int i = 0; i < ST_WORDS; i++) z.w[i] = ps_ld32(ls + z.st + 4 * (uint32_t)i);
    if (z.w[W_WRAP] != 0 || !z.w[W_WINDOW] || z.w[W_MODE] > SYNC) return 0;
    if (z.w[W_WBITS] < 8 || z.w[W_WBITS] > 15 || !in_ls(z.w[W_WINDOW], 1u << z.w[W_WBITS])) return 0;
    if (!in_ls(z.next_in, z.avail_in) || !in_ls(z.next_out, z.avail_out)) return 0;

    const int ret = inflate_ls(&z, flush);

    uint8_t* so = ls + z.strm;
    ps_st32(so + ZS_NEXT_IN, z.next_in);
    ps_st32(so + ZS_AVAIL_IN, z.avail_in);
    ps_st32(so + ZS_TOTAL_IN, z.total_in);
    ps_st32(so + ZS_NEXT_OUT, z.next_out);
    ps_st32(so + ZS_AVAIL_OUT, z.avail_out);
    ps_st32(so + ZS_TOTAL_OUT, z.total_out);
    ps_st32(so + ZS_MSG, z.msg);
    ps_st32(so + ZS_DATA_TYPE, z.data_type);
    for (int i = 0; i < ST_WORDS; i++) ps_st32(ls + z.st + 4 * (uint32_t)i, z.w[i]);
    /* the link register where the SPU ABI keeps it, in the caller's frame */
    {
        uint8_t* q = ls + ((ctx->gpr[1].u32[3] + 16) & PS_SPU_QW_MASK);
        for (int i = 0; i < 4; i++) ps_st32(q + 4 * i, ctx->gpr[0].u32[3 - i]);
    }
    for (int i = 0; i < 4; i++) ctx->gpr[3].u32[i] = (uint32_t)ret;
    ctx->pc = ctx->gpr[0].u32[3] & PS_SPU_PC_MASK;
    return 1;
}

/* GTA V's inflateServer task (spu_3ffe1d50f8a58e15, at 0x3000): its fixed tables and messages. */
static const zbuild k_3ffe = {
    .lenfix = 0x8D80, .distfix = 0x8D00,
    .msg_header_check = 0x8A00, .msg_compression_method = 0x8A20, .msg_window_size = 0x8A40,
    .msg_block_type = 0x8A60, .msg_stored_lengths = 0x8A80, .msg_too_many_symbols = 0x8AA0,
    .msg_code_lengths_set = 0x8AD0, .msg_bit_length_repeat = 0x8AF0, .msg_literal_lengths_set = 0x8B10,
    .msg_distances_set = 0x8B30, .msg_literal_length_code = 0x8B50, .msg_distance_code = 0x8B70,
    .msg_too_far_back = 0x8B90,
    .fast_literal_length_code = 0x89C0, .fast_distance_code = 0x89A0, .fast_too_far_back = 0x89E0,
};

int ps_zlib_3ffe_05190(SPUContext* ctx, uint8_t* ls) { return zlib_inflate_native(ctx, ls, &k_3ffe); }
