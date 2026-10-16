#!/bin/bash
# Speed survey (docs/28 speed pass 2): for each scene, a BOOT= build and a scripted run (tools/play/scene_run.sh),
# frames FROM..TO; prints / appends "RUN ..." lines to OUT/survey.txt. With SNAP=1 the EXE + ELF of each scene are kept
# in OUT/snap/ (the A/B reference: NOBUILD=1 EXE=... ELF=...). Pad scripts: pad_for SCENE.
# Usage: tools/play/survey.sh OUT SCENE[:BOOT_ID]...   (FROM=300 TO=1200; CHECK=frames; EMU=ds; GPU=frames;
# PERF_SPLIT=1: tools/play/scene_run.sh)
set -u
cd "$(dirname "$0")/../.."
OUT=$1; shift
FROM=${FROM:-300}; TO=${TO:-1200}
mkdir -p "$OUT/snap"
# Acts: Right held from frame 150, a jump (A) every 90 frames held for 12 (over badniks and steps).
act_pad() { local s="150:8" f; for f in $(seq 240 90 2940); do s="$s $f:24 $((f + 12)):8"; done; echo "$s"; }
# Special stages: Up held (run), turning left for 40 frames then right for 40 in every 120.
ss_pad() { local s="150:1" f; for f in $(seq 200 120 1640); do s="$s $f:5 $((f + 40)):1 $((f + 60)):9 $((f + 100)):1"; done; echo "$s"; }
pad_for() {
  case $1 in
    SS*) ss_pad ;;
    Title|Menu|Credits|DAgdn|Help|TAttack) echo "" ;;
    *) act_pad ;;
  esac
}
for sc in "$@"; do
  scene=${sc%%:*}; tag=${sc//:/_}
  if [ -n "${SNAPSHOT_RUN:-}" ]; then
    PAD="$(pad_for $scene)" NOBUILD=1 EXE=$OUT/snap/$tag.ps-exe ELF=$OUT/snap/$tag.elf tools/play/scene_run.sh "$OUT/$tag" "$sc" $FROM $TO > "$OUT/$tag.txt" 2>&1
  else
    PAD="$(pad_for $scene)" tools/play/scene_run.sh "$OUT/$tag" "$sc" $FROM $TO > "$OUT/$tag.txt" 2>&1
    [ "${SNAP:-0}" = 1 ] && cp rsdkv3.ps-exe "$OUT/snap/$tag.ps-exe" && cp rsdkv3.elf "$OUT/snap/$tag.elf"
  fi
  r=$(grep -E "^RUN " "$OUT/$tag.txt" || echo "RUN $tag (no result)")
  g=$(grep -E "^(SKIPPED|GUARDS|SPLIT|GPUCOST) " "$OUT/$tag.txt" | tr '\n' ' ')
  echo "$tag $r | $g" | tee -a "$OUT/survey.txt"
done
