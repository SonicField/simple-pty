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
else
PTY_LIBS =
endif

TARGET = simple-pty
HELPER = simple-pty-helper
BUILD_DIR = build
TEST_SESSION = $(BUILD_DIR)/test_session
TEST_WAIT = $(BUILD_DIR)/test_wait_output
TEST_ASSERT = $(BUILD_DIR)/test_assert

CORE_SOURCES = src/session.c src/io.c src/wait.c src/wait_output.c \
	src/helper_client.c src/helper_check.c

.PHONY: all clean install test sanitize analyze

all: $(TARGET) $(HELPER)

$(TARGET): src/main.c $(CORE_SOURCES)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -o $@ $^ -lpthread $(PTY_LIBS)

$(HELPER): src/helper.c
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -o $@ $^ $(PTY_LIBS)

$(BUILD_DIR):
	mkdir -p $@

$(TEST_SESSION): tests/test_session.c $(CORE_SOURCES) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -o $@ $^ -lpthread $(PTY_LIBS)

$(TEST_WAIT): tests/test_wait_output.c src/wait_output.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -o $@ $^

$(TEST_ASSERT): tests/test_assert.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -o $@ $^

test: all $(TEST_SESSION) $(TEST_WAIT) $(TEST_ASSERT)
	./$(TEST_ASSERT)
	./$(TEST_WAIT)
	PROGRAM=./$(TARGET) ./tests/test_wait_output_cli.sh
	PROGRAM=./$(TEST_SESSION) ./tests/run_session_test.sh

install: all
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	install -m 0755 $(HELPER) "$(DESTDIR)$(BINDIR)/$(HELPER)"

sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS='-O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer' \
		all $(TEST_SESSION) $(TEST_WAIT) $(TEST_ASSERT)
	ASAN_OPTIONS=detect_leaks=1 ./$(TEST_ASSERT)
	ASAN_OPTIONS=detect_leaks=1 ./$(TEST_WAIT)
	ASAN_OPTIONS=detect_leaks=0 PROGRAM=./$(TARGET) ./tests/test_wait_output_cli.sh
	ASAN_OPTIONS=detect_leaks=0 PROGRAM=./$(TEST_SESSION) ./tests/run_session_test.sh

analyze: | $(BUILD_DIR)
	gcc $(CPPFLAGS) $(BASE_CFLAGS) -O0 -g -fanalyzer \
		-o $(BUILD_DIR)/simple-pty-analyze src/main.c $(CORE_SOURCES) \
		-lpthread $(PTY_LIBS)

clean:
	rm -f $(TARGET) $(HELPER)
	rm -rf $(BUILD_DIR)
