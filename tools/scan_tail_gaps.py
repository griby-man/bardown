#!/usr/bin/env python3
# functions hidden after a tail branch: inside a known function's span, an unconditional b or blr
# followed by code that no branch of the function reaches and that codegen never labelled (jump table
# arms get loc_ labels). codegen folds these into the function before, so a call through a pointer
# lands on an unregistered address.
# usage: scan_tail_gaps.py <image.bin> <bardown_init.cpp> <loc labels file> [functions.toml]
import re, struct, sys

BASE = 0x82000000
MFLR_R12 = 0x7D8802A6

img = open(sys.argv[1], 'rb').read()
starts = sorted({int(m, 16) for m in re.findall(r'\{ 0x([0-9A-F]{8}), sub_', open(sys.argv[2]).read())})
known = set(starts)
labels = {int(l, 16) for l in open(sys.argv[3]).read().split()}
if len(sys.argv) > 4:
    known |= {int(m, 16) for m in re.findall(r'^0x([0-9A-Fa-f]{8})\s*=', open(sys.argv[4]).read(), re.M)}

def word(a):
    o = a - BASE
    return struct.unpack('>I', img[o:o + 4])[0] if 0 <= o < len(img) - 3 else None

def target(a, w):
    op = w >> 26
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000: li -= 0x04000000
        return (li if w & 2 else a + li) & 0xFFFFFFFF
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000: bd -= 0x10000
        return (bd if w & 2 else a + bd) & 0xFFFFFFFF
    return None

found = []
for i, s in enumerate(starts):
    end = starts[i + 1] if i + 1 < len(starts) else s
    if end - s > 0x40000: continue
    reach = s  # furthest address any branch inside reaches so far
    a = s
    while a < end:
        w = word(a)
        if w is None: break
        t = target(a, w)
        if t is not None and s <= t < end and not (w & 1): reach = max(reach, t)
        ends = w == 0x4E800020 or (w >> 26 == 18 and not (w & 1))
        if ends and a + 4 < end and reach <= a:
            n = a + 4
            while n < end and word(n) == 0: n += 4  # padding
            if n < end and n not in known and n not in labels and n > reach:
                found.append((n, s))
                break
        a += 4
for n, s in found:
    print(f'0x{n:08X} = {{ name = "sub_{n:08X}" }}  # after a tail branch ending sub_{s:08X}')
print(f'# {len(found)} found', file=sys.stderr)
