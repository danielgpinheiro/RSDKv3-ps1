#!/bin/bash
# A/B of one scene (docs/28 speed pass 2): the reference build (REF commit, default the speed pass 2 kit, built
# in the git worktree build/refwt; cached per scene in build/perf2/ref/) against the working tree, the same pad
# script (tools/play/survey.sh pad_for) and frames: both timings, then logic state + framebuffers compared at CHECK.
# The reference runs boot the reference data's disc (REF_DISC, default build/perf2/refdisc/game.cue: build/game.* as
# they were before the speed pass patched any bytecode), the new runs build/game.cue.
# Usage: tools/play/ab_run.sh SCENE[:BOOT_ID] [FROM TO]   (CHECK="600 1100" EMU=ds REBUILD_REF=1 NEW_ONLY=1)
set -u
cd "$(dirname "$0")/../.."
SC=$1; FROM=${2:-300}; TO=${3:-1200}; CHECK=${CHECK:-600 1100}
scene=${SC%%:*}; bid=; [ "$SC" != "$scene" ] && bid=${SC#*:}; tag=${SC//:/_}
REF=${REF:-b6ccce6}
R=build/perf2/ref; N=build/perf2/new; mkdir -p $R $N
eval "$(sed -n '/^act_pad()/p;/^ss_pad()/p;/^pad_for()/,/^}/p' tools/play/survey.sh)"
PADS="$(pad_for $scene)"
if [ ! -f $R/$tag.ps-exe ] || [ -n "${REBUILD_REF:-}" ]; then
  [ -d build/refwt ] || { git worktree add -q build/refwt $REF && ln -sfn ../../nugget build/nugget; }
  ( cd build/refwt && git checkout -q $REF && rm -f RSDKv3/main.o RSDKv3/RetroEngine.o && make -j8 BOOT=$scene BOOT_ID=$bid SAMPLE= > ../perf2/ref/$tag.build.log 2>&1 ) || { echo "REF BUILD FAIL"; exit 1; }
  cp build/refwt/rsdkv3.ps-exe $R/$tag.ps-exe; cp build/refwt/rsdkv3.elf $R/$tag.elf
fi
sfx=; [ "${EMU:-redux}" = ds ] && sfx=_ds
if [ -z "${NEW_ONLY:-}" ]; then
  EXTRA= DISC=${REF_DISC:-build/perf2/refdisc/game.cue} PAD="$PADS" CHECK="$CHECK" NOBUILD=1 EXE=$R/$tag.ps-exe ELF=$R/$tag.elf tools/play/scene_run.sh $R/$tag$sfx "$SC" $FROM $TO | grep -E "^(RUN|SKIPPED|GUARDS)" | sed 's/^/REF /'
fi
rm -f RSDKv3/main.o RSDKv3/RetroEngine.o
make -j8 BOOT=$scene BOOT_ID=$bid SAMPLE= > $N/$tag.build.log 2>&1 || { echo "NEW BUILD FAIL ($N/$tag.build.log)"; exit 1; }
cp rsdkv3.ps-exe $N/$tag.ps-exe; cp rsdkv3.elf $N/$tag.elf
PAD="$PADS" CHECK="$CHECK" NOBUILD=1 EXE=$N/$tag.ps-exe ELF=$N/$tag.elf tools/play/scene_run.sh $N/$tag$sfx "$SC" $FROM $TO | grep -E "^(RUN|SKIPPED|GUARDS)" | sed 's/^/NEW /'
if [ "${EMU:-redux}" != ds ]; then
  python3 tools/play/ab_compare.py $R/$tag $N/$tag $CHECK
else
  python3 - $R/$tag$sfx $N/$tag$sfx $CHECK <<'EOF'
import sys
a, b, fr = sys.argv[1], sys.argv[2], sys.argv[3:]
bad = sum(open('%s_state_%s.bin' % (a, f), 'rb').read() != open('%s_state_%s.bin' % (b, f), 'rb').read() for f in fr)
print('A/B (DuckStation, state only):', 'IDENTICAL' if not bad else 'DIFFER (pointer words may differ between builds: check with ab_compare.py)')
EOF
fi
