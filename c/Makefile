# The FB-7999 toolchain.
#
#   make            build everything, and install `dw` at the top of the tree
#   make verify     rebuild the outputs and diff them against out/ from the
#                   original Python scripts
#   make clean
#
# `dw` is the one to run -- ../dw, from anywhere. The single-purpose tools it
# grew out of are still built and still documented; it links the same objects
# rather than starting them as subprocesses.

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -pedantic -Wall -Wextra
# snprintf() truncation cannot actually happen with these path lengths, but the
# compiler has no way to prove it.
CFLAGS  += -Wno-format-truncation
LDLIBS  := -lm

SRCDIR  := src
BUILD   := build

# Shared: PE/resource reading, ROM lookup, WAVEDST decoding, WAV output, bank
# decoding.
COMMON  := pe rom wav wavedst bank
# The synthesis engine, used only by dwrender.
ENGINE  := dw_wavetable dw_dsp dw_synth fb02 fm_synth juno drumkit

COMMON_O := $(addprefix $(BUILD)/,$(addsuffix .o,$(COMMON)))
ENGINE_O := $(addprefix $(BUILD)/,$(addsuffix .o,$(ENGINE)))

BIN     := $(BUILD)/fbextract
RENDER  := $(BUILD)/dwrender
FMREND  := $(BUILD)/fb02render
JUREND  := $(BUILD)/junorender
DW      := $(BUILD)/dw
BINS    := $(BIN) $(RENDER) $(FMREND) $(JUREND) $(DW)

# Where `dw` is meant to be run from. The launcher finds the rest of the tree
# from its own path, so this is ergonomics rather than a requirement.
DW_INSTALLED := ../dw

# dwplay needs ALSA (sequencer for MIDI in, PCM for audio out). Everything else
# stays dependency-free, so a machine without ALSA still builds the rest.
HAVE_ALSA := $(shell pkg-config --exists alsa 2>/dev/null && echo yes)
ifeq ($(HAVE_ALSA),yes)
PLAY      := $(BUILD)/dwplay
ALSA_LIBS := $(shell pkg-config --libs alsa)
BINS      += $(PLAY)
# dw links the live loop directly rather than spawning dwplay, and plays its
# own renders through the same device. Without ALSA it still lists and renders;
# the commands that need a sound card say so and exit.
DW_ALSA    := $(BUILD)/dwplay.o
DW_DEFS    := -DDW_HAVE_ALSA=1
endif

# Paths relative to re/c
PLUGIN  ?= ../../windows/VST2-64/fb799964.dll
REF     ?= ../out
CHECK   := $(BUILD)/check

all: $(BINS) $(DW_INSTALLED)

$(BIN): $(COMMON_O) $(BUILD)/fbextract.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(RENDER): $(COMMON_O) $(ENGINE_O) $(BUILD)/dwrender.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(FMREND): $(COMMON_O) $(ENGINE_O) $(BUILD)/fb02render.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(JUREND): $(COMMON_O) $(ENGINE_O) $(BUILD)/junorender.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(PLAY): $(COMMON_O) $(ENGINE_O) $(BUILD)/dwplay.o $(BUILD)/dwplay_cli.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) $(ALSA_LIBS)

$(DW): $(COMMON_O) $(ENGINE_O) $(DW_ALSA) $(BUILD)/dw.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) $(ALSA_LIBS)

# A copy rather than a symlink: `dw` locates the tree through /proc/self/exe,
# and a symlink resolves to the target, which would put it back in build/.
# Copying keeps `../dw` and `c/build/dw` both able to find their way.
$(DW_INSTALLED): $(DW)
	cp -f $< $@

# Header dependencies come from the compiler (-MMD -MP), not a hand-written
# list. A stale list here is not a build annoyance: dw_filter and dw_synth are
# embedded by value, so a missed header change means two objects disagree about
# a struct's size and the result is stack corruption, not a link error.
$(BUILD)/dw.o: $(SRCDIR)/dw.c | $(BUILD)
	$(CC) $(CFLAGS) $(DW_DEFS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

-include $(wildcard $(BUILD)/*.d)

# Re-derive everything from the binary and compare against the Python output.
verify: $(BIN)
	@rm -rf $(CHECK)
	@mkdir -p $(CHECK)
	@$(BIN) all $(PLUGIN) $(CHECK) > $(CHECK)/run.log || { cat $(CHECK)/run.log; exit 1; }
	@echo
	@echo "--- resources vs $(REF)/resources/.rsrc ---"
# VERSION/1 is the raw VS_VERSIONINFO blob; 7z instead renders it as the text
# file version.txt, so the two trees can only be compared with both excluded.
	@diff -r -x VERSION -x version.txt $(CHECK)/resources $(REF)/resources/.rsrc \
	    && echo "identical (excluding VERSION)"
	@echo "--- rendered waves vs $(REF)/waves ---"
	@diff -r $(CHECK)/waves $(REF)/waves && echo "identical"
	@echo "--- shape verification ---"
	@grep -A4 'shape verification' $(CHECK)/run.log

# Where the extracted resources live, for the convenience targets below.
RSRC   ?= $(REF)/resources/.rsrc
BANK   ?= $(RSRC)/BANK_A/PROGINIT
PROG   ?= 0
NOTE   ?= 60

# make play PROG=5      -- render one program and play it
play: $(RENDER)
	@$(RENDER) -w $(PLUGIN) -b $(BANK) -p '$(PROG)' -n $(NOTE) $(BUILD)/play.wav
	@command -v pw-play >/dev/null && pw-play $(BUILD)/play.wav || aplay -q $(BUILD)/play.wav

# make list             -- show the bank's 64 programs
list: $(RENDER)
	@$(RENDER) -w $(PLUGIN) -b $(BANK) -l

# make demo             -- render every program of the bank to build/demo/
demo: $(RENDER)
	@$(RENDER) -w $(PLUGIN) -b $(BANK) -a $(BUILD)/demo -n $(NOTE) -g 1.2 -t 3.0

clean:
	rm -rf $(BUILD) $(DW_INSTALLED)

.PHONY: all verify clean play list demo
