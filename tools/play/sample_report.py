#!/usr/bin/env python3
"""Report of a PC-sampler run (tools/play/scene_run.sh with SAMPLE=1; ps1/pc_sampler.cpp), docs/28 speed pass 2.
Usage: sample_report.py OUT_PREFIX FRAMES [--elf rsdkv3.elf] [--top N] [--period 9973] [--ops N] [--lines FUNC ...]
Reads OUT_samples.bin ((EPC, script PC) word pairs), OUT_scripts.txt (OBJ lines), OUT_funcs.bin (scriptFunctionList),
OUT_code.bin (scriptCode). Prints, in hblanks a frame (samples x period / 2152 / FRAMES):
  - native functions (exclusive: where the CPU was);
  - script subs (inclusive: every sample taken while ProcessScript ran that sub, the engine calls it made included),
    split into the VM itself (ProcessScript) and the calls;
  - the hottest script instructions (disassembled).
Read the report before any rebuild: it maps through the current ELF."""
import bisect, os, re, struct, subprocess, sys
from collections import defaultdict
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import bytecode_scan as bs
import bytecode_dis as bd

HBL = 2152  # cycles per NTSC hblank (33.8688 MHz / 15,734 Hz)
args = sys.argv[1:]


def opt(name, default, conv=str):
    if name in args:
        i = args.index(name)
        v = conv(args[i + 1])
        del args[i:i + 2]
        return v
    return default


lines_for = []
if '--lines' in args:
    i = args.index('--lines')
    lines_for = args[i + 1:]
    del args[i:]
elf = opt('--elf', 'rsdkv3.elf')
top = opt('--top', 30, int)
period = opt('--period', 9973, int)
nops = opt('--ops', 25, int)
out, frames = args[0], int(args[1])
scale = period / HBL / frames

syms = []
for l in subprocess.run(['mipsel-none-elf-nm', '-n', '-S', '-C', '--defined-only', elf], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'([0-9a-f]{8}) (?:([0-9a-f]{8}) )?([tTwW]) (.*)', l)
    if m:
        syms.append((int(m.group(1), 16), m.group(4)))
syms.sort()
starts = [s[0] for s in syms]


def fn_of(pc):
    i = bisect.bisect_right(starts, pc) - 1
    return syms[i][1] if i >= 0 else '?'


data = open(out + '_samples.bin', 'rb').read()
pairs = struct.unpack('<%dI' % (len(data) // 4), data)
samples = [(pairs[i], pairs[i + 1]) for i in range(0, len(pairs) - 1, 2)]
n = len(samples)
print('%d samples, %d frames, period %d: %.1f hblanks a frame sampled' % (n, frames, period, n * scale))

# script subs: every code start -> label
starts_s = []
for l in open(out + '_scripts.txt'):
    p = l.split()
    if p[0] != 'OBJ':
        continue
    name = p[2]
    for lab, v in zip(('main', 'pinteract', 'draw', 'startup'), p[3:7]):
        v = int(v)
        if v and v < 0x10000:
            starts_s.append((v, '%s.%s' % (name, lab)))
fdata = open(out + '_funcs.bin', 'rb').read() if os.path.exists(out + '_funcs.bin') else b''
for i in range(len(fdata) // 8):
    c, j = struct.unpack_from('<ii', fdata, i * 8)
    if 0 < c < 0x10000:
        starts_s.append((c, 'function%d' % i))
starts_s.sort()
sk = [s[0] for s in starts_s]


def sub_of(spc):
    i = bisect.bisect_right(sk, spc) - 1
    return starts_s[i][1] if i >= 0 else '?'


by_fn = defaultdict(int)
by_sub = defaultdict(lambda: [0, 0])
by_op = defaultdict(int)
vm_total = 0
for pc, spc in samples:
    f = fn_of(pc)
    by_fn[f] += 1
    if spc:
        vm_total += 1
        s = by_sub[sub_of(spc)]
        s[0 if f.startswith('ProcessScript') else 1] += 1
        by_op[spc] += 1

print('\n== native functions (exclusive) ==')
for f, c in sorted(by_fn.items(), key=lambda x: -x[1])[:top]:
    print('%7.1f  %5.1f%%  %s' % (c * scale, 100.0 * c / n, f))
print('\n== script subs (inclusive; VM = ProcessScript itself, calls = engine functions it called) ==')
print('scripts total %.1f hblanks a frame (%.1f%%)' % (vm_total * scale, 100.0 * vm_total / max(n, 1)))
for s, (vm, calls) in sorted(by_sub.items(), key=lambda x: -(x[1][0] + x[1][1]))[:top]:
    print('%7.1f  VM %6.1f  calls %6.1f  %s' % ((vm + calls) * scale, vm * scale, calls * scale, s))

if os.path.exists(out + '_code.bin') and nops:
    cd = open(out + '_code.bin', 'rb').read()
    code = list(struct.unpack('<%di' % (len(cd) // 4), cd))
    names, vars_ = bs.tables()
    import io, contextlib
    print('\n== hottest script instructions (inclusive) ==')
    for spc, c in sorted(by_op.items(), key=lambda x: -x[1])[:nops]:
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            try:
                bd.dis(code, spc, names, vars_, limit=1)
            except Exception as e:
                print('?? %s' % e)
        print('%7.2f  %-28s %s' % (c * scale, sub_of(spc), buf.getvalue().strip()))

for want in lines_for:
    per = defaultdict(int)
    for pc, spc in samples:
        if want in fn_of(pc):
            per[pc] += 1
    if not per:
        continue
    addrs = sorted(per)
    res = subprocess.run(['mipsel-none-elf-addr2line', '-e', elf] + ['0x%x' % a for a in addrs], capture_output=True, text=True).stdout.split()
    by_line = defaultdict(int)
    for a, ln in zip(addrs, res):
        by_line[ln.split('/')[-1]] += per[a]
    print('\n== lines of %s ==' % want)
    for ln, c in sorted(by_line.items(), key=lambda x: -x[1])[:top]:
        print('%7.2f  %s' % (c * scale, ln))
