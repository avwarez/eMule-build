#!/usr/bin/env python3
"""Compare two symrepro outputs (Windows reference vs. Wine) line by line.

    compare.py windows.txt wine.txt [--md report.md]

Lines are "symbol|case|value", grouped under "@@ test" headers. A case whose
name starts with '~' depends on the machine (host name, free space, time
zone...) and is listed but never counted as a difference. Everything else
must match byte for byte.
"""
import sys
from collections import OrderedDict, defaultdict


def load(path):
    data = OrderedDict()
    meta = {}
    test = '#run'
    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            line = raw.rstrip('\r\n')
            if line.startswith('@@ '):
                test = line[3:].strip()
                continue
            parts = line.split('|', 2)
            if len(parts) != 3:
                # stray output (a helper's printf, a crash message): keep it
                key = (test, '#stray', str(len(data)))
                data[key] = line
                continue
            sym, case, value = parts
            if test == '#run':
                meta[case] = value
            key = (test, sym, case)
            # A test may legitimately print the same case twice; number them.
            n = 1
            k = key
            while k in data:
                n += 1
                k = (test, sym, case + '#%d' % n)
            data[k] = value
    return data, meta


def main():
    args = sys.argv[1:]
    md = None
    if '--md' in args:
        i = args.index('--md')
        md = args[i + 1]
        del args[i:i + 2]
    ref, refmeta = load(args[0])
    oth, othmeta = load(args[1])
    keys = list(ref.keys()) + [k for k in oth.keys() if k not in ref]
    same = 0
    diffs = defaultdict(list)       # symbol -> [(test, case, refval, othval)]
    envdiffs = defaultdict(list)
    only = []
    symbols = set()
    for k in keys:
        test, sym, case = k
        if test == '#run':
            continue
        symbols.add(sym)
        a, b = ref.get(k), oth.get(k)
        if a is None or b is None:
            only.append((k, a, b))
            continue
        if a == b:
            same += 1
        elif case.startswith('~'):
            envdiffs[sym].append((test, case, a, b))
        else:
            diffs[sym].append((test, case, a, b))
    out = []
    out.append('# symrepro: %s vs %s\n' % (args[0], args[1]))
    out.append('- reference: %s / %s' % (refmeta.get('~platform', '?'), refmeta.get('~arch', '?')))
    out.append('- compared:  %s / %s' % (othmeta.get('~platform', '?'), othmeta.get('~arch', '?')))
    ndiff = sum(len(v) for v in diffs.values())
    out.append('- lines identical: %d, differing: %d (in %d symbols), machine-dependent differing: %d, present on one side only: %d'
               % (same, ndiff, len(diffs), sum(len(v) for v in envdiffs.values()), len(only)))
    out.append('- symbols seen: %d, with a behavioural difference: %d\n' % (len(symbols), len(diffs)))
    if diffs:
        out.append('## Behavioural differences\n')
        for sym in sorted(diffs):
            out.append('### %s (%d)\n' % (sym, len(diffs[sym])))
            out.append('| test | case | Windows | Wine |')
            out.append('|---|---|---|---|')
            for test, case, a, b in diffs[sym]:
                out.append('| %s | `%s` | `%s` | `%s` |' % (test, case.replace('|', '\\|'), a.replace('|', '\\|'), b.replace('|', '\\|')))
            out.append('')
    if only:
        out.append('## Present on one side only\n')
        out.append('| test | symbol | case | Windows | Wine |')
        out.append('|---|---|---|---|---|')
        for (test, sym, case), a, b in only:
            out.append('| %s | %s | `%s` | `%s` | `%s` |' % (test, sym, case, a, b))
        out.append('')
    if envdiffs:
        out.append('## Machine-dependent values (not counted)\n')
        out.append('| symbol | case | Windows | Wine |')
        out.append('|---|---|---|---|')
        for sym in sorted(envdiffs):
            for test, case, a, b in envdiffs[sym]:
                out.append('| %s | `%s` | `%s` | `%s` |' % (sym, case, a[:200], b[:200]))
        out.append('')
    text = '\n'.join(out) + '\n'
    if md:
        with open(md, 'w', encoding='utf-8') as f:
            f.write(text)
    sys.stdout.write(text)


if __name__ == '__main__':
    main()
