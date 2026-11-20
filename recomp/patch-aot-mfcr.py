"""mfcr in fewer instructions (on C++ BackendC already wrote; no change to the AOT format or imports).

The CR is 32 one-byte bits in hCPU and 32 locals in translated code, and mfcr is the IML's 32 steps of
d = d << 1; d = d + bit - 64 dependent operations on 32 loaded (and usually spilled) bytes. CodeWarrior
functions that use CR2-CR4 save the CR with an mfcr in their prologue and restore it with mtcrf: in
v00.19's profile such functions took 9.4% of the game thread (the busiest translated function, 2.6%).

Here an mfcr in the code a function's start entry runs first (its start segment, before any CR bit is
changed: the bits are still the bytes the entry loaded) reads the 32 bytes straight from hCPU as two
SSE2 vectors, moves bit 0 of each into a 32-bit mask (pmovmskb) and reverses it (CR bit 0 is the
word's top bit): ~15 instructions. Any other mfcr becomes the bits shifted into place and ORed together
(the same value: CR bits are 0 or 1) - a tree, not a chain of 64.

  patch-aot-mfcr.py aot <dir>
Idempotent."""
import glob
import os
import re
import sys

MARK = '// (patch-aot-mfcr.py)'
CR_BASE = 132  # offsetof(PPCInterpreter_t, cr)


def rewrite_function(lines, stats):
    """the function's lines with its mfcr chains replaced"""
    # the start entry's CR loads (local -> byte) and the segment it goes to
    start_loads, start_seg = {}, None
    for i, l in enumerate(lines):
        if re.match(r'\tif \(entry == 0x[0-9a-f]{8}u\)$', l) and i + 1 < len(lines) and lines[i + 1] == '\t{':
            for l2 in lines[i + 2:]:
                m = re.match(r'\t\t(i\d+) = \(\*\(uint8\*\)\(h \+ (\d+)\)\);$', l2)
                if m:
                    start_loads[m.group(1)] = int(m.group(2))
                m = re.match(r'\t\tgoto (S\d+);$', l2)
                if m:
                    start_seg = m.group(1)
                    break
            break
    # (the start segment reached only from the entry - no loop or jump back to it - or its CR locals
    # might not be hCPU's bytes any more when the mfcr runs)
    if start_seg:
        jumps = sum(1 for l in lines if re.search(r'goto ' + start_seg + r';', l) and not re.match(r'\t\tgoto ' + start_seg + r';$', l) and
                    not re.match(r'\tcase 0x[0-9a-f]{8}u: goto ' + start_seg + r';$', l))
        if jumps:
            start_seg = None
    out = []
    i = 0
    seg = None
    written_since_start = set()
    while i < len(lines):
        l = lines[i]
        m = re.match(r'(S\d+):$', l)
        if m:
            seg = m.group(1)
            # (the CR locals written since the start segment began; past it any bit may have changed)
            written_since_start = set() if seg == start_seg else None
        m = re.match(r'\t(i\d+) = 0x00000000u;$', l)
        chain = None
        if m and i + 64 < len(lines):
            d = m.group(1)
            bits = []
            for k in range(32):
                a, b = lines[i + 1 + 2 * k], lines[i + 2 + 2 * k]
                mb = re.match(r'\t' + d + r' = ' + d + r' \+ (i\d+);$', b)
                if a != f'\t{d} = {d} << 1;' or not mb:
                    break
                bits.append(mb.group(1))
            if len(bits) == 32:
                chain = (d, bits)
        if chain:
            d, bits = chain
            direct = (seg == start_seg and written_since_start is not None and not (written_since_start & set(bits)) and
                      all(start_loads.get(b) == CR_BASE + k for k, b in enumerate(bits)))
            if direct:
                out.append('\t{ ' + MARK + ' mfcr from hCPU\'s 32 CR bytes')
                out.append('\t\tconst __m128i lo_ = _mm_loadu_si128((const __m128i*)(h + 132)), hi_ = _mm_loadu_si128((const __m128i*)(h + 148));')
                out.append(f'\t\t{d} = __builtin_bitreverse32((uint32)_mm_movemask_epi8(_mm_slli_epi16(lo_, 7)) | (uint32)_mm_movemask_epi8(_mm_slli_epi16(hi_, 7)) << 16); }}')
                stats['direct'] += 1
            else:
                terms = ' | '.join(f'({b} << {31 - k})' for k, b in enumerate(bits))
                out.append(f'\t{d} = {terms}; {MARK}')
                stats['tree'] += 1
            i += 65
            continue
        if written_since_start is not None:
            mw = re.match(r'\t(i\d+) = ', l)
            if mw:
                written_since_start.add(mw.group(1))
            for mw in re.finditer(r'\b(i\d+) = ', l):
                written_since_start.add(mw.group(1))
        out.append(l)
        i += 1
    return out


def main():
    if len(sys.argv) != 3 or sys.argv[1] != 'aot':
        sys.exit(__doc__)
    stats = {'direct': 0, 'tree': 0}
    for f in sorted(glob.glob(os.path.join(sys.argv[2], 'aot_[0-9]*.cpp'))):
        text = open(f, encoding='utf-8').read()
        if MARK in text or '<< 1;\n' not in text:
            continue
        lines = text.split('\n')
        out, fn = [], None
        for l in lines:
            if l.startswith('AOT_FN uint32 aot_'):
                fn = [l]
                continue
            if fn is not None:
                fn.append(l)
                if l == '}':
                    out += rewrite_function(fn, stats)
                    fn = None
                continue
            out.append(l)
        new = '\n'.join(out)
        if new != text:
            open(f, 'w', encoding='utf-8', newline='\n').write(new)
    print(f'patch-aot-mfcr: {stats["direct"]} mfcr from hCPU, {stats["tree"]} as an OR of the bits')


if __name__ == '__main__':
    main()
