# E-mu Darwin emulator. One Makefile for Windows, Linux and macOS:
#
#   Windows  MSYS2 mingw64 gcc:  make
#   Linux    gcc, libsdl2-dev, libasound2-dev, pkg-config:  make
#   macOS    Xcode command line tools, SDL2 (Homebrew: brew install sdl2):  make
#
# The program is Emu_Darwin.exe on Windows, Emu_Darwin elsewhere. Nuklear (the settings window) is
# a git submodule: after cloning, run  git submodule update --init  once.

ifeq ($(origin CC),default)
  CC := gcc
endif
UNAME := $(shell uname -s 2>/dev/null)
# objects and generated files; another folder keeps two systems' builds apart in one checkout
# (e.g. make BUILD=build-linux under WSL)
BUILD   ?= build

CFLAGS  := -std=c99 -O2 -g -Wall -Wextra -Wno-unused-parameter -Imusashi -Isrc
# make COVERT=1: windows titled "Covert Skunkworks" instead of "E-mu" (darwin.h DW_TITLE);
# make clean first when switching, the objects do not track the flag
COVERT  ?= 0
CFLAGS  += -DDW_COVERT=$(COVERT)
# make VERSION=1.1: the version the About tab shows (the release workflow passes the tag's)
ifdef VERSION
  CFLAGS += -DDW_VERSION='"$(VERSION)"'
endif
NK_INC  := -Ithird_party/nuklear

SRC := src/main.c src/machine.c src/duart.c src/rtc.c src/sim68306.c src/asic.c src/firmware.c \
       src/inflate.c src/gui.c src/v30.c src/v50.c src/wd33c93.c src/scsidisk.c src/panel.c src/settings.c \
       src/svg.c src/setdlg.c src/audio.c src/midi.c src/plat.c
MUS := musashi/m68kcpu.c musashi/m68kops.c musashi/m68kdasm.c musashi/softfloat/softfloat.c

# OS is not set in every shell: uname tells MSYS / MinGW too
ON_WINDOWS := $(filter Windows_NT,$(OS))$(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME))
ifneq ($(ON_WINDOWS),)
  EXE     := Emu_Darwin.exe
  HOSTEXE := .exe
  SRC     += src/midi_win.c
  EXTRA   := $(BUILD)/darwin_res.o
  # msvcrt's printf has no %llu; MinGW's own conforming one is needed for the 64-bit cycle counts
  CFLAGS  += -D__USE_MINGW_ANSI_STDIO=1
  LDFLAGS := -mwindows
  # SDL2 linked statically: the program runs without SDL2.dll
  LIBS    := $(filter-out -lSDL2main -mwindows -lmingw32,$(shell sdl2-config --static-libs)) -lwinmm -lole32 \
             -lcomdlg32 -lshell32 -static -static-libgcc
  # Put the compiler's own bin folder first on PATH: another toolchain earlier on PATH can bring an
  # incompatible libmpfr / libgmp DLL, which makes cc1 stop without a message.
  # MSYS2 make does not pass TMP / TEMP on, and gcc then tries C:\Windows; keep its temp files here
  $(shell mkdir -p $(BUILD)/tmp)
  export TMP := $(CURDIR)/$(BUILD)/tmp
  export TEMP := $(TMP)
  CC_BIN := $(shell command -v $(CC) 2>/dev/null)
  ifneq ($(CC_BIN),)
    CC_DIR := $(patsubst %/,%,$(dir $(CC_BIN)))
    export PATH := $(CC_DIR):$(PATH)
    CFLAGS += -I$(CC_DIR)/../include -I$(CC_DIR)/../include/SDL2
  endif
else ifeq ($(UNAME),Darwin)
  EXE     := Emu_Darwin
  SRC     += src/midi_mac.c
  # sdl2-config comes with Homebrew's SDL2; <SDL2/SDL.h> needs the prefix's include folder too.
  # A release (the build workflow) passes its own: SDL_CONFIG = a static universal SDL2's
  # sdl2-config, SDL_LIBS = its --static-libs, and CC = clang for arm64 and x86_64 together.
  SDL_CONFIG ?= sdl2-config
  SDL_CFLAGS ?= $(shell $(SDL_CONFIG) --cflags) -I$(shell $(SDL_CONFIG) --prefix)/include
  SDL_LIBS   ?= $(shell $(SDL_CONFIG) --libs)
  CFLAGS  += $(SDL_CFLAGS)
  LIBS    := $(SDL_LIBS) -framework CoreMIDI -framework CoreFoundation -lm
else
  EXE     := Emu_Darwin
  SRC     += src/midi_alsa.c
  # glibc hides POSIX calls (fseeko, strdup, ...) under -std=c99; disk images pass 2 GB
  CFLAGS  += -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 $(shell pkg-config --cflags sdl2 alsa)
  # pkg-config, not sdl2-config: under WSL the Windows MSYS2 sdl2-config can be on PATH
  LIBS    := $(shell pkg-config --libs sdl2 alsa) -lm -lpthread
endif

GEN := $(BUILD)/gen_ui_svg.c $(BUILD)/gen_font.c
OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC) $(MUS)) $(BUILD)/src/nk.o $(GEN:.c=.o) $(EXTRA)
HDR := $(wildcard src/*.h) musashi/m68kconf.h musashi/m68k.h

all: $(EXE)

$(EXE): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/src/setdlg.o: src/setdlg.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(NK_INC) -c -o $@ $<

$(BUILD)/src/%.o: src/%.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# CoreMIDI's MIDIPacketList interface has newer replacements but works everywhere: no warnings for it
$(BUILD)/src/midi_mac.o: src/midi_mac.c $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations -c -o $@ $<

# Nuklear is third-party code in its own style; its warnings are not ours to fix
$(BUILD)/src/nk.o: src/nk.c src/nk.h third_party/nuklear/nuklear.h
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -W%,$(CFLAGS)) -w $(NK_INC) -c -o $@ $<

# Files built into the program as C arrays: the panel artwork, and the settings window's font
$(BUILD)/bin2c$(HOSTEXE): tools/bin2c.c
	@mkdir -p build
	$(CC) -O2 -o $@ $<

$(BUILD)/gen_ui_svg.c: ../UI.svg $(BUILD)/bin2c$(HOSTEXE)
	$(BUILD)/bin2c$(HOSTEXE) $< $@ ui_svg

$(BUILD)/gen_font.c: third_party/nuklear/extra_font/DroidSans.ttf $(BUILD)/bin2c$(HOSTEXE)
	$(BUILD)/bin2c$(HOSTEXE) $< $@ font_ttf

$(BUILD)/gen_%.o: $(BUILD)/gen_%.c
	$(CC) -O2 -c -o $@ $<

# Explorer shows it for the file; gui.c puts it on the window and the taskbar. Elsewhere gui.c uses
# res/icon_rgba.h.
$(BUILD)/darwin_res.o: res/darwin.rc res/darwin.ico
	@mkdir -p build
	windres -i res/darwin.rc -o $(BUILD)/darwin_res.o

# Musashi is third-party code in its own style; its warnings are not ours to fix
$(BUILD)/musashi/%.o: musashi/%.c musashi/m68kconf.h musashi/m68kcpu.h
	@mkdir -p $(dir $@)
	$(CC) -O2 -g -w -Imusashi -c -o $@ $<

# The same program under another name, for testing while the program runs (Windows locks a running exe)
test: $(BUILD)/Emu_Darwin_test$(HOSTEXE)

$(BUILD)/Emu_Darwin_test$(HOSTEXE): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -rf build Emu_Darwin.exe Emu_Darwin

.PHONY: all clean test
