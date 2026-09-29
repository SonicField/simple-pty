#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t resized;

static void on_resize(int signal_number)
{
    (void)signal_number;
    resized = 1;
}

static int raw_terminal(void)
{
    struct termios mode;
    if (tcgetattr(STDIN_FILENO, &mode) < 0) return -1;
    mode.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    mode.c_oflag &= (tcflag_t)~OPOST;
    mode.c_cflag |= CS8;
    mode.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    mode.c_cc[VMIN] = 1;
    mode.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSANOW, &mode);
}

static int read_bytes(int expected)
{
    if (raw_terminal() < 0) return 10;
    printf("READY");
    fflush(stdout);
    for (int received = 0; received < expected;) {
        unsigned char buffer[128];
        size_t wanted = (size_t)(expected - received);
        if (wanted > sizeof(buffer)) wanted = sizeof(buffer);
        ssize_t count = read(STDIN_FILENO, buffer, wanted);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 11;
        for (ssize_t i = 0; i < count; i++) printf(" %02x", buffer[i]);
        fflush(stdout);
        received += (int)count;
    }
    return 0;
}

static void print_size(void)
{
    struct winsize size;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0) {
        printf("SIZE %u %u\n", (unsigned int)size.ws_row,
               (unsigned int)size.ws_col);
        fflush(stdout);
    }
}

static int watch_resize(void)
{
    signal(SIGWINCH, on_resize);
    print_size();
    for (;;) {
        pause();
        if (resized) {
            resized = 0;
            print_size();
        }
    }
    return 0;
}

static int probe(int argc, char **argv)
{
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return 12;
    printf("TTY %d%d%d FG %d CWD %s ARGC %d\n",
           isatty(STDIN_FILENO), isatty(STDOUT_FILENO),
           isatty(STDERR_FILENO),
           tcgetpgrp(STDIN_FILENO) == getpgrp(), cwd, argc - 2);
    for (int i = 2; i < argc; i++) printf("ARG %d <%s>\n", i - 2, argv[i]);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 64;
    if (!strcmp(argv[1], "probe")) return probe(argc, argv);
    if (!strcmp(argv[1], "bytes") && argc == 3)
        return read_bytes(atoi(argv[2]));
    if (!strcmp(argv[1], "resize")) return watch_resize();
    if (!strcmp(argv[1], "ignore-term")) {
        signal(SIGTERM, SIG_IGN);
        for (;;) pause();
    }
    if (!strcmp(argv[1], "split")) {
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 50000000L };
        printf("SPLIT_");
        fflush(stdout);
        nanosleep(&delay, NULL);
        printf("PATTERN");
        return 0;
    }
    if (!strcmp(argv[1], "exit") && argc == 3) return atoi(argv[2]);
    return 64;
}
