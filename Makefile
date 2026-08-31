# TinyPod Makefile — WSL host + musl-static N31
#
# Decoding is in-process: Helix fixed-point AAC and MP3 from third_party/,
# populated by tools/fetch-decoders.sh. On the N31 there is no mpv or ffmpeg to
# shell out to, so the decoders and the ALSA output are linked in.
#
#   make                      host build, external player backend
#   make TARGET=n31 TINYALSA_DIR=... n31 build with ALSA output
#   make decoders             fetch third_party/helix-*

TARGET ?= host
UI_LVGL ?= 0

# The graphical build and the plain one do not share a directory.
#
# They used to, and the objects have different -D flags - TP_WITH_LVGL among
# them - which the makefile does not track. So building one after the other
# reused objects compiled for the wrong configuration, and the failure was an
# undefined reference to a function that is right there in the source. Cheap
# to avoid, and confusing to debug.
BUILD  := build/$(TARGET)$(if $(filter 1,$(UI_LVGL)),-lvgl,)$(if $(filter 1,$(FFMPEG)),-ff,)
OUT    := out/$(TARGET)

SRC_APP := \
	src/main.c \
	src/tp_app.c \
	src/util/tp_build.c \
	src/util/tp_diag.c \
	src/util/tp_log.c \
	src/util/tp_util.c \
	src/util/tp_config.c \
	src/fs/tp_mount_detect.c \
	src/fs/tp_path_resolve.c \
	src/fs/tp_file_probe.c \
	src/db/tp_db_lib.c \
	src/db/tp_db_detect.c \
	src/db/tp_db_sqlite_itdb.c \
	src/db/tp_db_classic_itunesdb.c \
	src/db/tp_db_raw_scan.c \
	src/codec/tp_mp4.c \
	src/codec/tp_decode.c \
	src/playback/tp_player.c \
	src/playback/tp_sink.c \
	src/ui/tp_ui_fb.c \
	src/ui/tp_ui_keys.c

SRC_SQLITE := third_party/sqlite/sqlite3.c

HELIX_AAC_DIR := third_party/helix-aac
HELIX_MP3_DIR := third_party/helix-mp3
SRC_AAC  := $(wildcard $(HELIX_AAC_DIR)/*.c)
SRC_MP3  := $(wildcard $(HELIX_MP3_DIR)/*.c)
SRC_MP3R := $(wildcard $(HELIX_MP3_DIR)/real/*.c)

INCLUDES := -Isrc -Isrc/codec -Isrc/db -Isrc/fs -Isrc/playback -Isrc/ui -Isrc/util \
	-Ithird_party/sqlite \
	-I$(HELIX_AAC_DIR) -I$(HELIX_MP3_DIR)/pub -I$(HELIX_MP3_DIR)/real

# ---- build stamp -----------------------------------------------------------
#
# Which build is this. A copy on the device that is months old looks exactly
# like the one just compiled, right up until an afternoon goes into
# reproducing a fault that was fixed in April.
#
# Only tp_build.o carries it, and that object depends on BUILD_FORCE - which
# has no rule and no file, so it is always out of date. A fresh stamp is one
# recompile, not a rebuild of the world; and a stamp cached with the rest of
# the objects would say "current" while being stale, which is the exact
# confusion it exists to end.
BUILD_STAMP := $(shell date -u +%Y%m%d.%H%M)
BUILD_GIT   := $(shell git rev-parse --short=7 HEAD 2>/dev/null || echo nogit)
STAMP_DEFS  := -DTP_BUILD_STAMP='"$(BUILD_STAMP)"' -DTP_BUILD_GIT='"$(BUILD_GIT)"'

WARN := -Wall -Wextra -Wpedantic -Werror
# sqlite and the Helix decoders are third_party — compiled without -Werror
CDEFS := -D_GNU_SOURCE -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 \
	-DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0

# The graphical UI, off unless asked for. The core and its tests must keep
# building with no LVGL anywhere - a UI for one device should not be able to
# break the library reader's CI.
#
#   make TARGET=n31 UI_LVGL=1 LVGL=/path/to/lvgl TINYALSA_DIR=...
#
# The LVGL archive comes from src/ui/lvgl/Makefile.lvgl, which builds it into
# this app's own tree. Build that first.
ifeq ($(UI_LVGL),1)
  LVGL ?= ../../NanoApps/lvgl
  LVGL_LIB ?= src/ui/lvgl/build-$(TARGET)/liblvgl.a
  SRC_APP += src/ui/lvgl/tp_lv_ui.c src/ui/lvgl/tp_lv_screens.c \
	src/ui/lvgl/tp_lv_input.c
  INCLUDES += -Isrc/ui/lvgl -I$(LVGL) -I$(LVGL)/src -I$(LVGL)/include \
	-I$(LVGL)/include/lvgl
  CDEFS += -DTP_WITH_LVGL=1 -DLV_CONF_INCLUDE_SIMPLE \
	-DLV_CONF_PATH='"lv_conf_tp.h"'
endif

# FFmpeg, for everything the Helix decoders do not cover: FLAC, Vorbis, Opus,
# ALAC, WMA, Musepack, AC3 and the rest.
#
#   ./tools/fetch-ffmpeg.sh                              host libraries
#   CROSS=<prefix> ./tools/fetch-ffmpeg.sh               device libraries
#   make FFMPEG=1 ...
#
# Off by default. AAC and MP3 keep going through Helix either way - this is
# the fallback for formats that otherwise refuse to open, not a replacement
# for the path that already works on the device.
FFMPEG ?= 0
ifeq ($(FFMPEG),1)
  FFMPEG_DIR ?= third_party/ffmpeg-build/$(TARGET)
  SRC_APP += src/codec/tp_dec_ff.c
  INCLUDES += -I$(FFMPEG_DIR)/include
  CDEFS += -DTP_WITH_FFMPEG=1
  # Order matters to a static link: avformat needs avcodec, which needs
  # swresample and avutil.
  FFMPEG_LIBS := $(FFMPEG_DIR)/lib/libavformat.a \
                 $(FFMPEG_DIR)/lib/libavcodec.a \
                 $(FFMPEG_DIR)/lib/libswresample.a \
                 $(FFMPEG_DIR)/lib/libavutil.a
endif

# tinyalsa: point at an unpacked tinyalsa tree to get real audio output.
# Without it the build still decodes (see the "decode" command) but cannot play.
TINYALSA_DIR ?=
ifneq ($(TINYALSA_DIR),)
  CDEFS += -DTINYPOD_HAVE_TINYALSA
  INCLUDES += -I$(TINYALSA_DIR)/include
  TINYALSA_LIB := $(TINYALSA_DIR)/src/libtinyalsa.a
endif

ifeq ($(TARGET),n31)
  CROSS ?= arm-linux-musleabi-
  CC := $(CROSS)gcc
  # armv7-a: Helix AAC's fast path uses ssat (ARMv6+), and -DARM selects the
  # smull inline assembly in Helix MP3.
  #
  # And the floating-point unit the device actually has. This used to be
  # -march=armv7-a alone, so GCC assumed no FPU and put every float operation
  # through a soft-float library call. The Helix decoders are fixed-point and
  # did not care, which is why it went unnoticed; FFmpeg's decoders are mostly
  # float and would have cared a great deal.
  #
  # softfp is the ABI, not the arithmetic: FP instructions are generated and
  # arguments are passed in integer registers, which is what this eabi (not
  # eabihf) toolchain wants. The same flags every other app here uses.
  ARCH := -mcpu=cortex-a8 -mfpu=vfpv3-d16 -mfloat-abi=softfp
  CFLAGS := -Os $(ARCH) -static $(WARN) $(INCLUDES) $(CDEFS) -DTINYPOD_N31
  DECFLAGS := -O2 $(ARCH) -DARM $(INCLUDES) $(CDEFS)
  LDFLAGS := -static $(TINYALSA_LIB) $(LVGL_LIB) $(FFMPEG_LIBS) -lpthread -ldl -lm
else
  CC ?= gcc
  ARCH :=
  CFLAGS := -O2 -g $(WARN) $(INCLUDES) $(CDEFS)
  DECFLAGS := -O2 $(INCLUDES) $(CDEFS)
  LDFLAGS := $(TINYALSA_LIB) $(LVGL_LIB) $(FFMPEG_LIBS) -lpthread -ldl -lm
endif

APP_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC_APP))
# Everything but the CLI entry point, for tests that bring their own main.
APP_OBJS_NOMAIN := $(filter-out $(BUILD)/main.o,$(APP_OBJS))
SQLITE_OBJ := $(BUILD)/sqlite3.o
AAC_OBJS  := $(patsubst $(HELIX_AAC_DIR)/%.c,$(BUILD)/helix-aac/%.o,$(SRC_AAC))
MP3_OBJS  := $(patsubst $(HELIX_MP3_DIR)/%.c,$(BUILD)/helix-mp3/%.o,$(SRC_MP3))
MP3R_OBJS := $(patsubst $(HELIX_MP3_DIR)/real/%.c,$(BUILD)/helix-mp3-real/%.o,$(SRC_MP3R))
# Both Helix trees have a bitstream.c, a buffers.c and more: separate object
# directories, or they quietly overwrite each other.
DEC_OBJS := $(AAC_OBJS) $(MP3_OBJS) $(MP3R_OBJS)

.PHONY: all binaries clean test selftest check-real n31 host dirs tools decoders

# Two passes, deliberately. SRC_AAC and SRC_MP3 are wildcards expanded when
# this file is parsed, so on a fresh clone - where third_party is still empty -
# they come out empty and the decoders never make it into the link, however
# early the fetch runs. Re-entering make is what gives the wildcards a second
# look at a directory that now has files in it.
all:
	@$(MAKE) --no-print-directory decoders
	@$(MAKE) --no-print-directory binaries

binaries: dirs $(OUT)/tinypod tools

host:
	$(MAKE) TARGET=host all

n31:
	$(MAKE) TARGET=n31 all

# Run directly, not through `sh`: the script needs bash (set -o pipefail) and
# saying `sh` here overrides its shebang with whatever /bin/sh happens to be -
# which on Ubuntu is dash, and dash has no pipefail.
decoders:
	@test -f $(HELIX_AAC_DIR)/aacdec.c && test -f $(HELIX_MP3_DIR)/mp3dec.c || \
		./tools/fetch-decoders.sh
	@# third_party is fetched rather than vendored, so a fix made in it is lost
	@# on the next fetch. This re-applies the one mingw needs, and is
	@# idempotent, so running it every time costs nothing.
	@./tools/patch-decoders.sh

dirs:
	@mkdir -p $(BUILD) $(OUT) $(BUILD)/util $(BUILD)/fs $(BUILD)/db \
		$(BUILD)/codec $(BUILD)/playback $(BUILD)/ui $(BUILD)/ui/lvgl \
		$(BUILD)/helix-aac $(BUILD)/helix-mp3 $(BUILD)/helix-mp3-real

# Where the image builder looks for the device binary. The image is packed
# from here, so staging by hand meant an image could be packed from whatever
# was last copied over rather than what was last built - and a stale binary in
# an image looks exactly like a bug in the app.
IPOD_ARTIFACTS ?= /mnt/c/src/ipod/artifacts/linux-n31

$(OUT)/tinypod: $(APP_OBJS) $(SQLITE_OBJ) $(DEC_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@# Device builds only: the host binary is not what gets packed.
	@#
	@# The wide build is staged under a different name. The initramfs is a
	@# tmpfs, so everything in it holds system RAM for the whole session, and
	@# FFmpeg takes this binary from 2 MB to 4.4 MB - five per cent of the
	@# machine, permanently, for decoders most tracks never reach. The disk
	@# pages on demand and has fifteen gigabytes, so the wide build goes
	@# there and the lean one stays in RAM. n31-autostart searches the disk
	@# first, so the wide one runs whenever the volume is mounted and the
	@# lean one still starts when it is not.
	@if [ "$(TARGET)" = "n31" ] && [ -d "$(IPOD_ARTIFACTS)" ]; then \
		name=$(if $(filter 1,$(FFMPEG)),tinypod-full,tinypod); \
		cp -f $@ $(IPOD_ARTIFACTS)/$$name && \
		echo "  staged -> $(IPOD_ARTIFACTS)/$$name"; \
	fi

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: BUILD_FORCE
BUILD_FORCE:

$(BUILD)/util/tp_build.o: src/util/tp_build.c BUILD_FORCE
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(STAMP_DEFS) -c -o $@ $<

$(SQLITE_OBJ): $(SRC_SQLITE)
	@mkdir -p $(dir $@)
	$(CC) -O2 $(ARCH) $(INCLUDES) $(CDEFS) -c -o $@ $<

$(BUILD)/helix-aac/%.o: $(HELIX_AAC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(DECFLAGS) -c -o $@ $<

$(BUILD)/helix-mp3/%.o: $(HELIX_MP3_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(DECFLAGS) -c -o $@ $<

$(BUILD)/helix-mp3-real/%.o: $(HELIX_MP3_DIR)/real/%.c
	@mkdir -p $(dir $@)
	$(CC) $(DECFLAGS) -c -o $@ $<

tools: $(OUT)/tinypod
	@mkdir -p $(OUT)
	@printf '%s\n' '#!/bin/sh' 'exec "$$(dirname "$$0")/tinypod" libcheck "$$@"' > $(OUT)/tinypod-libcheck
	@printf '%s\n' '#!/bin/sh' 'exec "$$(dirname "$$0")/tinypod" export-json "$$@"' > $(OUT)/tinypod-db-dump
	@printf '%s\n' '#!/bin/sh' 'exec "$$(dirname "$$0")/tinypod-selftest.bin" "$$@"' > $(OUT)/tinypod-selftest
	@chmod +x $(OUT)/tinypod-libcheck $(OUT)/tinypod-db-dump $(OUT)/tinypod-selftest
	$(CC) $(CFLAGS) -o $(OUT)/tinypod-selftest.bin tests/unit/test_all.c \
		$(filter-out $(BUILD)/main.o,$(APP_OBJS)) $(SQLITE_OBJ) $(DEC_OBJS) $(LDFLAGS)

# Starting playback must not block the caller. It is a measurement rather than
# an assertion, and only means anything running natively, so it is skipped when
# cross-compiling.
.PHONY: asynctest
asynctest: all
	@if [ "$(TARGET)" = host ]; then \
	  $(CC) $(CFLAGS) -o $(OUT)/tinypod-asynctest tests/unit/test_async_start.c \
	    $(APP_OBJS_NOMAIN) $(SQLITE_OBJ) $(DEC_OBJS) $(LDFLAGS) && \
	  $(OUT)/tinypod-asynctest; \
	else echo 'asynctest: host only, skipped'; fi

test selftest: all asynctest
	$(OUT)/tinypod-selftest.bin

check-real: all
	@if [ -z "$$TINYPOD_MOUNT" ] && [ -f testdata/local.env ]; then \
		. testdata/local.env; \
	fi; \
	if [ -z "$$TINYPOD_MOUNT" ]; then \
		echo "Set TINYPOD_MOUNT to your volume root (test only). See testdata/README.md"; \
		exit 1; \
	fi; \
	$(OUT)/tinypod --mount "$$TINYPOD_MOUNT" --backend null libcheck

clean:
	rm -rf build out

# Convenience: copy host binary to cwd
tinypod: $(OUT)/tinypod
	cp -f $(OUT)/tinypod ./tinypod
