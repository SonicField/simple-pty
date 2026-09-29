# simple-pty

`simple-pty` gives scripts and automated agents a persistent, real terminal.
It starts one program in a pseudo-terminal, returns a short handle, and lets
later commands send input, wait for output, resize the terminal, or inspect
the captured byte stream. This makes interactive programs such as GDB and Vim
usable without pretending they are ordinary pipes.

The design is deliberately small. The runtime is C plus operating-system
libraries. There is no service to install, no privileged helper, no implicit
shell, and no dependency on an agent framework. Each live session has one
private relay process and one target process.

## Build and install

On Ubuntu, install a C compiler and Make (the `build-essential` package is
sufficient). On macOS, install the Xcode Command Line Tools. Then run:

```sh
make
make test
```

The executable is `./simple-pty`. Install it under `/usr/local` with:

```sh
make install
```

For a user-local installation:

```sh
make install PREFIX="$HOME/.local"
```

Packagers may stage files with `DESTDIR`. The CI matrix builds and tests
Ubuntu on x86-64 and ARM64 with GCC and Clang, and macOS on Intel and Apple
silicon with Apple Clang.

## Quick start

Arguments after `--` are passed directly to the program. They are not parsed
by a shell.

```sh
handle=$(simple-pty start --name=demo -- /bin/sh)
simple-pty wait-output "$handle" '$' --timeout=5000
position=$(simple-pty position "$handle")
simple-pty send "$handle" 'printf "hello from the PTY\n"'
simple-pty wait-output "$handle" 'hello from the PTY' \
  --from="$position" --timeout=5000
simple-pty output "$handle"
simple-pty stop "$handle"
simple-pty remove "$handle"
```

The shell in this example exists only because it was explicitly requested as
the target program. Prefer direct arguments when a shell is unnecessary:

```sh
handle=$(simple-pty start --rows=40 --cols=120 -- gdb -q ./my-program)
```

## Commands

```text
simple-pty start [--rows=N] [--cols=N] [--cwd=DIR] [--name=NAME] \
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

`start` prints a handle only after the target has successfully executed.
Defaults are 24 rows and 80 columns. `--cwd` changes the target's working
directory, while `--name` records a short label. The target inherits the
environment and is found with `PATH` in the normal `execvp(3)` manner.

`write` copies standard input byte-for-byte. `send` sends one text argument
followed by carriage return (the terminal Enter key). `key` accepts `Enter`,
`Escape`, `Tab`, `Backspace`, `Up`, `Down`, `Left`, `Right`, `Home`, `End`,
`PageUp`, `PageDown`, `Delete`, and `Ctrl-A` through `Ctrl-Z`.

`output` emits the complete raw PTY byte stream. It may contain echoed input,
ANSI escapes, carriage returns, and overwritten screen content. `position`
prints its current byte length. `wait-output` performs a literal raw-byte
search, including across separate reads, from byte zero or `--from`. Its
default timeout is 30000 ms. An occurrence before `--from` does not match.

`resize` updates the PTY dimensions and causes the operating system to notify
the foreground program. `status` prints `running` or `exited N`. `wait` prints
the target's numeric exit status once known; without `--timeout` it waits
indefinitely. `stop` requests termination, escalates after one second, and
retains the output and exit status. `remove` refuses to remove a running
session. `list` prints retained handles in sorted order.

Exit statuses are stable: 0 for success, 1 for a runtime or I/O error, 2 for
a missing or incompatible session state, 3 for a timeout, and 4 for invalid
arguments. The target program's exit status is printed by `wait`; it is not
used as the `simple-pty wait` process status.

## Terminal screens with simple-termshot

PTY output is a history of terminal control bytes, not a screen. Pipe it to
[`simple-termshot`](https://github.com/SonicField/simple-termshot) to recover
the final visible screen at the same dimensions:

```sh
simple-pty output "$handle" | simple-termshot --width=120 --height=40
```

Use `wait-output` to synchronise on stable raw markers such as prompts or
program messages. For full-screen programs, inspect the rendered screen as
well: visible words can be separated or overwritten by ANSI control codes and
therefore need not occur contiguously in the raw stream.

## Safe automation pattern

Before each action, record the output position. Send exactly one action, then
wait from that position. This prevents an old prompt from satisfying a new
wait:

```sh
position=$(simple-pty position "$handle")
simple-pty send "$handle" 'next'
simple-pty wait-output "$handle" '(gdb)' \
  --from="$position" --timeout=10000
simple-pty output "$handle" | simple-termshot --width=120 --height=40
```

For Vim and other full-screen applications, use named keys rather than trying
to embed escape bytes in shell strings:

```sh
handle=$(simple-pty start --rows=40 --cols=120 -- vim README.md)
simple-pty key "$handle" Escape
simple-pty send "$handle" ':write'
simple-pty send "$handle" ':quit'
simple-pty wait "$handle" --timeout=5000
simple-pty remove "$handle"
```

The GDB and Vim snippets are illustrative because their exact screens and
startup messages vary by installed version. The CLI behavior they use is
covered by the portable fixture tests.

## State and security

By default, sessions live under `$HOME/.simple-pty/sessions`. Set
`SIMPLE_PTY_DIR` to use another session root, which is especially useful in
tests. The root and session directories are mode 0700; regular files and
FIFOs are mode 0600. Existing roots are accepted only when owned by the
current user. Handles are restricted to eight lowercase hexadecimal digits,
so they cannot traverse paths.

Anyone able to act as the same operating-system user can still inspect that
user's processes and files. `simple-pty` is a local automation tool, not a
security boundary. Captured output remains until `remove` is called.

## Verification

```sh
make test       # unit, black-box CLI, PTY, lifecycle, and documentation checks
make test-tools # real GDB and Vim sessions (requires both programs)
make sanitize   # the same core behavior under AddressSanitizer and UBSan
make analyze    # GCC path-sensitive static analysis
```

Warnings fail the build. CI also runs ShellCheck and verifies each advertised
runner's actual architecture. The behavioral contract is in
[`docs/CONTRACT.md`](docs/CONTRACT.md).

## Origin and license

`simple-pty` was extracted from
[`SonicField/nbs-framework`](https://github.com/SonicField/nbs-framework).
Its path-filtered history retains the original authorship and commit messages;
the standalone runtime then removed the NBS service and policy layers.

Released under the [MIT License](LICENSE).
