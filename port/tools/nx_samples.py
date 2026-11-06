#!/usr/bin/env python3
"""What the Switch's busy threads did, from DHD_NX_SAMPLE's samples.bin (port/nx/src/sdl_nx.c).

Usage: nx_samples.py samples.bin dhd.elf [--top N] [--slow MS]

For each thread: its top functions by share of samples, waits included (a thread blocked in the
kernel shows as an svc stub; the caller named is its lr). Then fields slower than MS, and what each
thread did in those fields against all the others. The console's clock is sampled about every
millisecond, so a short function's share is only roughly right.
"""
import argparse
import bisect
import collections
import os
import struct
import subprocess
from pathlib import Path

NAMES = ('game', 'vu', 'gs', 'native')
SAMPLE = struct.Struct('<IIiiI')


def symbols(elf):
    kit = Path(os.environ.get('DEVKITPRO', '/opt/devkitpro'))
    out = subprocess.run([str(kit/'devkitA64/bin/aarch64-none-elf-nm'), '-n', str(elf)], capture_output=True, text=True,
                         errors='replace', check=True).stdout
    table = [(int(p[0], 16), p[2]) for p in (line.split() for line in out.splitlines()) if len(p) == 3 and p[1] in 'Tt']
    return [a for a, _ in table], [n for _, n in table]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('samples', type=Path)
    ap.add_argument('elf', type=Path)
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--slow', type=float, default=22.0)
    a = ap.parse_args()
    data = a.samples.read_bytes()
    magic, anchor = struct.unpack_from('<QQ', data)
    if magic != 0x31534d5044:
        raise SystemExit('not a samples file')
    rows = list(SAMPLE.iter_unpack(data[16:len(data) - (len(data) - 16) % SAMPLE.size]))
    addresses, names = symbols(a.elf)
    link = addresses[names.index('thread_entry')]

    def name(offset):
        i = bisect.bisect_right(addresses, link + offset) - 1
        return names[i] if i >= 0 else '?'

    fields = sorted({r[1] for r in rows})
    seconds = (rows[-1][0] - rows[0][0]) / 1e5
    print('%d samples, %.1f s, fields %d-%d, %.1f fields/s' % (len(rows), seconds, fields[0], fields[-1],
                                                              (fields[-1] - fields[0]) / seconds))
    for thread, title in enumerate(NAMES):
        own = [r for r in rows if r[4] == thread]
        if not own:
            continue
        print('\n%s: %d samples' % (title, len(own)))
        top = collections.Counter(name(r[2]) for r in own)
        callers = collections.defaultdict(collections.Counter)
        for r in own:
            callers[name(r[2])][name(r[3])] += 1
        for function, n in top.most_common(a.top):
            print('  %5.1f%%  %s  (from %s)' % (100 * n / len(own), function[:60], callers[function].most_common(1)[0][0][:40]))
    begin = {}
    for r in rows:
        begin.setdefault(r[1], r[0])
    ms = {f: (begin[g] - begin[f]) / 100 for f, g in zip(fields, fields[1:]) if g == f + 1}
    times = sorted(ms.values())
    if not times:
        return
    print('\nfield time: p50 %.1f ms, p90 %.1f, p99 %.1f, max %.1f' % (times[len(times) // 2], times[len(times) * 9 // 10],
                                                                       times[len(times) * 99 // 100], times[-1]))
    slow = {f for f, t in ms.items() if t >= a.slow}
    print('%d of %d fields at %.0f ms or more; the slowest: %s' % (len(slow), len(ms), a.slow, ', '.join(
        '%d (%.0f)' % x for x in sorted(ms.items(), key=lambda x: -x[1])[:12])))
    for thread, title in enumerate(NAMES):
        in_slow = collections.Counter(name(r[2]) for r in rows if r[4] == thread and r[1] in slow)
        rest = collections.Counter(name(r[2]) for r in rows if r[4] == thread and r[1] not in slow)
        total_slow, total_rest = sum(in_slow.values()), sum(rest.values())
        if not total_slow or not total_rest:
            continue
        print('\n%s, most over-represented in slow fields (share slow, share other):' % title)
        rise = sorted(in_slow, key=lambda f: rest[f] / total_rest - in_slow[f] / total_slow)
        for function in rise[:10]:
            print('  %5.1f%% %5.1f%%  %s' % (100 * in_slow[function] / total_slow, 100 * rest[function] / total_rest, function[:60]))


if __name__ == '__main__':
    main()
