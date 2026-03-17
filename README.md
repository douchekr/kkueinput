# kkueinput

IME input helper for CLI programs that don't support input method composition display (e.g. Claude CLI, Node.js readline in raw mode).

## Problem

Programs using raw terminal mode intercept individual keystrokes, preventing the IME from showing the composition process. For Korean input, you can't see the syllable being built (ㅎ → 하 → 한) — the characters appear only after composition is complete, or not at all.

## Solution

A minimal GTK3 floating input window that:

1. Provides a standard text entry with full IME support
2. On **Enter**, injects the composed text directly into the controlling terminal via `TIOCSTI` ioctl
3. The target CLI program receives the text as if it were typed on the keyboard

## Requirements

- Linux with X11 (Wayland not supported)
- GTK3 (`libgtk-3-dev`)
- Kernel < 6.2 for `--tty` mode (TIOCSTI disabled on 6.2+)
- tmux for `--tmux` mode (works on any kernel)

## Build

```sh
sudo apt install libgtk-3-dev   # one-time
make
```

## Usage

```sh
# TIOCSTI mode — inject into controlling tty (kernel < 6.2)
./kkueinput --tty &

# tmux mode — send-keys to a named tmux session (any kernel)
tmux new-session -d -s work 'claude'
./kkueinput --tmux=work

# remote tmux via SSH (uses ~/.ssh/config aliases)
./kkueinput --ssh=devsvr --tmux=work
./kkueinput --ssh=user@host --tmux=work
```

Type in the floating input window, press Enter to send. Run without arguments to see help.

## Keyboard Shortcuts

| Key         | Action                      |
|-------------|-----------------------------|
| Enter       | Send text (single-line)     |
| Ctrl+Enter  | Send text (multi-line)      |
| Ctrl+X      | Close                       |
| F5          | Shrink font                 |
| F6          | Grow font                   |
| F11         | Shrink width                |
| F12         | Grow width                  |

## Mouse

### ⌨ Icon (left side)

| Action      | Function        |
|-------------|-----------------|
| Left drag   | Move window     |
| Right click | Context menu    |

### ⊕/⊖ Icon (right side)

| Action      | Function              |
|-------------|-----------------------|
| Left click  | Toggle multi-line     |

## How It Works

- **GTK3 GtkEntry** handles IME composition with full visual feedback
- **TIOCSTI ioctl** pushes each byte of the composed UTF-8 text into the terminal's input queue
- The text is sent to `/dev/tty` (the controlling terminal of the kkueinput process), so it reaches whatever program is running in that terminal
- `\r` is appended to simulate Enter (works in both raw and cooked mode)

## Why GTK3 instead of GTK4?

GTK4 + fcitx5-frontend-gtk4 (v5.0.12) has a [known bug](https://gitlab.gnome.org/GNOME/gtk/-/issues/4679) where space during Korean composition is inserted at the wrong position. GTK3 does not have this issue.

## Limitations

- **`--tty` mode**: TIOCSTI is disabled on Linux 6.2+ kernels (`CONFIG_LEGACY_TIOCSTI`). Use `--tmux` mode on 6.2+.
- **`--tty` mode**: Only works on the terminal where `kkueinput` was launched (controlling terminal).
- X11 only — Wayland compositors don't support the transparent/always-on-top window hints the same way.

## Roadmap: Kernel 6.2+ Support

TIOCSTI is disabled on Linux 6.2+ (`CONFIG_LEGACY_TIOCSTI`). Three alternative approaches are under consideration:

### Option A: forkpty (Recommended)

kkueinput spawns a PTY and runs the target CLI inside it:

```sh
./kkueinput claude    # kkueinput creates PTY, launches claude inside
```

- Writes to the PTY master fd → appears as keyboard input to the child process
- Kernel version independent — standard POSIX pty API
- Requires embedding a terminal emulator (VTE) or raw PTY I/O forwarding
- Changes the launch model from background helper to wrapper

### Option B: tmux send-keys ✅ Implemented

Delegate input injection to tmux, which owns the PTY master:

```sh
tmux new-session -d -s work 'claude'
./kkueinput --tmux=work
```

- `tmux send-keys -t <session> "<text>" Enter`
- Simple, no low-level PTY work needed
- Requires tmux as a runtime dependency

### Option C: Write to /proc/PID/fd/0

Write directly to the target process's stdin fd:

- Works in cooked mode (shell prompt)
- Does **not** work in raw mode (Claude CLI, vim, etc.) — bytes go to stdout side
- Not a viable general solution

### Decision

Option A (forkpty) is the most robust path forward. It removes the TIOCSTI dependency entirely and works on any kernel. The trade-off is increased complexity and a change from `./kkueinput &` to `./kkueinput <command>`.

## License

MIT
