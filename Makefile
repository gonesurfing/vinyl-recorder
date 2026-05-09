CC      ?= cc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
OPT     := -O2 -g
DEFINES := -D_POSIX_C_SOURCE=200809L

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  AUDIO_SRC      := src/audio_coreaudio.c
  PLAT_LIBS      := -framework CoreAudio -framework AudioUnit -framework AudioToolbox -framework CoreFoundation
  # ncursesw lacks a pkg-config file in macOS's bundled ncurses; link plain -lncurses.
  PKG_CFLAGS_APP := $(shell pkg-config --cflags sndfile 2>/dev/null)
  PKG_LIBS_APP   := $(shell pkg-config --libs   sndfile 2>/dev/null) -lncurses
else
  AUDIO_SRC      := src/audio_alsa.c
  PKGS_APP       := alsa ncursesw sndfile
  # libatomic provides __atomic_*_8 helpers needed for _Atomic uint64_t on
  # 32-bit ARM (e.g. Raspberry Pi OS armhf). Harmless on x86_64.
  PLAT_LIBS      := -latomic
  PKG_CFLAGS_APP := $(shell pkg-config --cflags $(PKGS_APP))
  PKG_LIBS_APP   := $(shell pkg-config --libs   $(PKGS_APP))
endif

PKGS_TEST       := sndfile
PKG_CFLAGS_TEST := $(shell pkg-config --cflags $(PKGS_TEST))
PKG_LIBS_TEST   := $(shell pkg-config --libs   $(PKGS_TEST))

CFLAGS  := $(CSTD) $(WARN) $(OPT) $(DEFINES) $(PKG_CFLAGS_APP) -Isrc
LDFLAGS := -pthread -lm $(PKG_LIBS_APP) $(PLAT_LIBS)

APP_SRCS := src/main.c src/ring.c src/level.c $(AUDIO_SRC) src/recorder.c src/ui.c
APP_OBJS := $(APP_SRCS:.c=.o)

TEST_LIB_SRCS := src/ring.c src/level.c src/recorder.c
TEST_LIB_OBJS := $(TEST_LIB_SRCS:.c=.o)
TEST_SRCS := tests/run_all.c tests/test_ring.c tests/test_level.c tests/test_recorder.c
TEST_OBJS := $(TEST_SRCS:.c=.o)

# Tests don't need ALSA or ncurses — only sndfile (for the WAV roundtrip test).
# This lets the test suite build on dev hosts that lack libasound (e.g. macOS).
$(TEST_OBJS) $(TEST_LIB_OBJS): CFLAGS := $(CSTD) $(WARN) $(OPT) $(DEFINES) $(PKG_CFLAGS_TEST) -Isrc

.PHONY: all test clean

all: vinyl_recorder

vinyl_recorder: $(APP_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

run_tests: $(TEST_OBJS) $(TEST_LIB_OBJS)
	$(CC) -o $@ $^ -pthread -lm $(PKG_LIBS_TEST)

test: run_tests
	./run_tests

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(APP_OBJS) $(TEST_OBJS) $(TEST_LIB_OBJS) vinyl_recorder run_tests
