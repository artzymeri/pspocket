#!/usr/bin/env python3
"""What the PS3 system software holds and how each part meets pspocket's iOS-native engine (D-176).

Reads the user's installed firmware (a /dev_flash from PS3UPDAT.PUP, D-156) and writes a report under
inventory/firmware/analysis/ (gitignored: it is derived from Sony's files, rule 1). Every module of sys/external and
sys/internal is decrypted (pspocket-firmware --decrypt) and inspected (ps3inspect --json); the host's own functions
come from pspocket-boot --hle-list; RPCS3's default for each module (its g_prx_list: HLE or the module itself) from an
RPCS3 checkout when given.

    python3 scripts/firmware-report.py [--flash inventory/firmware/installed/dev_flash] [--build build/host]
                                       [--rpcs3 RPCS3_DIR] [--out inventory/firmware/analysis]

Output: report.md (per family and per module: purpose, how it meets iOS, the host's coverage, SPU programs, what each
module needs) and report.json. docs/firmware.md is the committed summary, written from it by hand.
"""

import argparse
import collections
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

# How each part of the system software meets the engine. The plan per family, in plain words:
#   ours       pspocket's own implementation on iOS's frameworks, always (the module never runs as it is, D-175)
#   ios        to be replaced by an iOS framework that does the same job in hardware or tuned code
#   translate  Sony's code itself, translated once to ARM64 like a game's (no iOS equivalent, or exact behaviour needed)
#   data       files the engine (or Sony's translated code) reads as they are; kept on the phone
#   none       of no use to games on an iPhone
FAMILIES = [
    # (family, plan, iOS side, purpose, module file patterns)
    ("Process and kernel services", "ours", "pspocket's lv2 and host modules",
     "threads, memory, modules, timers, printf: the process runtime every game links",
     [r"liblv2\.sprx", r"libsysmodule\.sprx"]),
    ("SPURS task system", "ours", "pspocket's SPURS scheduler on host threads (D-037)",
     "runs jobs and tasks on the SPUs; also cellSync, cellSheap, cellDaisy, cellOvis",
     [r"libsre\.sprx"]),
    ("Graphics", "ours", "Metal (RSX commands translated to Metal)",
     "the RSX driver library: command buffers, flips, tiles, IO mapping",
     [r"libgcm_sys\.sprx"]),
    ("Sound output", "ours", "iOS audio (AudioUnit)",
     "the audio ports games mix into, and the system audio service",
     [r"libaudio\.sprx", r"sys_audio\.s(prx|elf)"]),
    ("Voice chat and microphone", "ours", "AVAudioSession input (later)",
     "voice chat and the microphone", [r"libvoice.*\.sprx", r"sysvoice_modules\.sprx", r"libmic\.sprx"]),
    ("Controllers", "ours", "GameController framework and touch",
     "pads, keyboard, mouse", [r"libio\.sprx"]),
    ("Files", "ours", "the iPhone's storage (disc image read in place, save folders)",
     "file system calls, AIO, streaming", [r"libfs(_155)?\.sprx", r"libfs_utility.*\.sprx"]),
    ("Network", "ours", "BSD sockets and the Network framework",
     "sockets and connection state", [r"libnet\.sprx", r"libnetctl\.sprx"]),
    ("System menus, saves, game data, PSN", "ours", "UIKit dialogs, files, Game Center later",
     "dialogs, save data, game data, trophies, PSN, music/photo/video utilities",
     [r"libsysutil.*\.sprx"]),
    ("Clock", "ours", "the host clock", "real-time clock and dates", [r"librtc\.sprx"]),
    ("PS3 accessories", "ours", "AVFoundation camera later; the rest has no iPhone counterpart",
     "EyeToy camera, Move, USB devices, PSP link", [r"libcamera\.sprx", r"libgem\.sprx", r"libusbd\.sprx",
                                                     r"libusbpspcm\.sprx"]),
    ("Image decoders", "ios", "ImageIO (hardware JPEG, PNG, GIF, TIFF decode)",
     "PNG, JPEG, GIF, TIFF, EXIF: game UI images and photos",
     [r"libpngdec\.sprx", r"libjpgdec\.sprx", r"libgifdec\.sprx", r"libtiffdec\.sprx", r"libexif\.sprx",
      r"libavchatjpgdec\.sprx"]),
    ("Image encoders", "ios", "ImageIO encode", "screenshots and photo export", [r"libpngenc\.sprx", r"libjpgenc\.sprx"]),
    ("Video decoder front end", "translate", "VideoToolbox behind it for H.264",
     "cellVdec: feeds compressed video to a decoder core and returns pictures", [r"libvdec\.sprx"]),
    ("Video decoder cores", "ios", "VideoToolbox for H.264/AVC; the other formats translated",
     "H.264/AVC, MVC (3D), MPEG-2, MPEG-4, VC-1, DivX decoders running on the SPUs",
     [r"libavcdec\.sprx", r"libmvcdec\.sprx", r"libsjvtd\.sprx", r"libsmvd2\.sprx", r"libsmvd4\.sprx",
      r"libsvc1d\.sprx", r"libdivx.*\.sprx"]),
    ("Audio decoder front end", "translate", "AudioToolbox behind it for AAC, MP3, AC-3",
     "cellAdec: feeds compressed audio to a decoder core and returns PCM", [r"libadec.*\.sprx"]),
    ("Audio decoder cores", "translate", "AudioToolbox for AAC/MP3/AC-3; ATRAC, WMA, DTS, TrueHD translated",
     "ATRAC3, ATRAC3plus/ATRAC X, MP3, AAC, AC-3, Dolby Digital Plus, DTS, TrueHD, WMA, MPEG audio, CELP decoders",
     [r"libat3.*dec\.sprx", r"libatxdec.*\.sprx", r"libmp3s?dec\.sprx", r"libm[24].*aacdec.*\.sprx",
      r"libac3dec.*\.sprx", r"libddpdec\.sprx", r"libdts.*dec\.sprx", r"libtrhddec\.sprx", r"libwmadec\.sprx",
      r"libmpl1dec\.sprx", r"libm2bcdec\.sprx", r"libcelp8?dec\.sprx"]),
    ("ATRAC players", "translate", "-", "cellAtrac, cellAtracMulti: ATRAC streams for game music and voices",
     [r"libatrac3plus\.sprx", r"libatrac3multi\.sprx"]),
    ("Movie containers and players", "translate", "-",
     "PAMF movies (cellPamf, cellDmux), the Sail player, MP4 and ASF parsers: cutscenes and intro videos",
     [r"libpamf\.sprx", r"libdmux.*\.sprx", r"libsail.*\.sprx", r"libmp4\.sprx", r"libasfparser.*\.sprx",
      r"libbeisobmf\.sprx", r"libbemp2sys\.sprx"]),
    ("Video and audio post-processing", "ios", "Metal compute or vImage; AudioConverter for resampling",
     "colour conversion and scaling of decoded video; audio resampling and effects",
     [r"libvpost.*\.sprx", r"libapostsrc.*\.sprx", r"libdsee\.sprx", r"libchnnlsv\.sprx"]),
    ("Encoders", "translate", "VideoToolbox/AudioToolbox encode if a game needs speed",
     "AVC, MPEG-4, AAC, MP3, ATRAC, DTS, Dolby, CELP encoders: video recording and voice chat",
     [r"libavcenc.*\.sprx", r"libm4venc\.sprx", r"libm4hdenc\.sprx", r"libaacenc.*\.sprx", r"libmp3enc\.sprx",
      r"libat3enc.*\.sprx", r"libatxenc\.sprx", r"libddlenc2\.sprx", r"libdtsenc2\.sprx", r"libcelp8?enc\.sprx"]),
    ("Fonts", "translate", "the system fonts read from the firmware (data/font)",
     "cellFont and FreeType: game text drawn from the PS3's own fonts, with the PS3's exact metrics",
     [r"libfont.*\.sprx", r"libfreetype.*\.sprx", r"libft2d\.sprx"]),
    ("Text and keyboard", "translate", "-", "character set conversion (cellL10n), key codes to characters",
     [r"libl10n\.sprx", r"libkey2char\.sprx"]),
    ("Threads and sync helpers", "translate", "-",
     "cellSync2, fibers, the SPURS job queue: built on lv2 and SPURS, which are ours",
     [r"libsync2\.sprx", r"libfiber\.sprx", r"libspurs_jq\.sprx"]),
    ("Resolution scaler", "translate", "-", "cellResc: scales a game's frames to the display through the RSX",
     [r"libresc\.sprx"]),
    ("Web, TLS and reliable UDP", "translate", "URLSession and Security's TLS if needed",
     "HTTP, SSL with the firmware's root certificates (data/cert), reliable UDP for online play",
     [r"libhttp\.sprx", r"libssl\.sprx", r"librudp\.sprx"]),
    ("In-game advertising and media", "translate", "-", "in-game ads (libad_*), media helpers",
     [r"libad_.*\.sprx", r"libmedi\.sprx", r"libmcadpt\.sprx"]),
    ("Debug and system-only", "none", "-", "crash dumps, debugger support, the boot-time OSD",
     [r"liblv2coredump\.sprx", r"liblv2dbg_for_cex\.sprx", r"sys_init_osd\.self"]),
]

DATA = [
    ("data/font", "data", "The PS3's system fonts (cellFont's system font sets). Kept on the phone."),
    ("data/cert", "data", "Root certificates for cellSsl and HTTPS. Kept on the phone."),
    ("data/dic", "none", "On-screen keyboard dictionaries: the iPhone's own keyboard replaces the PS3's."),
    ("vsh", "none", "The XMB menu and its resources: pspocket has its own app shell."),
    ("bdplayer", "none", "The Blu-ray movie player."),
    ("ps1emu", "none", "The PS1 emulator."),
    ("ps2emu", "none", "The PS2 emulator."),
    ("pspemu", "none", "The PSP emulator (minis)."),
]

PLAN_TEXT = {
    "ours": "ours on iOS",
    "ios": "iOS framework",
    "translate": "Sony's code, translated once",
    "data": "data, kept",
    "none": "not used",
}


def family_of(file: str):
    for fam in FAMILIES:
        if any(re.fullmatch(p, file) for p in fam[4]):
            return fam
    return None


def run(cmd):
    print("+", " ".join(str(c) for c in cmd), file=sys.stderr)
    subprocess.run([str(c) for c in cmd], check=True)


def rpcs3_prx_list(rpcs3: pathlib.Path):
    src = (rpcs3 / "rpcs3" / "Emu" / "Cell" / "lv2" / "sys_prx.cpp").read_text(errors="replace")
    body = src[src.index("g_prx_list"):]
    body = body[: body.index("};")]
    return {m.group(1): int(m.group(2)) for m in re.finditer(r'\{\s*"([^"]+)",\s*(\d)\s*\}', body)}


def dir_stats(path: pathlib.Path):
    files = [p for p in path.rglob("*") if p.is_file()]
    return len(files), sum(p.stat().st_size for p in files)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--flash", type=pathlib.Path, default=ROOT / "inventory/firmware/installed/dev_flash")
    ap.add_argument("--build", type=pathlib.Path, default=ROOT / "build/host")
    ap.add_argument("--rpcs3", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path, default=ROOT / "inventory/firmware/analysis")
    args = ap.parse_args()
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    if out.resolve().is_relative_to(ROOT) and not out.resolve().is_relative_to(ROOT / "inventory"):
        print("firmware-report: --out must be under inventory/ (rule 1)", file=sys.stderr)
        return 2

    run([args.build / "recomp/common/pspocket-firmware", "--decrypt", args.flash, out / "elf"])
    elfs = sorted((out / "elf").glob("*/*.elf"))
    run([args.build / "tools/ps3inspect/ps3inspect", "--quiet", "--json", out / "modules.json",
         "--spu-extract", out / "spu", *elfs])
    hle = subprocess.run([str(args.build / "runtime/loader/pspocket-boot"), "--hle-list"], check=True,
                         capture_output=True, text=True).stdout
    host = collections.defaultdict(set)  # library -> NIDs the host implements
    for line in hle.splitlines():
        lib, nid, _ = line.split(" ", 2)
        host[lib].add(int(nid, 16))
    prx_list = rpcs3_prx_list(args.rpcs3) if args.rpcs3 else {}

    inspected = json.loads((out / "modules.json").read_text())["files"]
    modules = []
    exporter = {}  # library -> module file
    for f in inspected:
        path = pathlib.Path(f["path"])
        file = path.name.removesuffix(".elf")
        where = path.parent.name
        libs = []
        for e in f.get("exports") or []:
            if not e.get("module") or e.get("management"):
                continue
            nids = {int(x["nid"], 16) if isinstance(x["nid"], str) else x["nid"] for x in e["functions"]}
            libs.append({"library": e["module"], "functions": len(e["functions"]), "variables": len(e["variables"]),
                         "host": len(nids & host.get(e["module"], set()))})
            exporter.setdefault(e["module"], file)
        imports = {}
        for i in f.get("imports") or []:
            imports[i["module"]] = {"functions": len(i["functions"]), "variables": len(i["variables"])}
        spu = f.get("spu_images") or {}
        modules.append({
            "file": file, "where": where, "size": f["size"],
            "name": (f.get("module_info") or {}).get("name", ""),
            "libraries": libs, "imports": imports,
            "spu_programs": spu.get("count", 0), "spu_bytes": spu.get("total_bytes", 0),
            "rpcs3": {0: "the module itself", 1: "HLE"}.get(prx_list.get(file), "-"),
        })
    for m in modules:
        fam = family_of(m["file"])
        m["family"] = fam[0] if fam else "Unclassified"
        m["plan"] = fam[1] if fam else "translate"
        m["needs"] = sorted({exporter.get(lib, f"({lib}: no module)") for lib in m["imports"]} - {m["file"]})

    data = []
    for sub, plan, what in DATA:
        p = args.flash / sub
        if p.exists():
            n, size = dir_stats(p)
            data.append({"path": sub, "files": n, "bytes": size, "plan": plan, "what": what})
    fonts = sorted(p.name for p in (args.flash / "data/font").rglob("*") if p.is_file()) if (args.flash / "data/font").exists() else []

    (out / "report.json").write_text(json.dumps({"modules": modules, "data": data, "fonts": fonts}, indent=1))

    # report.md
    lines = ["# PS3 system software: what it holds and how it meets pspocket (generated; D-176)", ""]
    by_plan = collections.Counter(m["plan"] for m in modules)
    spu_total = sum(m["spu_programs"] for m in modules)
    lines += [f"{len(modules)} modules ({sum(1 for m in modules if m['where'] == 'external')} in sys/external, "
              f"{sum(1 for m in modules if m['where'] == 'internal')} in sys/internal), "
              f"{sum(len(m['libraries']) for m in modules)} libraries, "
              f"{sum(l['functions'] for m in modules for l in m['libraries'])} exported functions, "
              f"{spu_total} SPU programs. Plan: " +
              ", ".join(f"{PLAN_TEXT[k]} {v}" for k, v in sorted(by_plan.items())) + ".", ""]
    lines += ["## Families", "", "| Family | Plan | iOS side | Modules | Functions (host has) | SPU programs |",
              "|---|---|---|---|---|---|"]
    for fam in FAMILIES + [("Unclassified", "translate", "-", "", [])]:
        ms = [m for m in modules if m["family"] == fam[0]]
        if not ms:
            continue
        fn = sum(l["functions"] for m in ms for l in m["libraries"])
        hs = sum(l["host"] for m in ms for l in m["libraries"])
        lines.append(f"| {fam[0]} | {PLAN_TEXT[fam[1]]} | {fam[2]} | {len(ms)} | {fn} ({hs}) | "
                     f"{sum(m['spu_programs'] for m in ms)} |")
    lines += ["", "## Data", "", "| Path | Files | MB | Plan | What |", "|---|---|---|---|---|"]
    for d in data:
        lines.append(f"| {d['path']} | {d['files']} | {d['bytes'] / 1048576:.1f} | {PLAN_TEXT[d['plan']]} | {d['what']} |")
    if fonts:
        lines += ["", "System fonts: " + ", ".join(fonts), ""]
    lines += ["", "## Modules", "",
              "| Module | Where | KB | Libraries (functions, host has) | SPU | RPCS3 runs | Needs | Plan |",
              "|---|---|---|---|---|---|---|---|"]
    for m in sorted(modules, key=lambda m: (m["family"], m["file"])):
        libs = ", ".join(f"{l['library']} ({l['functions']}{'+' + str(l['variables']) + 'v' if l['variables'] else ''}"
                         f", {l['host']})" for l in m["libraries"]) or "-"
        lines.append(f"| {m['file']} | {m['where']} | {m['size'] // 1024} | {libs} | {m['spu_programs']} | {m['rpcs3']} | "
                     f"{', '.join(m['needs']) or '-'} | {PLAN_TEXT[m['plan']]} |")
    (out / "report.md").write_text("\n".join(lines) + "\n")
    print(f"firmware-report: {len(modules)} modules, {spu_total} SPU programs -> {out / 'report.md'}")
    unclassified = [m["file"] for m in modules if m["family"] == "Unclassified"]
    if unclassified:
        print("firmware-report: unclassified: " + ", ".join(unclassified), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
