# Standalone ROMX 0.2.0 frontend/adapter test target.
#
# This target intentionally does not become part of RetroArch's default
# binary. It links the ROMX adapter with the small libretro-common I/O layer
# and uses libromx's public API to create fresh fixtures at runtime.

ROOT            := ../..
LIBRETRO_COMMON := $(ROOT)/libretro-common
ROMX_ROOT       ?= /Volumes/Repositories/libromx
ROMX_PREFIX     ?= $(ROMX_ROOT)/build-libretro

CC              ?= cc
CFLAGS          ?= -O0 -g
CPPFLAGS        ?=
CPPFLAGS        += -std=gnu99 -Wall -Wextra -Wno-unused-parameter \
                   -DRARCH_INTERNAL -DHAVE_ROMX -DHAVE_COMPRESSION \
                   -DHAVE_MMAP -DHAVE_THREADS \
                   -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE \
                   -I$(ROOT) -I$(LIBRETRO_COMMON)/include \
                   -I$(ROMX_ROOT)/include
LDFLAGS         ?=
LDLIBS          ?=
ROMX_LIBS       ?= -L$(ROMX_PREFIX) -lromx
ARCHIVE_LIBS    ?= -lz

ifneq ($(SANITIZER),)
   CFLAGS  += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
   LDFLAGS += -fsanitize=$(SANITIZER)
endif

COMMON_SOURCES := \
   $(LIBRETRO_COMMON)/compat/compat_strl.c \
   $(LIBRETRO_COMMON)/compat/compat_posix_string.c \
   $(LIBRETRO_COMMON)/compat/compat_strcasestr.c \
   $(LIBRETRO_COMMON)/compat/compat_fnmatch.c \
   $(LIBRETRO_COMMON)/compat/fopen_utf8.c \
   $(LIBRETRO_COMMON)/encodings/encoding_deflate.c \
   $(LIBRETRO_COMMON)/encodings/encoding_crc32.c \
   $(LIBRETRO_COMMON)/encodings/encoding_utf.c \
   $(LIBRETRO_COMMON)/file/archive_file.c \
   $(LIBRETRO_COMMON)/file/archive_file_zlib.c \
   $(LIBRETRO_COMMON)/file/file_path.c \
   $(LIBRETRO_COMMON)/file/file_path_io.c \
   $(LIBRETRO_COMMON)/file/retro_dirent.c \
   $(LIBRETRO_COMMON)/rthreads/rthreads.c \
   $(LIBRETRO_COMMON)/streams/file_stream.c \
   $(LIBRETRO_COMMON)/streams/trans_stream.c \
   $(LIBRETRO_COMMON)/streams/trans_stream_deflate.c \
   $(LIBRETRO_COMMON)/streams/trans_stream_pipe.c \
   $(LIBRETRO_COMMON)/string/stdstring.c \
   $(LIBRETRO_COMMON)/time/rtime.c \
   $(LIBRETRO_COMMON)/vfs/vfs_implementation.c \
   $(LIBRETRO_COMMON)/lists/string_list.c

SOURCES := \
   romx_frontend_test.c \
   $(ROOT)/romx/romx_frontend.c \
   $(ROOT)/romx/romx_vfs.c \
   $(ROOT)/romx/romx_host_transaction.c \
   $(ROOT)/romx/romx_save_adapter.c \
   $(COMMON_SOURCES)

TARGET := romx_frontend_test

ifeq ($(shell uname -s 2>/dev/null),Darwin)
   PLATFORM_LIBS += -framework CoreFoundation
endif

.PHONY: all check run clean

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(SOURCES) $(ROMX_LIBS) \
	   $(PLATFORM_LIBS) -pthread $(ARCHIVE_LIBS) $(LDLIBS) -o $@

# Use a fresh directory so each run starts without stale mutable objects or
# thumbnail files. Set ROMX_TEST_DIR and use `make run` to retain artifacts.
check: $(TARGET)
	@set -e; test_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/romx-test.XXXXXX"); \
	trap 'rm -rf "$$test_dir"' EXIT INT TERM; \
	./$(TARGET) "$$test_dir"

run: $(TARGET)
	@test -n "$(ROMX_TEST_DIR)" || { echo "ROMX_TEST_DIR is required"; exit 2; }
	./$(TARGET) "$(ROMX_TEST_DIR)"

clean:
	rm -f $(TARGET)
