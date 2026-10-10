#!/usr/bin/env python3
"""Groups the AddressSanitizer, LeakSanitizer and UBSan reports in test logs by their code locations (D-604).

    asan_report.py LOG... [--examples FILE] [--quiet]

Reports: an AddressSanitizer error ("ERROR: AddressSanitizer: heap-buffer-overflow ..." and its stacks), each leak of
a LeakSanitizer report ("Direct leak of N byte(s) ..." with its allocation stack), and each UBSan finding
("path:line:col: runtime error: ..." and, with print_stacktrace=1, its stack). A report's key is its kind and where it
is in pspocket's own code: the first frame of its first stack in pspocket's sources (past the big-endian accessors,
Catch2 and the standard library), or for UBSan the place the runtime error names. Prints, per log, the number of
reports and of distinct keys, then each key with its count, most frequent first. --examples writes the first full
report of each key to FILE. --quiet prints only the per-log lines. Exit 0 always: asan.sh decides what a count means.
"""

import collections
import re
import sys

FRAME = re.compile(r"^\s+#(\d+) 0x[0-9a-f]+ in (.*?) (\S+?):(\d+)(?::\d+)?$")
FRAME_MODULE = re.compile(r"^\s+#(\d+) 0x[0-9a-f]+ (?:in (.*?) )?\((\S+?)\+0x[0-9a-f]+\)")
OWN = re.compile(r"/((?:runtime|tests|recomp|tools|third_party|app)/.*)$")
UBSAN = re.compile(r"^(\S+?):(\d+):(\d+): runtime error: (.*)$")
ASAN = re.compile(r"^==\d+==ERROR: AddressSanitizer: (\S+)")
LSAN = re.compile(r"^==\d+==ERROR: LeakSanitizer: detected memory leaks")
LEAK = re.compile(r"^(Direct|Indirect) leak of (\d+) byte\(s\) in (\d+) object\(s\) allocated from:")
# pspocket frames that say where an access happens, not why.
PASS_THROUGH = (
    ("runtime/core/include/pspocket/core/be.h", ("ps_",)),
    ("third_party/catch2/", ("Catch::",)),
)


def rel_path(path):
    m = OWN.search(path)
    return m.group(1) if m else None


def own_location(frames):
    """The first frame in pspocket's own code (past its accessors), as 'path:line func'."""
    fallback = None
    for func, path, line in frames:
        rel = rel_path(path)
        if not rel:
            continue
        loc = f"{rel}:{line} {short(func)}"
        if any(rel.startswith(p) and func.startswith(n) for p, n in PASS_THROUGH):
            fallback = fallback or loc
            continue
        return loc
    if fallback:
        return fallback
    if frames:
        func, path, line = frames[0]
        return f"{path.rsplit('/', 1)[-1]}:{line} {short(func)}"
    return "?"


def short(func):
    """A function name without its arguments, templates and return type."""
    func = (func or "").strip()
    if func.endswith(" const"):
        func = func[: -len(" const")]
    out, depth = [], 0
    for c in func:
        if c in "(<":
            depth += 1
        elif c in ")>":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(c)
    name = "".join(out).strip().split(" ")[-1]
    name = re.sub(r"::\$_\d+::operator$", "::lambda", name)
    return name or func


def frames_from(lines, i):
    """The stack starting at lines[i]: (frames, next index)."""
    frames = []
    while i < len(lines):
        fm = FRAME.match(lines[i])
        if fm:
            frames.append((fm.group(2), fm.group(3), fm.group(4)))
            i += 1
            continue
        mm = FRAME_MODULE.match(lines[i])
        if mm:
            frames.append((mm.group(2) or "", mm.group(3), "0"))
            i += 1
            continue
        break
    return frames, i


def parse(path):
    """Yields (kind, location, text) per report."""
    with open(path, errors="replace") as f:
        lines = f.read().splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        um = UBSAN.match(line)
        if um:
            text = [line]
            frames, j = frames_from(lines, i + 1)
            text += lines[i + 1:j]
            rel = rel_path(um.group(1)) or um.group(1).rsplit("/", 1)[-1]
            message = re.sub(r"0x[0-9a-f]+", "ADDR", um.group(4))
            message = re.sub(r"-?\d+", "N", message)
            yield f"ubsan: {message}", f"{rel}:{um.group(2)}", "\n".join(text)
            i = max(j, i + 1)
            continue
        am = ASAN.match(line)
        if am:
            text = [line]
            j = i + 1
            first = None
            while j < len(lines) and not lines[j].startswith("SUMMARY: AddressSanitizer"):
                if first is None and FRAME.match(lines[j]):
                    first, _ = frames_from(lines, j)
                text.append(lines[j])
                j += 1
            if j < len(lines):
                text.append(lines[j])
            yield f"asan: {am.group(1)}", own_location(first or []), "\n".join(text)
            i = j + 1
            continue
        if LSAN.match(line):
            j = i + 1
            while j < len(lines) and not lines[j].startswith("SUMMARY: AddressSanitizer"):
                lm = LEAK.match(lines[j])
                if lm:
                    frames, k = frames_from(lines, j + 1)
                    yield f"lsan: {lm.group(1).lower()} leak", own_location(frames), "\n".join(lines[j:k])
                    j = k
                    continue
                j += 1
            i = j + 1
            continue
        i += 1


def compact(text, depth=12):
    out = []
    for line in text.splitlines():
        fm = re.match(r"^\s+#(\d+) ", line)
        if fm and int(fm.group(1)) >= depth:
            continue
        line = re.sub(r" \(BuildId: [0-9a-f]+\)", "", line)
        line = re.sub(r"/\S*?/((?:runtime|tests|recomp|tools|third_party)/)", r"\1", line)
        out.append(line)
    return "\n".join(out)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 64
    examples_path = None
    quiet = False
    logs = []
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--examples":
            examples_path = args.pop(0)
        elif a == "--quiet":
            quiet = True
        else:
            logs.append(a)
    total = collections.Counter()
    first = {}
    for log in logs:
        counts = collections.Counter()
        for kind, loc, text in parse(log):
            k = (kind, loc)
            counts[k] += 1
            total[k] += 1
            first.setdefault(k, (log, text))
        if counts:
            print(f"{log}: {sum(counts.values())} reports, {len(counts)} distinct")
    if not quiet and total:
        print()
        for (kind, loc), n in total.most_common():
            print(f"{n:5d}  {kind}\n       {loc}")
    if examples_path:
        with open(examples_path, "w") as out:
            for k, n in total.most_common():
                log, text = first[k]
                out.write(f"### {n} x {k[0]}: {k[1]}  (first in {log})\n{compact(text, 30)}\n\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
