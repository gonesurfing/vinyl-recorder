CC      ?= cc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
OPT     := -O2 -g
DEFINES := -D_POSIX_C_SOURCE=200809L

PKGS_APP  := alsa ncursesw sndfile
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS_APP))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS_APP))

CFLAGS  := $(CSTD) $(WARN) $(OPT) $(DEFINES) $(PKG_CFLAGS) -Isrc
LDFLAGS := -pthread -lm $(PKG_LIBS)

APP_SRCS := src/main.c src/ring.c src/level.c src/audio.c src/recorder.c src/ui.c
APP_OBJS := $(APP_SRCS:.c=.o)

TEST_LIB_SRCS := src/ring.c src/level.c src/recorder.c
TEST_LIB_OBJS := $(TEST_LIB_SRCS:.c=.o)
TEST_SRCS := tests/run_all.c tests/test_ring.c tests/test_level.c tests/test_recorder.c
TEST_OBJS := $(TEST_SRCS:.c=.o)

.PHONY: all test clean

all: simple_record

simple_record: $(APP_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

run_tests: $(TEST_OBJS) $(TEST_LIB_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

test: run_tests
	./run_tests

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(APP_OBJS) $(TEST_OBJS) $(TEST_LIB_OBJS) simple_record run_tests
