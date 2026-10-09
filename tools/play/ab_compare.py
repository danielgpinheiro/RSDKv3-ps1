#!/usr/bin/env python3
"""A/B exactness check of two scene_run.sh runs with the same CHECK frames (docs/28 speed pass 2).
Usage: ab_compare.py OUT_A OUT_B FRAME...
Logic: OUT_state_F.bin (entities, players, script engine, globals) byte-identical.
Pictures: both framebuffers of OUT_F_vram.bin (320 x 240 at y 0 and y 256) pixel-identical.
Prints one line per frame and exits 1 on any difference."""
import struct, sys

a, b, frames = sys.argv[1], sys.argv[2], sys.argv[3:]
bad = 0
for f in frames:
    sa = open('%s_state_%s.bin' % (a, f), 'rb').read()
    sb = open('%s_state_%s.bin' % (b, f), 'rb').read()
    # playerList (after the 0x13a80 bytes of entities, 0x128 bytes) holds two pointers (animationFile, boundEntity):
    # a word that is a RAM address in both builds counts as equal there (the BSS layout moves between builds)
    wa = struct.unpack('<%dI' % (len(sa) // 4), sa)
    wb = struct.unpack('<%dI' % (len(sb) // 4), sb)
    ds = abs(len(sa) - len(sb))
    for i, (x, y) in enumerate(zip(wa, wb)):
        if x != y and not (0x13a80 <= i * 4 < 0x13ba8 and 0x80000000 <= x < 0x80200000 and 0x80000000 <= y < 0x80200000):
            ds += 1
    va = open('%s_%s_vram.bin' % (a, f), 'rb').read()
    vb = open('%s_%s_vram.bin' % (b, f), 'rb').read()
    dp = 0
    for y0 in (0, 256):
        for y in range(y0, y0 + 240):
            o = y * 2048
            if va[o:o + 640] != vb[o:o + 640]:
                ra = struct.unpack('<320H', va[o:o + 640])
                rb = struct.unpack('<320H', vb[o:o + 640])
                dp += sum(1 for x, y2 in zip(ra, rb) if x != y2)
    print('frame %s: state words differing %d (of %d bytes), framebuffer pixels differing %d' % (f, ds, len(sa), dp))
    bad += ds + dp
print('A/B IDENTICAL' if bad == 0 else 'A/B DIFFER')
sys.exit(1 if bad else 0)
