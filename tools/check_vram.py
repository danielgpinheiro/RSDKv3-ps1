#!/usr/bin/python3
"""Check a stage's VRAM residents against the build-time files (phase 3.1).

  tile pages : Data/Stages/<Stage>/16x16Tiles.vram pages vs VRAM X 576-831 (tileset.hh layout)
  atlas      : every block of Data/Sprites/Atlas/<Stage>.atl vs its VRAM rectangle
  palette    : optional PALETTE.bin (GDB dump of activePalette32, 256 x {r,g,b}) entries 0x80-0xFF vs
               the tile GIF's colour table (the .vram header)
Usage: check_vram.py VRAM.bin DATA_DIR STAGE [PALETTE.bin]   (STAGE may carry the atlas suffix: R11A_t)
"""
import struct, sys
import numpy as np


def main():
    vram_path, data, stage = sys.argv[1:4]
    vram = np.frombuffer(open(vram_path, 'rb').read(), '<u2').reshape(512, 1024)
    ok = True
    atlas = stage
    stage = stage[:-2] if stage.endswith('_t') else stage
    t = open('%s/Stages/%s/16x16Tiles.vram' % (data, stage), 'rb').read()
    pal = t[4:4 + 768]
    pages = np.frombuffer(t[4 + 768:], '<u2').reshape(4, 256, 128)
    for p in range(4):
        x, y = (9 + 2 * (p & 1)) * 64, (p >> 1) * 256
        same = np.array_equal(vram[y:y + 256, x:x + 128], pages[p])
        ok &= same
        print('tile page %d at (%d,%d): %s' % (p, x, y, 'identical' if same else 'DIFFERENT'))
    a = open('%s/Sprites/Atlas/%s.atl' % (data, atlas), 'rb').read()
    sheets, clusters, cluts, blocks, fx, fy, off = struct.unpack_from('<6HI', a, 4)
    p = 20 + sheets * 56 + clusters * 16 + cluts * 20
    bad = 0
    for b in range(blocks):
        x, y, w, h, o = struct.unpack_from('<4HI', a, p + 12 * b)
        blk = np.frombuffer(a, '<u2', w * h, off + o).reshape(h, w)
        bad += not np.array_equal(vram[y:y + h, x:x + w], blk)
    ok &= bad == 0
    print('atlas: %d blocks, %d different' % (blocks, bad))
    if len(sys.argv) > 4:
        got = open(sys.argv[4], 'rb').read()
        diff = [c for c in range(0x80, 0x100) if got[c * 3:c * 3 + 3] != pal[c * 3:c * 3 + 3]]
        ok &= not diff
        print('palette 0x80-0xFF vs tile GIF: %s' % ('identical' if not diff else 'DIFFERENT at %s' % diff[:8]))
    print('OK' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
