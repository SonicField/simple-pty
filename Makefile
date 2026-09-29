CC ?= cc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=

CPPFLAGS ?= -Isrc
BASE_CFLAGS = -Wall -Wextra -Wshadow -Werror -std=c11 \
	-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= -O2
ALL_CFLAGS = $(BASE_CFLAGS) $(CFLAGS)

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
PTY_LIBS = -lutil
else ifeq ($(UNAME_S),Darwin)
PTY_LIBS =
else
$(error simple-pty supports Linux and macOS)
endif

TARGET = simple-pty
BUILD_DIR = build
SOURCES = src/main.c src/runtime.c src/wait_output.c
HEADERS = src/runtime.h src/wait_output.h
TEST_WAIT = $(BUILD_DIR)/test_wait_output
TEST_FIXTURE = $(BUILD_DIR)/pty_fixture

.PHONY: all clean install test test-docs sanitize analyze

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -o $@ $(SOURCES) $(PTY_LIBS)

$(BUILD_DIR):
	mkdir -p $@

$(TEST_WAIT): tests/test_wait_output.c src/wait_output.c src/wait_output.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -o $@ tests/test_wait_output.c src/wait_output.c

$(TEST_FIXTURE): tests/pty_fixture.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -o $@ $<

test: all $(TEST_WAIT) $(TEST_FIXTURE)
	./$(TEST_WAIT)
	PROGRAM=./$(TARGET) FIXTURE=./$(TEST_FIXTURE) ./tests/test_cli.sh
	$(MAKE) test-docs

test-docs:
	./tests/test_docs.sh

install: $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer' \
		all $(TEST_WAIT) $(TEST_FIXTURE)
	ASAN_OPTIONS=detect_leaks=1 ./$(TEST_WAIT)
	ASAN_OPTIONS=detect_leaks=0 PROGRAM=./$(TARGET) \
		FIXTURE=./$(TEST_FIXTURE) ./tests/test_cli.sh

analyze: | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(BASE_CFLAGS) -O0 -g -fanalyzer \
		-o $(BUILD_DIR)/simple-pty-analyze $(SOURCES) $(PTY_LIBS)

clean:
	rm -f $(TARGET)
	rm -rf $(BUILD_DIR)
