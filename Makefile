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
BUILD  := build/$(TARGET)
OUT    := out/$(TARGET)

SRC_APP := \
	src/main.c \
	src/tp_app.c \
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

WARN := -Wall -Wextra -Wpedantic -Werror
# sqlite and the Helix decoders are third_party — compiled without -Werror
CDEFS := -D_GNU_SOURCE -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 \
	-DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0

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
  ARCH := -march=armv7-a
  CFLAGS := -Os $(ARCH) -static $(WARN) $(INCLUDES) $(CDEFS) -DTINYPOD_N31
  DECFLAGS := -O2 $(ARCH) -DARM $(INCLUDES) $(CDEFS)
  LDFLAGS := -static $(TINYALSA_LIB) -lpthread -ldl -lm
else
  CC ?= gcc
  ARCH :=
  CFLAGS := -O2 -g $(WARN) $(INCLUDES) $(CDEFS)
  DECFLAGS := -O2 $(INCLUDES) $(CDEFS)
  LDFLAGS := $(TINYALSA_LIB) -lpthread -ldl -lm
endif

APP_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC_APP))
SQLITE_OBJ := $(BUILD)/sqlite3.o
AAC_OBJS  := $(patsubst $(HELIX_AAC_DIR)/%.c,$(BUILD)/helix-aac/%.o,$(SRC_AAC))
MP3_OBJS  := $(patsubst $(HELIX_MP3_DIR)/%.c,$(BUILD)/helix-mp3/%.o,$(SRC_MP3))
MP3R_OBJS := $(patsubst $(HELIX_MP3_DIR)/real/%.c,$(BUILD)/helix-mp3-real/%.o,$(SRC_MP3R))
# Both Helix trees have a bitstream.c, a buffers.c and more: separate object
# directories, or they quietly overwrite each other.
DEC_OBJS := $(AAC_OBJS) $(MP3_OBJS) $(MP3R_OBJS)

.PHONY: all clean test selftest check-real n31 host dirs tools decoders

all: decoders dirs $(OUT)/tinypod tools

host:
	$(MAKE) TARGET=host all

n31:
	$(MAKE) TARGET=n31 all

decoders:
	@test -f $(HELIX_AAC_DIR)/aacdec.c && test -f $(HELIX_MP3_DIR)/mp3dec.c || \
		sh tools/fetch-decoders.sh

dirs:
	@mkdir -p $(BUILD) $(OUT) $(BUILD)/util $(BUILD)/fs $(BUILD)/db \
		$(BUILD)/codec $(BUILD)/playback $(BUILD)/ui \
		$(BUILD)/helix-aac $(BUILD)/helix-mp3 $(BUILD)/helix-mp3-real

$(OUT)/tinypod: $(APP_OBJS) $(SQLITE_OBJ) $(DEC_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

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

test selftest: all
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
