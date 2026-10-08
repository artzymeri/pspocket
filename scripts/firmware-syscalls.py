#!/usr/bin/env python3
# firmware-syscalls.py DIR: the lv2 syscalls the PS3 firmware's own modules issue (an `sc` with `li r11, N` before it),
# against pspocket's handlers (PS_LV2_BIND, syscall_null.inc, syscall_stub.inc). DIR holds the decrypted modules in
# external/ and internal/ (inventory/firmware/analysis/elf: derived from the user's firmware, never committed). Since
# D-157 the firmware's modules run as the game's code, so a syscall one of them issues that pspocket lacks is a gap
# for whatever game takes that path (CLAUDE.md rule 4: the firmware is inventory). Modules pspocket serves itself
# (recomp::firmware_module_native) never run: their calls are listed apart.
import os, re, struct, sys, collections
root = sys.argv[1]  # inventory/firmware/analysis/elf
names = {}
for line in open('recomp/common/data/syscalls.txt'):
    m = re.match(r'\s*(\d+)\s+(\S+)', line)
    if m: names[int(m.group(1))] = m.group(2)
own = set()
for path in os.listdir('runtime/lv2/src'):
    if path.endswith('.cpp'):
        for m in re.finditer(r'PS_LV2_BIND\(table, (\d+),', open('runtime/lv2/src/' + path).read()): own.add(int(m.group(1)))
answered = set(int(n) for n in re.findall(r'^\{(\d+),', open('runtime/lv2/src/syscall_null.inc').read(), re.M))
answered |= set(int(n) for n in re.findall(r'^\{(\d+),', open('runtime/lv2/src/syscall_stub.inc').read(), re.M))
used = collections.defaultdict(set)
unknown_sc = 0
for sub in ('external', 'internal'):
    for f in sorted(os.listdir(os.path.join(root, sub))):
        b = open(os.path.join(root, sub, f), 'rb').read()
        if b[:4] != b'\x7fELF': continue
        phoff = struct.unpack('>Q', b[0x20:0x28])[0]; phnum = struct.unpack('>H', b[0x38:0x3a])[0]
        for i in range(phnum):
            ph = b[phoff + i*56: phoff + (i+1)*56]
            ptype, pflags = struct.unpack('>II', ph[:8]); off = struct.unpack('>Q', ph[8:16])[0]; filesz = struct.unpack('>Q', ph[32:40])[0]
            if ptype != 1 or not (pflags & 1): continue
            code = b[off:off+filesz]
            words = struct.unpack('>%dI' % (len(code)//4), code[:len(code)//4*4])
            for j, w in enumerate(words):
                if w != 0x44000002: continue
                n = None
                for k in range(j-1, max(-1, j-12), -1):
                    v = words[k]
                    if (v & 0xFFFF0000) == 0x39600000: n = v & 0xFFFF; break   # li r11,N
                    if (v >> 26) == 18 or v == 0x4e800020: break                 # a branch before it: give up
                if n is None: unknown_sc += 1
                else: used[n].add(sub + '/' + f.replace('.sprx.elf','').replace('.elf',''))
NATIVE_PREFIX = ('liblv2', 'libsysutil', 'libfs_utility')
NATIVE = {'libsysmodule', 'libsre', 'libgcm_sys', 'libaudio', 'libvoice', 'libio', 'libfs', 'libfs_155', 'libnet', 'libnetctl',
          'libcamera', 'libmic', 'libgem', 'libusbd', 'libusbpspcm', 'sys_audio', 'sysvoice_modules', 'libvoice_internal'}
def runs(m):  # recomp::firmware_module_translated: .sprx modules pspocket does not serve itself
    sub, name = m.split('/', 1)
    if name.endswith('.self'): return False
    return not name.startswith(NATIVE_PREFIX) and name not in NATIVE
for n in list(used): used[n] = {m for m in used[n] if runs(m)} or {'(only modules pspocket serves itself)'}
missing = sorted(n for n in used if n not in own and n not in answered and '(only modules pspocket serves itself)' not in used[n])
print('syscalls the firmware modules issue: %d (own %d, answered as RPCS3 %d); missing from modules that run translated: %d; sc without a found number: %d' % (
    len(used), sum(1 for n in used if n in own), sum(1 for n in used if n in answered and n not in own), len(missing), unknown_sc))
for n in missing:
    print('  %4d %-45s %s' % (n, names.get(n, '?'), ', '.join(sorted(used[n]))[:150]))
