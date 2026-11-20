"""Small leaf functions inlined into their callers (on C++ BackendC already wrote; no change to the AOT
format, its imports or its functions' symbols).

A call between translated functions stores the caller's changed registers to hCPU, calls, the callee
checks its flag and the host stack, loads its registers from hCPU, runs, stores what it changed, returns,
and the caller loads again what it reads: for a getter of a few instructions most of the cost. In
v00.19's profile, functions of at most 16 guest instructions took 14% of the translated code's time.

Here each file gets static always-inline copies (aot_inl_<address>) of the small leaf functions it calls
by name - no calls of their own, at most MAX_BYTES of guest code - and those calls go to the copies.
Inlined, the compiler sees the caller's stores to hCPU and the copy's loads of the same fields (and the
other way round at the return) and drops most of them, and the copy's entry switch folds to its start.
A copy still checks the callee's flag and the host stack as the callee does (a function the loader
switched off still returns its address for the bridge), and keeps its AOT_HWCHECK (hardware accesses).
The functions themselves stay as they were (the jump table, other callers, the bridge's calls).

  patch-aot-inline.py aot <dir> <symbols.txt> [max_bytes]   (default 64: 16 instructions)
Idempotent (a file that already has the copies is left alone)."""
import glob
import os
import re
import sys

MARK = '// (patch-aot-inline.py)'

FLAG_RE = re.compile(r'^extern "C" AOT_VIS uint8 (aot_[0-9a-f]{8})_on;$', re.M)
CALL_RE = re.compile(r'if \((aot_[0-9a-f]{8})\) next = \1\(h, m, (0x[0-9a-f]{8}u)\); else \{ next = aot_imports\.hle\(h, \2\); if \(!next\) return \2; \}')
# calls, tail calls, indirect calls, code run in place, interpreter hand-overs: not a leaf
NOT_LEAF = re.compile(r'next = |musttail|aot_target\(|aot_imports\.(call|leave|hle|yield)\(')


def functions(text):
    """(name, hwcheck, definition text) for each function in a file, in order"""
    out = []
    for m in FLAG_RE.finditer(text):
        name = m.group(1)
        start = text.index('AOT_FN uint32 ' + name + '(', m.end())
        hw = re.search(r'#define AOT_HWCHECK (\d)', text[m.end():start])
        end = text.index('\n}\n', start) + 3
        out.append((name, hw.group(1) if hw else '1', text[start:end]))
    return out


def main():
    if len(sys.argv) < 4 or sys.argv[1] != 'aot':
        sys.exit(__doc__)
    d, symbols = sys.argv[2], sys.argv[3]
    max_bytes = int(sys.argv[4], 0) if len(sys.argv) > 4 else 64
    sizes = {}
    for line in open(symbols):
        m = re.match(r'\S+ = \.text:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)', line)
        if m:
            sizes['aot_%08x' % int(m.group(1), 16)] = int(m.group(2), 16)
    files = sorted(glob.glob(os.path.join(d, 'aot_[0-9]*.cpp')))
    texts = {f: open(f, encoding='utf-8').read() for f in files}
    if any(MARK in t for t in texts.values()):
        print('patch-aot-inline: already done in', d)
        return
    inlinable = {}
    for f in files:
        for name, hw, body in functions(texts[f]):
            if sizes.get(name, 1 << 30) <= max_bytes and not NOT_LEAF.search(body):
                inlinable[name] = (hw, body)
    print(f'patch-aot-inline: {len(inlinable)} leaf functions of at most {max_bytes} bytes of guest code')
    sites = 0
    for f in files:
        t = texts[f]
        used = []

        def repl(m):
            nonlocal sites
            name = m.group(1)
            if name not in inlinable:
                return m.group(0)
            sites += 1
            if name not in used:
                used.append(name)
            return f'next = aot_inl_{name[4:]}(h, m, {m.group(2)});'

        t = CALL_RE.sub(repl, t)
        if not used:
            continue
        # the copies go before the file's first function (its flag), each with its own AOT_HWCHECK
        copies = [MARK + ' small leaf functions this file calls, inlined into the calls']
        for name in used:
            hw, body = inlinable[name]
            copies.append(f'extern "C" AOT_VIS uint8 {name}_on;')
            copies.append('#undef AOT_HWCHECK')
            copies.append(f'#define AOT_HWCHECK {hw}')
            copies.append(body.replace(f'AOT_FN uint32 {name}(', f'static inline __attribute__((always_inline)) uint32 aot_inl_{name[4:]}(', 1).rstrip('\n'))
        first = FLAG_RE.search(t)
        t = t[:first.start()] + '\n'.join(copies) + '\n' + t[first.start():]
        open(f, 'w', encoding='utf-8', newline='\n').write(t)
    print(f'patch-aot-inline: {sites} calls inlined')


if __name__ == '__main__':
    main()
