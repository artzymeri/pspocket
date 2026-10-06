#!/usr/bin/env python3
"""Generate runtime/hle/src/rpcs3_stubs.inc: the functions RPCS3's own modules leave as stubs (D-182).

A PS3 library pspocket always serves itself (recomp::firmware_module_native: sysutil and everything on it, input,
audio, the camera, the microphone, Move, netctl, voice, sysmodule) has no firmware fallback: a function the host does
not implement returns CELL_ENOSYS to the game. RPCS3 serves the same libraries itself by default (its g_prx_list in
rpcs3/Emu/Cell/lv2/sys_prx.cpp says HLE for them), and many of its functions there are stubs: they log and return a
constant (CELL_OK nearly always). Games run on RPCS3 with those answers, so pspocket answers them the same way, with
a log of the call (process.cpp), until a real implementation takes the function's place in the HLE registry, which
always wins.

    python3 runtime/hle/scripts/gen_rpcs3_stubs.py RPCS3_DIR            # regenerate
    python3 runtime/hle/scripts/gen_rpcs3_stubs.py RPCS3_DIR --check    # exit 1 if the committed file is stale

A function is a stub when its body, comments and log statements (`log.todo(...)`, `.warning`, `.notice`, `.trace`,
`.error`, `.success`, UNIMPLEMENTED_FUNC) removed, is nothing (a void function) or `return X;` with X a constant:
CELL_OK, 0, 1, vm::null, not_an_error(N), a number, or an error constant RPCS3 defines. Anything else (an argument
check, a call) is an implementation, which pspocket ports by hand.

Only the libraries below are harvested. Libraries RPCS3 loads from the firmware by default (liblv2, libsre,
libgcm_sys, libfs, libnet, libusbd, libusbpspcm) are left out: RPCS3's stubs there are not what games meet on RPCS3.

Output: one `{"library", "name", 0xNID, 0xR3, sets_r3},` line per stub, sorted by library and name. The NID is the
REG_FNID's own, else the NID of the name (recomp/common/scripts/harvest_nids.py's nid_of). R3 is the value as the
PPU's r3 holds it: a 32-bit signed result sign-extended, an unsigned one or a pointer zero-extended; a void function
leaves r3 alone (sets_r3 false).
"""

import argparse
import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
DST = ROOT / "runtime" / "hle" / "src" / "rpcs3_stubs.inc"
NID_SUFFIX = bytes.fromhex("67 59 65 99 04 25 04 90 56 64 27 49 94 89 74 1A")

# RPCS3 library names whose firmware files pspocket keeps native and RPCS3 serves with HLE by default.
LIBRARIES = {
    # libsysutil*.sprx
    "cellSysutil", "cellSaveData", "cellGame", "cellSysutilAvconfExt", "cellUserInfo", "cellScreenShotUtility",
    "cellSearchUtility", "cellMusicUtility", "cellMusicDecodeUtility", "cellMusicExportUtility", "cellPhotoUtility",
    "cellPhotoImportUtil", "cellPhotoDecodeUtil", "cellImeJpUtility", "cellBGDLUtility", "cellSysutilAvc2",
    "cellSysutilAvcExt", "cellSubDisplay", "cellRemotePlay", "cellVideoExportUtility", "cellVideoUpload",
    "cellVideoPlayerUtility", "cellPrintUtility", "cellAuthDialogUtility", "cellOskExtUtility", "cellGameExec",
    "cellRec", "cellSysutilMisc", "cellCrossController", "cellSysconfExtUtility", "cellSysutilNpEula",
    "cellSysutilAp", "cellDtcpIpUtility", "cellMinisSaveData", "cellPesmUtility", "cellRtcAlarm",
    "sceNp", "sceNp2", "sceNpTrophy", "sceNpTus", "sceNpClans", "sceNpCommerce2", "sceNpSns", "sceNpUtil",
    "sceNpMatchingInt", "sceNpPlus",
    # libsysmodule, libaudio, libvoice, libio, libcamera, libmic, libgem, libnetctl, libfs_utility_init
    "cellSysmodule", "cellAudio", "cellVoice", "sys_io", "cellCamera", "cellMic", "libgem", "cellNetCtl",
    "fs_utility_init",
}

LOG_STATEMENT = re.compile(
    r"\b\w+\.(?:todo|warning|notice|trace|error|success)\s*\((?:[^;]|;(?!\s*$))*?\)\s*;", re.S | re.M)
UNIMPLEMENTED = re.compile(r"UNIMPLEMENTED_FUNC\s*\(\s*\w+\s*\)\s*;")
DEFINITION = re.compile(r"^(?:static\s+)?([\w:<>]+(?:\s*\*)?)\s+(\w+)\s*\(([^;{]*?)\)\s*\n\{", re.M)
REG_FUNC = re.compile(r"REG_FUNC\(\s*(\w+)\s*,\s*(\w+)\s*\)")
REG_FNID = re.compile(r"REG_FNID\(\s*(\w+)\s*,\s*(\"[^\"]+\"|0x[0-9A-Fa-f]+|\d+)\s*,\s*(\w+)\s*\)")
SIGNED = {"error_code", "s32", "int", "s64"}
UNSIGNED = {"u32", "u64", "b8", "bool", "u16", "u8"}


def nid_of(name: str) -> int:
    return int.from_bytes(hashlib.sha1(name.encode("utf-8") + NID_SUFFIX).digest()[:4], "little")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def definitions(src: str) -> dict[str, tuple[str, str]]:
    out: dict[str, tuple[str, str]] = {}
    for m in DEFINITION.finditer(src):
        depth, i = 1, m.end()
        while depth and i < len(src):
            depth += {"{": 1, "}": -1}.get(src[i], 0)
            i += 1
        out.setdefault(m.group(2), (m.group(1), src[m.end() : i - 1]))
    return out


def error_constants(rpcs3: pathlib.Path) -> dict[str, int]:
    consts: dict[str, int] = {}
    for header in (rpcs3 / "rpcs3" / "Emu" / "Cell").rglob("*.h"):
        for m in re.finditer(r"\b(CELL_\w+|SCE_\w+)\s*=\s*(0x[0-9A-Fa-f]+)\b", header.read_text(errors="replace")):
            consts.setdefault(m.group(1), int(m.group(2), 16))
    return consts


def stub_result(ret_type: str, body: str, consts: dict[str, int]) -> tuple[int, bool] | None:
    rest = UNIMPLEMENTED.sub("", LOG_STATEMENT.sub("", body)).strip()
    if not rest:
        return (0, False) if ret_type == "void" else None
    m = re.fullmatch(r"return\s+(.+?)\s*;", rest, re.S)
    if not m:
        return None
    expr = m.group(1).strip()
    if expr in ("CELL_OK", "vm::null", "nullptr"):
        value = 0
    elif re.fullmatch(r"not_an_error\(\s*(\w+)\s*\)", expr):
        inner = re.fullmatch(r"not_an_error\(\s*(\w+)\s*\)", expr).group(1)
        value = int(inner, 0) if re.fullmatch(r"0x[0-9A-Fa-f]+|\d+", inner) else consts.get(inner)
        if value is None:
            return None
    elif re.fullmatch(r"0x[0-9A-Fa-f]+|\d+", expr):
        value = int(expr, 0)
    elif expr in consts:
        value = consts[expr]
    else:
        return None
    base = ret_type.split("<")[0].split("::")[-1].strip()
    if ret_type.startswith("vm::") or base in UNSIGNED:
        return value & 0xFFFFFFFF, True
    if base in SIGNED:
        value &= 0xFFFFFFFF
        return (value | 0xFFFFFFFF00000000 if value & 0x80000000 else value), True
    return None


def harvest(rpcs3: pathlib.Path) -> list[tuple[str, str, int, int, bool]]:
    modules = rpcs3 / "rpcs3" / "Emu" / "Cell" / "Modules"
    if not modules.is_dir():
        raise SystemExit(f"{modules}: not an RPCS3 source tree")
    consts = error_constants(rpcs3)
    out = {}
    for path in sorted(modules.glob("*.cpp")):
        src = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        defs = definitions(src)
        regs = [(m.group(1), m.group(2), nid_of(m.group(2)), m.group(2)) for m in REG_FUNC.finditer(src)]
        for m in REG_FNID.finditer(src):
            tag = m.group(2)
            if tag.startswith('"'):
                regs.append((m.group(1), tag.strip('"'), nid_of(tag.strip('"')), m.group(3)))
            else:
                regs.append((m.group(1), m.group(3), int(tag, 0), m.group(3)))
        for library, name, nid, fn in regs:
            if library not in LIBRARIES or fn not in defs:
                continue
            result = stub_result(*defs[fn], consts)
            if result is not None:
                out[(library, nid)] = (library, name, nid, result[0], result[1])
    return sorted(out.values(), key=lambda e: (e[0], e[1]))


def render(entries: list[tuple[str, str, int, int, bool]]) -> str:
    lines = [
        "// The functions RPCS3's own HLE modules leave as stubs (a log, then a constant), in the libraries pspocket",
        "// always serves itself (D-182). GENERATED by runtime/hle/scripts/gen_rpcs3_stubs.py from RPCS3",
        "// rpcs3/Emu/Cell/Modules/*.cpp (GPLv2). Do not edit. Included by runtime/hle/src/rpcs3_stubs.cpp.",
    ]
    for library, name, nid, r3, sets in entries:
        lines.append(f'{{"{library}", "{name}", 0x{nid:08X}, 0x{r3:X}, {"true" if sets else "false"}}},')
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rpcs3_dir", type=pathlib.Path)
    ap.add_argument("--check", action="store_true", help="fail if the committed table is out of date")
    args = ap.parse_args()
    if nid_of("cellPadInit") != 0x1CF98800:
        raise SystemExit("gen_rpcs3_stubs: nid_of is wrong")
    text = render(harvest(args.rpcs3_dir))
    if args.check:
        if not DST.exists() or DST.read_text() != text:
            print(f"gen_rpcs3_stubs: {DST} is out of date", file=sys.stderr)
            return 1
        return 0
    DST.write_text(text)
    print(f"gen_rpcs3_stubs: {text.count(chr(10)) - 3} stubs -> {DST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
