# RSDKv3 (Sonic CD) PS1 port — Makefile
#
# Builds a .ps-exe via the psyqo toolchain (mipsel-none-elf, C++20), like the RSDKv2 port
# (../RSDKv2-ps1/Makefile: the flags below come from there). Plan: ../docs/28.
#
# The RSDKv3 engine (RSDKv3/, PS1 app in RSDKv3/main.cpp) on the PS1 platform layer (ps1/, copied
# from RSDKv2-ps1). `make PLATFORM_TEST=1`: the phase 0 smoke test (ps1/platform_test.cpp) instead.
TARGET = rsdkv3
TYPE = ps-exe

SRCS = \
RSDKv3/Animation.cpp \
RSDKv3/Audio.cpp \
RSDKv3/Collision.cpp \
RSDKv3/Debug.cpp \
RSDKv3/Ini.cpp \
RSDKv3/Input.cpp \
RSDKv3/Math.cpp \
RSDKv3/ModAPI.cpp \
RSDKv3/Object.cpp \
RSDKv3/Palette.cpp \
RSDKv3/Player.cpp \
RSDKv3/Reader.cpp \
RSDKv3/RetroEngine.cpp \
RSDKv3/Scene.cpp \
RSDKv3/Scene3D.cpp \
RSDKv3/Script.cpp \
RSDKv3/Sprite.cpp \
RSDKv3/String.cpp \
RSDKv3/Text.cpp \
RSDKv3/Userdata.cpp \
RSDKv3/Video.cpp \
RSDKv3/main.cpp \
ps1/libc/string.cpp \
ps1/libc/stdlib.cpp \
ps1/libc/math.cpp \
ps1/libc/stdio.cpp \
ps1/cdrom_fs.cpp \
ps1/cdrom_stream.cpp \
ps1/irq_guard.cpp \
ps1/gpu_chain.cpp \
ps1/pad.cpp \
ps1/spu.cpp \
ps1/xa_music.cpp \
ps1/mdec.cpp \
ps1/video.cpp \
ps1/fmv.cpp \
ps1/loading.cpp \
ps1/sprite_atlas.cpp \
ps1/tileset.cpp \
ps1/render.cpp \

ifeq ($(PLATFORM_TEST),1)
SRCS := ps1/platform_test.cpp $(filter-out RSDKv3/%,$(SRCS))
endif

# ps1/libc comes FIRST: its C/C++ shim headers (<string>, <cmath>, <stdio.h>, ...) shadow any
# system headers the freestanding toolchain might otherwise surface.
CPPFLAGS = -I ps1/libc -I RSDKv3 -DRETRO_USE_MOD_LOADER=0

# `make BOOT=<stage folder>` (e.g. R11A): start in that stage (test harness): its first GameConfig
# entry unless BOOT_ID is given (acts 2/3 have ids 2/3), started like the Stage Select does (Options.GameMode = BOOT_GAMEMODE, default
# 0; the Menu sets 1, Time Attack 2) with player BOOT_PLAYER (0 Sonic, 1 Tails). Rebuild
# RSDKv3/main.cpp when switching (touch it).
ifneq ($(BOOT),)
CPPFLAGS += -DPS1_BOOT_FOLDER='"$(BOOT)"' -DPS1_BOOT_ID='"$(BOOT_ID)"' -DPS1_BOOT_GAMEMODE=$(or $(BOOT_GAMEMODE),0) -DPS1_BOOT_PLAYER=$(or $(BOOT_PLAYER),0)
endif

# `make SAMPLE=1`: PC sampler (ps1/pc_sampler.cpp, docs/28 speed pass 2) for tools/play/scene_run.sh; 2 MB as always.
# Rebuilds what the define touches when it is switched (build/gen/sample.txt).
SRCS += ps1/pc_sampler.cpp
ifneq ($(SAMPLE),)
CPPFLAGS += -DPS1_SAMPLE=1
endif
PS1_SAMPLE_OBJS := RSDKv3/main.o RSDKv3/Script.o ps1/pc_sampler.o
# (make 3.81 compares whole seconds: the objects are deleted rather than trusted to be older than the stamp)
$(shell mkdir -p build/gen; [ -f build/gen/sample.txt ] && [ "$$(cat build/gen/sample.txt)" = "$(SAMPLE)" ] || { echo "$(SAMPLE)" > build/gen/sample.txt; rm -f $(PS1_SAMPLE_OBJS); })

# `make PERF_SPLIT=1`: GPU tail, flip gap and window sums of the section counters in RSDKv3/main.cpp (docs/43; tools/play/scene_run.sh prints them):
# harness only. Rebuilds main.o and relinks when switched (build/gen/perfsplit.txt; the ELF goes too: make 3.81 compares
# whole seconds). The EXE is always re-made from the ELF (one objcopy) for the same reason.
ifneq ($(PERF_SPLIT),)
CPPFLAGS += -DPS1_PERF_SPLIT=1
endif
$(shell mkdir -p build/gen; [ -f build/gen/perfsplit.txt ] && [ "$$(cat build/gen/perfsplit.txt)" = "$(PERF_SPLIT)" ] || { echo "$(PERF_SPLIT)" > build/gen/perfsplit.txt; rm -f RSDKv3/main.o rsdkv3.elf; })

# `make TEST=1`: the regression test mode (ps1/test_mode.cpp, ../docs/45; the scenario comes from the test disc's
# Data/TEST.SEL, written by ../tests/run.py): harness only, make dist and tools/export_builder.sh refuse it. Rebuilds
# main.o and relinks when switched (build/gen/test.txt), as PERF_SPLIT.
ifeq ($(TEST),1)
CPPFLAGS += -DPS1_TEST=1
SRCS += ps1/test_mode.cpp
endif
$(shell mkdir -p build/gen; [ -f build/gen/test.txt ] && [ "$$(cat build/gen/test.txt)" = "$(TEST)" ] || { echo "$(TEST)" > build/gen/test.txt; rm -f RSDKv3/main.o rsdkv3.elf; })

# `make MEMCARD_TEST=1`: memory card self-test (ps1/memcard_test.cpp; tools/memcard/selftest.sh).
ifeq ($(MEMCARD_TEST),1)
CPPFLAGS += -DPS1_MEMCARD_TEST=1
SRCS += ps1/memcard_test.cpp
endif
# Saves (docs/28 phase 8): Sonic CD's save RAM on the memory card.
SRCS += ps1/memcard.cpp

# C++20 is the minimum required by psyqo.
CXXFLAGS = -std=c++20

# This activates the PSYQo library and the rest of the toolchain.
include ../nugget/psyqo/psyqo.mk
$(PS1_SAMPLE_OBJS): build/gen/sample.txt
RSDKv3/main.o: build/gen/perfsplit.txt
.PHONY: rsdkv3.ps-exe

# The R3000A has no FPU and the engine uses float: soft-float + libgcc's helpers. Must come
# AFTER the include so it overrides common.mk's -mfp32 ARCHFLAGS.
CPPFLAGS += -msoft-float
LDFLAGS   += -msoft-float
LDFLAGS   += -lgcc
# Retail PS1 = 2 MB RAM (EXE at 0x80010000, ~1.94 MB usable). Never raise this for a release.
LDFLAGS   += -Wl,--defsym,RAM_SIZE=2M
# nugget's hardware-measured memcpy/memset (8-word / 4-word blocks).
LDFLAGS   += -Wl,-wrap,memcpy -Wl,-wrap,memset

# Trig tables computed at build time on the host (tools/trig/gen_trig.c), included by Math.cpp.
CPPFLAGS += -I build/gen
build/gen/trig_tables.inc: tools/trig/gen_trig.c
	mkdir -p build/gen
	cc -O2 -o build/gen/gen_trig $<
	build/gen/gen_trig > $@
RSDKv3/Math.o RSDKv3/Math.dep: build/gen/trig_tables.inc

# The FMV Huffman (VLC) decoder is the hot loop of video playback: optimise it for speed.
ps1/mdec.o: CPPFLAGS += -O2
# Renderer hot paths (tile bands, chain appends): speed over size (docs/28 3.6).
ps1/render.o ps1/gpu_chain.o: CPPFLAGS += -O2
# Script VM + collision (object logic): speed over size (user decision 2026-09-24: 60 fps, spend RAM).
RSDKv3/Script.o RSDKv3/Object.o RSDKv3/Collision.o: CPPFLAGS += -O2

# Cancel make's built-in `%: %.o` link rule: the extensionless libc shim headers
# (ps1/libc/string, vector, ...) listed in .dep files must not be rebuilt from `string.o`.
%: %.o

# Test disc (phase 0): a hand-picked subset of ../SonicCD_Extracted/Data (tools/smoke_disc.sh),
# then mkpsxiso + the byte-level verifier. The real asset pipeline arrives in phase 1.
disc:
	/usr/bin/python3 tools/disc/build_disc.py build/iso rsdkv3.ps-exe none build/game
	/usr/bin/python3 tools/disc/verify_disc.py build/game.bin build/iso rsdkv3.ps-exe none

# Distribution image (docs/28 phase 10): the natural game (not a BOOT= / MEMCARD_TEST build) on a disc with
# the license sectors from the workspace's LICENSE.DAT (read by path, never copied; LICENSE=none for an
# unlicensed image), verified, as build/dist/SonicCD-PS1-<EN|JP>.bin/.cue + SHA256SUMS-<EN|JP>.
LICENSE ?= ../LICENSE.DAT
dist:
	@if mipsel-none-elf-nm rsdkv3.elf | grep -q g_ps1BootBuild; then echo "dist: rsdkv3.ps-exe is a BOOT= test build"; exit 1; fi
	@if mipsel-none-elf-nm rsdkv3.elf | grep -q g_ps1SampleRing; then echo "dist: rsdkv3.ps-exe is a SAMPLE= build"; exit 1; fi
	@if mipsel-none-elf-nm rsdkv3.elf | grep -q g_ps1TestBuild; then echo "dist: rsdkv3.ps-exe is a TEST=1 build"; exit 1; fi
	@if mipsel-none-elf-nm rsdkv3.elf | grep -q g_ps1PerfGpuTail; then echo "dist: rsdkv3.ps-exe is a PERF_SPLIT build"; exit 1; fi
	@lang=$$(/usr/bin/python3 -c "print({0: 'EN', 5: 'JP'}.get(open('build/iso/Data/Game/PS1Language.bin', 'rb').read()[0], 'XX'))"); \
	mkdir -p build/dist && \
	/usr/bin/python3 tools/disc/build_disc.py build/iso rsdkv3.ps-exe $(LICENSE) build/dist/SonicCD-PS1-$$lang && \
	/usr/bin/python3 tools/disc/verify_disc.py build/dist/SonicCD-PS1-$$lang.bin build/iso rsdkv3.ps-exe $(LICENSE) && \
	cd build/dist && shasum -a 256 SonicCD-PS1-$$lang.bin SonicCD-PS1-$$lang.cue > SHA256SUMS-$$lang && cat SHA256SUMS-$$lang

.PHONY: disc dist

# Regression suite (../docs/44 ledger, ../docs/45 design, ../tests/README.md): `make test` = this port's quick tier (run it
# before every commit), `make test-full` = every scenario on both emulators, perf and memory gates (after engine,
# interpreter, renderer or perf changes). The suite builds in its own shadow copy (../.rt-<port>), never here.
.PHONY: test test-full
test:
	/usr/bin/python3 ../tests/run.py --port cd
test-full:
	/usr/bin/python3 ../tests/run.py --tier full --port cd
