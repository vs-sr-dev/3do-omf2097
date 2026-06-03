# port_3do/host_tools/Makefile
#
# Host-side OMF asset converter for the 3DO port. Builds `dump_har` (Linux/WSL)
# linking against openomf-master/src/ sources WITHOUT modifying upstream
# (per [[feedback-isolated-project]]).
#
# Phase A: list moves of a FIGHTR*.AF file.
# Phase B (TODO): add BK palette loading + sprite RLE decode + PNG output.
#
# Run from WSL Ubuntu-24.04:
#   make           # build ./build/dump_har
#   make run-list OMF_DATA=/path/to/OMF   # smoke test against your OMF data
#   make clean

CC      = gcc
CSTD    = -std=gnu11
WARN    = -Wall -Wextra -Wno-unused-parameter
OPT     = -O0 -g
INCS    = -I../../openomf-master/src

# -ffunction-sections + --gc-sections lets the linker drop unreferenced
# functions (e.g. sd_af_save -> sd_writer_*) so we don't have to compile
# the saver/writer .c files for a read-only tool.
CFLAGS  = $(CSTD) $(WARN) $(OPT) $(INCS) -ffunction-sections -fdata-sections
LDFLAGS = -Wl,--gc-sections
LDLIBS  = -lpng

OMF_SRC := ../../openomf-master/src

# Phase A — minimal closure for sd_af_load + iterating moves.
# These are the files that must compile/link together. The list will likely
# grow as Phase B (BK + sprite decode + PNG) is added.
OPENOMF_SRCS = \
    $(OMF_SRC)/formats/af.c                   \
    $(OMF_SRC)/formats/bk.c                   \
    $(OMF_SRC)/formats/bkanim.c               \
    $(OMF_SRC)/formats/fonts.c                \
    $(OMF_SRC)/formats/move.c                 \
    $(OMF_SRC)/formats/animation.c            \
    $(OMF_SRC)/formats/script.c               \
    $(OMF_SRC)/formats/sounds.c               \
    $(OMF_SRC)/formats/sprite.c               \
    $(OMF_SRC)/formats/pcx.c                  \
    $(OMF_SRC)/formats/error.c                \
    $(OMF_SRC)/formats/taglist.c              \
    $(OMF_SRC)/formats/taglist_helpers.c      \
    $(OMF_SRC)/formats/internal/reader.c      \
    $(OMF_SRC)/formats/internal/writer.c      \
    $(OMF_SRC)/formats/internal/memreader.c   \
    $(OMF_SRC)/formats/language.c             \
    $(OMF_SRC)/formats/rgba_image.c           \
    $(OMF_SRC)/formats/vga_image.c            \
    $(OMF_SRC)/formats/palette.c              \
    $(OMF_SRC)/formats/altpal.c               \
    $(OMF_SRC)/utils/allocator.c              \
    $(OMF_SRC)/utils/c_string_util.c          \
    $(OMF_SRC)/utils/path.c                   \
    $(OMF_SRC)/utils/str.c                    \
    $(OMF_SRC)/utils/list.c                   \
    $(OMF_SRC)/utils/vector.c                 \
    $(OMF_SRC)/utils/iterator.c               \
    $(OMF_SRC)/utils/crash.c                  \
    $(OMF_SRC)/video/vga_palette.c            \
    $(OMF_SRC)/video/vga_remap.c
    # NOTE: video/vga_state.c is NOT compiled — it pulls in SDL via
    # game/game_state.h. Replaced by host_tools/vga_state_stub.c (below).

LOCAL_SHARED  = log_stub.c vga_state_stub.c
HAR_SRCS      = dump_har.c     $(LOCAL_SHARED)
FONT_SRCS     = dump_font.c    $(LOCAL_SHARED)
BK_SRCS       = dump_bk_bg.c   $(LOCAL_SHARED)
PAL_SRCS      = dump_altpals.c $(LOCAL_SHARED)
LANG_SRCS     = dump_lang.c    $(LOCAL_SHARED)

# Each tool gets its own object subtree so the OPENOMF_SRCS .o files are
# shared (built once, linked into all five).
OMF_OBJS   = $(OPENOMF_SRCS:%.c=build/%.o)
HAR_OBJS   = $(HAR_SRCS:%.c=build/%.o)   $(OMF_OBJS)
FONT_OBJS  = $(FONT_SRCS:%.c=build/%.o)  $(OMF_OBJS)
BK_OBJS    = $(BK_SRCS:%.c=build/%.o)    $(OMF_OBJS)
PAL_OBJS   = $(PAL_SRCS:%.c=build/%.o)   $(OMF_OBJS)
LANG_OBJS  = $(LANG_SRCS:%.c=build/%.o)  $(OMF_OBJS)

all: build/dump_har build/dump_font build/dump_bk_bg build/dump_altpals build/dump_lang build/build_atlas

# build_atlas is standalone — it only parses 3DO CEL files, no openomf deps.
build/build_atlas: build_atlas.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(WARN) $(OPT) build_atlas.c -o $@

build/dump_har: $(HAR_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

build/dump_font: $(FONT_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

build/dump_bk_bg: $(BK_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

build/dump_altpals: $(PAL_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

build/dump_lang: $(LANG_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Convenience: smoke-test against YOUR OMF:2097 data. Point OMF_DATA at your
# own copy of the game files, e.g.  make run-list OMF_DATA=/path/to/OMF
# (no game data ships with this repo — see README).
OMF_DATA ?= $(error set OMF_DATA to your OMF:2097 data directory, e.g. make run-list OMF_DATA=/path/to/OMF)

run-list: build/dump_har
	./build/dump_har list "$(OMF_DATA)/FIGHTR0.AF"

run-font: build/dump_font
	./build/dump_font dump "$(OMF_DATA)/CHARSMAL.DAT" 6 /tmp/font_small_stage

run-melee-bg: build/dump_bk_bg
	./build/dump_bk_bg dump "$(OMF_DATA)/MELEE.BK" /tmp/melee_bg.png

clean:
	rm -rf build

.PHONY: all clean run-list run-font run-melee-bg
