# simple-pty contract

This document defines the behavior that the public CLI, README, examples,
tests, and agent skill must share. A contradiction between this contract and
the executable is a defect.

## Purpose

`simple-pty` launches one program in a real pseudo-terminal and allows later
CLI invocations to observe and operate that program. It is intended for
automated operation of terminal programs such as GDB and Vim.

`simple-pty` manages raw PTY bytes. `simple-termshot` independently converts
the captured byte stream into a visible terminal screen.

## Runtime boundary

The runtime MUST consist of C code and operating-system libraries. The runtime
MUST NOT invoke a shell unless the caller explicitly names that shell as the
program. The runtime MUST NOT require a central service, privilege escalation,
an NBS component, or an AI-provider component.

Each live session MUST have exactly one private relay process. The relay MUST
own the PTY master. The target program MUST receive the PTY slave as standard
input, standard output, standard error, and controlling terminal.

## Commands

```text
simple-pty start [--rows=N] [--cols=N] [--cwd=DIR] [--name=NAME]
                 -- PROGRAM [ARG...]
simple-pty write HANDLE
simple-pty send HANDLE TEXT
simple-pty key HANDLE KEY
simple-pty output HANDLE
simple-pty position HANDLE
simple-pty wait-output HANDLE LITERAL [--from=OFFSET] [--timeout=MS]
simple-pty resize HANDLE ROWS COLS
simple-pty status HANDLE
simple-pty wait HANDLE [--timeout=MS]
simple-pty stop HANDLE
simple-pty remove HANDLE
simple-pty list
```

`start` MUST preserve each argument after `--` as exactly one target argument.
`start` MUST print exactly one session handle after the target has executed
successfully. IF target execution fails, THEN `start` MUST return a runtime
error, MUST NOT print a handle, and MUST remove the incomplete session. The
target MUST inherit the caller's environment. Program lookup MUST use `PATH`.
The defaults MUST be 24 rows and 80 columns.

`write` MUST copy standard input to the PTY without adding or removing bytes.
`send` MUST write `TEXT` followed by one carriage-return byte. `key` MUST map a
documented key name to a stable byte sequence.

`output` MUST write the complete captured PTY byte stream without text
conversion. `position` MUST print the current output byte count.

`wait-output` MUST search raw bytes at or after `OFFSET`. The default `OFFSET`
MUST be zero. `wait-output` MUST detect a literal split across any number of
PTY reads or file appends. `wait-output` MUST use a monotonic timeout.

`resize` MUST ask the owning relay to update the PTY window size. The operating
system MUST deliver the resulting terminal resize notification to the
foreground application.

`status` MUST print `running` or `exited N`. `wait` MUST wait for the target to
exit and print `N`; the default wait is unbounded. `stop` MUST request SIGTERM,
escalate to SIGKILL after one second, and retain session evidence. `remove`
MUST reject a running session and MUST delete an exited session. `list` MUST
print retained handles in lexical order, one per line.

## Session state

The environment variable `SIMPLE_PTY_DIR` MAY select the session root. The
default session root MUST be `$HOME/.simple-pty/sessions`. The root MUST be
owned by the current user. The session root and each session directory MUST
use mode `0700`. Session files and FIFOs MUST use mode `0600`.

The relay MUST leave the captured output and exit status available after the
target exits. A reboot MAY end live sessions.

## Exit status

```text
0  success
1  runtime or I/O error
2  session not found or incompatible session state
3  timeout
4  invalid command-line arguments
```

## Portability

The source MUST build as C11 on:

- Ubuntu x86-64 with GCC and Clang
- Ubuntu ARM64 with GCC and Clang
- macOS Intel with Apple Clang
- macOS ARM64 with Apple Clang

Platform-specific PTY headers and linker flags MAY differ. Public behavior
MUST remain the same.

## Documentation gate

Every command shown in `README.md` or `SKILL.md` MUST either run in automated
tests or be explicitly marked as illustrative. `SKILL.md` MUST NOT depend on a
specific AI provider, agent framework, or tool-call protocol.
