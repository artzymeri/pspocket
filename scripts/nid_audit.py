#!/usr/bin/env python3
"""Sony's exported functions pspocket lacks, from the firmware's own modules, each sorted by what its code does (D-1062).

    scripts/nid_audit.py [--firmware DIR] [--boot PSPOCKET_BOOT] [--json OUT] [--imports LIB:NID ...]

DIR holds the decrypted firmware modules (*.sprx.elf; default inventory/firmware/analysis/elf). Each module's exports
come from ps3inspect --json; pspocket's own functions from pspocket-boot --hle-list. For every export the registry
lacks, its first instructions decide its kind:

    alias     its entry branches (b) straight to another export's entry or body, or sets registers (li/lis/ori/mr)
              and then does: the same code as that export with some arguments fixed (GTA IV's cellGcmSys 0xb70b4ad3 is
              _cellGcmInitBody with the caller's fifth argument). Implement it as the named one with that argument.
    constant  li r3,K / blr (or lis+ori): returns K and does nothing else. A stub returning K is exact.
    void      blr alone. A stub returning nothing is exact.
    thunk     a few instructions and a branch to an import stub (another library's function): implement it as a call.
    code      anything else: real code to write (its size in instructions is given).

--imports names imported functions (a game's) to look at first; with none, every module is listed. Prints a summary
by library and kind; --json writes every entry. Reads firmware the user installed: its output stays in inventory/.
"""
import argparse
import glob
import json
import os
import struct
import subprocess
import sys
from collections import defaultdict


def registry(boot):
    out = subprocess.run([boot, "--hle-list"], capture_output=True, text=True).stdout
    have = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            try:
                have.add((parts[0], int(parts[1], 16)))
            except ValueError:
                pass
    return have


class Image:
    """A module ELF's LOAD segments, read by virtual address."""

    def __init__(self, path, segments):
        self.data = open(path, "rb").read()
        self.segs = [(int(s["vaddr"], 16), int(s["offset"], 16), int(s["filesz"], 16)) for s in segments
                     if s["type_name"] == "LOAD"]

    def word(self, va):
        for v, off, size in self.segs:
            if v <= va < v + size - 3:
                return struct.unpack(">I", self.data[off + va - v:off + va - v + 4])[0]
        return None


def branch_target(insn, pc):
    """b/bl target (opcode 18) and whether it links; None if not a direct branch."""
    if insn >> 26 != 18:
        return None
    li = insn & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return ((li if insn & 2 else pc + li) & 0xFFFFFFFF, bool(insn & 1))


def is_setup(insn):
    """li/lis/addi/ori/oris/mr: registers set before the shared body."""
    op = insn >> 26
    if op in (14, 15, 24, 25):  # addi(li), addis(lis), ori, oris
        return True
    return op == 31 and (insn >> 1) & 0x3FF == 444  # or (mr)


def lead(img, entry):
    """The register set-up and the plain branch an entry starts with: (set-up words, target) or None."""
    setup = []
    for i in range(8):
        w = img.word(entry + 4 * i)
        if w is None:
            return None
        bt = branch_target(w, entry + 4 * i)
        if bt and not bt[1]:
            return setup, bt[0]
        if not is_setup(w):
            return None
        setup.append(w)
    return None


def setup_text(words):
    """li/lis rD,K as text: what an alias fixes."""
    out = []
    for w in words:
        op, d, a, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 14 and a == 0:
            out.append(f"r{d}={struct.unpack('>h', struct.pack('>H', imm))[0]}")
        elif op == 15 and a == 0:
            out.append(f"r{d}={imm << 16:#x}")
        else:
            out.append(f"{w:#010x}")
    return " ".join(out) or "nothing"


def classify(img, entry, named_leads, stubs):
    """The kind of the code at entry (see the module doc) and a detail. named_leads: target -> [(name, set-up)]."""
    w0 = img.word(entry)
    if w0 is None:
        return "code", "unreadable"
    if w0 == 0x4E800020:
        return "void", ""
    w1 = img.word(entry + 4)
    if w1 == 0x4E800020 and w0 >> 26 == 14 and (w0 >> 21) & 31 == 3 and (w0 >> 16) & 31 == 0:
        return "constant", f"returns {struct.unpack('>h', struct.pack('>H', w0 & 0xFFFF))[0]}"
    w2 = img.word(entry + 8)
    if w2 == 0x4E800020 and w0 >> 26 == 15 and w1 is not None and w1 >> 26 == 24:
        return "constant", f"returns {((w0 & 0xFFFF) << 16 | (w1 & 0xFFFF)):#x}"
    ld = lead(img, entry)
    if ld:
        setup, target = ld
        if target in stubs:
            return "thunk", f"to {stubs[target]}"
        if target in named_leads:
            partners = "; ".join(f"{n} (which sets {setup_text(su)})" for n, su in named_leads[target])
            return "alias", f"sets {setup_text(setup)}, then the body {target:#x} shared with {partners}"
        return "alias", f"sets {setup_text(setup)}, then branches to {target:#x} (no named export shares it)"
    for i in range(1, 8):
        w = img.word(entry + 4 * i)
        if w is None:
            break
        bt = branch_target(w, entry + 4 * i)
        if bt and bt[1] and bt[0] in stubs:
            return "thunk", f"calls {stubs[bt[0]]}"
    n = next((i for i in range(64) if img.word(entry + 4 * i) == 0x4E800020), 63)
    return "code", f"{n + 1}+ instructions"


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--firmware", default="inventory/firmware/analysis/elf")
    ap.add_argument("--boot", default="build/host/runtime/loader/pspocket-boot")
    ap.add_argument("--inspect", default="build/host/tools/ps3inspect/ps3inspect")
    ap.add_argument("--json")
    ap.add_argument("--imports", nargs="*", default=[])
    a = ap.parse_args(argv[1:])
    have = registry(a.boot)
    want = {(x.split(":")[0], int(x.split(":")[1], 16)) for x in a.imports}
    rows = []
    tmp = os.path.join("inventory", "nid-audit")
    os.makedirs(tmp, exist_ok=True)
    for elf in sorted(glob.glob(os.path.join(a.firmware, "**", "*.sprx.elf"), recursive=True)):
        js = os.path.join(tmp, os.path.basename(elf) + ".json")
        if not os.path.exists(js) or os.path.getmtime(js) < os.path.getmtime(elf):
            subprocess.run([a.inspect, "--json", js, elf], capture_output=True)
        try:
            f = json.load(open(js))["files"][0]
        except (OSError, ValueError, KeyError, IndexError):
            continue
        img = Image(elf, f.get("segments", []))
        stubs = {}
        for imp in f.get("imports", []):
            for fn in imp.get("functions", []):
                if fn.get("stub"):
                    stubs[int(fn["stub"], 16)] = f"{imp['module']}::{fn.get('name') or fn['nid']}"
        for ex in f.get("exports", []):
            lib = ex.get("module")
            if not lib or ex.get("management"):
                continue
            named_leads = defaultdict(list) # a shared body's target -> the named exports that branch to it
            for fn in ex.get("functions", []):
                if fn.get("entry") and fn.get("name"):
                    ld = lead(img, int(fn["entry"], 16))
                    if ld:
                        named_leads[ld[1]].append((fn["name"], ld[0]))
            for fn in ex.get("functions", []):
                nid = int(fn["nid"], 16)
                if (lib, nid) in have or not fn.get("entry"):
                    continue
                if want and (lib, nid) not in want:
                    continue
                kind, detail = classify(img, int(fn["entry"], 16), named_leads, stubs)
                rows.append({"library": lib, "nid": f"{nid:#010x}", "name": fn.get("name") or "",
                             "module": os.path.basename(elf), "kind": kind, "detail": detail})
    by = defaultdict(lambda: defaultdict(int))
    for r in rows:
        by[r["library"]][r["kind"]] += 1
    kinds = ["alias", "constant", "void", "thunk", "code"]
    print(f"{len(rows)} exported functions pspocket lacks, in {len(by)} libraries")
    print(f"{'library':28}" + "".join(f"{k:>10}" for k in kinds))
    for lib in sorted(by, key=lambda l: -sum(by[l].values())):
        print(f"{lib:28}" + "".join(f"{by[lib][k]:>10}" for k in kinds))
    tot = defaultdict(int)
    for r in rows:
        tot[r["kind"]] += 1
    print(f"{'total':28}" + "".join(f"{tot[k]:>10}" for k in kinds))
    if want:
        for r in rows:
            print(f"  {r['library']} {r['nid']} {r['name'] or '?'}: {r['kind']} ({r['detail']}) in {r['module']}")
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
