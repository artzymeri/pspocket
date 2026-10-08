# The mapping tables of pspocket's cellL10n, from public mapping data only: Python's codecs (the Unicode consortium's
# and the vendors' published tables: cp932, euc_jp (JIS X 0212), cp949, gbk, gb2312, gb18030, cp950, the ISO 8859
# and DOS/Windows code pages) and libaribcaption's ARIB STD-B24 additional-symbol table (MIT). Where Sony's module
# answers differently from those tables the comparison found it; each such place is a named patch below.
import codecs, unicodedata, re, os

def dec(b, cp):
    try:
        u = b.decode(cp)
        return ord(u) if len(u) == 1 else 0xFFFF
    except Exception:
        return 0xFFFF

def kuten_to_sjis(row, col):
    hi = row + 32; t = (hi - 33) >> 1; s1 = t + 129
    if s1 > 159: s1 = t + 193
    if hi & 1: s2 = col + 63 if col <= 63 else col + 64
    else: s2 = col + 158
    return bytes([s1, s2])

def cp932_cell(row, col): return dec(kuten_to_sjis(row, col), 'cp932')

# JIS X 0208 as cp932 reads it: rows 1-8, 16-84 (row 84 to its 6th cell), 7150 cells.
JIS_ROWS = list(range(1, 9)) + list(range(16, 85))
jis0208 = [cp932_cell(r, c) for r in JIS_ROWS for c in range(1, 95)][:7150]
row13 = [cp932_cell(13, c) for c in range(1, 95)]
necsel = [cp932_cell(r, c) for r in range(89, 93) for c in range(1, 95)]                      # 376
ibmext = [cp932_cell(r, c) for r in range(115, 120) for c in range(1, 95)][:388]                # FA40..FC4B
# An IBM extension character's JIS code (SJIStoEUCJP, SJISstoJISs): the same character's NEC row 13 cell, else its
# NEC-selected cell, else its JIS X 0208 cell (Sony's order, found by comparison: ¬ and ∵ take the NEC cells).
first = {}
for code, u in ([(0x2D21 + i, u) for i, u in enumerate(row13)] +
                [(((89 + i // 94 + 0x20) << 8) | (i % 94 + 0x21), u) for i, u in enumerate(necsel)] +
                [(((JIS_ROWS[i // 94] + 0x20) << 8) | (i % 94 + 0x21), u) for i, u in enumerate(jis0208)]):
    if u != 0xFFFF and u not in first: first[u] = code
ibm2jis = [first.get(u, 0) for u in ibmext]
# JIS X 0212 (EUC-JP 8F xx xx): rows 2, 6, 7, 9, 10, 11, 16-77 to EDE3 (6365 cells), as euc_jp reads it, with two
# cells as Sony reads them: 2-23 (A2B7) FULLWIDTH TILDE and 2-35 (A2C3) FULLWIDTH BROKEN BAR (Microsoft's EUC-JP).
X0212_ROWS = [2, 6, 7, 9, 10, 11] + list(range(16, 78))
jis0212 = [dec(bytes([0x8F, r + 0xA0, c + 0xA0]), 'euc_jp') for r in X0212_ROWS for c in range(1, 95)][:6365]
jis0212[(0) * 94 + 0x17 - 1] = 0xFF5E
jis0212[(0) * 94 + 0x23 - 1] = 0xFFE4
# KS X 1001 rows A1-AC, B0-C8, CA-FD as cp949 reads them, with A2E8 (KS X 1001:2002's postal code mark, U+327E).
KS_ROWS = list(range(0xA1, 0xAD)) + list(range(0xB0, 0xC9)) + list(range(0xCA, 0xFE))
ksx1001 = [dec(bytes([r, 0xA0 + c]), 'cp949') for r in KS_ROWS for c in range(1, 95)]
ksx1001[KS_ROWS.index(0xA2) * 94 + 0xE8 - 0xA1] = 0x327E
def uhc_trail(i, n):
    c = i % n
    return 0x41 + c if c < 26 else (0x61 + c - 26 if c < 52 else 0x81 + c - 52)
uhc1 = [dec(bytes([0x81 + i // 178, uhc_trail(i, 178)]), 'cp949') for i in range(5696)]  # leads 81-A0
uhc2 = [dec(bytes([0xA1 + i // 84, uhc_trail(i, 84)]), 'cp949') for i in range(3126)]    # leads A1-C6, to C652
# GB2312 rows A1-A9, B0-F7: the cells GB2312 assigns, as gbk reads them (A1A4 U+00B7, A1AA U+2014).
GB_ROWS = list(range(0xA1, 0xAA)) + list(range(0xB0, 0xF8))
gb2312 = []
for r in GB_ROWS:
    for c in range(0xA1, 0xFF):
        b = bytes([r, c])
        gb2312.append(dec(b, 'gbk') if dec(b, 'gb2312') != 0xFFFF else 0xFFFF)
gbkrows = [dec(bytes([r, c]), 'gbk') for r in (0xA2, 0xA6, 0xA8) for c in range(0xA1, 0xFF)]   # 282
def lowtrail(i, n):
    c = i % n
    return 0x40 + c if c < 63 else 0x41 + c
gbk3 = [dec(bytes([0x81 + i // 190, lowtrail(i, 190)]), 'gbk') for i in range(6080)]
gbk4 = [dec(bytes([0xA8 + i // 96, lowtrail(i, 96)]), 'gbk') for i in range(8272)]
# GB18030's own cells (Sony's are GB18030-2000's: A8BF is U+E7C8, not 2005's U+01F9).
gbrows18030 = [dec(bytes([r, c]), 'gb18030') for r in (0xA2, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9) for c in range(0xA1, 0xFF)]
gbrows18030[5 * 94 + 0xBF - 0xA1] = 0xE7C8
gbd7 = [dec(bytes([0xD7, c]), 'gb18030') for c in range(0xA1, 0xFF)]
gba8 = [dec(bytes([0xA8 + i // 96, lowtrail(i, 96)]), 'gb18030') for i in range(192)]
gbfe = [dec(bytes([0xFE, lowtrail(i, 96)]), 'gb18030') for i in range(96)]
# GB18030's four-byte area as Sony serves it: CJK Extension A only (U+3400-U+4DB5), as ranges (linear, ucs, count).
gbranges = []
for u in range(0x3400, 0x4DB6):
    b = chr(u).encode('gb18030')
    if len(b) != 4: continue
    L = ((((b[0] - 0x81) * 10 + (b[1] - 0x30)) * 126 + (b[2] - 0x81)) * 10 + (b[3] - 0x30))
    if gbranges and gbranges[-1][0] + gbranges[-1][2] == L and gbranges[-1][1] + gbranges[-1][2] == u:
        gbranges[-1][2] += 1
    else:
        gbranges.append([L, u, 1])
# Big5 leads A1-C6, C9-F9 as cp950 reads them, without the ETEN row C6A1-C6FE (Sony leaves it unassigned).
B5_LEADS = list(range(0xA1, 0xC7)) + list(range(0xC9, 0xFA))
big5 = []
for i in range(87 * 157):
    lead = B5_LEADS[i // 157]; c = i % 157
    trail = 0x40 + c if c < 63 else 0xA1 + c - 63
    big5.append(0xFFFF if lead == 0xC6 and trail >= 0xA1 else dec(bytes([lead, trail]), 'cp950'))
# ARIB STD-B24's kanji set rows 1-3: JIS X 0208 as cp932 reads it, but ARIB's non-spacing marks (1-13..1-18 and 2-94)
# read as combining characters.
arib13 = jis0208[:3 * 94][:278]
for k, u in {0x212D: 0x0301, 0x212E: 0x0300, 0x212F: 0x0308, 0x2130: 0x0302, 0x2131: 0x0305, 0x2132: 0x0332, 0x227E: 0x20DD}.items():
    arib13[((k >> 8) - 0x21) * 94 + (k & 0xFF) - 0x21] = u
# ARIB additional symbols (rows 85-86 and 90-94, 658 cells): the Unicode libaribcaption gives (its Unicode 5.2 table,
# compatibility ideographs read as their canonical forms) for the cells Sony's module maps; every other cell a
# private-use code from U+E080 in cell order (E080-E0FF, E180-E1FF, ...: the ARIB gaiji PUA convention). Which cells
# Sony maps, and three cells it reads otherwise (7565 U+6147, 7C76 ®, 7C77 ©), the comparison found.
src = open(os.path.join(os.path.dirname(__file__), 'aribcaption_gaiji.hpp')).read()
uni = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', re.search(r'kAdditionalSymbolsTable_Unicode\[\] = \{(.*?)\};', src, re.S).group(1))]
ARIB_MAPPED = [(2, 8), (10, 12), (15, 30), (33, 37), (39, 44), (47, 51), (55, 65), (68, 82), (84, 86), (88, 91), (95, 98),
               (100, 103), (105, 136), (386, 390), (392, 400), (417, 421), (423, 426), (461, 462), (470, 481), (484, 484),
               (508, 514), (517, 517), (519, 528), (530, 530), (533, 535), (541, 544), (547, 547), (559, 559), (564, 591),
               (628, 653)]
ARIB_OVERRIDE = {0x7565: 0x6147, 0x7C76: 0x00AE, 0x7C77: 0x00A9}
cells = [(85, c) for c in range(94)] + [(86, c) for c in range(94)] + [(r, c) for r in range(90, 95) for c in range(94)]
mapped = set(i for a, b in ARIB_MAPPED for i in range(a, b + 1))
aribadd = []; k = 0
for i, (r, c) in enumerate(cells):
    code = 0x7521 + ((r - 85) << 8) + c
    if i in mapped:
        u = ARIB_OVERRIDE.get(code, uni[(r - 85) * 94 + c])
        n = unicodedata.normalize('NFC', chr(u))
        aribadd.append(ord(n))
    else:
        aribadd.append(0xE080 + ((k >> 7) << 8) + (k & 0x7F)); k += 1
# The single-byte code pages' upper halves.
SBCS = [(5, 'iso8859-1'), (6, 'iso8859-2'), (7, 'iso8859-3'), (8, 'iso8859-4'), (9, 'iso8859-5'), (10, 'iso8859-6'),
        (11, 'iso8859-7'), (12, 'iso8859-8'), (13, 'iso8859-9'), (14, 'iso8859-10'), (15, 'iso8859-11'),
        (16, 'iso8859-13'), (17, 'iso8859-14'), (18, 'iso8859-15'), (19, 'iso8859-16'), (20, 'cp437'), (21, 'cp850'),
        (22, 'cp863'), (23, 'cp866'), (28, 'cp1251'), (29, 'cp1252'), (38, 'cp852'), (39, 'cp1250'), (40, 'cp737'),
        (41, 'cp1253'), (42, 'cp857'), (43, 'cp1254'), (44, 'cp775'), (45, 'cp1257'), (46, 'cp855'), (47, 'cp858'),
        (48, 'cp860'), (49, 'cp861'), (50, 'cp865'), (51, 'cp869')]
sbcs = [dec(bytes([0x80 + i]), cp) for _, cp in SBCS for i in range(128)]
