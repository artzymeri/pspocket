#!/usr/bin/env python3
"""The open-world bench (D-1060, D-1063): each case of games/<game>/bench/cases.json run on a fresh copy of the game's
hdd0 (from a save, or fresh for a first install), as the phone runs it, then checked by scripts/bench_check.py.

    scripts/bench.py [CASE ...] [--game gta5|ac1|all] [--mode M|all] [--accept] [--keep-frames N] [--out DIR]
                     [--nightly]

cases.json:
    "data": the inventory/ folder of the game's data (decrypt/EBOOT.elf, disc.iso, hdd0, shaders, saves/)
    "flash": the dev_flash to mount (default inventory/firmware/dev_flash)
    "library": {"spu_images": "spu-images"} to run the game from a library pspocket-prepare makes (incrementally) in
               inventory/<data>/bench-library, as the phone does; absent: the build's own translation (GTA V)
    "modes": {name: {ENV: value}}
    "cases": [{"name", "timeout", "from_s", "route" (a scripts/route.py file) or "press" (a press file),
               "save" (inventory/<data>/saves/<name>, savedata dirs) or "fresh": [hdd0 paths to empty first]}]
Outputs (game data, gitignored): inventory/bench/<stamp>/<game>-<case>-<mode>/ and index.html. --nightly refuses a
busy Mac (another pspocket-boot or ninja, load average over 4, under 15 GB free) and keeps it awake.
"""
import argparse
import datetime
import glob
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT = "build/host/runtime/loader/pspocket-boot"
PREPARE = "build/host/runtime/prepare/pspocket-prepare"


def sh(cmd, **kw):
    return subprocess.run(cmd, **kw)


def busy():
    for p in ("pspocket-boot", "ninja"):
        if sh(["pgrep", "-x", p], capture_output=True).returncode == 0:
            return f"{p} is running"
    load = float(sh(["sysctl", "-n", "vm.loadavg"], capture_output=True, text=True).stdout.split()[2])
    if load > 4:
        return f"load average {load}"
    free = shutil.disk_usage(ROOT).free / 1e9
    if free < 15:
        return f"{free:.0f} GB free"
    return None


def library(game, spec):
    """The game's library from pspocket-prepare (incremental: unchanged files are kept), or None."""
    lib = spec.get("library")
    if not lib:
        return None
    data = os.path.join("inventory", spec["data"])
    out = os.path.join(data, "bench-library")
    log = os.path.join(data, "bench-library.log")
    cmd = [PREPARE, "--title", spec["title"], "--exe", os.path.join(data, "decrypt", "EBOOT.elf"), "--game-dir", out,
           "--spu-images", os.path.join(data, lib["spu_images"]), "-j", "4", "--quiet"]
    print(f"==> {game}: preparing its library ({out})", flush=True)
    with open(log, "w") as f:
        r = sh(["perl", "-e", "alarm 3600; exec @ARGV"] + cmd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        sys.exit(f"bench: pspocket-prepare failed for {game} ({log})")
    return os.path.join(out, "game.dylib")


def run_case(game, spec, case, mode, out, lib, a):
    data = os.path.join("inventory", spec["data"])
    run = os.path.join(out, f"{game}-{case['name']}-{mode}")
    os.makedirs(os.path.join(run, "frames"), exist_ok=True)
    hdd0, shaders = os.path.join(run, "hdd0"), os.path.join(run, "shaders")
    sh(["cp", "-c", "-R", os.path.join(data, "hdd0"), hdd0], check=True)
    for rel in case.get("fresh", []): # a first install: what it would have written, gone
        for p in glob.glob(os.path.join(hdd0, rel)):
            shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else os.remove(p)
    if case.get("save"):
        dst = os.path.join(hdd0, "home", "00000001", "savedata")
        os.makedirs(dst, exist_ok=True)
        for d in glob.glob(os.path.join(data, "saves", case["save"], "*")):
            shutil.copytree(d, os.path.join(dst, os.path.basename(d)), dirs_exist_ok=True)
    if os.path.isdir(os.path.join(data, "shaders")):
        sh(["cp", "-c", "-R", os.path.join(data, "shaders"), shaders], check=True)
    else:
        os.makedirs(shaders)
    press = os.path.join(run, "press.txt")
    if case.get("route"):
        with open(press, "w") as f:
            sh([sys.executable, "scripts/route.py", os.path.join("games", game, "bench", case["route"])], stdout=f,
               check=True)
    else:
        shutil.copy(os.path.join("games", game, case["press"]), press)
    env = dict(os.environ, **spec.get("modes", {}).get(mode, {}))
    cmd = [BOOT, os.path.join(data, "decrypt", "EBOOT.elf"), "--gpu", "metal", "--timeout", str(case["timeout"]),
           "--hdd0", hdd0, "--bdvd", os.path.join(data, "disc.iso"),
           "--flash", spec.get("flash", "inventory/firmware/dev_flash"), "--shader-cache", shaders,
           "--async-shaders", "4", "--spu-translate", os.path.join("inventory", "bench", "spu-translate", game),
           "--press-file", press, "--frames", os.path.join(run, "frames"), "--frame-every", "60", "--frame-jumps",
           "--summary", os.path.join(run, "summary.json")]
    if lib:
        cmd += ["--game-library", lib]
    os.makedirs(os.path.join("inventory", "bench", "spu-translate", game), exist_ok=True)
    print(f"==> {game} {case['name']} ({mode}): {case['timeout']} s", flush=True)
    with open(os.path.join(run, "run.log"), "w") as f:
        sh(["perl", "-e", f"alarm {case['timeout'] + 600}; exec @ARGV"] + cmd, env=env, stdout=f,
           stderr=subprocess.STDOUT)
    for p in (hdd0, shaders, os.path.join(run, "hdd1")):
        shutil.rmtree(p, ignore_errors=True)
    check = [sys.executable, "scripts/bench_check.py", run, "--case", f"{game}/{case['name']}", "--mode", mode,
             "--from-s", str(case.get("from_s", 0)), "--keep", str(a.keep_frames)] + (["--accept"] if a.accept else [])
    r = sh(check, capture_output=True, text=True)
    print(r.stdout.strip(), flush=True)
    with open(os.path.join(out, "results.txt"), "a") as f:
        f.write(r.stdout)
    return r.returncode == 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cases", nargs="*")
    ap.add_argument("--game", default="gta5")
    ap.add_argument("--mode", default=None)
    ap.add_argument("--accept", action="store_true")
    ap.add_argument("--keep-frames", type=int, default=6)
    ap.add_argument("--out")
    ap.add_argument("--nightly", action="store_true")
    a = ap.parse_args(argv[1:])
    os.chdir(ROOT)
    if a.nightly:
        why = busy()
        if why:
            print(f"bench: not tonight: {why}")
            return 3
        subprocess.Popen(["caffeinate", "-dimsu", "-w", str(os.getpid())]) # awake while the bench runs
    games = [os.path.basename(os.path.dirname(os.path.dirname(p))) for p in sorted(glob.glob("games/*/bench/cases.json"))] \
        if a.game == "all" else [a.game]
    head = sh(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    out = a.out or os.path.join("inventory", "bench", datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + head)
    os.makedirs(out, exist_ok=True)
    ok = True
    for game in games:
        spec = json.load(open(os.path.join("games", game, "bench", "cases.json")))
        data = os.path.join("inventory", spec["data"])
        if not os.path.exists(os.path.join(data, "disc.iso")):
            print(f"bench: {game}: no {data}/disc.iso, skipped")
            continue
        lib = library(game, spec)
        modes = list(spec.get("modes", {"default": {}})) if a.mode == "all" else \
            [a.mode or next(iter(spec.get("modes", {"default": {}})))]
        for case in spec["cases"]:
            if a.cases and case["name"] not in a.cases:
                continue
            for mode in modes:
                ok &= run_case(game, spec, case, mode, out, lib, a)
    sh([sys.executable, "scripts/bench_report.py", out], capture_output=True)
    print(f"bench: {out}/index.html")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
