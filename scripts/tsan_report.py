#!/usr/bin/env python3
"""Groups the ThreadSanitizer reports in test logs by their code locations (D-585).

    tsan_report.py LOG... [--examples FILE] [--show TEXT] [--quiet]

A report is one "WARNING: ThreadSanitizer: ..." block. Its key is its kind and the pspocket source lines of its stacks:
for a data race the two accesses (the first frame in pspocket's own source, past the big-endian accessors and the
standard library), for a lock-order inversion the places each mutex of the cycle was taken, otherwise the first stack.
Prints, per log, the number of reports and of distinct keys, then each key with its count, most frequent first.
--examples writes the first full report of each key to FILE, for reading one of each. --quiet prints only the per-log
lines. --show prints the first full report of each key whose locations contain TEXT. Exit 0 always: tsan.sh decides
what a count means.
"""

import collections
import re
import sys

FRAME = re.compile(r"^\s+#(\d+) (.*?) (\S+?):(\d+)(?::\d+)? \(")
FRAME_NOFILE = re.compile(r"^\s+#(\d+) (.*?) \(")
# Source directories of pspocket itself; a frame elsewhere (libc++, the sanitizer, the system) is skipped.
OWN = re.compile(r"/((?:runtime|tests|recomp|tools|third_party)/.*)$")
# pspocket frames that say where an access happens, not why: the accessors every component uses and Catch2, by file
# and by the names of what they define. With symbolize_inline_frames=0 (tsan.sh) an accessor inlined into a function
# shows as that function at the accessor's line, and counts as the function.
PASS_THROUGH = (
    ("runtime/core/include/pspocket/core/be.h", ("ps_",)),
    ("runtime/hle/src/guest.hpp", ("pspocket::hle::atomic_", "pspocket::hle::host_u", "bool pspocket::hle::reservation_op")),
    ("third_party/catch2/", ("Catch::",)),
)


def pass_through(rel, func):
    return any(rel.startswith(path) and func.startswith(names) for path, names in PASS_THROUGH)


def own_location(frames):
    """The first frame in pspocket's own code (skipping its accessors), as 'path:line func'."""
    fallback = None
    for func, path, line in frames:
        m = OWN.search(path)
        if not m:
            continue
        rel = m.group(1)
        loc = f"{rel}:{line} {short(func)}"
        if pass_through(rel, func):
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
    """A function name without its arguments, templates and return type, for one line; a lambda as 'f::lambda'."""
    func = func.strip()
    if func.endswith(" const"):
        func = func[: -len(" const")]
    out, depth = [], 0
    for c in func:  # drop every (...) and <...>, nested or not
        if c in "(<":
            depth += 1
        elif c in ")>":
            depth = max(0, depth - 1)
        elif depth == 0:
            out.append(c)
    name = "".join(out).strip().split(" ")[-1]
    name = re.sub(r"::\$_\d+::operator$", "::lambda", name)
    return name or func


def parse(path):
    """Yields (kind, stacks, text) per report: stacks a list of (heading, [(func, path, line)])."""
    with open(path, errors="replace") as f:
        lines = f.read().splitlines()
    i = 0
    while i < len(lines):
        m = re.match(r"^WARNING: ThreadSanitizer: (.*?)(?: \(pid=\d+\))?$", lines[i])
        if not m:
            i += 1
            continue
        kind = m.group(1)
        text = [lines[i]]
        stacks = []
        heading = None
        frames = None
        i += 1
        while i < len(lines) and not lines[i].startswith("SUMMARY: ThreadSanitizer") and \
                not lines[i].startswith("WARNING: ThreadSanitizer:"):
            line = lines[i]
            text.append(line)
            fm = FRAME.match(line)
            if fm:
                if frames is None:
                    frames = []
                    stacks.append((heading or "", frames))
                frames.append((fm.group(2), fm.group(3), fm.group(4)))
            elif FRAME_NOFILE.match(line):
                if frames is None:
                    frames = []
                    stacks.append((heading or "", frames))
                frames.append((FRAME_NOFILE.match(line).group(2), "", "0"))
            elif line.strip():
                heading = line.strip()
                frames = None
            i += 1
        if i < len(lines) and lines[i].startswith("SUMMARY"):
            text.append(lines[i])
            i += 1
        yield kind, stacks, "\n".join(text)


def compact(text, depth=10):
    """A report for reading: paths from the source root, no build IDs or offsets, each stack cut to `depth` frames."""
    out = []
    for line in text.splitlines():
        fm = re.match(r"^\s+#(\d+) ", line)
        if fm and int(fm.group(1)) >= depth:
            continue
        line = re.sub(r" \(BuildId: [0-9a-f]+\)", "", line)
        line = re.sub(r" \([^ ()]+\+0x[0-9a-f]+\)$", "", line)
        line = re.sub(r"/\S*?/((?:runtime|tests|recomp|tools|third_party)/)", r"\1", line)
        out.append(line)
    return "\n".join(out)


def key_of(kind, stacks):
    def wanted(h):
        return not h.startswith(("Location is", "Thread T", "Mutex M", "Main thread")) or "acquired here" in h

    # Thread creation stacks and locations say nothing about where the race is.
    access = [(h, f) for h, f in stacks if wanted(h)]
    if kind.startswith("lock-order-inversion"):
        locs = sorted({own_location(f) for h, f in stacks if "acquired here" in h})
        return (kind, tuple(locs))
    if kind.startswith("data race"):
        locs = sorted(own_location(f) for _, f in access[:2])
        return (kind, tuple(locs))
    return (kind, (own_location(access[0][1]) if access else "?",))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 64
    examples_path = None
    show = None
    quiet = False
    logs = []
    args = argv[1:]
    while args:
        a = args.pop(0)
        if a == "--examples":
            examples_path = args.pop(0)
        elif a == "--show":
            show = args.pop(0)
        elif a == "--quiet":
            quiet = True
        else:
            logs.append(a)
    total = collections.Counter()
    first = {}
    for log in logs:
        counts = collections.Counter()
        for kind, stacks, text in parse(log):
            k = key_of(kind, stacks)
            counts[k] += 1
            total[k] += 1
            first.setdefault(k, (log, text))
        print(f"{log}: {sum(counts.values())} reports, {len(counts)} distinct")
    if not quiet and total:
        print()
        for k, n in total.most_common():
            kind, locs = k
            print(f"{n:5d}  {kind}")
            for loc in locs:
                print(f"       {loc}")
    if show is not None:
        for k, n in total.most_common():
            if show in " ".join(k[1]):
                log, text = first[k]
                print(f"\n### {n} x {k[0]}: {' <-> '.join(k[1])}  (first in {log})\n{compact(text)}")
    if examples_path:
        with open(examples_path, "w") as out:
            for k, n in total.most_common():
                log, text = first[k]
                out.write(f"### {n} x {k[0]}: {' <-> '.join(k[1])}  (first in {log})\n{compact(text, 30)}\n\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
