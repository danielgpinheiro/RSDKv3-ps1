#!/usr/bin/python3
"""Disassemble one object's subs from a Sonic CD bytecode file (debug aid, docs/28 phase 4).

Usage: bytecode_dis.py DATA_DIR FILE OBJECT_INDEX [GLOBALS]   (OBJECT_INDEX: 0-based within the
file; GLOBALS = 1 if the stage loads GS000 first, so offsets are absolute after it)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bytecode_scan as bs


def dis(code, start, names, vars_, limit=100000):
    p = start
    n = 0
    while 0 <= p < len(code) and n < limit:
        at = p
        op = code[p]
        p += 1
        if op >= len(names):
            print('%6d ??? %d' % (at, op))
            break
        name, size = names[op]
        ops = []
        for _ in range(size):
            t = code[p]
            p += 1
            if t == 1:
                arr = code[p]
                p += 1
                idx = ''
                if arr in (1, 2, 3):
                    idx = '[%s%s%d]' % ('+' if arr == 2 else '-' if arr == 3 else '', 'pos' if code[p] == 1 else '', code[p + 1])
                    p += 2
                ops.append(vars_[code[p]].replace('VAR_', '') + idx)
                p += 1
            elif t == 2:
                ops.append(str(code[p]))
                p += 1
            elif t == 3:
                ln = code[p]
                ops.append('"%s"' % ''.join(chr((code[p + 1 + c // 4] >> (24 - 8 * (c % 4))) & 0xFF) for c in range(ln)))
                p += ln // 4 + 2
        print('%6d %s %s' % (at, name, ', '.join(ops)))
        n += 1
        if name == 'End':
            break


def main():
    data, fn, obj = sys.argv[1], sys.argv[2], int(sys.argv[3])
    glob = len(sys.argv) > 4 and sys.argv[4] == '1'
    bc = os.path.join(data, 'Scripts', 'ByteCode')
    code = (bs.ints(os.path.join(bc, 'GS000.bin')) if glob else []) + bs.ints(os.path.join(bc, fn))
    names, vars_ = bs.tables()
    subs = bs.subs(os.path.join(bc, fn))[obj]
    for label, off in zip(('main', 'playerInteraction', 'draw', 'startup'), subs):
        if off == 0x3FFFF:
            continue
        print('--- %s @%d' % (label, off))
        dis(code, off, names, vars_)


if __name__ == '__main__':
    main()
