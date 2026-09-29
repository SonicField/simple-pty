#include "wait_output.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int tests_run;
static int tests_failed;

#define CHECK(condition, message) do { \
    tests_run++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        tests_failed++; \
    } \
} while (0)

static int make_file(char *path, size_t size)
{
    snprintf(path, size, "/tmp/simple-pty-wait.XXXXXX");
    return mkstemp(path);
}

static void write_all(int fd, const char *data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        ssize_t count = write(fd, data + offset, len - offset);
        if (count <= 0) _exit(90);
        offset += (size_t)count;
    }
}

static void test_existing_pattern(void)
{
    char path[64];
    int fd = make_file(path, sizeof(path));
    CHECK(fd >= 0, "existing: create temporary file");
    if (fd < 0) return;
    write_all(fd, "before TARGET after", 19);
    close(fd);
    CHECK(wait_output_file(path, "TARGET", 0, 0) == WAIT_OUTPUT_FOUND,
          "existing: zero-timeout scan finds existing pattern");
    unlink(path);
}

static void test_every_split(void)
{
    static const char pattern[] = "SPLIT_PATTERN_BOUNDARY";
    size_t length = strlen(pattern);

    for (size_t split = 1; split < length; split++) {
        char path[64];
        int fd = make_file(path, sizeof(path));
        CHECK(fd >= 0, "split: create temporary file");
        if (fd < 0) return;

        write_all(fd, "prefix:", 7);
        write_all(fd, pattern, split);

        pid_t writer = fork();
        CHECK(writer >= 0, "split: fork delayed writer");
        if (writer == 0) {
            struct timespec pause = { .tv_sec = 0, .tv_nsec = 20000000L };
            nanosleep(&pause, NULL);
            write_all(fd, pattern + split, length - split);
            close(fd);
            _exit(0);
        }
        close(fd);

        int result = wait_output_file(path, pattern, 0, 1000);
        int status = 0;
        waitpid(writer, &status, 0);
        CHECK(result == WAIT_OUTPUT_FOUND,
              "split: pattern found across every append boundary");
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "split: delayed writer exits successfully");
        unlink(path);
    }
}

static void test_read_buffer_boundary(void)
{
    char path[64];
    int fd = make_file(path, sizeof(path));
    CHECK(fd >= 0, "buffer boundary: create temporary file");
    if (fd < 0) return;

    char fill[4096];
    memset(fill, 'x', sizeof(fill));
    size_t remaining = 65533;
    while (remaining > 0) {
        size_t amount = remaining < sizeof(fill) ? remaining : sizeof(fill);
        write_all(fd, fill, amount);
        remaining -= amount;
    }
    write_all(fd, "BOUNDARY", 8);
    close(fd);

    CHECK(wait_output_file(path, "BOUNDARY", 0, 100) == WAIT_OUTPUT_FOUND,
          "buffer boundary: pattern spans internal read chunks");
    unlink(path);
}

static void test_start_offset(void)
{
    char path[64];
    int fd = make_file(path, sizeof(path));
    CHECK(fd >= 0, "offset: create temporary file");
    if (fd < 0) return;
    write_all(fd, "PROMPT old output ", 18);
    off_t boundary = lseek(fd, 0, SEEK_END);
    write_all(fd, "new PROMPT", 10);
    close(fd);

    CHECK(wait_output_file(path, "PROMPT", boundary, 0) == WAIT_OUTPUT_FOUND,
          "offset: finds occurrence after requested offset");
    CHECK(wait_output_file(path, "old", boundary, 20) == WAIT_OUTPUT_TIMEOUT,
          "offset: ignores occurrences before requested offset");
    unlink(path);
}

static void test_timeout_and_validation(void)
{
    char path[64];
    int fd = make_file(path, sizeof(path));
    CHECK(fd >= 0, "timeout: create temporary file");
    if (fd < 0) return;
    close(fd);

    CHECK(wait_output_file(path, "absent", 0, 30) == WAIT_OUTPUT_TIMEOUT,
          "timeout: absent pattern returns timeout");
    CHECK(wait_output_file(path, "", 0, 10) == WAIT_OUTPUT_ERROR,
          "validation: empty pattern is rejected");
    CHECK(wait_output_file(path, "x", -1, 10) == WAIT_OUTPUT_ERROR,
          "validation: negative offset is rejected");

    char *oversized = malloc(WAIT_OUTPUT_MAX_PATTERN + 2);
    CHECK(oversized != NULL, "validation: allocate oversized pattern");
    if (oversized) {
        memset(oversized, 'a', WAIT_OUTPUT_MAX_PATTERN + 1);
        oversized[WAIT_OUTPUT_MAX_PATTERN + 1] = '\0';
        CHECK(wait_output_file(path, oversized, 0, 10) == WAIT_OUTPUT_ERROR,
              "validation: oversized pattern is rejected");
        free(oversized);
    }
    unlink(path);
}

static void test_raw_bytes_are_not_screen_text(void)
{
    char path[64];
    int fd = make_file(path, sizeof(path));
    CHECK(fd >= 0, "raw bytes: create temporary file");
    if (fd < 0) return;
    static const char styled[] = "VIS\033[31mIBLE";
    write_all(fd, styled, strlen(styled));
    close(fd);

    CHECK(wait_output_file(path, "VISIBLE", 0, 20) == WAIT_OUTPUT_TIMEOUT,
          "raw bytes: ANSI-separated visual text does not falsely match");
    unlink(path);
}

int main(void)
{
    test_existing_pattern();
    test_every_split();
    test_read_buffer_boundary();
    test_start_offset();
    test_timeout_and_validation();
    test_raw_bytes_are_not_screen_text();

    printf("wait-output tests: %d passed, %d failed\n",
           tests_run - tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
