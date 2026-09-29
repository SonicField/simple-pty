#include "wait_output.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WAIT_OUTPUT_CHUNK 65536
#define WAIT_OUTPUT_POLL_NS 10000000L

static int contains_bytes(const unsigned char *haystack, size_t haystack_len,
                          const unsigned char *needle, size_t needle_len)
{
    if (needle_len > haystack_len) return 0;
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (haystack[i] == needle[0] &&
            memcmp(haystack + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int monotonic_ms(int64_t *out)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    *out = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    return 0;
}

int wait_output_file(const char *path, const char *pattern,
                     off_t start_offset, int timeout_ms)
{
    if (!path || !pattern || pattern[0] == '\0' || start_offset < 0 ||
        timeout_ms < 0) {
        errno = EINVAL;
        return WAIT_OUTPUT_ERROR;
    }

    size_t pattern_len = strlen(pattern);
    if (pattern_len > WAIT_OUTPUT_MAX_PATTERN) {
        errno = E2BIG;
        return WAIT_OUTPUT_ERROR;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) return WAIT_OUTPUT_ERROR;

    unsigned char *buf = malloc(WAIT_OUTPUT_CHUNK);
    if (!buf) {
        close(fd);
        return WAIT_OUTPUT_ERROR;
    }

    int64_t started;
    if (monotonic_ms(&started) != 0) {
        free(buf);
        close(fd);
        return WAIT_OUTPUT_ERROR;
    }
    int64_t deadline = started + timeout_ms;
    off_t search_offset = start_offset;

    for (;;) {
        ssize_t count = pread(fd, buf, WAIT_OUTPUT_CHUNK, search_offset);
        if (count > 0) {
            if (contains_bytes(buf, (size_t)count,
                               (const unsigned char *)pattern, pattern_len)) {
                free(buf);
                close(fd);
                return WAIT_OUTPUT_FOUND;
            }

            size_t consumed = (size_t)count;
            size_t overlap = pattern_len - 1;
            if (consumed > overlap) {
                search_offset += (off_t)(consumed - overlap);
            }
        } else if (count < 0 && errno != EINTR) {
            free(buf);
            close(fd);
            return WAIT_OUTPUT_ERROR;
        }

        int64_t now;
        if (monotonic_ms(&now) != 0) {
            free(buf);
            close(fd);
            return WAIT_OUTPUT_ERROR;
        }
        if (now >= deadline) {
            free(buf);
            close(fd);
            return WAIT_OUTPUT_TIMEOUT;
        }

        int64_t remaining_ms = deadline - now;
        struct timespec pause = {
            .tv_sec = 0,
            .tv_nsec = WAIT_OUTPUT_POLL_NS,
        };
        if (remaining_ms < 10) pause.tv_nsec = remaining_ms * 1000000L;
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    }
}
