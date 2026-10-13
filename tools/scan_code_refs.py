#!/usr/bin/env python3
"""find code addresses referenced from data that the recompiler has no function for.

these are the indirect-call targets (callbacks, vtables without rtti, function tables)
that only show up at runtime as "call to invalid or unregistered function".

  BARDOWN_DUMP_IMAGE=image.bin scripts/run.sh   # dumps the loaded xex, then quit
  tools/scan_code_refs.py image.bin generated/default/bardown_init.cpp

prints manifest lines for candidates that look like real function entries, with the
evidence, so each one can be checked before it goes into bardown_functions.toml.
"""
import re
import struct
import sys
from collections import defaultdict

IMAGE_BASE = 0x82000000
CODE_START, CODE_END = 0x82440000, 0x8396F798

BLR = 0x4E800020
BCTR = 0x4E800420
MTCTR_R11 = 0x7D6903A6
MFLR_R12 = 0x7D8802A6


def word(img, addr):
    off = addr - IMAGE_BASE
    return struct.unpack_from(">I", img, off)[0]


def is_stwu_r1(w):
    return (w & 0xFFFF0000) == 0x94210000


def is_uncond_branch(w):
    # b / ba (no link): opcode 18, LK=0
    return (w >> 26) == 18 and (w & 1) == 0


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img = open(sys.argv[1], "rb").read()
    known = {int(m, 16) for m in re.findall(r"\{ 0x([0-9A-F]{8}), sub_", open(sys.argv[2]).read())}
    print(f"# {len(known)} known functions", file=sys.stderr)

    refs = defaultdict(list)
    for off in range(0, len(img) - 3, 4):
        src = IMAGE_BASE + off
        if CODE_START <= src < CODE_END:
            continue  # instructions, not data
        v = struct.unpack_from(">I", img, off)[0]
        if CODE_START <= v < CODE_END and v % 4 == 0 and v not in known:
            refs[v].append(src)

    # callbacks built in code: lis rD,hi then addi/ori rD,rD,lo a few instructions later
    for src in range(CODE_START, CODE_END - 4, 4):
        w = word(img, src)
        if (w >> 26) != 15 or (w >> 16) & 0x1F != 0:  # lis = addis rD,0,imm
            continue
        rd = (w >> 21) & 0x1F
        hi = (w & 0xFFFF) << 16
        for nxt in range(src + 4, min(src + 32, CODE_END), 4):
            n = word(img, nxt)
            op, nd, na = n >> 26, (n >> 21) & 0x1F, (n >> 16) & 0x1F
            if op == 14 and nd == rd and na == rd:  # addi rD,rD,lo (signed)
                lo = n & 0xFFFF
                v = (hi + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
            elif op == 24 and nd == rd and na == rd:  # ori rA,rS,lo
                v = hi | (n & 0xFFFF)
            else:
                if nd == rd and op not in (36, 37, 38, 44, 45, 54, 55):  # rD clobbered by something else
                    break
                continue
            if CODE_START <= v < CODE_END and v % 4 == 0 and v not in known:
                refs[v].append(src)
            break

    out = []
    for target, srcs in sorted(refs.items()):
        first = word(img, target)
        prev = word(img, target - 4)
        if word(img, target + 8) == MTCTR_R11 and word(img, target + 12) == BCTR:
            continue  # import thunk: two import-record words patched at load, then mtctr/bctr
        entry_like = first == MFLR_R12 or is_stwu_r1(first)
        boundary = prev in (BLR, BCTR, 0) or is_uncond_branch(prev)
        if not (entry_like or boundary):
            continue
        why = []
        if first == MFLR_R12:
            why.append("mflr r12")
        elif is_stwu_r1(first):
            why.append("stwu r1")
        if prev == BLR:
            why.append("after blr")
        elif prev == BCTR:
            why.append("after bctr")
        elif prev == 0:
            why.append("after padding")
        elif is_uncond_branch(prev):
            why.append("after b")
        grade = "strong" if entry_like and boundary else "weak"
        srcs_s = ",".join(f"{s:08X}" for s in srcs[:4]) + ("..." if len(srcs) > 4 else "")
        out.append((grade, target, f'0x{target:08X} = {{ name = "sub_{target:08X}" }}  # {grade}: {", ".join(why)}; data refs {srcs_s}'))

    for grade in ("strong", "weak"):
        rows = [r for r in out if r[0] == grade]
        print(f"# {grade}: {len(rows)}")
        for _, _, line in rows:
            print(line)


if __name__ == "__main__":
    main()
