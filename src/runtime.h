#ifndef SIMPLE_PTY_RUNTIME_H
#define SIMPLE_PTY_RUNTIME_H

#define SPTY_OK 0
#define SPTY_ERROR 1
#define SPTY_STATE 2
#define SPTY_TIMEOUT 3
#define SPTY_BAD_ARGS 4

#define SPTY_MAX_OFFSET 9223372036854775807LL
#define SPTY_MAX_TIMEOUT_MS 86400000LL

struct spty_start {
    int rows;
    int cols;
    const char *cwd;
    const char *name;
    char *const *argv;
};

int spty_valid_handle(const char *handle);
int spty_valid_name(const char *name);
int spty_start(const struct spty_start *options);
int spty_write_stdin(const char *handle);
int spty_send_text(const char *handle, const char *text);
int spty_send_key(const char *handle, const char *key);
int spty_output(const char *handle);
int spty_position(const char *handle);
int spty_wait_output(const char *handle, const char *literal,
                     long long from, int timeout_ms);
int spty_resize(const char *handle, int rows, int cols);
int spty_status(const char *handle);
int spty_wait(const char *handle, int timeout_ms);
int spty_stop(const char *handle);
int spty_remove(const char *handle);
int spty_list(void);

#endif
