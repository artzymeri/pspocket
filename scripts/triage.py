#!/usr/bin/env python3
"""Sorts a run into buckets with the next action for each (D-1043).

    scripts/triage.py [--summary SUMMARY.json] [LOG ...] [--json]

Reads pspocket-boot's --summary (every way a run ends writes one: its timeout, the game's exit, a fault, a fatal
error, a stall) and/or run logs: pspocket-boot's run.log on the Mac or the iPhone app's game-*.log (scripts/ios.sh
pull-log, inventory/phone/live). Prints the buckets most serious first, each with its evidence and what to do next;
--json prints them as JSON (scripts/shake.sh reads it). Exit 0 when nothing but "clean" was found, 1 otherwise.
Game output: keep logs and summaries in inventory/, never in the repository (CLAUDE.md rule 1).
"""

import argparse
import json
import re
import sys

# Bucket: severity (lower first), title, next action.
BUCKETS = {
    "fault": (0, "a fault in guest or host code",
              "read the fault's line and the thread report; rerun with --dump-on-fault FILE to see the memory, and "
              "the call site lines (the HLE function the PPU was in, its arguments)"),
    "fatal": (0, "a fatal error (fail loudly, CLAUDE.md rule 5)",
              "the message says what was not handled (an indirect target, an SPU hash, an RSX method): implement it "
              "or translate what is missing"),
    "deadlock": (1, "a deadlock: every PPU thread blocked for good, the SPUs and the RSX idle",
                 "the thread report says what each thread waits on: find the wake that never came (a missing "
                 "event, a lost signal, a waker-first race, D-999/D-1041); rerun with the same seed under "
                 "scripts/shake.sh to see whether it is a race"),
    "freeze": (1, "a freeze: frames came, then none for 10 s",
               "the thread report at the freeze: the RSX blocked on a semaphore (a label the PPU never wrote), a PPU "
               "thread spinning, or a wait that never ended"),
    "livelock": (2, "a livelock: frames come, the picture stays",
                 "the game runs but does not move on: a load that never ends, an ignored controller, a cellSync "
                 "spin (the phone's AC 'stuck on loading'); look at what the PPU threads do across two reports "
                 "(--report-every)"),
    "unimplemented": (2, "unimplemented library functions or syscalls the game called",
                      "implement them (rule 4: the game uses them), checked against Sony's module where there is "
                      "one (tests/hle/*_compare.cpp)"),
    "rsx": (3, "RSX failures (unhandled methods, FIFO errors)", "implement the method or fix the FIFO parsing; "
            "--trace-frame N shows the frame"),
    "spu_interpreted": (3, "SPU code interpreted (no translation)",
                        "translate the images: scripts/lift-spu.sh over --spu-dump DIR, or the session translator "
                        "(--spu-translate)"),
    "ppu_interpreted": (3, "PPU code interpreted (no translation)",
                        "translate it: --ppu-landings FILE, then pspocket-prepare --ppu-continuations FILE (D-255)"),
    "stubs": (4, "functions answered as RPCS3's stubs", "check whether the game needs more than the stub's answer"),
    "slow": (4, "slow: frames under 25 a second for most of the run", "profile the scene (docs/improve.md)"),
    "footprint": (1, "the memory footprint passed --max-footprint (a leak)",
                  "the 5 s lines' memory says when it began to grow; the Metal backend's window lines (textures, "
                  "surfaces, GPU MB) and alarms say what grew; A/B the switch of what changed (D-1047)"),
    "unknown_exit": (1, "the run ended without a summary or a reason", "rerun with --summary; look at the log's end"),
    "clean": (9, "nothing found", "none"),
}

RE_UNIMPL_FN = re.compile(r"UNIMPLEMENTED (\S+::\S+) \((0x[0-9a-fA-F]+)\)")
RE_UNIMPL_SC = re.compile(r"UNIMPLEMENTED syscall (\d+) \(([^)]*)\)")
RE_STUB = re.compile(r"STUB (\S+::\S+) \((0x[0-9a-fA-F]+)\)")
RE_FATAL = re.compile(r"\[fatal\] (.*)")
RE_STALL = re.compile(r"(?:\[warn\] (?:watch|game): )?(freeze|deadlock|livelock): (.*)")
RE_FPS = re.compile(r"(\d+\.\d) fps \(")
RE_SPU_INTERP = re.compile(r"(?:no lifted program: interpreted|SPU interpreted: (\d+))")
RE_PPU_INTERP = re.compile(r"interpreted in [\d.]+ s: (\d+) PPU and (\d+) SPU")


class Triage:
    def __init__(self):
        self.found = {}  # bucket -> list of evidence lines (in order, unique)

    def add(self, bucket, evidence):
        lines = self.found.setdefault(bucket, [])
        if evidence and evidence not in lines and len(lines) < 12:
            lines.append(evidence)
        elif not evidence and not lines:
            pass

    def summary(self, s):
        exit_ = s.get("exit", "")
        detail = s.get("exit_detail", "")
        if exit_ == "fault":
            self.add("fault", detail or "the run ended on a fault")
        elif exit_ == "fatal":
            self.add("fatal", detail or "the run ended on a fatal error")
        elif exit_ == "footprint":
            self.add("footprint", detail)
        elif exit_ in ("deadlock", "freeze", "livelock"):
            self.add(exit_, detail)
        elif exit_ not in ("timeout", "exited"):
            self.add("unknown_exit", f"exit {exit_!r}")
        for st in s.get("stalls", []):
            self.add(st.get("kind", "freeze"), f"{st.get('title', '')} (at {st.get('at', 0)} s)")
        for f in s.get("unimplemented", []):
            self.add("unimplemented", f"{f['name']} ({f['nid']}): {f['calls']} calls")
        if s.get("unimplemented_syscalls"):
            self.add("unimplemented", f"{s['unimplemented_syscalls']} unimplemented syscall calls")
        for f in s.get("stubs", []):
            self.add("stubs", f"{f['name']} ({f['nid']}): {f['calls']} calls")
        if s.get("rsx_failures"):
            self.add("rsx", f"{s['rsx_failures']} RSX failures")
        loads = s.get("spu_loads", {})
        spu_ins = s.get("spu_interpreted", {}).get("instructions", 0)
        if loads.get("interpreted") or spu_ins:
            self.add("spu_interpreted", f"{loads.get('interpreted', 0)} SPU loads interpreted, {spu_ins} instructions")
        ppu = s.get("ppu_interpreted", {})
        for owner, n in ppu.get("by_owner", {}).items():
            if n:
                self.add("ppu_interpreted", f"{n} instructions in {owner}")
        fps = s.get("fps", [])
        if len(fps) >= 4:
            ordered = sorted(fps)
            median = ordered[len(ordered) // 2]
            if median < 25:
                self.add("slow", f"median {median:.1f} fps over {len(fps)} windows")

    def log(self, path):
        fps = []
        try:
            text = open(path, errors="replace").read().splitlines()
        except OSError as e:
            self.add("unknown_exit", f"cannot read {path}: {e}")
            return
        saw_summary_line = False
        for line in text:
            if m := RE_UNIMPL_FN.search(line):
                self.add("unimplemented", f"{m.group(1)} ({m.group(2)})")
            elif m := RE_UNIMPL_SC.search(line):
                self.add("unimplemented", f"syscall {m.group(1)} ({m.group(2)})")
            elif m := RE_STUB.search(line):
                self.add("stubs", f"{m.group(1)} ({m.group(2)})")
            if m := RE_FATAL.search(line):
                msg = m.group(1).strip()
                kind = "fault" if ("SIGBUS" in msg or "SIGSEGV" in msg) else "fatal"
                self.add(kind, msg[:240])
            if m := RE_STALL.search(line):
                title = m.group(2)
                # The report's first line is the title; its other lines are the threads.
                if any(k in title for k in ("no frame for", "blocked without a timeout", "show the same picture")):
                    self.add(m.group(1), title[:240])
            if m := RE_FPS.search(line):
                fps.append(float(m.group(1)))
            if "has no lifted program: interpreted" in line:
                self.add("spu_interpreted", line.strip()[:200])
            if m := RE_PPU_INTERP.search(line):
                if int(m.group(1)):
                    self.add("ppu_interpreted", line.strip()[:200])
            if line.startswith("summary"):
                saw_summary_line = True
        if len(fps) >= 4:
            ordered = sorted(fps)
            median = ordered[len(ordered) // 2]
            if median < 25:
                self.add("slow", f"median {median:.1f} fps over {len(fps)} windows ({path})")
        _ = saw_summary_line

    def result(self):
        if not self.found:
            self.found["clean"] = []
        out = []
        for bucket in sorted(self.found, key=lambda b: (BUCKETS[b][0], b)):
            sev, title, action = BUCKETS[bucket]
            out.append({"bucket": bucket, "severity": sev, "title": title, "next": action,
                        "evidence": self.found[bucket]})
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--summary", action="append", default=[], help="pspocket-boot --summary JSON (repeatable)")
    ap.add_argument("logs", nargs="*", help="run logs (Mac run.log, phone game-*.log)")
    ap.add_argument("--json", action="store_true", help="print the buckets as JSON")
    args = ap.parse_args()
    if not args.summary and not args.logs:
        ap.error("give a --summary, a log, or both")
    t = Triage()
    for path in args.summary:
        try:
            t.summary(json.load(open(path)))
        except (OSError, ValueError) as e:
            t.add("unknown_exit", f"cannot read the summary {path}: {e}")
    for path in args.logs:
        t.log(path)
    result = t.result()
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for b in result:
            print(f"[{b['bucket']}] {b['title']}")
            for e in b["evidence"]:
                print(f"    {e}")
            print(f"    next: {b['next']}")
    return 0 if [b["bucket"] for b in result] == ["clean"] else 1


if __name__ == "__main__":
    sys.exit(main())
