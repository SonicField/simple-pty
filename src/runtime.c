#include "runtime.h"
#include "wait_output.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#else
#error "simple-pty currently supports Linux and macOS"
#endif

#define SPTY_PATH 4096
#define RELAY_POLL_MS 100
#define COPY_BUFFER 16384

struct startup_message {
    int ok;
    int stage;
    int error_number;
};

enum startup_stage {
    STARTUP_RELAY = 1,
    STARTUP_CHDIR = 2,
    STARTUP_EXEC = 3,
};

static volatile sig_atomic_t relay_stop_requested;

static void relay_signal(int signal_number)
{
    (void)signal_number;
    relay_stop_requested = 1;
}

static int path_join(char *result, size_t size, const char *left,
                     const char *right)
{
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    if (left_length + 1 + right_length + 1 > size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(result, left, left_length);
    result[left_length] = '/';
    memcpy(result + left_length + 1, right, right_length + 1);
    return 0;
}

static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t done = 0;
    while (done < length) {
        ssize_t count = write(fd, bytes + done, length - done);
        if (count > 0) {
            done += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd writable = { .fd = fd, .events = POLLOUT };
            if (poll(&writable, 1, 1000) < 0 && errno != EINTR) return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

static int copy_fd(int source, int destination)
{
    unsigned char buffer[COPY_BUFFER];
    for (;;) {
        ssize_t count = read(source, buffer, sizeof(buffer));
        if (count == 0) return 0;
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (write_all(destination, buffer, (size_t)count) < 0) return -1;
    }
}

static int sleep_ms(int milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&delay, &delay) < 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int ensure_private_directory(const char *path)
{
    struct stat info;
    if (lstat(path, &info) < 0) {
        if (errno != ENOENT || mkdir(path, 0700) < 0) return -1;
        if (lstat(path, &info) < 0) return -1;
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != getuid()) {
        errno = EPERM;
        return -1;
    }
    if ((info.st_mode & 0777) != 0700 && chmod(path, 0700) < 0) return -1;
    return 0;
}

static int session_root(char *result, size_t size, int create)
{
    const char *configured = getenv("SIMPLE_PTY_DIR");
    if (configured && *configured) {
        if (strlen(configured) + 1 > size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(result, configured, strlen(configured) + 1);
        if (create && ensure_private_directory(result) < 0) return -1;
        return 0;
    }

    const char *home = getenv("HOME");
    if (!home || !*home) {
        errno = ENOENT;
        return -1;
    }
    char base[SPTY_PATH];
    if (path_join(base, sizeof(base), home, ".simple-pty") < 0 ||
        path_join(result, size, base, "sessions") < 0)
        return -1;
    if (create && (ensure_private_directory(base) < 0 ||
                   ensure_private_directory(result) < 0))
        return -1;
    return 0;
}

static int session_directory(const char *handle, char *result, size_t size)
{
    char root[SPTY_PATH];
    if (session_root(root, sizeof(root), 0) < 0) return -1;
    return path_join(result, size, root, handle);
}

static int checked_session_directory(const char *handle, char *result,
                                     size_t size)
{
    struct stat info;
    if (session_directory(handle, result, size) < 0 || lstat(result, &info) < 0 ||
        !S_ISDIR(info.st_mode) || info.st_uid != getuid()) {
        fprintf(stderr, "simple-pty: session '%s' not found\n", handle);
        return SPTY_STATE;
    }
    return SPTY_OK;
}

static int file_path(const char *directory, const char *name,
                     char *result, size_t size)
{
    return path_join(result, size, directory, name);
}

static int create_regular_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    int result = fchmod(fd, 0600);
    int saved = errno;
    if (close(fd) < 0 && result == 0) result = -1;
    errno = saved;
    return result;
}

static int write_text_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int result = fchmod(fd, 0600);
    if (result == 0) result = write_all(fd, text, strlen(text));
    int saved = errno;
    if (close(fd) < 0 && result == 0) result = -1;
    errno = saved;
    return result;
}

static int write_number_file(const char *path, long number)
{
    char buffer[64];
    int length = snprintf(buffer, sizeof(buffer), "%ld\n", number);
    if (length < 0 || (size_t)length >= sizeof(buffer)) return -1;
    return write_text_file(path, buffer);
}

static int read_number_file(const char *path, long *number)
{
    char buffer[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t count;
    do {
        count = read(fd, buffer, sizeof(buffer) - 1);
    } while (count < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    errno = saved;
    if (count <= 0) return -1;
    buffer[count] = '\0';
    char *end = NULL;
    errno = 0;
    long parsed = strtol(buffer, &end, 10);
    if (errno || end == buffer || (*end != '\0' && *end != '\n')) return -1;
    *number = parsed;
    return 0;
}

static int file_is_regular(const char *path)
{
    struct stat info;
    return lstat(path, &info) == 0 && S_ISREG(info.st_mode) &&
           info.st_uid == getuid();
}

static int file_is_fifo(const char *path)
{
    struct stat info;
    return lstat(path, &info) == 0 && S_ISFIFO(info.st_mode) &&
           info.st_uid == getuid();
}

int spty_valid_handle(const char *handle)
{
    if (!handle || strlen(handle) != 8) return 0;
    for (size_t i = 0; i < 8; i++) {
        if (!((handle[i] >= '0' && handle[i] <= '9') ||
              (handle[i] >= 'a' && handle[i] <= 'f')))
            return 0;
    }
    return 1;
}

int spty_valid_name(const char *name)
{
    if (!name || !*name || strlen(name) > 64) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' ||
               *p == '-'))
            return 0;
    }
    return 1;
}

static int random_handle(char handle[9])
{
    unsigned char bytes[4];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < sizeof(bytes)) {
        ssize_t count = read(fd, bytes + done, sizeof(bytes) - done);
        if (count > 0) done += (size_t)count;
        else if (count < 0 && errno == EINTR) continue;
        else {
            close(fd);
            return -1;
        }
    }
    close(fd);
    snprintf(handle, 9, "%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3]);
    return 0;
}

static int make_session(char handle[9], char directory[SPTY_PATH])
{
    char root[SPTY_PATH];
    if (session_root(root, sizeof(root), 1) < 0) return -1;
    for (int attempt = 0; attempt < 100; attempt++) {
        if (random_handle(handle) < 0 ||
            path_join(directory, SPTY_PATH, root, handle) < 0)
            return -1;
        if (mkdir(directory, 0700) == 0) {
            if (chmod(directory, 0700) == 0) return 0;
            rmdir(directory);
            return -1;
        }
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

static void remove_known_files(const char *directory)
{
    static const char *const names[] = {
        "output", "input", "control", "pid", "relay_pid", "exit_code",
        "exit_code.tmp", "name", "program", "rows", "cols", NULL
    };
    char path[SPTY_PATH];
    for (size_t i = 0; names[i]; i++) {
        if (file_path(directory, names[i], path, sizeof(path)) == 0)
            unlink(path);
    }
    rmdir(directory);
}

static int write_master(int master_fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t done = 0;
    while (done < length) {
        ssize_t count = write(master_fd, bytes + done, length - done);
        if (count > 0) {
            done += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd writable = { .fd = master_fd, .events = POLLOUT };
            if (poll(&writable, 1, 1000) < 0 && errno != EINTR) return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

static int drain_master(int master_fd, int output_fd)
{
    unsigned char buffer[COPY_BUFFER];
    for (;;) {
        ssize_t count = read(master_fd, buffer, sizeof(buffer));
        if (count > 0) {
            if (write_all(output_fd, buffer, (size_t)count) < 0) return -1;
        } else if (count == 0 || (count < 0 && errno == EIO)) {
            return 1;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            return -1;
        }
    }
}

static int write_exit_code(const char *directory, int code)
{
    char final_path[SPTY_PATH], temporary_path[SPTY_PATH];
    if (file_path(directory, "exit_code", final_path, sizeof(final_path)) < 0 ||
        file_path(directory, "exit_code.tmp", temporary_path,
                  sizeof(temporary_path)) < 0)
        return -1;
    if (write_number_file(temporary_path, code) < 0) return -1;
    return rename(temporary_path, final_path);
}

static int relay_loop(int master_fd, int input_fd, int control_fd,
                      int output_fd, pid_t target_pid, const char *directory)
{
    int flags = fcntl(master_fd, F_GETFL);
    if (flags >= 0) (void)fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    int target_status = 0;
    int target_exited = 0;
    unsigned char input_buffer[COPY_BUFFER];
    char control_buffer[128];

    while (!target_exited) {
        if (relay_stop_requested) {
            (void)kill(-target_pid, SIGTERM);
            (void)kill(target_pid, SIGTERM);
            relay_stop_requested = 0;
        }

        struct pollfd descriptors[3] = {
            { .fd = master_fd, .events = POLLIN },
            { .fd = input_fd, .events = POLLIN },
            { .fd = control_fd, .events = POLLIN },
        };
        int ready = poll(descriptors, 3, RELAY_POLL_MS);
        if (ready < 0 && errno != EINTR) break;

        if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR))
            (void)drain_master(master_fd, output_fd);

        if (descriptors[1].revents & POLLIN) {
            ssize_t count = read(input_fd, input_buffer, sizeof(input_buffer));
            if (count > 0)
                (void)write_master(master_fd, input_buffer, (size_t)count);
        }

        if (descriptors[2].revents & POLLIN) {
            ssize_t count = read(control_fd, control_buffer,
                                 sizeof(control_buffer) - 1);
            if (count > 0) {
                control_buffer[count] = '\0';
                unsigned int rows, cols;
                if (sscanf(control_buffer, "resize %u %u", &rows, &cols) == 2 &&
                    rows > 0 && rows <= 10000 && cols > 0 && cols <= 10000) {
                    struct winsize window = {
                        .ws_row = (unsigned short)rows,
                        .ws_col = (unsigned short)cols,
                    };
                    (void)ioctl(master_fd, TIOCSWINSZ, &window);
                } else if (!strncmp(control_buffer, "stop\n", 5)) {
                    (void)kill(-target_pid, SIGTERM);
                    (void)kill(target_pid, SIGTERM);
                } else if (!strncmp(control_buffer, "kill\n", 5)) {
                    (void)kill(-target_pid, SIGKILL);
                    (void)kill(target_pid, SIGKILL);
                }
            }
        }

        pid_t waited = waitpid(target_pid, &target_status, WNOHANG);
        if (waited == target_pid) target_exited = 1;
        else if (waited < 0 && errno != EINTR) break;
    }

    for (int attempt = 0; attempt < 10; attempt++) {
        int drained = drain_master(master_fd, output_fd);
        if (drained != 0) break;
        sleep_ms(10);
    }

    int code = 1;
    if (target_exited && WIFEXITED(target_status)) code = WEXITSTATUS(target_status);
    else if (target_exited && WIFSIGNALED(target_status))
        code = 128 + WTERMSIG(target_status);
    (void)write_exit_code(directory, code);
    return code;
}

static void target_process(int master_fd, int slave_fd, int ready_fd,
                           int exec_fd, const struct spty_start *options)
{
    close(master_fd);
    close(ready_fd);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
        struct startup_message message = { 0, STARTUP_RELAY, errno };
        (void)write_all(exec_fd, &message, sizeof(message));
        _exit(126);
    }
    if (options->cwd && chdir(options->cwd) < 0) {
        struct startup_message message = { 0, STARTUP_CHDIR, errno };
        (void)write_all(exec_fd, &message, sizeof(message));
        _exit(126);
    }
    if (dup2(slave_fd, STDIN_FILENO) < 0 ||
        dup2(slave_fd, STDOUT_FILENO) < 0 ||
        dup2(slave_fd, STDERR_FILENO) < 0) {
        struct startup_message message = { 0, STARTUP_RELAY, errno };
        (void)write_all(exec_fd, &message, sizeof(message));
        _exit(126);
    }
    if (slave_fd > STDERR_FILENO) close(slave_fd);
    execvp(options->argv[0], options->argv);
    struct startup_message message = { 0, STARTUP_EXEC, errno };
    (void)write_all(exec_fd, &message, sizeof(message));
    _exit(127);
}

static void relay_process(int master_fd, int slave_fd, int ready_fd,
                          const struct spty_start *options,
                          const char *directory)
{
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, relay_signal);
    signal(SIGINT, relay_signal);

    char output_path[SPTY_PATH], input_path[SPTY_PATH], control_path[SPTY_PATH];
    char pid_path[SPTY_PATH];
    if (file_path(directory, "output", output_path, sizeof(output_path)) < 0 ||
        file_path(directory, "input", input_path, sizeof(input_path)) < 0 ||
        file_path(directory, "control", control_path, sizeof(control_path)) < 0 ||
        file_path(directory, "pid", pid_path, sizeof(pid_path)) < 0)
        _exit(1);

    int output_fd = open(output_path, O_WRONLY | O_APPEND);
    int input_fd = open(input_path, O_RDWR | O_NONBLOCK);
    int control_fd = open(control_path, O_RDWR | O_NONBLOCK);
    int exec_pipe[2];
    if (output_fd < 0 || input_fd < 0 || control_fd < 0 || pipe(exec_pipe) < 0 ||
        set_cloexec(output_fd) < 0 || set_cloexec(input_fd) < 0 ||
        set_cloexec(control_fd) < 0 ||
        set_cloexec(exec_pipe[1]) < 0) {
        struct startup_message message = { 0, STARTUP_RELAY, errno };
        (void)write_all(ready_fd, &message, sizeof(message));
        _exit(1);
    }

    pid_t target_pid = fork();
    if (target_pid < 0) {
        struct startup_message message = { 0, STARTUP_RELAY, errno };
        (void)write_all(ready_fd, &message, sizeof(message));
        _exit(1);
    }
    if (target_pid == 0) {
        close(exec_pipe[0]);
        target_process(master_fd, slave_fd, ready_fd, exec_pipe[1], options);
    }

    close(slave_fd);
    close(exec_pipe[1]);
    struct startup_message target_message;
    ssize_t count;
    do {
        count = read(exec_pipe[0], &target_message, sizeof(target_message));
    } while (count < 0 && errno == EINTR);
    close(exec_pipe[0]);

    if (count != 0) {
        if (count < 0) {
            target_message.ok = 0;
            target_message.stage = STARTUP_RELAY;
            target_message.error_number = errno;
        }
        (void)write_all(ready_fd, &target_message, sizeof(target_message));
        (void)waitpid(target_pid, NULL, 0);
        _exit(1);
    }

    if (write_number_file(pid_path, (long)target_pid) < 0) {
        struct startup_message message = { 0, STARTUP_RELAY, errno };
        (void)write_all(ready_fd, &message, sizeof(message));
        (void)kill(-target_pid, SIGKILL);
        (void)kill(target_pid, SIGKILL);
        (void)waitpid(target_pid, NULL, 0);
        _exit(1);
    }

    struct startup_message ready = { 1, 0, 0 };
    (void)write_all(ready_fd, &ready, sizeof(ready));
    close(ready_fd);

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        (void)dup2(null_fd, STDIN_FILENO);
        (void)dup2(null_fd, STDOUT_FILENO);
        (void)dup2(null_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO) close(null_fd);
    }

    (void)relay_loop(master_fd, input_fd, control_fd, output_fd,
                     target_pid, directory);
    close(master_fd);
    close(input_fd);
    close(control_fd);
    close(output_fd);
    unlink(input_path);
    unlink(control_path);
    _exit(0);
}

static const char *startup_stage_name(int stage)
{
    if (stage == STARTUP_CHDIR) return "chdir";
    if (stage == STARTUP_EXEC) return "exec";
    return "relay setup";
}

int spty_start(const struct spty_start *options)
{
    char handle[9], directory[SPTY_PATH];
    char output_path[SPTY_PATH], input_path[SPTY_PATH], control_path[SPTY_PATH];
    char relay_pid_path[SPTY_PATH], name_path[SPTY_PATH], program_path[SPTY_PATH];
    char rows_path[SPTY_PATH], cols_path[SPTY_PATH];
    int master_fd = -1, slave_fd = -1, ready_pipe[2] = {-1, -1};

    if (make_session(handle, directory) < 0) {
        fprintf(stderr, "simple-pty: cannot create private session: %s\n",
                strerror(errno));
        return SPTY_ERROR;
    }
    if (file_path(directory, "output", output_path, sizeof(output_path)) < 0 ||
        file_path(directory, "input", input_path, sizeof(input_path)) < 0 ||
        file_path(directory, "control", control_path, sizeof(control_path)) < 0 ||
        file_path(directory, "relay_pid", relay_pid_path,
                  sizeof(relay_pid_path)) < 0 ||
        file_path(directory, "name", name_path, sizeof(name_path)) < 0 ||
        file_path(directory, "program", program_path, sizeof(program_path)) < 0 ||
        file_path(directory, "rows", rows_path, sizeof(rows_path)) < 0 ||
        file_path(directory, "cols", cols_path, sizeof(cols_path)) < 0 ||
        create_regular_file(output_path) < 0 || mkfifo(input_path, 0600) < 0 ||
        mkfifo(control_path, 0600) < 0 ||
        chmod(input_path, 0600) < 0 || chmod(control_path, 0600) < 0 ||
        write_text_file(program_path, options->argv[0]) < 0 ||
        write_number_file(rows_path, options->rows) < 0 ||
        write_number_file(cols_path, options->cols) < 0 ||
        (options->name && write_text_file(name_path, options->name) < 0)) {
        fprintf(stderr, "simple-pty: cannot initialise session: %s\n",
                strerror(errno));
        remove_known_files(directory);
        return SPTY_ERROR;
    }

    struct winsize window = {
        .ws_row = (unsigned short)options->rows,
        .ws_col = (unsigned short)options->cols,
    };
    if (openpty(&master_fd, &slave_fd, NULL, NULL, &window) < 0 ||
        pipe(ready_pipe) < 0) {
        fprintf(stderr, "simple-pty: cannot allocate terminal: %s\n",
                strerror(errno));
        if (master_fd >= 0) close(master_fd);
        if (slave_fd >= 0) close(slave_fd);
        remove_known_files(directory);
        return SPTY_ERROR;
    }

    pid_t relay_pid = fork();
    if (relay_pid < 0) {
        fprintf(stderr, "simple-pty: cannot fork relay: %s\n", strerror(errno));
        close(master_fd);
        close(slave_fd);
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        remove_known_files(directory);
        return SPTY_ERROR;
    }
    if (relay_pid == 0) {
        close(ready_pipe[0]);
        relay_process(master_fd, slave_fd, ready_pipe[1], options, directory);
    }

    close(master_fd);
    close(slave_fd);
    close(ready_pipe[1]);
    if (write_number_file(relay_pid_path, (long)relay_pid) < 0) {
        (void)kill(relay_pid, SIGTERM);
        close(ready_pipe[0]);
        (void)waitpid(relay_pid, NULL, 0);
        remove_known_files(directory);
        return SPTY_ERROR;
    }

    struct startup_message message;
    ssize_t count;
    do {
        count = read(ready_pipe[0], &message, sizeof(message));
    } while (count < 0 && errno == EINTR);
    close(ready_pipe[0]);
    if (count != (ssize_t)sizeof(message) || !message.ok) {
        int error_number = count == (ssize_t)sizeof(message)
                         ? message.error_number : EIO;
        int stage = count == (ssize_t)sizeof(message)
                  ? message.stage : STARTUP_RELAY;
        fprintf(stderr, "simple-pty: %s failed for '%s': %s\n",
                startup_stage_name(stage), options->argv[0],
                strerror(error_number));
        (void)waitpid(relay_pid, NULL, 0);
        remove_known_files(directory);
        return SPTY_ERROR;
    }

    printf("%s\n", handle);
    return SPTY_OK;
}

static int open_session_fifo(const char *handle, const char *name)
{
    char directory[SPTY_PATH], path[SPTY_PATH];
    int rc = checked_session_directory(handle, directory, sizeof(directory));
    if (rc) return -rc;
    if (file_path(directory, name, path, sizeof(path)) < 0 || !file_is_fifo(path)) {
        fprintf(stderr, "simple-pty: session '%s' is not running\n", handle);
        return -SPTY_STATE;
    }
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "simple-pty: session '%s' is not accepting input\n", handle);
        return -SPTY_STATE;
    }
    return fd;
}

static int send_bytes(const char *handle, const void *bytes, size_t length)
{
    int fd = open_session_fifo(handle, "input");
    if (fd < 0) return -fd;
    int result = write_all(fd, bytes, length);
    int saved = errno;
    close(fd);
    errno = saved;
    if (result < 0) {
        fprintf(stderr, "simple-pty: cannot write to session '%s': %s\n",
                handle, strerror(errno));
        return SPTY_ERROR;
    }
    return SPTY_OK;
}

int spty_write_stdin(const char *handle)
{
    int fd = open_session_fifo(handle, "input");
    if (fd < 0) return -fd;
    int result = copy_fd(STDIN_FILENO, fd);
    int saved = errno;
    close(fd);
    errno = saved;
    if (result < 0) {
        fprintf(stderr, "simple-pty: write failed: %s\n", strerror(errno));
        return SPTY_ERROR;
    }
    return SPTY_OK;
}

int spty_send_text(const char *handle, const char *text)
{
    size_t length = strlen(text);
    unsigned char *buffer = malloc(length + 1);
    if (!buffer) return SPTY_ERROR;
    memcpy(buffer, text, length);
    buffer[length] = '\r';
    int result = send_bytes(handle, buffer, length + 1);
    free(buffer);
    return result;
}

int spty_send_key(const char *handle, const char *key)
{
    static const struct {
        const char *name;
        const char *bytes;
        size_t length;
    } keys[] = {
        { "Enter", "\r", 1 }, { "Escape", "\033", 1 },
        { "Tab", "\t", 1 }, { "Backspace", "\177", 1 },
        { "Up", "\033[A", 3 }, { "Down", "\033[B", 3 },
        { "Right", "\033[C", 3 }, { "Left", "\033[D", 3 },
        { "Home", "\033[H", 3 }, { "End", "\033[F", 3 },
        { "PageUp", "\033[5~", 4 }, { "PageDown", "\033[6~", 4 },
        { "Delete", "\033[3~", 4 },
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (!strcmp(key, keys[i].name))
            return send_bytes(handle, keys[i].bytes, keys[i].length);
    }
    if (!strncmp(key, "Ctrl-", 5) && key[5] >= 'A' && key[5] <= 'Z' &&
        key[6] == '\0') {
        unsigned char byte = (unsigned char)(key[5] - 'A' + 1);
        return send_bytes(handle, &byte, 1);
    }
    fprintf(stderr, "simple-pty: unknown key '%s'\n", key);
    return SPTY_BAD_ARGS;
}

static int output_path_for(const char *handle, char path[SPTY_PATH])
{
    char directory[SPTY_PATH];
    int rc = checked_session_directory(handle, directory, sizeof(directory));
    if (rc) return rc;
    if (file_path(directory, "output", path, SPTY_PATH) < 0 ||
        !file_is_regular(path)) {
        fprintf(stderr, "simple-pty: session '%s' has no valid output\n", handle);
        return SPTY_STATE;
    }
    return SPTY_OK;
}

int spty_output(const char *handle)
{
    char path[SPTY_PATH];
    int rc = output_path_for(handle, path);
    if (rc) return rc;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || copy_fd(fd, STDOUT_FILENO) < 0) {
        if (fd >= 0) close(fd);
        fprintf(stderr, "simple-pty: cannot read output: %s\n", strerror(errno));
        return SPTY_ERROR;
    }
    close(fd);
    return SPTY_OK;
}

int spty_position(const char *handle)
{
    char path[SPTY_PATH];
    int rc = output_path_for(handle, path);
    if (rc) return rc;
    struct stat info;
    if (stat(path, &info) < 0) return SPTY_ERROR;
    printf("%lld\n", (long long)info.st_size);
    return SPTY_OK;
}

int spty_wait_output(const char *handle, const char *literal,
                     long long from, int timeout_ms)
{
    char path[SPTY_PATH];
    int rc = output_path_for(handle, path);
    if (rc) return rc;
    off_t offset = (off_t)from;
    if ((long long)offset != from || !*literal) {
        fprintf(stderr, "simple-pty: invalid wait-output argument\n");
        return SPTY_BAD_ARGS;
    }
    int result = wait_output_file(path, literal, offset, timeout_ms);
    if (result == WAIT_OUTPUT_TIMEOUT) return SPTY_TIMEOUT;
    if (result == WAIT_OUTPUT_ERROR) {
        fprintf(stderr, "simple-pty: wait-output failed: %s\n", strerror(errno));
        return SPTY_ERROR;
    }
    return SPTY_OK;
}

int spty_resize(const char *handle, int rows, int cols)
{
    int fd = open_session_fifo(handle, "control");
    if (fd < 0) return -fd;
    char request[64];
    int length = snprintf(request, sizeof(request), "resize %d %d\n", rows, cols);
    int result = length > 0 && (size_t)length < sizeof(request)
               ? write_all(fd, request, (size_t)length) : -1;
    close(fd);
    if (result < 0) {
        fprintf(stderr, "simple-pty: resize failed: %s\n", strerror(errno));
        return SPTY_ERROR;
    }
    return SPTY_OK;
}

static int exit_code_for(const char *handle, int *code)
{
    char directory[SPTY_PATH], path[SPTY_PATH];
    int rc = checked_session_directory(handle, directory, sizeof(directory));
    if (rc) return rc;
    if (file_path(directory, "exit_code", path, sizeof(path)) < 0)
        return SPTY_ERROR;
    long value;
    if (read_number_file(path, &value) < 0) return -1;
    if (value < 0 || value > 255) return SPTY_ERROR;
    *code = (int)value;
    return SPTY_OK;
}

static int target_pid_for(const char *handle, pid_t *pid)
{
    char directory[SPTY_PATH], path[SPTY_PATH];
    int rc = checked_session_directory(handle, directory, sizeof(directory));
    if (rc) return rc;
    if (file_path(directory, "pid", path, sizeof(path)) < 0) return SPTY_ERROR;
    long value;
    if (read_number_file(path, &value) < 0 || value <= 1 || value > INT_MAX)
        return SPTY_STATE;
    *pid = (pid_t)value;
    return SPTY_OK;
}

int spty_status(const char *handle)
{
    int code;
    int result = exit_code_for(handle, &code);
    if (result == SPTY_OK) {
        printf("exited %d\n", code);
        return SPTY_OK;
    }
    if (result != -1) return result;
    pid_t pid;
    result = target_pid_for(handle, &pid);
    if (result) return result;
    if (kill(pid, 0) == 0 || errno == EPERM) {
        puts("running");
        return SPTY_OK;
    }
    fprintf(stderr, "simple-pty: session '%s' has inconsistent state\n", handle);
    return SPTY_STATE;
}

int spty_wait(const char *handle, int timeout_ms)
{
    int64_t started = monotonic_ms();
    if (started < 0) return SPTY_ERROR;
    for (;;) {
        int code;
        int result = exit_code_for(handle, &code);
        if (result == SPTY_OK) {
            printf("%d\n", code);
            return SPTY_OK;
        }
        if (result != -1) return result;
        int64_t now = monotonic_ms();
        if (now < 0) return SPTY_ERROR;
        if (timeout_ms >= 0 && now - started >= timeout_ms)
            return SPTY_TIMEOUT;
        sleep_ms(10);
    }
}

int spty_stop(const char *handle)
{
    int code;
    int result = exit_code_for(handle, &code);
    if (result == SPTY_OK) return SPTY_OK;
    if (result != -1) return result;

    static const char stop_request[] = "stop\n";
    int fd = open_session_fifo(handle, "control");
    if (fd < 0) return -fd;
    result = write_all(fd, stop_request, sizeof(stop_request) - 1);
    close(fd);
    if (result < 0) return SPTY_ERROR;

    int64_t deadline = monotonic_ms() + 1000;
    while (monotonic_ms() < deadline) {
        result = exit_code_for(handle, &code);
        if (result == SPTY_OK) return SPTY_OK;
        sleep_ms(10);
    }

    static const char kill_request[] = "kill\n";
    fd = open_session_fifo(handle, "control");
    if (fd < 0) return -fd;
    result = write_all(fd, kill_request, sizeof(kill_request) - 1);
    close(fd);
    if (result < 0) return SPTY_ERROR;

    deadline = monotonic_ms() + 2000;
    while (monotonic_ms() < deadline) {
        result = exit_code_for(handle, &code);
        if (result == SPTY_OK) return SPTY_OK;
        sleep_ms(10);
    }
    return SPTY_ERROR;
}

int spty_remove(const char *handle)
{
    char directory[SPTY_PATH];
    int rc = checked_session_directory(handle, directory, sizeof(directory));
    if (rc) return rc;
    int code;
    rc = exit_code_for(handle, &code);
    if (rc == -1) {
        fprintf(stderr, "simple-pty: cannot remove running session '%s'\n", handle);
        return SPTY_STATE;
    }
    if (rc) return rc;
    remove_known_files(directory);
    struct stat info;
    if (lstat(directory, &info) == 0) {
        fprintf(stderr, "simple-pty: session contains unknown files\n");
        return SPTY_ERROR;
    }
    return SPTY_OK;
}

static int compare_strings(const void *left, const void *right)
{
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

int spty_list(void)
{
    char root[SPTY_PATH];
    if (session_root(root, sizeof(root), 0) < 0) return SPTY_OK;
    DIR *directory = opendir(root);
    if (!directory) {
        if (errno == ENOENT) return SPTY_OK;
        return SPTY_ERROR;
    }
    char **handles = NULL;
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!spty_valid_handle(entry->d_name)) continue;
        char **larger = realloc(handles, (count + 1) * sizeof(*handles));
        if (!larger) {
            closedir(directory);
            for (size_t i = 0; i < count; i++) free(handles[i]);
            free(handles);
            return SPTY_ERROR;
        }
        handles = larger;
        handles[count] = strdup(entry->d_name);
        if (!handles[count]) {
            closedir(directory);
            for (size_t i = 0; i < count; i++) free(handles[i]);
            free(handles);
            return SPTY_ERROR;
        }
        count++;
    }
    closedir(directory);
    if (count > 1) qsort(handles, count, sizeof(*handles), compare_strings);
    for (size_t i = 0; i < count; i++) {
        puts(handles[i]);
        free(handles[i]);
    }
    free(handles);
    return SPTY_OK;
}
