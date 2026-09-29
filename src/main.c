#include "runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad_usage(const char *message)
{
    if (message) fprintf(stderr, "simple-pty: %s\n", message);
    fprintf(stderr, "Try 'simple-pty help' for usage.\n");
    return SPTY_BAD_ARGS;
}

static int parse_positive(const char *text, int maximum, int *value)
{
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno || end == text || *end || parsed <= 0 || parsed > maximum)
        return -1;
    *value = (int)parsed;
    return 0;
}

static int parse_nonnegative(const char *text, long long maximum,
                             long long *value)
{
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(text, &end, 10);
    if (errno || end == text || *end || parsed < 0 || parsed > maximum)
        return -1;
    *value = parsed;
    return 0;
}

static const char *option_value(const char *argument, const char *option)
{
    size_t length = strlen(option);
    if (strncmp(argument, option, length) != 0 || argument[length] != '=')
        return NULL;
    return argument + length + 1;
}

static int help(void)
{
    puts("Usage: simple-pty COMMAND [ARGUMENTS]\n"
         "\n"
         "Commands:\n"
         "  start [--rows=N] [--cols=N] [--cwd=DIR] [--name=NAME] -- PROGRAM [ARG...]\n"
         "  write HANDLE                         Copy stdin to the terminal\n"
         "  send HANDLE TEXT                     Send TEXT followed by Enter\n"
         "  key HANDLE KEY                       Send a named key\n"
         "  output HANDLE                        Print the captured raw byte stream\n"
         "  position HANDLE                      Print captured byte count\n"
         "  wait-output HANDLE LITERAL [--from=N] [--timeout=MS]\n"
         "  resize HANDLE ROWS COLS              Change terminal dimensions\n"
         "  status HANDLE                        Print 'running' or 'exited N'\n"
         "  wait HANDLE [--timeout=MS]           Wait and print target exit status\n"
         "  stop HANDLE                          Terminate, retaining evidence\n"
         "  remove HANDLE                        Remove an exited session\n"
         "  list                                 List retained sessions\n"
         "  help                                 Show this help\n"
         "\n"
         "Keys: Enter Escape Tab Backspace Up Down Left Right Home End\n"
         "      PageUp PageDown Delete Ctrl-A through Ctrl-Z\n"
         "Exit status: 0 success, 1 runtime error, 2 session state,\n"
         "             3 timeout, 4 invalid arguments");
    return SPTY_OK;
}

static int dispatch_start(int argc, char **argv)
{
    struct spty_start options = {
        .rows = 24,
        .cols = 80,
        .cwd = NULL,
        .name = NULL,
        .argv = NULL,
    };
    int separator = -1;

    for (int i = 2; i < argc; i++) {
        const char *value;
        if (strcmp(argv[i], "--") == 0) {
            separator = i;
            break;
        } else if ((value = option_value(argv[i], "--rows")) != NULL) {
            if (parse_positive(value, 10000, &options.rows) < 0)
                return bad_usage("--rows must be an integer from 1 to 10000");
        } else if ((value = option_value(argv[i], "--cols")) != NULL) {
            if (parse_positive(value, 10000, &options.cols) < 0)
                return bad_usage("--cols must be an integer from 1 to 10000");
        } else if ((value = option_value(argv[i], "--cwd")) != NULL) {
            if (!*value) return bad_usage("--cwd must not be empty");
            options.cwd = value;
        } else if ((value = option_value(argv[i], "--name")) != NULL) {
            if (!spty_valid_name(value))
                return bad_usage("--name must match [A-Za-z0-9_.-]{1,64}");
            options.name = value;
        } else {
            return bad_usage("start options must precede a required '--'");
        }
    }

    if (separator < 0 || separator + 1 >= argc)
        return bad_usage("start requires '-- PROGRAM [ARG...]'");
    options.argv = &argv[separator + 1];
    return spty_start(&options);
}

static int parse_wait_options(int argc, char **argv, int first,
                              long long *from, int *timeout_ms)
{
    for (int i = first; i < argc; i++) {
        const char *value;
        long long parsed;
        if (from && (value = option_value(argv[i], "--from")) != NULL) {
            if (parse_nonnegative(value, SPTY_MAX_OFFSET, &parsed) < 0)
                return bad_usage("--from must be a non-negative byte offset");
            *from = parsed;
        } else if ((value = option_value(argv[i], "--timeout")) != NULL) {
            if (parse_nonnegative(value, SPTY_MAX_TIMEOUT_MS, &parsed) < 0)
                return bad_usage("--timeout must be 0..86400000 milliseconds");
            *timeout_ms = (int)parsed;
        } else {
            return bad_usage("unrecognised option");
        }
    }
    return SPTY_OK;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "--help") ||
        !strcmp(argv[1], "-h"))
        return help();

    const char *command = argv[1];
    if (!strcmp(command, "start")) return dispatch_start(argc, argv);
    if (!strcmp(command, "list")) {
        if (argc != 2) return bad_usage("list takes no arguments");
        return spty_list();
    }

    if (argc < 3) return bad_usage("command requires a session handle");
    const char *handle = argv[2];
    if (!spty_valid_handle(handle))
        return bad_usage("handle must contain exactly eight lowercase hex digits");

    if (!strcmp(command, "write")) {
        if (argc != 3) return bad_usage("write takes only a handle");
        return spty_write_stdin(handle);
    }
    if (!strcmp(command, "send")) {
        if (argc != 4) return bad_usage("send requires HANDLE and one TEXT argument");
        return spty_send_text(handle, argv[3]);
    }
    if (!strcmp(command, "key")) {
        if (argc != 4) return bad_usage("key requires HANDLE and KEY");
        return spty_send_key(handle, argv[3]);
    }
    if (!strcmp(command, "output")) {
        if (argc != 3) return bad_usage("output takes only a handle");
        return spty_output(handle);
    }
    if (!strcmp(command, "position")) {
        if (argc != 3) return bad_usage("position takes only a handle");
        return spty_position(handle);
    }
    if (!strcmp(command, "wait-output")) {
        if (argc < 4) return bad_usage("wait-output requires HANDLE and LITERAL");
        long long from = 0;
        int timeout_ms = 30000;
        int rc = parse_wait_options(argc, argv, 4, &from, &timeout_ms);
        if (rc) return rc;
        return spty_wait_output(handle, argv[3], from, timeout_ms);
    }
    if (!strcmp(command, "resize")) {
        int rows, cols;
        if (argc != 5 || parse_positive(argv[3], 10000, &rows) < 0 ||
            parse_positive(argv[4], 10000, &cols) < 0)
            return bad_usage("resize requires ROWS and COLS from 1 to 10000");
        return spty_resize(handle, rows, cols);
    }
    if (!strcmp(command, "status")) {
        if (argc != 3) return bad_usage("status takes only a handle");
        return spty_status(handle);
    }
    if (!strcmp(command, "wait")) {
        int timeout_ms = -1;
        int rc = parse_wait_options(argc, argv, 3, NULL, &timeout_ms);
        if (rc) return rc;
        return spty_wait(handle, timeout_ms);
    }
    if (!strcmp(command, "stop")) {
        if (argc != 3) return bad_usage("stop takes only a handle");
        return spty_stop(handle);
    }
    if (!strcmp(command, "remove")) {
        if (argc != 3) return bad_usage("remove takes only a handle");
        return spty_remove(handle);
    }

    return bad_usage("unknown command");
}
