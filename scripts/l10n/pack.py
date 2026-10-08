# Packs a u16 table into bytes. The decoder keeps `last` (the last value that was not FFFF, starting at 0):
#   00-3F  n+1 values last+1, last+2, ...                 (runs)
#   40-5F  n+1 FFFF values                               (gaps)
#   60-BF  one value last+1+(t-0x90)  (-48..+47)         (small steps)
#   C0-DF  one value last+1+((t&0x1F)<<8|b)-0x1000       (steps of -4096..+4095)
#   E0     one value b<<8|b                              (any value)
def pack(t):
    out = bytearray(); last = 0; i = 0; n = len(t)
    while i < n:
        v = t[i]
        if v == 0xFFFF:
            k = 0
            while i + k < n and t[i + k] == 0xFFFF and k < 32: k += 1
            out.append(0x40 + k - 1); i += k; continue
        if v == (last + 1) & 0xFFFF:
            k = 0; x = last
            while i + k < n and k < 64 and t[i + k] == (x + 1) & 0xFFFF: x = t[i + k]; k += 1
            out.append(k - 1); last = x; i += k; continue
        d = ((v - last - 1 + 0x8000) & 0xFFFF) - 0x8000
        if -48 <= d <= 47: out.append(0x90 + d)
        elif -4096 <= d <= 4095: d += 0x1000; out += bytes([0xC0 | (d >> 8), d & 0xFF])
        else: out += bytes([0xE0, v >> 8, v & 0xFF])
        last = v; i += 1
    return bytes(out)
def unpack(b, n):
    t = []; last = 0; i = 0
    while len(t) < n:
        c = b[i]; i += 1
        if c < 0x40:
            for _ in range(c + 1): last = (last + 1) & 0xFFFF; t.append(last)
        elif c < 0x60: t += [0xFFFF] * (c - 0x3F)
        elif c < 0xC0: last = (last + 1 + c - 0x90) & 0xFFFF; t.append(last)
        elif c < 0xE0: last = (last + 1 + (((c & 0x1F) << 8) | b[i]) - 0x1000) & 0xFFFF; i += 1; t.append(last)
        else: last = (b[i] << 8) | b[i + 1]; i += 2; t.append(last)
    return t
