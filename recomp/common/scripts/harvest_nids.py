#!/usr/bin/env python3
"""Harvest PS3 library NID names and lv2 syscall names from an RPCS3 source tree.

    harvest_nids.py <rpcs3-dir> [--out-dir DIR] [--check]

Writes two data files that recomp/common embeds at build time (recomp/common/data/ by default):

  nids.txt      [module] headers, then one entry per line: `name` (NID = nid_of(name)) or `0xNNNNNNNN name`.
                `[]` is the nameless module-management library (module_start, module_stop, ...).
  syscalls.txt  `number name`, one lv2 syscall per line.

Sources (RPCS3 is GPLv2, like this project, see docs/decisions.md D-001; names are API identifiers):

  - Module registrations anywhere under rpcs3/Emu: REG_FUNC, REG_FNID, REG_VAR, REG_VNID
    (macros in rpcs3/Emu/Cell/PPUModule.h). The module is the macro's first token, which RPCS3 requires to equal
    the library name given to DECLARE(ppu_module_manager::X)("name", ...). REG_HIDDEN_FUNC has no module and no
    NID (internal callbacks), so it contributes nothing. Commented-out registrations are ignored.
  - Explicit NID tables in rpcs3/Emu/Cell/PPUFunction.cpp (ppu_get_function_name / ppu_get_variable_name):
    sys_libc, sys_libm, sys_libstdcxx, sysPrxForUser and the nameless library.
  - The syscall table g_ppu_syscall_table in rpcs3/Emu/Cell/lv2/lv2.cpp (BIND_SYSC and NULL_FUNC entries;
    null_func / uns_func slots have no name).

The NID of a name is the first 4 bytes of SHA-1(name + suffix) read as a little-endian u32
(RPCS3 rpcs3/Emu/Cell/PPUModule.cpp, ppu_generate_id). The script checks its implementation against known
vectors before writing anything. Output is deterministic (sorted), so re-running on the same RPCS3 commit gives
identical files; --check exits 1 when the files on disk differ from a fresh harvest.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

NID_SUFFIX = bytes.fromhex("67 59 65 99 04 25 04 90 56 64 27 49 94 89 74 1A")

# Vectors verified against RPCS3 (docs: recomp/common tests use the same list).
NID_VECTORS = {
    "cellSysutilRegisterCallback": 0x9D98AFA0,
    "sys_process_exit": 0xE6F2C1E7,
    "_cellGcmInitBody": 0x15BAE46B,
    "cellGcmSetFlipMode": 0x4AE8D215,
    "cellPadInit": 0x1CF98800,
    "sys_lwmutex_lock": 0x1573DC3F,
    "cellFsOpen": 0x718BF5F8,
    "_sys_printf": 0x9F04F7AF,
    "cellSpursInitialize": 0xACFC8DBC,
    "sys_ppu_thread_exit": 0xAFF080A4,
}

SYSCALL_TABLE_SIZE = 1024


def nid_of(name: str) -> int:
    digest = hashlib.sha1(name.encode("utf-8") + NID_SUFFIX).digest()
    return int.from_bytes(digest[:4], "little")


class HarvestError(Exception):
    pass


def warn(msg: str) -> None:
    print(f"harvest_nids: warning: {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------------------------------------------
# C++ source helpers


def strip_comments(text: str) -> str:
    """Removes // and /* */ comments, keeping string and character literals and line structure intact."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            block = text[i : (n if j < 0 else j + 2)]
            out.append("\n" * block.count("\n"))
            i = n if j < 0 else j + 2
            continue
        if c == "R" and i + 1 < n and text[i + 1] == '"':
            # Raw string literal R"delim( ... )delim"
            m = re.match(r'R"([^()\\ ]{0,16})\(', text[i:])
            if m:
                end_marker = ")" + m.group(1) + '"'
                j = text.find(end_marker, i + m.end())
                j = n if j < 0 else j + len(end_marker)
                out.append(text[i:j])
                i = j
                continue
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == "\\":
                    j += 1
                elif text[j] == "\n":
                    break
                j += 1
            out.append(text[i : j + 1])
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def split_args(text: str, start: int) -> tuple[list[str], int]:
    """Parses a parenthesised, comma-separated argument list beginning at text[start] == '('.

    Returns the top-level arguments (whitespace-trimmed) and the index just past the closing ')'.
    """
    if text[start] != "(":
        raise HarvestError(f"expected '(' at offset {start}")
    depth = 0
    args = []
    cur = []
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == "\\":
                    j += 1
                j += 1
            cur.append(text[i : j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
            if depth > 1:
                cur.append(c)
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(cur).strip())
                return args, i + 1
            cur.append(c)
        elif c == "," and depth == 1:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    raise HarvestError("unterminated argument list")


def match_brace(text: str, open_index: int) -> int:
    """Returns the index of the '}' matching text[open_index] == '{' (comments must already be stripped)."""
    depth = 0
    i = open_index
    n = len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == "\\":
                    j += 1
                j += 1
            i = j + 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise HarvestError("unbalanced braces")


def c_string_value(literal: str) -> str:
    if not (literal.startswith('"') and literal.endswith('"')):
        raise HarvestError(f"not a plain string literal: {literal}")
    body = literal[1:-1]
    if "\\" in body:
        raise HarvestError(f"escape sequences in a registered name are not supported: {literal}")
    return body


def parse_int_literal(token: str) -> int | None:
    token = token.strip().rstrip("uUlL")
    try:
        if token.lower().startswith("0x"):
            return int(token, 16)
        if token.isdigit():
            return int(token, 10)
    except ValueError:
        return None
    return None


IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


# ---------------------------------------------------------------------------------------------------------------
# NID harvesting


class NidTable:
    def __init__(self) -> None:
        # module -> {(nid, name)}
        self.modules: dict[str, set[tuple[int, str]]] = {}
        self.sources: dict[str, int] = {}

    def add(self, module: str, nid: int, name: str, source: str) -> None:
        if not name or any(ch.isspace() for ch in name):
            raise HarvestError(f"name with whitespace or empty: {name!r} ({source})")
        if not 0 <= nid <= 0xFFFFFFFF:
            raise HarvestError(f"NID out of range: {nid:#x} ({source})")
        if module.startswith("[") or "]" in module:
            raise HarvestError(f"bad module name {module!r} ({source})")
        self.modules.setdefault(module, set()).add((nid, name))
        self.sources[source] = self.sources.get(source, 0) + 1

    def entry_count(self) -> int:
        return sum(len(v) for v in self.modules.values())


REG_MACRO = re.compile(r"(?<![A-Za-z0-9_])(REG_FUNC|REG_FNID|REG_VAR|REG_VNID)\s*(?=\()")
DECLARE_MODULE = re.compile(r'DECLARE\s*\(\s*ppu_module_manager\s*::\s*(\w+)\s*\)\s*\(\s*"([^"]*)"')
# Modules declared locally, e.g. `static ppu_static_module sysPs2Disc("sysPs2Disc", ...)` in sysPrxForUser.cpp.
LOCAL_MODULE = re.compile(r'(?<![A-Za-z0-9_])ppu_static_module\s+(\w+)\s*\(\s*"([^"]*)"')
# RPCS3's own pseudo-libraries: no PS3 executable imports these.
RPCS3_INTERNAL_MODULES = {"RPCS3_HLE_LIBRARY", "static_hle"}
STATIC_MODULE = re.compile(r"static\s+const\s+ppu_static_module\s+(\w+)\s*;")


def harvest_registrations(rpcs3: Path, table: NidTable) -> tuple[set[str], int]:
    emu = rpcs3 / "rpcs3" / "Emu"
    if not emu.is_dir():
        raise HarvestError(f"{emu} not found: pass the RPCS3 source root")

    declared: dict[str, str] = {}
    registrations = []  # (file, macro, args)
    for path in sorted(emu.rglob("*.cpp")):
        text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for m in DECLARE_MODULE.finditer(text):
            declared[m.group(2)] = m.group(1)
        for m in LOCAL_MODULE.finditer(text):
            declared[m.group(2)] = m.group(1)
        for m in REG_MACRO.finditer(text):
            line_start = text.rfind("\n", 0, m.start()) + 1
            if text[line_start : m.start()].lstrip().startswith("#"):
                continue  # a macro definition, not a registration
            args, _ = split_args(text, m.end())
            line = text.count("\n", 0, m.start()) + 1
            registrations.append((f"{path.relative_to(rpcs3)}:{line}", m.group(1), args))

    header = (emu / "Cell" / "PPUModule.h").read_text(encoding="utf-8", errors="replace")
    static_modules = set(STATIC_MODULE.findall(strip_comments(header)))
    declared_vars = set(declared.values())
    skipped = 0
    for missing in sorted(static_modules - declared_vars):
        warn(f"ppu_module_manager::{missing} is declared in PPUModule.h but no DECLARE(...) was found")

    for source, macro, args in registrations:
        if macro in ("REG_FUNC", "REG_VAR"):
            if len(args) != 2 or not IDENT.match(args[0]) or not IDENT.match(args[1]):
                raise HarvestError(f"{source}: unexpected {macro} arguments {args}")
            module, name = args
            nid = nid_of(name)
        else:
            if len(args) != 3 or not IDENT.match(args[0]) or not IDENT.match(args[2]):
                raise HarvestError(f"{source}: unexpected {macro} arguments {args}")
            module = args[0]
            if args[1].startswith('"'):
                # ppu_select_name(#func, "str") picks the string, ppu_generate_id hashes it.
                name = c_string_value(args[1])
                nid = nid_of(name)
            else:
                value = parse_int_literal(args[1])
                if value is None:
                    raise HarvestError(f"{source}: cannot parse NID {args[1]!r}")
                name = args[2]
                nid = value
        if module not in declared:
            warn(f"{source}: module {module!r} is not declared with that library name")
        if module in RPCS3_INTERNAL_MODULES:
            skipped += 1
            continue
        table.add(module, nid, name, "registrations")
    return set(declared) - RPCS3_INTERNAL_MODULES, len(registrations) - skipped


SWITCH_HEAD = re.compile(r"if\s*\((?P<cond>[^{};]*?)\)\s*switch\s*\(\s*(?P<var>fnid|vnid)\s*\)\s*\{")
CASE_RETURN = re.compile(r'case\s+(0x[0-9A-Fa-f]+)\s*:\s*return\s+"([^"]*)"\s*;')


def harvest_explicit_tables(rpcs3: Path, table: NidTable) -> int:
    path = rpcs3 / "rpcs3" / "Emu" / "Cell" / "PPUFunction.cpp"
    text = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
    count = 0
    for m in SWITCH_HEAD.finditer(text):
        cond = m.group("cond")
        modules = re.findall(r'_module\s*==\s*"([^"]*)"', cond)
        if "_module.empty()" in cond:
            modules.append("")
        if not modules:
            raise HarvestError(f"PPUFunction.cpp: cannot read module condition {cond!r}")
        body_end = match_brace(text, m.end() - 1)
        body = text[m.end() : body_end]
        cases = CASE_RETURN.findall(body)
        leftovers = CASE_RETURN.sub("", body).strip()
        if leftovers:
            raise HarvestError(f"PPUFunction.cpp: unexpected content in switch for {modules}: {leftovers[:80]!r}")
        for module in modules:
            for nid_text, name in cases:
                table.add(module, int(nid_text, 16), name, "PPUFunction.cpp")
                count += 1
    if count == 0:
        raise HarvestError("PPUFunction.cpp: no explicit NID tables found (format changed?)")
    return count


def format_nids(table: NidTable, source: str, stats: dict[str, int]) -> str:
    lines = [
        "# PS3 library NID names. Generated by recomp/common/scripts/harvest_nids.py; do not edit by hand.",
        f"# Source: {source} (GPLv2): REG_FUNC/REG_FNID/REG_VAR/REG_VNID registrations under rpcs3/Emu",
        "# and the explicit tables in rpcs3/Emu/Cell/PPUFunction.cpp.",
        "# Format: [module] header ([] = nameless module-management library), then `name` (NID = nid_of(name))",
        "# or `0xNNNNNNNN name` (explicit NID). nid_of = first 4 bytes of SHA-1(name + suffix), little-endian.",
        f"# {stats['modules']} modules, {stats['entries']} entries.",
    ]
    for module in sorted(table.modules):
        lines.append("")
        lines.append(f"[{module}]")
        for nid, name in sorted(table.modules[module], key=lambda e: (e[1], e[0])):
            if nid_of(name) == nid:
                lines.append(name)
            else:
                lines.append(f"0x{nid:08X} {name}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------------------------------
# Syscalls


SYSCALL_ENTRY = re.compile(r"^(?:(BIND_SYSC|NULL_FUNC)\s*\(\s*(\w+)\s*\)|(null_func|uns_func))$")


def harvest_syscalls(rpcs3: Path) -> list[tuple[int, str, str]]:
    path = rpcs3 / "rpcs3" / "Emu" / "Cell" / "lv2" / "lv2.cpp"
    raw = path.read_text(encoding="utf-8", errors="replace")
    anchor = raw.find("g_ppu_syscall_table")
    if anchor < 0:
        raise HarvestError("lv2.cpp: g_ppu_syscall_table not found")
    open_index = raw.find("{", anchor)
    # The initializer has no braces of its own, but comments may; strip block comments only for matching.
    close_index = raw.find("};", open_index)
    if open_index < 0 or close_index < 0:
        raise HarvestError("lv2.cpp: cannot delimit g_ppu_syscall_table")
    body = re.sub(r"/\*.*?\*/", "", raw[open_index + 1 : close_index], flags=re.S)

    entries: list[tuple[int, str, str]] = []
    index = 0
    mismatches = 0
    for line in body.split("\n"):
        code, _, comment = line.partition("//")
        tokens = [t.strip() for t in code.split(",") if t.strip()]
        first = index
        for token in tokens:
            m = SYSCALL_ENTRY.match(token)
            if not m:
                raise HarvestError(f"lv2.cpp: unexpected syscall table entry {token!r}")
            if m.group(1):
                entries.append((index, m.group(2), "implemented" if m.group(1) == "BIND_SYSC" else "stub"))
            index += 1
        # Cross-check the index comments where present ("//N (0x...)", "//N" naming the last entry of a line,
        # "//N-M" ranges). They are informational: positions decide, and the table size is checked below.
        m = re.match(r"\s*(\d+)(?:\s*-\s*(\d+))?", comment) if tokens else None
        if m:
            last = index - 1
            lo, hi = int(m.group(1)), int(m.group(2)) if m.group(2) else None
            if not (lo in (first, last) or hi == last):
                mismatches += 1
    if index != SYSCALL_TABLE_SIZE:
        raise HarvestError(f"lv2.cpp: syscall table has {index} entries, expected {SYSCALL_TABLE_SIZE}")
    if mismatches:
        warn(f"lv2.cpp: {mismatches} index comments disagree with entry positions (positions are used)")
    return entries


def format_syscalls(entries: list[tuple[int, str, str]], source: str) -> str:
    lines = [
        "# lv2 syscall numbers. Generated by recomp/common/scripts/harvest_nids.py; do not edit by hand.",
        f"# Source: {source} (GPLv2), rpcs3/Emu/Cell/lv2/lv2.cpp g_ppu_syscall_table",
        "# (BIND_SYSC and NULL_FUNC entries; unnamed null_func/uns_func slots are omitted).",
        "# Format: `number name` (decimal).",
    ]
    for number, name, _status in entries:
        lines.append(f"{number} {name}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------------------------------


def describe_source(rpcs3: Path) -> str:
    """Names the RPCS3 tree: its own commit, or the enclosing repository's commit and the path inside it."""

    def git(*args: str) -> str:
        out = subprocess.run(["git", "-C", str(rpcs3), *args], capture_output=True, text=True, check=True)
        return out.stdout.strip()

    try:
        top = Path(git("rev-parse", "--show-toplevel")).resolve()
        commit = git("rev-parse", "HEAD")
    except (OSError, subprocess.CalledProcessError):
        return "RPCS3 (not a git checkout)"
    here = rpcs3.resolve()
    if top == here:
        return f"RPCS3 {commit}"
    return f"RPCS3 tree {here.relative_to(top)} in {top.name}@{commit[:12]}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("rpcs3_dir", type=Path, help="RPCS3 source root (the directory containing rpcs3/Emu)")
    parser.add_argument("--out-dir", type=Path, default=Path(__file__).resolve().parent.parent / "data")
    parser.add_argument("--check", action="store_true", help="compare with the files on disk instead of writing")
    parser.add_argument("--source-label", help="describe the RPCS3 tree in the file headers (default: from git)")
    args = parser.parse_args()

    for name, expected in NID_VECTORS.items():
        got = nid_of(name)
        if got != expected:
            print(f"harvest_nids: nid_of({name}) = {got:#010x}, expected {expected:#010x}", file=sys.stderr)
            return 2

    try:
        table = NidTable()
        declared, registration_count = harvest_registrations(args.rpcs3_dir, table)
        explicit_count = harvest_explicit_tables(args.rpcs3_dir, table)
        syscalls = harvest_syscalls(args.rpcs3_dir)
    except (HarvestError, OSError) as e:
        print(f"harvest_nids: error: {e}", file=sys.stderr)
        return 2

    source = args.source_label or describe_source(args.rpcs3_dir)
    stats = {"modules": len(table.modules), "entries": table.entry_count()}
    outputs = {
        "nids.txt": format_nids(table, source, stats),
        "syscalls.txt": format_syscalls(syscalls, source),
    }

    if args.check:
        stale = [n for n, text in outputs.items() if not (args.out_dir / n).is_file() or (args.out_dir / n).read_text() != text]
        for n in stale:
            print(f"harvest_nids: {args.out_dir / n} is out of date", file=sys.stderr)
        return 1 if stale else 0

    args.out_dir.mkdir(parents=True, exist_ok=True)
    for n, text in outputs.items():
        (args.out_dir / n).write_text(text)
    print(
        f"harvest_nids: {len(declared)} declared modules, {registration_count} registrations, "
        f"{explicit_count} explicit table entries -> {stats['entries']} NID entries in {stats['modules']} modules; "
        f"{len(syscalls)} named syscalls ({source})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
