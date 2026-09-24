#!/usr/bin/env python3
"""Sonic CD PS1 feasibility report (docs/28 phase 1): SPU RAM, disc size, script/stage RAM.

(VRAM for sprites: tools/atlas/build_atlas.py DATA MANIFESTS --report.)

- SPU RAM: every stage's global (GameConfig) + stage (StageConfig) SFX as SPU-ADPCM (16 bytes per
  28 samples, mono) at 44.1 / 32 / 22.05 / 18.9 kHz, against the 520,160 B the Nexus port has
  free for samples (0x1000..0x7FFE0).
- Disc: music as 37.8 kHz 4-bit stereo CD-XA, 8 channels interleaved at 2x (a file lasts as long
  as its longest channel; 150 sectors/s), tracks packed longest-first into the least-filled
  channel; video = one sample encoded with psxavenc (-t str, the chosen 320x176 @ 15 fps)
  extrapolated to every .ogv; data = the non-source files of the Data tree.
- RAM: bytecode ints per stage (global GS000 + the stage's own file) vs SCRIPTDATA/JUMPTABLE,
  and the largest act/background layer sizes vs the TILELAYER arrays.

Usage: feasibility.py DATA_DIR VIDEOS_DIR [--psxavenc PATH]
"""
import glob, os, struct, subprocess, sys, tempfile, wave

SPU_FREE = 0x7FFE0 - 0x1000
RATES = (44100, 32000, 22050, 18900)
SECTOR = 2352


class Reader:
    def __init__(self, path):
        self.d, self.p = open(path, 'rb').read(), 0

    def u8(self):
        self.p += 1
        return self.d[self.p - 1]

    def s(self):
        n = self.u8()
        self.p += n
        return self.d[self.p - n:self.p].decode('latin1')

    def u32(self):
        self.p += 4
        return struct.unpack_from('<I', self.d, self.p - 4)[0]


def game_config(data):
    r = Reader(os.path.join(data, 'Game', 'GameConfig.bin'))
    r.s(), r.s(), r.s()
    n = r.u8()
    [r.s() for _ in range(n)], [r.s() for _ in range(n)]
    for _ in range(r.u8()):
        r.s(); r.p += 4
    sfx = [r.s() for _ in range(r.u8())]
    [r.s() for _ in range(r.u8())]
    lists = []
    for c in range(4):
        lists.append([(r.s(), r.s(), r.s(), r.u8())[0] for _ in range(r.u8())])
    return sfx, lists


def stage_config(data, folder):
    r = Reader(os.path.join(data, 'Stages', folder, 'StageConfig.bin'))
    globals_ = r.u8()
    r.p += 96
    n = r.u8()
    [r.s() for _ in range(n)], [r.s() for _ in range(n)]
    return globals_, [r.s() for _ in range(r.u8())]


def adpcm_bytes(path, rate):
    with wave.open(path) as w:
        n = w.getnframes() * rate // w.getframerate()
    return (n + 27) // 28 * 16 + 16


def bytecode_ints(path):
    r = Reader(path)

    def count():
        total = left = r.u32()
        while left > 0:
            h = r.u8()
            n = h & 0x7F
            r.p += n * (4 if h >= 0x80 else 1)
            left -= n
        return total
    return count(), count()


def layers(data, folder):
    out = []
    for act in sorted(glob.glob(os.path.join(data, 'Stages', folder, 'Act*.bin'))):
        r = Reader(act)
        r.s(); r.p += 5
        out.append(('fg', r.u8(), r.u8()))
    bg = os.path.join(data, 'Stages', folder, 'Backgrounds.bin')
    if os.path.exists(bg):
        r = Reader(bg)
        n = r.u8()
        for _ in range(2):  # h / v parallax entries, 4 bytes each (count read first: see +=)
            count = r.u8()
            r.p += 4 * count
        for _ in range(n):
            xs, ys = r.u8(), r.u8()
            r.p += 4
            while True:
                b = r.u8()
                if b == 0xFF:
                    if r.u8() == 0xFF:
                        break
                    r.p += 1
            r.p += xs * ys * 2
            out.append(('bg', xs, ys))
    return out


def duration(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', path],
                         capture_output=True, text=True).stdout.strip()
    return float(out or 0)


def main():
    a = sys.argv[1:]
    data, videos = a[0], a[1]
    psxavenc = a[a.index('--psxavenc') + 1] if '--psxavenc' in a else 'psxavenc'
    gsfx, lists = game_config(data)
    folders = sorted({f for l in lists for f in l})
    print('## SPU RAM (sample area %d B)' % SPU_FREE)
    gl = {rt: sum(adpcm_bytes(os.path.join(data, 'SoundFX', s), rt) for s in gsfx) for rt in RATES}
    print('global set: %d SFX, %s' % (len(gsfx), ', '.join('%d Hz %d B' % (rt, gl[rt]) for rt in RATES)))
    worst, best_rates = None, {}
    for f in folders:
        glob_, ssfx = stage_config(data, f)
        for rt in RATES:
            need = (gl[rt] if glob_ else 0) + sum(adpcm_bytes(os.path.join(data, 'SoundFX', s), rt) for s in ssfx)
            if need <= SPU_FREE:
                best_rates[f] = (rt, need, len(ssfx))
                break
        else:
            best_rates[f] = (None, need, len(ssfx))
    for rt in RATES + (None,):
        st = [f for f, v in best_rates.items() if v[0] == rt]
        if st:
            print('  best rate %s: %d stages %s' % (rt or 'NONE FITS', len(st), ' '.join(st) if len(st) < 12 else ''))
    big = max(best_rates.items(), key=lambda kv: kv[1][1] if kv[1][0] == RATES[-1] or kv[1][0] is None else 0)

    print('\n## Disc')
    music = sorted(glob.glob(os.path.join(data, 'Music', '**', '*.ogg'), recursive=True))
    durs = sorted((duration(m) for m in music), reverse=True)
    chans = [0.0] * 8
    for d in durs:
        chans[chans.index(min(chans))] += d
    xa_sectors = int(max(chans) * 150) + 8 * len(durs)
    print('music: %d tracks, %.1f min; one 8-channel XA file: longest channel %.1f min -> %d sectors = %.0f MB'
          % (len(durs), sum(durs) / 60, max(chans) / 60, xa_sectors, xa_sectors * SECTOR / 1e6))
    vids = sorted(glob.glob(os.path.join(videos, '*.ogv')))
    vtot = sum(duration(v) for v in vids)
    sample = min(vids, key=duration)
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, 's.str')
        subprocess.run([psxavenc, '-q', '-t', 'str', '-v', 'v2', '-f', '37800', '-b', '4', '-c', '2', '-s', '320x176',
                        '-r', '15', '-x', '2', sample, out], check=True)
        per_s = os.path.getsize(out) / 2336 * SECTOR / duration(sample)
    print('video: %d files, %.1f min; sample %s at 320x176 @ 15 fps = %.0f KB/s -> %.0f MB'
          % (len(vids), vtot / 60, os.path.basename(sample), per_s / 1000, per_s * vtot / 1e6))
    src = ('.ogg', '.wav', '.gif', '.txt')
    dsize = sum(os.path.getsize(p) for p in glob.glob(os.path.join(data, '**', '*'), recursive=True)
                if os.path.isfile(p) and not p.endswith(src))
    total = xa_sectors * SECTOR + per_s * vtot + dsize + 30e6  # + atlases/strips/.vag estimate
    print('data (non-source files) %.0f MB + converted atlases/strips/SFX ~30 MB -> disc ~%.0f MB of 650'
          % (dsize / 1e6, total / 1e6))

    print('\n## RAM')
    bc = os.path.join(data, 'Scripts', 'ByteCode')
    g = bytecode_ints(os.path.join(bc, 'GS000.bin'))
    rows = []
    for li, l in enumerate(lists):
        for pos, f in enumerate(l):
            p = os.path.join(bc, '%sS%03d.bin' % ('PRSB'[li], pos))  # GameConfig order: P, R, S, B lists
            if os.path.exists(p):
                sc, jt = bytecode_ints(p)
                rows.append((f, sc + g[0], jt + g[1]))
    mx_sc = max(rows, key=lambda r: r[1])
    mx_jt = max(rows, key=lambda r: r[2])
    print('bytecode: global %d + stage -> max %d script ints (%s, %d KB), max %d jump ints (%s); arrays '
          'SCRIPTDATA 0x40000 (1 MB), JUMPTABLE 0x4000' % (g[0], mx_sc[1], mx_sc[0], mx_sc[1] * 4 // 1024,
                                                           mx_jt[2], mx_jt[0]))
    ly = [(f,) + l for f in folders for l in layers(data, f)]
    fg = max((l for l in ly if l[1] == 'fg'), key=lambda l: l[3])
    bg = max((l for l in ly if l[1] == 'bg'), key=lambda l: l[3])
    wmax = max(l[2] for l in ly)
    nbg = max(sum(1 for l in layers(data, f) if l[0] == 'bg') for f in folders)
    print('layers: max width %d chunks; tallest FG %d chunks (%s), tallest BG %d (%s); max BG layers per stage %d; '
          'arrays 9 x 256 x 256 chunks (1.2 MB) + 9 x 32 KB line scroll' % (wmax, fg[3], fg[0], bg[3], bg[0], nbg))


if __name__ == '__main__':
    main()
