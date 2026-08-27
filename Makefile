# TinyPod Makefile — WSL host + musl-static N31

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
	src/playback/tp_player.c \
	src/ui/tp_ui_fb.c

SRC_SQLITE := third_party/sqlite/sqlite3.c

INCLUDES := -Isrc -Isrc/db -Isrc/fs -Isrc/playback -Isrc/ui -Isrc/util -Ithird_party/sqlite

WARN := -Wall -Wextra -Wpedantic -Werror
# sqlite amalgamation is third_party — compiled separately without -Werror
CDEFS := -D_GNU_SOURCE -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 \
	-DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0

ifeq ($(TARGET),n31)
  CROSS ?= arm-linux-musleabi-
  CC := $(CROSS)gcc
  CFLAGS := -Os -static $(WARN) $(INCLUDES) $(CDEFS) -DTINYPOD_N31
  LDFLAGS := -static -lpthread -ldl -lm
else
  CC ?= gcc
  CFLAGS := -O2 -g $(WARN) $(INCLUDES) $(CDEFS)
  LDFLAGS := -lpthread -ldl -lm
endif

APP_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC_APP))
SQLITE_OBJ := $(BUILD)/sqlite3.o

.PHONY: all clean test selftest check-real n31 host dirs tools

all: dirs $(OUT)/tinypod tools

host:
	$(MAKE) TARGET=host all

n31:
	$(MAKE) TARGET=n31 all

dirs:
	@mkdir -p $(BUILD) $(OUT) $(BUILD)/util $(BUILD)/fs $(BUILD)/db $(BUILD)/playback $(BUILD)/ui

$(OUT)/tinypod: $(APP_OBJS) $(SQLITE_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SQLITE_OBJ): $(SRC_SQLITE)
	@mkdir -p $(dir $@)
	$(CC) -O2 $(INCLUDES) $(CDEFS) -c -o $@ $<

tools: $(OUT)/tinypod
	@mkdir -p $(OUT)
	@printf '%s\n' '#!/bin/sh' 'exec "$$(dirname "$$0")/tinypod" libcheck "$$@"' > $(OUT)/tinypod-libcheck
	@printf '%s\n' '#!/bin/sh' 'exec "$$(dirname "$$0")/tinypod" export-json "$$@"' > $(OUT)/tinypod-db-dump
	@printf '%s\n' '#!/bin/sh' 'exec "$$(dirname "$$0")/tinypod-selftest.bin" "$$@"' > $(OUT)/tinypod-selftest
	@chmod +x $(OUT)/tinypod-libcheck $(OUT)/tinypod-db-dump $(OUT)/tinypod-selftest
	$(CC) $(CFLAGS) -o $(OUT)/tinypod-selftest.bin tests/unit/test_all.c \
		$(filter-out $(BUILD)/main.o,$(APP_OBJS)) $(SQLITE_OBJ) $(LDFLAGS) || \
	$(CC) $(CFLAGS) -o $(OUT)/tinypod-selftest.bin tests/unit/test_all.c \
		src/util/tp_log.c src/util/tp_util.c src/util/tp_config.c \
		src/fs/tp_mount_detect.c src/fs/tp_path_resolve.c src/fs/tp_file_probe.c \
		src/db/tp_db_lib.c src/db/tp_db_detect.c src/db/tp_db_sqlite_itdb.c \
		src/db/tp_db_classic_itunesdb.c src/db/tp_db_raw_scan.c \
		src/playback/tp_player.c src/tp_app.c src/ui/tp_ui_fb.c \
		$(SQLITE_OBJ) $(LDFLAGS)

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
