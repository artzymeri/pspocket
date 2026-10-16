#!/usr/bin/env python3
"""A new game's first look (D-1068): what it imports that nothing answers, each sorted by its firmware code, and a
first boot's verdict, in one report.

    scripts/bringup.py GAME.iso | inventory/<TITLE>/decrypt/EBOOT.elf [--seconds S] [--flash DIR] [--prepare]
                       [--no-boot]

1. The title ID from the disc image (pspocket-boot --probe-disc) names inventory/<TITLE>/; the disc's EBOOT.BIN is
   decrypted there once (ps3disc --decrypt, D-121).
2. Its imports (pspocket-boot --imports): the ones nothing answers (D-1061), then scripts/nid_audit.py on just those:
   alias (its named function with an argument fixed), constant, void, thunk or code (D-1062).
3. A first boot under the build's runtime (pspocket-boot --summary, frames every 300th flip, --seconds, 120 by
   default) on a fresh hdd0 copy, from the game's library in inventory/<TITLE>/bench-library when there is one
   (--prepare makes or updates it with pspocket-prepare first, as the phone does; without one the PPU code is
   interpreted, slowly), with the SPU programs translated during the session: how it ended, fps, RSX failures, unimplemented calls, interpreted code, the most
   frequent warning shapes.
The report is inventory/<TITLE>/bringup.md (game-derived: stays in inventory/). Exit 0 when nothing is missing and the
boot reached its timeout without failures.
"""
import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = "build/host"
BOOT = f"{BUILD}/runtime/loader/pspocket-boot"
DISC = f"{BUILD}/runtime/disc/ps3disc"


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def default_flash():
    for d in ("inventory/firmware/installed/dev_flash", "inventory/firmware/dev_flash"):
        if os.path.isdir(d):
            return d
    return None


def missing_imports(elf, flash):
    """(library, name, nid) for every import nothing answers: no host function, RPCS3 stub or firmware module's export
    (the loader's own list, D-1061), and --imports' total line."""
    cmd = [BOOT, elf, "--imports"] + (["--flash", flash] if flash else [])
    r = run(cmd)
    rows, total = [], ""
    for line in (r.stdout + r.stderr).splitlines():
        m = re.search(r"nothing answers the imported function (\S+?)::(\S+) \((0x[0-9a-fA-F]{8})\)", line)
        if m:
            rows.append((m.group(1), m.group(2), int(m.group(3), 16)))
        if line.startswith("total:"):
            total = line[len("total: "):]
    return rows, total


def audit(rows):
    """nid_audit.py's kind and detail for each missing import (by library and NID)."""
    if not rows:
        return {}
    out = os.path.join("inventory", "nid-audit", "bringup.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    run([sys.executable, "scripts/nid_audit.py", "--json", out, "--imports"] + [f"{l}:{n:#x}" for l, _, n in rows])
    try:
        return {(r["library"], int(r["nid"], 16)): r for r in json.load(open(out))}
    except (OSError, ValueError):
        return {}


def shapes(log_path, n=8):
    """The most frequent warning and error lines, numbers and addresses folded."""
    c = collections.Counter()
    try:
        for line in open(log_path, errors="replace"):
            if line.startswith(("[warn]", "[error]", "[fatal]")):
                c[re.sub(r"0x[0-9a-fA-F]+|\d+", "N", line.strip())[:160]] += 1
    except OSError:
        pass
    return c.most_common(n)


def prepare(elf, game, title):
    """The game's library from pspocket-prepare (incremental), as the bench makes it (bench.py)."""
    out = os.path.join(game, "bench-library")
    cmd = [f"{BUILD}/runtime/prepare/pspocket-prepare", "--title", title, "--exe", elf, "--game-dir", out, "-j", "4",
           "--quiet"]
    spu = os.path.join(game, "spu-images")
    if os.path.isdir(spu):
        cmd += ["--spu-images", spu]
    print(f"bringup: preparing the game's library ({out})", flush=True)
    with open(os.path.join(game, "bench-library.log"), "w") as f:
        r = subprocess.run(["perl", "-e", "alarm 3600; exec @ARGV"] + cmd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        sys.exit(f"bringup: pspocket-prepare failed ({game}/bench-library.log)")


def boot(elf, game, iso, flash, seconds):
    run_dir = os.path.join(game, "bringup")
    shutil.rmtree(run_dir, ignore_errors=True)
    os.makedirs(os.path.join(run_dir, "frames"))
    os.makedirs(os.path.join(game, "spu-translate"), exist_ok=True)
    hdd0 = os.path.join(run_dir, "hdd0")
    if os.path.isdir(os.path.join(game, "hdd0")):
        run(["cp", "-c", "-R", os.path.join(game, "hdd0"), hdd0])
    else:
        os.makedirs(hdd0)
    cmd = [BOOT, elf, "--gpu", "metal", "--timeout", str(seconds), "--hdd0", hdd0, "--async-shaders", "4",
           "--shader-cache", os.path.join(run_dir, "shaders"), "--frames", os.path.join(run_dir, "frames"),
           "--frame-every", "300", "--summary", os.path.join(run_dir, "summary.json"),
           "--spu-translate", os.path.join(game, "spu-translate")]
    lib = os.path.join(game, "bench-library", "game.dylib")
    if os.path.exists(lib):
        cmd += ["--game-library", lib]
    if iso:
        cmd += ["--bdvd", iso]
    if flash:
        cmd += ["--flash", flash]
    log = os.path.join(run_dir, "run.log")
    print(f"bringup: booting for {seconds} s ({log})", flush=True)
    with open(log, "w") as f:
        subprocess.run(["perl", "-e", f"alarm {seconds + 300}; exec @ARGV"] + cmd, stdout=f, stderr=subprocess.STDOUT)
    for p in (hdd0, os.path.join(run_dir, "shaders"), os.path.join(run_dir, "hdd1")):
        shutil.rmtree(p, ignore_errors=True)
    try:
        summary = json.load(open(os.path.join(run_dir, "summary.json")))
    except (OSError, ValueError):
        summary = {"exit": "no summary (the process died before writing it)"}
    return summary, log


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--flash", default=None)
    ap.add_argument("--prepare", action="store_true")
    ap.add_argument("--no-boot", action="store_true")
    a = ap.parse_args(argv[1:])
    os.chdir(ROOT)
    run(["ninja", "-C", BUILD, "pspocket-boot", "ps3disc"])
    flash = a.flash or default_flash()
    iso = None
    if a.input.lower().endswith(".iso"):
        iso = os.path.abspath(a.input)
        probe = run([BOOT, "--probe-disc", iso]).stdout.strip().splitlines()
        title = probe[-1].split()[0] if probe else ""
        if not title:
            sys.exit(f"bringup: {iso} is not a readable PS3 game disc")
        game = os.path.join("inventory", title)
        elf = os.path.join(game, "decrypt", "EBOOT.elf")
        if not os.path.exists(elf):
            os.makedirs(os.path.dirname(elf), exist_ok=True)
            if run([DISC, "--decrypt", iso, elf]).returncode != 0:
                sys.exit(f"bringup: could not decrypt {iso}'s EBOOT.BIN")
    else:
        elf = a.input
        game = os.path.dirname(os.path.dirname(os.path.abspath(elf)))
        title = os.path.basename(game)
        if os.path.exists(os.path.join(game, "disc.iso")):
            iso = os.path.join(game, "disc.iso")

    rows, total = missing_imports(elf, flash)
    kinds = audit(rows)
    md = [f"# {title}: bring-up", "", f"- Executable: {elf}", f"- Imports: {total or '?'}",
          f"- Imports nothing answers: {len(rows)}", ""]
    if rows:
        md += ["| Library | Function | NID | Firmware code | Detail |", "|---|---|---|---|---|"]
        for lib, name, nid in rows:
            k = kinds.get((lib, nid), {})
            md.append(f"| {lib} | {name} | {nid:#010x} | {k.get('kind', 'not in the firmware')} | "
                      f"{k.get('detail', '')} |")
        md.append("")
    ok = not rows
    if a.prepare:
        run(["ninja", "-C", BUILD, "pspocket-prepare"])
        prepare(elf, game, title)
    if not a.no_boot:
        summary, log = boot(elf, game, iso, flash, a.seconds)
        fps = summary.get("fps") or []
        md += ["## First boot", "", f"- Ended: {summary.get('exit')} after {summary.get('seconds', '?')} s, "
               f"{summary.get('flips', 0)} flips", f"- fps (5 s windows): {', '.join(f'{x:.0f}' for x in fps[-12:])}",
               f"- RSX failures: {summary.get('rsx_failures', '?')}",
               f"- Unimplemented calls hit: {summary.get('unimplemented_syscalls', '?')} system calls, "
               f"{summary.get('unimplemented_functions', '?')} functions",
               f"- Library: {'the game library' if os.path.exists(os.path.join(game, 'bench-library', 'game.dylib')) else 'none: PPU code interpreted (--prepare)'}",
               f"- SPU programs: {summary.get('spu_loads', {})}", "", "Most frequent warnings:", ""]
        md += [f"- {n}× `{s}`" for s, n in shapes(log)] or ["- none"]
        md.append("")
        ok = ok and summary.get("exit") == "timeout" and not summary.get("rsx_failures") \
            and not summary.get("unimplemented_syscalls") and not summary.get("unimplemented_functions")
    out = os.path.join(game, "bringup.md")
    open(out, "w").write("\n".join(md))
    print("\n".join(md))
    print(f"bringup: {out}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
