"""Counters of some functions set to 0 in a text profile (llvm-profdata merge --text), in place.

The PC run's OS idle loop (aot_80387480: the game thread idle ~80% of the time there) counted
~3e14 - a loop whose trip count the instrumented build adds at once - against ~1e8 for the busiest
game functions: left in, the profile's summary would call nothing but it hot. Its counters go to 0
(it is cold on the PS4, where the game thread is busy).

  pgo-zero.py <profile.proftext> <function> [function...]
"""
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path, names = sys.argv[1], set(sys.argv[2:])
    lines = open(path, encoding='utf-8').read().split('\n')
    out, i, done = [], 0, set()
    while i < len(lines):
        line = lines[i]
        out.append(line)
        name = line.split(';')[-1]
        if name in names and i + 5 < len(lines) and lines[i + 1] == '# Func Hash:' and lines[i + 3] == '# Num Counters:' and lines[i + 5] == '# Counter Values:':
            count = int(lines[i + 4])
            out += lines[i + 1:i + 6]
            out += ['0'] * count
            i += 6 + count
            done.add(name)
            continue
        i += 1
    open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    missing = names - done
    print(f'pgo-zero: {len(done)} functions zeroed' + (f', not found: {" ".join(sorted(missing))}' if missing else ''))
    if missing:
        sys.exit(1)


if __name__ == '__main__':
    main()
