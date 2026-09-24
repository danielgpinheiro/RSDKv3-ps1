#!/usr/bin/python3
"""Build-time asset pipeline for the RSDKv3 (Sonic CD) PS1 port: game data -> the disc's Data/ tree.

Steps (golden rule: nothing is decoded at runtime; every converter checks its own output):
  1. data     tools/extract_rsdk.py Data.rsdk -> loose tree (or copy an extracted Data/ tree)
  2. manifest tools/rsdkmanifest (host build of the engine's bytecode VM) -> build/manifest/*.txt
  3. atlas    tools/atlas/build_atlas.py -> Data/Sprites/Atlas/<Stage>.atl (per-stage VRAM sprites)
  4. tiles    tools/tiles/convert_tiles.py -> Data/Stages/*/16x16Tiles.vram (VRAM tile pages)
The PC source files stay in the tree (.gif .wav .ogg); tools/disc/build_disc.py leaves them off the
disc. Later phases add audio (SPU-ADPCM, CD-XA) and video (STR) steps.

Output: OUT/build/iso/Data/** and OUT/build/manifest/ (OUT = the repo by default, so `make disc`
works on it). Needs: a C++17 compiler (CXX), Python with Pillow + numpy.

Usage: build_assets.py (--rsdk PATH/Data.rsdk | --data PATH/Data) [--out DIR]
"""
import argparse, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..'))
PY = sys.executable
# Stages whose sprites don't fit the VRAM sprite area yet (docs/28 phase 1: the Secrets gallery is
# 4 page-columns over; handled in phase 4). The build continues without their atlas.
ATLAS_KNOWN_OVER = {'Secrets'}


def run(step, cmd, **kw):
    print('[%s] %s' % (step, ' '.join(os.path.relpath(c, REPO) if os.path.isabs(c) else c for c in cmd)), flush=True)
    return subprocess.run(cmd, check=kw.pop('check', True), **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument('--rsdk', help="Sonic CD's Data.rsdk (Steam)")
    src.add_argument('--data', help='an already extracted Data/ directory')
    ap.add_argument('--out', default=REPO, help='output root (gets build/iso/Data and build/manifest)')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    iso, manifest = os.path.join(out, 'build', 'iso'), os.path.join(out, 'build', 'manifest')
    for d in (iso, manifest):
        if os.path.exists(d):
            shutil.rmtree(d)
    os.makedirs(iso)

    if a.rsdk:
        run('1 data', [PY, os.path.join(HERE, 'extract_rsdk.py'), os.path.abspath(a.rsdk), iso])
    else:
        print('[1 data] copy %s' % a.data, flush=True)
        shutil.copytree(a.data, os.path.join(iso, 'Data'), ignore=shutil.ignore_patterns('.DS_Store'))
    data = os.path.join(iso, 'Data')

    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++') or 'c++'
    run('2 manifest', ['make', '-s', '-C', os.path.join(HERE, 'rsdkmanifest'), 'CXX=' + cxx])
    # One manifest + atlas set per selectable player (docs/28 3.2: Sonic and Tails don't fit one atlas):
    # player list position 0 = Sonic -> <Stage>.atl, 1 = Tails -> <Stage>_t.atl (the act's placed
    # Player Object loads the player's .ani; ps1/sprite_atlas picks the file by playerListPos).
    for player, suffix in ((0, ''), (1, '_t')):
        mdir = manifest + suffix
        if os.path.exists(mdir):
            shutil.rmtree(mdir)
        run('2 manifest', [os.path.join(HERE, 'rsdkmanifest', 'rsdkmanifest'), iso, mdir, str(player)], stdout=subprocess.DEVNULL)
        r = run('3 atlas', [PY, os.path.join(HERE, 'atlas', 'build_atlas.py'), data, mdir, '--suffix', suffix],
                check=False, capture_output=True, text=True)
        errors = [l for l in r.stdout.splitlines() if l.startswith('ERROR')]
        unexpected = [l for l in errors if l.split()[1].rstrip(':') not in ATLAS_KNOWN_OVER]  # 'ERROR <stage>: ...'
        atl = len([f for f in os.listdir(os.path.join(data, 'Sprites', 'Atlas')) if f.endswith(suffix + '.atl')
                   and (suffix or not f.endswith('_t.atl'))])
        print('[3 atlas] player %d: %d %s.atl written; known over budget: %s' % (player, atl, '<Stage>' + suffix,
                                                                           ', '.join(sorted(ATLAS_KNOWN_OVER))))
        if unexpected or (r.returncode and not errors):
            sys.exit('atlas failed:\n' + '\n'.join(unexpected or [r.stdout[-2000:], r.stderr[-2000:]]))

    run('4 tiles', [PY, os.path.join(HERE, 'tiles', 'convert_tiles.py'), data])
    print('assets ready in %s' % data)


if __name__ == '__main__':
    main()
