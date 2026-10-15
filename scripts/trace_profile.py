#!/usr/bin/env python3
"""Summarise an Instruments Time Profiler trace (xctrace) by thread.

    scripts/trace_profile.py TRACE                 every thread's share of the samples
    scripts/trace_profile.py TRACE THREAD [N]      one thread (a substring of its name): the N functions with the
                                                   most samples of their own and the N with the most in all

Used for scripts/ios.sh perftrace on the iPhone and xctrace recordings of pspocket-boot on the Mac (the plan for a
steady 30 fps, item 7). The trace is the user's: it stays in the gitignored inventory/.
"""
import collections
import subprocess
import sys
import xml.etree.ElementTree as ET


def rows(trace):
    xml = subprocess.run(['xcrun', 'xctrace', 'export', '--input', trace, '--xpath',
                          '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'],
                         capture_output=True, text=True, check=True).stdout
    ids = {}

    def resolve(e):  # xctrace writes an element once with an id, then refers to it
        if e.get('ref') is not None:
            return ids[e.get('ref')]
        if e.get('id') is not None:
            ids[e.get('id')] = e
        for c in e:
            resolve(c)
        return e

    for row in ET.fromstring(xml).iter('row'):
        thread = backtrace = None
        for c in row:
            r = resolve(c)
            if c.tag == 'thread':
                thread = r
            elif c.tag == 'backtrace':
                backtrace = r
        if thread is None:
            continue
        frames = []
        if backtrace is not None:
            for f in backtrace.iter('frame'):
                f = ids.get(f.get('ref'), f) if f.get('ref') else f
                frames.append(f.get('name', '?'))
        yield thread.get('fmt', '').split(' 0x')[0], frames


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    trace = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 else None
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    threads = collections.Counter()
    own, total = collections.Counter(), collections.Counter()
    samples = 0
    for thread, frames in rows(trace):
        threads[thread] += 1
        if want is None or want not in thread:
            continue
        samples += 1
        if frames:
            own[frames[0]] += 1
        for f in set(frames):
            total[f] += 1
    if want is None:
        all_samples = sum(threads.values()) or 1
        for t, c in threads.most_common(40):
            print(f'{100 * c / all_samples:5.1f}%  {c:7d}  {t}')
        return
    if not samples:
        sys.exit(f'no samples of a thread named like "{want}"')
    print(f'{samples} samples of "{want}"')
    print('--- their own')
    for k, c in own.most_common(n):
        print(f'{100 * c / samples:5.1f}%  {k[:150]}')
    print('--- in all')
    for k, c in total.most_common(n):
        print(f'{100 * c / samples:5.1f}%  {k[:150]}')


if __name__ == '__main__':
    main()
