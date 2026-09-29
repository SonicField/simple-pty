#ifndef SIMPLE_PTY_WAIT_OUTPUT_H
#define SIMPLE_PTY_WAIT_OUTPUT_H

#include <sys/types.h>

#define WAIT_OUTPUT_FOUND 0
#define WAIT_OUTPUT_ERROR 1
#define WAIT_OUTPUT_TIMEOUT 3
#define WAIT_OUTPUT_MAX_PATTERN 4096

/*
 * Wait until pattern appears in an append-only file at or after start_offset.
 * timeout_ms uses CLOCK_MONOTONIC. A zero timeout performs one complete scan.
 */
int wait_output_file(const char *path, const char *pattern,
                     off_t start_offset, int timeout_ms);

#endif
