#!/usr/bin/env python3
"""The bench's report page (D-1060): one table of every run's verdict, fps and CPU ms a frame by workload, the reasons,
and the frames each run kept (the worst partly black ones).

    scripts/bench_report.py OUT_DIR      writes OUT_DIR/index.html (local: it shows game frames, never published)
"""
import glob
import html
import json
import os
import sys

WORK = ["EDGE/GFX", "dependency_spu", "GraphicsMain", "mixer", "rsx", "ppu_main"]


def main(out):
    rows = []
    for rf in sorted(glob.glob(os.path.join(out, "*", "result.json"))):
        r = json.load(open(rf))
        r["dir"] = os.path.basename(os.path.dirname(rf))
        rows.append(r)
    colour = {"PASS": "#1d7a46", "WARN": "#9a6100", "FAIL": "#b42318"}
    h = ["<!doctype html><meta charset=utf-8><title>pspocket bench</title>",
         "<style>body{font:14px -apple-system,sans-serif;margin:24px;background:#f5f7f8;color:#142027}"
         "table{border-collapse:collapse}td,th{padding:5px 8px;border-bottom:1px solid #d9e0e4;text-align:left;"
         "vertical-align:top}td.n{text-align:right;font-variant-numeric:tabular-nums}img{width:320px;margin:2px}</style>",
         f"<h1>Bench {html.escape(os.path.basename(out.rstrip('/')))}</h1>",
         "<table><tr><th>case</th><th>verdict</th><th>fps</th><th>CPU ms</th><th>draws</th>"
         + "".join(f"<th>{w}</th>" for w in WORK) + "<th>why</th></tr>"]
    for r in rows:
        b = r.get("budget") or {}
        wm = b.get("work_ms", {})
        h.append(f"<tr><td>{html.escape(r['dir'])}</td><td style='color:{colour[r['verdict']]};font-weight:600'>"
                 f"{r['verdict']}</td><td class=n>{b.get('fps', 0):.1f}</td><td class=n>{b.get('cpu_ms', 0):.1f}</td>"
                 f"<td class=n>{b.get('draws', 0):.0f}</td>"
                 + "".join(f"<td class=n>{wm.get(w, 0):.1f}</td>" for w in WORK)
                 + f"<td>{html.escape('; '.join(r['fails'] + r['warns']) or 'ok')}</td></tr>")
    h.append("</table>")
    for r in rows:
        kept = r.get("frames", {}).get("kept", [])
        if kept:
            h.append(f"<h2>{html.escape(r['dir'])}</h2>")
            h += [f"<img src='{html.escape(r['dir'])}/frames/{html.escape(k)}' title='{html.escape(k)}'>" for k in kept]
    open(os.path.join(out, "index.html"), "w").write("\n".join(h))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
