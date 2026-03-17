# kkueinput

IME input helper for CLI programs that don't support input method composition display (e.g. Claude CLI, Node.js readline in raw mode).

## Problem

Programs using raw terminal mode intercept individual keystrokes, preventing the IME from showing the composition process. For Korean input, you can't see the syllable being built (ㅎ → 하 → 한) — the characters appear only after composition is complete, or not at all.

## Solution

A minimal GTK3 floating input window that:

1. Provides a standard text entry with full IME support
2. On **Enter**, injects the composed text into a tmux session via `tmux send-keys`
3. The target CLI program receives the text as if it were typed on the keyboard

## Requirements

- Linux with X11 (Wayland not supported)
- GTK3 (`libgtk-3-dev`)
- tmux

## Build

```sh
sudo apt install libgtk-3-dev   # one-time
make
```

## Usage

```sh
# local tmux session
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
- **tmux send-keys** injects the composed text into the target tmux session
- Supports remote sessions via SSH (`--ssh=HOST`)

## Why GTK3 instead of GTK4?

GTK4 + fcitx5-frontend-gtk4 (v5.0.12) has a [known bug](https://gitlab.gnome.org/GNOME/gtk/-/issues/4679) where space during Korean composition is inserted at the wrong position. GTK3 does not have this issue.

## Limitations

- X11 only — Wayland compositors don't support the transparent/always-on-top window hints the same way.
- Requires tmux as a runtime dependency.

## License

MIT
