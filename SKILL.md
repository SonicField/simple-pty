---
name: simple-pty
description: Operate interactive terminal programs through a persistent PTY and render their terminal screen with simple-termshot.
---

# Using simple-pty safely

Use `simple-pty` when a program expects a real terminal: debuggers, editors,
REPLs, installers, and other interactive text interfaces. Use ordinary process
execution for non-interactive commands.

The program started after `--` receives its arguments directly. No shell is
inserted. If shell syntax is required, explicitly start a shell, for example
`simple-pty start -- /bin/sh -lc 'command | pipeline'`.

## Required interaction loop

1. Start one target at known dimensions and save its handle.
2. Wait for a stable raw-output marker when one is available.
3. Record `simple-pty position HANDLE` immediately before an action.
4. Perform one action with `send`, `write`, or `key`.
5. Use `simple-pty wait-output HANDLE LITERAL --from=POSITION` with a bounded
   timeout. Never let output from before the action satisfy the wait.
6. For a full-screen interface, render the current evidence with
   `simple-pty output HANDLE | simple-termshot` using matching dimensions.
7. Verify the resulting state before choosing the next action.
8. On completion, use `stop` if necessary and then `remove`.

Do not send several speculative actions at once. A timeout means the expected
raw bytes did not arrive; inspect `status` and the rendered screen before
deciding whether retrying is safe.

## Core recipe

```sh
handle=$(simple-pty start --rows=40 --cols=120 -- PROGRAM ARGUMENT)

simple-pty wait-output "$handle" 'READY_MARKER' --timeout=10000
position=$(simple-pty position "$handle")
simple-pty send "$handle" 'one line of input'
simple-pty wait-output "$handle" 'NEXT_MARKER' \
  --from="$position" --timeout=10000

simple-pty output "$handle" | simple-termshot --width=120 --height=40
simple-pty status "$handle"
simple-pty stop "$handle"
simple-pty remove "$handle"
```

`PROGRAM`, `ARGUMENT`, and the marker strings above are placeholders; this
recipe is illustrative. Quote every handle and user-controlled argument.

## Input choices

- `simple-pty send HANDLE TEXT` sends the text followed by Enter.
- `simple-pty write HANDLE` copies standard input exactly, including binary
  bytes. Use it when Enter must not be appended.
- `simple-pty key HANDLE KEY` sends a documented terminal key. Prefer it for
  Escape, arrows, paging, deletion, and control keys.
- `simple-pty resize HANDLE ROWS COLS` changes the PTY dimensions. Render with
  those same dimensions afterward.

Run `simple-pty help` for the complete key list and command syntax.

## Raw output versus the visible screen

`wait-output` searches the captured PTY bytes literally. It deliberately does
not interpret ANSI terminal controls. A word that appears on screen might not
exist contiguously in raw output, and a word in scrollback might no longer be
visible. Use literal waits for protocol-like markers; use `simple-termshot`
when the actual screen is the evidence that matters.

## Lifecycle and recovery

`simple-pty wait HANDLE --timeout=MS` prints the target's exit status. A
`simple-pty` process status of 3 means timeout, not target failure. `status`
prints either `running` or `exited N`.

If an interaction goes wrong, capture `output` before taking further action.
`stop` first requests graceful termination and then bounds the wait; it keeps
the evidence. `remove` is intentionally separate and refuses live sessions.
Use `list` to find retained handles after an interrupted automation run.

Session data is private to the current operating-system user but can contain
sensitive terminal output. Remove it when it is no longer needed.
