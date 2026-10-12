#!/usr/bin/env python3
"""How far the engine is, counted from the code (and docs/progress/data.json for what cannot be counted).

    scripts/progress.py [--boot build/host/runtime/loader/pspocket-boot]

Writes docs/progress/*.svg (badges, the system library map, the kernel map, the speed charts), docs/progress/summary.json
and the Status section of README.md between its progress markers. Run it before every push (the user's ask, 2026-10-07).

What is counted, and against what:
- System libraries: every function and variable RPCS3 registers (recomp/common/data/nids.txt, 148 libraries) against
  the ones pspocket implements itself (`pspocket-boot --hle-list`) and the ones it answers as RPCS3's own stubs do
  (runtime/hle/src/rpcs3_stubs.inc, D-182). A function neither has goes to the PS3's own firmware module when the user
  installed the system software (D-175, D-178), else it stops with an error.
- Kernel calls: every lv2 syscall RPCS3 knows (recomp/common/data/syscalls.txt) against pspocket's own handlers
  (PS_LV2_BIND) and the ones answered as RPCS3 answers them (syscall_null.inc: a log, then OK; syscall_stub.inc: a
  constant; D-166, D-184).
- PPU and SPU instructions: the instruction tables (ops.inc) against the ones the translators handle; every one of those
  is checked against the interpreter by the differential fuzz tests (CLAUDE.md rule 3). The ones left out are marked
  OPF_NOSUPPORT; those the PS3's processors do not have are not counted at all (NOT_ON_PS3).
- Everything else (graphics, sound, the app, games, speed) is a checklist and measurements kept in data.json, each item
  with the decision or run behind it.

No game data is read: only the repository's sources and the tool's own registry.
"""

import argparse
import html
import json
import math
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs", "progress")

GREEN = "#2da44e"
LIGHT = "#9be9a8"
GREY = "#8c959f"
AMBER = "#d4a72c"
RED = "#cf222e"


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


# ---------------------------------------------------------------------------------------------------- counting


def known_functions():
    """{module: [(nid, name)]} from nids.txt."""
    mods = {}
    cur = None
    for line in read("recomp/common/data/nids.txt").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^\[(.*)\]$", line)
        if m:
            cur = m.group(1) or "(module management)"
            mods.setdefault(cur, [])
            continue
        parts = line.split()
        if len(parts) == 2 and parts[0].startswith("0x"):
            mods[cur].append((int(parts[0], 16), parts[1]))
        else:
            mods[cur].append((None, parts[0]))
    return mods


def own_functions(boot):
    """{(module, name)} pspocket implements, from pspocket-boot --hle-list."""
    out = subprocess.run([boot, "--hle-list"], capture_output=True, text=True, check=True).stdout
    own = set()
    for line in out.splitlines():
        # "module nid name"; the nameless module-management library's lines start with its empty name (D-918).
        parts = line.split(" ")
        if len(parts) == 3:
            own.add((parts[0] or "(module management)", parts[2]))
    return own


def stub_functions():
    stubs = set()
    for m in re.finditer(r'^\{"([^"]+)", "([^"]+)", 0x', read("runtime/hle/src/rpcs3_stubs.inc"), re.M):
        stubs.add((m.group(1), m.group(2)))
    return stubs


def libraries(boot):
    known = known_functions()
    own = own_functions(boot)
    stubs = stub_functions()
    mods = []
    for mod, funcs in known.items():
        cells = []
        for _, name in funcs:
            if (mod, name) in own:
                cells.append("own")
            elif (mod, name) in stubs:
                cells.append("stub")
            else:
                cells.append("missing")
        if cells:
            mods.append({"name": mod, "cells": cells})
    return mods


def kernel():
    names = {}
    for line in read("recomp/common/data/syscalls.txt").splitlines():
        m = re.match(r"^(\d+) (\S+)", line)
        if m:
            names[int(m.group(1))] = m.group(2)
    own = set()
    for path in os.listdir(os.path.join(ROOT, "runtime/lv2/src")):
        if path.endswith(".cpp"):
            for m in re.finditer(r"PS_LV2_BIND\(table, (\d+),", read("runtime/lv2/src/" + path)):
                own.add(int(m.group(1)))
    answered = set(int(n) for n in re.findall(r"^\{(\d+),", read("runtime/lv2/src/syscall_null.inc"), re.M))
    answered |= set(int(n) for n in re.findall(r"^\{(\d+),", read("runtime/lv2/src/syscall_stub.inc"), re.M))
    groups = {}
    for n, name in sorted(names.items()):
        g = re.sub(r"^_*", "", name)
        g = "_".join(g.split("_")[:2]) if g.startswith("sys_") else g.split("_")[0]
        state = "own" if n in own else "stub" if n in answered else "missing"
        groups.setdefault(g, []).append(state)
    return [{"name": g, "cells": c} for g, c in groups.items()]


# In the tables, not on the PS3: the SPU's double-precision compares came with the PowerXCell 8i's SPU (ISA 1.2), and
# eciwx/ecowx are the PowerPC's optional external control the PS3 does not provide. RPCS3 stops on all of them
# (SPUInterpreter.cpp DFCEQ..., PPUInterpreter.cpp ECIWX/ECOWX); a game that ran one would fault on the PS3 too.
NOT_ON_PS3 = {"DFCGT", "DFCMGT", "DFTSV", "DFCEQ", "DFCMEQ", "ECIWX", "ECOWX"}


def instructions(path, macro):
    total = supported = 0
    for line in read(path).splitlines():
        if line.startswith(macro + "("):
            name = line[len(macro) + 1:].split(",")[0].strip()
            if name in NOT_ON_PS3:
                continue
            total += 1
            if "OPF_NOSUPPORT" not in line:
                supported += 1
    return supported, total


# ---------------------------------------------------------------------------------------------------- drawing


def squarify(items, x, y, w, h):
    """items: [(weight, payload)] sorted descending; returns [(x, y, w, h, payload)] (squarified treemap)."""
    out = []
    items = [i for i in items if i[0] > 0]
    total = sum(i[0] for i in items)
    if not items or total <= 0:
        return out
    scale = w * h / total
    rest = [(wt * scale, p) for wt, p in items]

    def worst(row, side):
        s = sum(r[0] for r in row)
        return max(max(side * side * r[0] / (s * s), (s * s) / (side * side * r[0])) for r in row)

    while rest:
        side = min(w, h)
        row = [rest.pop(0)]
        while rest and worst(row + [rest[0]], side) <= worst(row, side):
            row.append(rest.pop(0))
        s = sum(r[0] for r in row)
        if w >= h:  # a column at the left
            cw = s / h
            cy = y
            for a, p in row:
                ch = a / cw
                out.append((x, cy, cw, ch, p))
                cy += ch
            x += cw
            w -= cw
        else:  # a row at the top
            rh = s / w
            cx = x
            for a, p in row:
                rw = a / rh
                out.append((cx, y, rw, rh, p))
                cx += rw
            y += rh
            h -= rh
    return out


COLOURS = {"own": GREEN, "stub": LIGHT, "missing": GREY}


def treemap_svg(title, mods, width=900, height=440):
    head = 30
    rects = squarify(sorted(((len(m["cells"]), m) for m in mods), key=lambda t: -t[0]), 0, head, width, height - head)
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
             f'font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">',
             f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
             f'<text x="4" y="20" font-size="15" font-weight="600" fill="#24292f">{html.escape(title)}</text>']
    for x, y, w, h, m in rects:
        n = len(m["cells"])
        pad = 1.0
        iw, ih = max(w - 2 * pad, 1), max(h - 2 * pad, 1)
        # cells in a grid of about square cells filling the block
        cols = max(1, round(math.sqrt(n * iw / ih)))
        rows = math.ceil(n / cols)
        cw, ch = iw / cols, ih / rows
        parts.append(f'<g><title>{html.escape(m["name"])}: {sum(c == "own" for c in m["cells"])} own, '
                     f'{sum(c == "stub" for c in m["cells"])} answered as RPCS3, '
                     f'{sum(c == "missing" for c in m["cells"])} not yet, of {n}</title>')
        parts.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" fill="#e6e8eb" stroke="#ffffff" '
                     f'stroke-width="1.5"/>')
        order = sorted(m["cells"], key=lambda c: {"own": 0, "stub": 1, "missing": 2}[c])
        for i, c in enumerate(order):
            cx = x + pad + (i % cols) * cw
            cy = y + pad + (i // cols) * ch
            parts.append(f'<rect x="{cx:.1f}" y="{cy:.1f}" width="{max(cw - 0.6, 0.4):.1f}" height="{max(ch - 0.6, 0.4):.1f}" '
                         f'fill="{COLOURS[c]}"/>')
        if w > 34 and h > 14:
            label = m["name"]
            maxch = int(w / 6.6)
            if len(label) > maxch:
                label = label[: max(maxch - 1, 1)] + "…"
            parts.append(f'<text x="{x + 3:.1f}" y="{y + 12:.1f}" font-size="10.5" fill="#ffffff" stroke="#24292f" '
                         f'stroke-width="2.4" paint-order="stroke">{html.escape(label)}</text>')
        parts.append("</g>")
    parts.append("</svg>")
    return "\n".join(parts)


def badge_svg(label, value, colour):
    lw = 7 * len(label) + 12
    vw = 7 * len(value) + 12
    w = lw + vw
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="20" role="img" aria-label="{html.escape(label)}: '
            f'{html.escape(value)}"><rect width="{lw}" height="20" fill="#555"/><rect x="{lw}" width="{vw}" height="20" '
            f'fill="{colour}"/><g fill="#fff" font-family="Verdana,DejaVu Sans,sans-serif" font-size="11">'
            f'<text x="{lw / 2}" y="14" text-anchor="middle">{html.escape(label)}</text>'
            f'<text x="{lw + vw / 2}" y="14" text-anchor="middle">{html.escape(value)}</text></g></svg>')


def pct_colour(p):
    return GREEN if p >= 90 else "#57ab5a" if p >= 75 else AMBER if p >= 50 else RED


def bars_svg(title, rows, width=900):
    """rows: [(label, fraction 0..1, text)]."""
    rh = 26
    height = 34 + rh * len(rows)
    lw = 300
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
             f'font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">',
             f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
             f'<text x="4" y="20" font-size="15" font-weight="600" fill="#24292f">{html.escape(title)}</text>']
    bw = width - lw - 70
    for i, (label, frac, text) in enumerate(rows):
        y = 32 + i * rh
        parts.append(f'<text x="4" y="{y + 15}" font-size="12.5" fill="#24292f">{html.escape(label)}</text>')
        parts.append(f'<rect x="{lw}" y="{y + 3}" width="{bw}" height="16" rx="3" fill="#e6e8eb"/>')
        parts.append(f'<rect x="{lw}" y="{y + 3}" width="{bw * max(0.0, min(frac, 1.0)):.1f}" height="16" rx="3" '
                     f'fill="{pct_colour(100 * frac)}"/>')
        parts.append(f'<text x="{lw + bw + 8}" y="{y + 15}" font-size="12.5" fill="#24292f">{html.escape(text)}</text>')
    parts.append("</svg>")
    return "\n".join(parts)


def line_chart_svg(title, series, ylabel, width=440, height=260, higher_is_better=True, xname="run"):
    """series: [(name, colour, [(x, y)])] with x = run number (or xname: "s" for seconds into a run)."""
    pts = [p for _, _, s in series for p in s]
    if not pts:
        return None
    x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
    y1 = max(p[1] for p in pts) * 1.15
    y0 = 0
    if x1 == x0:
        x1 = x0 + 1
    L, R, T, B = 46, 12, 52, 34
    pw, ph = width - L - R, height - T - B

    def sx(x):
        return L + (x - x0) / (x1 - x0) * pw

    def sy(y):
        return T + ph - (y - y0) / (y1 - y0) * ph

    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
             f'font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif">',
             f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
             f'<text x="4" y="18" font-size="13.5" font-weight="600" fill="#24292f">{html.escape(title)}</text>']
    for k in range(5):
        yv = y0 + (y1 - y0) * k / 4
        parts.append(f'<line x1="{L}" x2="{width - R}" y1="{sy(yv):.1f}" y2="{sy(yv):.1f}" stroke="#eaeef2"/>')
        parts.append(f'<text x="{L - 6}" y="{sy(yv) + 4:.1f}" font-size="10" text-anchor="end" fill="#57606a">{yv:.0f}</text>')
    xl = (lambda x: f"run {x}") if xname == "run" else (lambda x: f"{x} {xname}")
    parts.append(f'<text x="{L}" y="{height - 8}" font-size="10" fill="#57606a">{xl(x0)}</text>')
    parts.append(f'<text x="{width - R}" y="{height - 8}" font-size="10" text-anchor="end" fill="#57606a">{xl(x1)}</text>')
    parts.append(f'<text x="{(L + width - R) / 2}" y="{height - 8}" font-size="10" text-anchor="middle" fill="#57606a">'
                 f'{html.escape(ylabel)}</text>')
    lx = L + 4
    for name, colour, s in series:
        s = sorted(s)
        if not s:
            continue
        d = " ".join(f'{"M" if i == 0 else "L"}{sx(x):.1f},{sy(y):.1f}' for i, (x, y) in enumerate(s))
        parts.append(f'<path d="{d}" fill="none" stroke="{colour}" stroke-width="2"/>')
        for x, y in s:
            parts.append(f'<circle cx="{sx(x):.1f}" cy="{sy(y):.1f}" r="2.6" fill="{colour}"><title>{xl(x)}: {y}</title></circle>')
        parts.append(f'<rect x="{lx}" y="{T - 22}" width="10" height="3" fill="{colour}"/>')
        parts.append(f'<text x="{lx + 14}" y="{T - 18}" font-size="10" fill="#24292f">{html.escape(name)}</text>')
        lx += 18 + 6 * len(name)
    parts.append("</svg>")
    return "\n".join(parts)


# ---------------------------------------------------------------------------------------------------- report


def count(mods, state):
    return sum(c == state for m in mods for c in m["cells"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--boot", default=os.path.join(ROOT, "build/host/runtime/loader/pspocket-boot"))
    args = ap.parse_args()
    data = json.loads(read("docs/progress/data.json"))
    os.makedirs(OUT, exist_ok=True)

    libs = libraries(args.boot)
    kern = kernel()
    ppu = instructions("runtime/ppu/include/pspocket/ppu/ops.inc", "PS_PPU_OP")
    spu = instructions("runtime/spu/include/pspocket/spu/ops.inc", "PS_SPU_OP")
    lib_total = sum(len(m["cells"]) for m in libs)
    lib_own, lib_stub = count(libs, "own"), count(libs, "stub")
    k_total = sum(len(m["cells"]) for m in kern)
    k_own, k_stub = count(kern, "own"), count(kern, "stub")

    def write(name, text):
        with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
            f.write(text + "\n")

    pc = lambda a, b: 100.0 * a / b if b else 0.0
    lib_pct, k_pct = pc(lib_own, lib_total), pc(k_own + k_stub, k_total)
    write("libraries.svg", treemap_svg(
        f"System libraries: {lib_own} of {lib_total} pspocket's own ({lib_pct:.1f}%), {lib_stub} answered as RPCS3", libs))
    write("kernel.svg", treemap_svg(
        f"Kernel calls: {k_own} own, {k_stub} answered as RPCS3 answers them, of {k_total} ({k_pct:.1f}%)", kern,
        height=300))

    # the checklist: done 1, partial 0.5
    areas = []
    for area in data["checklist"]:
        items = area["items"]
        score = sum({"done": 1.0, "partial": 0.5}.get(i["state"], 0.0) for i in items)
        areas.append((area["area"], score / len(items), f'{100 * score / len(items):.0f}%  ({sum(i["state"] == "done" for i in items)}/{len(items)} done)'))
    write("engine.svg", bars_svg("The engine, part by part (checklist in docs/progress/data.json)", areas))

    games = data["games"]
    rating_score = {"Flawless": 1.0, "Playable": 0.66, "Boots": 0.33, "Broken": 0.0}
    game_rows = [(f'{g["title"]} ({g["id"]})', rating_score[g["rating"]], g["rating"]) for g in games]
    write("games.svg", bars_svg("Games against the Flawless bar (docs/compatibility.md)", game_rows))

    charts = []
    for chart in data["charts"]:
        svg = line_chart_svg(chart["title"], [(s["name"], s["colour"], [tuple(p) for p in s["points"]]) for s in chart["series"]],
                             chart["ylabel"], xname=chart.get("xname", "run"))
        if svg:
            write(chart["file"], svg)
            charts.append(chart)

    # overall: the counted parts and the checklist, each equally
    parts = [lib_pct / 100, k_pct / 100, ppu[0] / ppu[1], spu[0] / spu[1]] + [a[1] for a in areas]
    overall = 100 * sum(parts) / len(parts)
    flawless = sum(g["rating"] == "Flawless" for g in games)
    badges = [
        ("engine", f"{overall:.1f}%", pct_colour(overall)),
        ("system libraries", f"{lib_pct:.2f}%", pct_colour(lib_pct)),
        ("kernel calls", f"{k_pct:.2f}%", pct_colour(k_pct)),
        ("PPU instructions", f"{100 * ppu[0] / ppu[1]:.1f}%", pct_colour(100 * ppu[0] / ppu[1])),
        ("SPU instructions", f"{100 * spu[0] / spu[1]:.1f}%", pct_colour(100 * spu[0] / spu[1])),
        ("games flawless", f"{flawless}/{len(games)}", GREEN if flawless == len(games) else AMBER if flawless else RED),
    ]
    for i, (label, value, colour) in enumerate(badges):
        write(f"badge-{i}.svg", badge_svg(label, value, colour))

    summary = {
        "engine_percent": round(overall, 1),
        "libraries": {"own": lib_own, "answered_as_rpcs3": lib_stub, "known": lib_total},
        "kernel": {"own": k_own, "answered_as_rpcs3": k_stub, "known": k_total},
        "ppu_instructions": {"translated_and_fuzzed": ppu[0], "known": ppu[1]},
        "spu_instructions": {"translated_and_fuzzed": spu[0], "known": spu[1]},
        "checklist": {a[0]: round(100 * a[1]) for a in areas},
        "games": {g["id"]: g["rating"] for g in games},
    }
    write("summary.json", json.dumps(summary, indent=2))

    # README status
    lines = ["<!-- progress:start (generated by scripts/progress.py; do not edit by hand) -->", "## Status", ""]
    lines.append(" ".join(f"![{l}](docs/progress/badge-{i}.svg)" for i, (l, _, _) in enumerate(badges)))
    lines += ["", f"_{data['as_of']}._ {data['headline']}", "",
              "![The engine, part by part](docs/progress/engine.svg)", "",
              "![Games](docs/progress/games.svg)", ""]
    for c in charts:
        lines.append(f"![{c['title']}](docs/progress/{c['file']})")
    lines += ["", "![System libraries](docs/progress/libraries.svg)", "",
              "Green: pspocket's own code. Light green: answered as RPCS3's own stub answers. Grey: not yet pspocket's "
              "own (served by the PS3's own firmware module when the system software is installed, D-175). Counted "
              "against every function RPCS3 registers, not every function the PS3 has.", "",
              "![Kernel calls](docs/progress/kernel.svg)", "",
              f"PPU instructions: {ppu[0]} of {ppu[1]} translated, each checked against the interpreter by the "
              f"differential fuzz tests (the two left out, eciwx and ecowx, are external control instructions games do "
              f"not use). SPU instructions: {spu[0]} of {spu[1]} (the five left out are ISA 1.2 double-precision "
              f"compares the PS3's SPUs do not have).", "",
              "Per game: [docs/compatibility.md](docs/compatibility.md) · runs: [docs/runs](docs/runs/README.md) · "
              "where each part can improve: [docs/improve.md](docs/improve.md)",
              "<!-- progress:end -->"]
    readme = read("README.md")
    block = "\n".join(lines)
    if "<!-- progress:start" in readme:
        readme = re.sub(r"<!-- progress:start.*?<!-- progress:end -->", lambda _: block, readme, flags=re.S)
    else:
        print("README.md has no progress markers", file=sys.stderr)
        return 1
    with open(os.path.join(ROOT, "README.md"), "w", encoding="utf-8") as f:
        f.write(readme)
    print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
