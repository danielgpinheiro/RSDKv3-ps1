#!/bin/bash
# PC-sampler profile of a scene's scripted run (docs/28 speed pass 2): SAMPLE=1 BOOT= build of the working tree, the
# survey's pad script, frames FROM..TO; the report (tools/play/sample_report.py) goes to OUT.report.txt, read at once
# (it maps through the ELF just built). Usage: tools/play/sample_scene.sh OUT SCENE[:BOOT_ID] [FROM TO] [report args]
set -u
cd "$(dirname "$0")/../.."
OUT=$1; SC=$2; FROM=${3:-300}; TO=${4:-1200}; shift 4 2>/dev/null || shift $#
eval "$(sed -n '/^act_pad()/p;/^ss_pad()/p;/^pad_for()/,/^}/p' tools/play/survey.sh)"
PAD="$(pad_for ${SC%%:*})" SAMPLE=1 tools/play/scene_run.sh "$OUT" "$SC" $FROM $TO | grep -E "^(RUN|SKIPPED|SAMPLES)"
frames=$(grep -E "^RUN " "$OUT.gdb.log" | sed -E 's/.*frames=([0-9]+).*/\1/')
python3 tools/play/sample_report.py "$OUT" ${frames:-$((TO - FROM))} "$@" > "$OUT.report.txt"
cat "$OUT.report.txt"
