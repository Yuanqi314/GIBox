"""Names the game methods behind the glow trace's caller addresses.

The trace (debug log level + 追踪游戏对材质的写入) ends each glow-write line with "by A<B<C": RVAs of the code that made the
write, then two frames further up. This maps them to the methods of an Il2CppDumper dump.cs of the same build (the method
whose RVA is the largest one not above the address) and prints each glow-write line with the names added.

Usage: python3 tools/glow_callers.py dump.cs "GIBox Log.txt" [more logs]
"""
import bisect
import re
import sys

RVA = re.compile(r'RVA: 0x([0-9A-Fa-f]+)')
TYPE = re.compile(r'^(?:public|private|internal|protected)?[\w\s]*?(?:class|struct)\s+([^\s:<]+)')


def methods(dump):
    starts, names, owner, pending = [], [], '?', None
    with open(dump, encoding='utf-8', errors='replace') as f:
        for line in f:
            s = line.strip()
            if not s:
                continue
            if s.startswith('//'):
                m = RVA.search(s)
                pending = int(m.group(1), 16) if m else None
                continue
            m = TYPE.match(s)
            if m and '(' not in s:
                owner = m.group(1)
                continue
            if pending is not None:
                name = s.split('(')[0].split()[-1] if '(' in s else s
                starts.append(pending)
                names.append(owner + '.' + name)
                pending = None
    order = sorted(range(len(starts)), key=starts.__getitem__)
    return [starts[i] for i in order], [names[i] for i in order]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    starts, names = methods(sys.argv[1])

    def name(text):
        rva = int(text, 16)
        i = bisect.bisect_right(starts, rva) - 1
        if i < 0 or rva - starts[i] > 0x10000:
            return text + ' (not in the dump)'
        return '%s %s+0x%x' % (text, names[i], rva - starts[i])

    for log in sys.argv[2:]:
        with open(log, encoding='utf-8', errors='replace') as f:
            for line in f:
                if 'glow-write' not in line or ' by 0x' not in line:
                    continue
                head, chain = line.rstrip('\n').rsplit(' by ', 1)
                print(head)
                for part in chain.split('<'):
                    print('    ' + name(part))


if __name__ == '__main__':
    main()
