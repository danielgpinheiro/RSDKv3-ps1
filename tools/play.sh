#!/bin/bash
# Manual play test: build the exe (optionally straight into a stage) and open PCSX-Redux with the
# game disc (build/game.cue from tools/data_disc.sh) and the pad. No -bios / -memcard flags, so
# ~/.config/pcsx-redux/pcsx.json is left alone.
#   tools/play.sh              # boots to the title
#   tools/play.sh R41A         # Tidal Tempest zone 1 (acts: R11A R12A ... R83C, time periods A-D)
#   tools/play.sh R41A 1       # ... as Tails
# Pad (PC defaults): D-pad, Cross = jump (A), Circle = B, Square = C, Start = pause.
cd "$(dirname "$0")/.."
touch RSDKv3/main.cpp RSDKv3/RetroEngine.cpp
if [ -n "${1:-}" ]; then make -s -j8 BOOT=$1 BOOT_PLAYER=${2:-0}; else make -s -j8; fi || exit 1
[ -f build/game.cue ] || { echo "build/game.cue missing: run tools/data_disc.sh first"; exit 1; }
exec /Applications/PCSX-Redux.app/Contents/MacOS/PCSX-Redux -iso build/game.cue -exe rsdkv3.ps-exe -run
