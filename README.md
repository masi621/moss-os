# mOSs-Native — System Architecture & Developer Guide

> A custom bootable Linux distribution where the `index.html` mOSs
> aesthetic is compiled into a hardware-isolated, native C environment.

> Build-fix note: this packaged revision normalizes the source tree to `sdk/`, `shell/`, `services/`, `rootfs/`, and `boot/grub/`, and fixes the libmoss / shell / bridge build path so the native shell and window manager launch cleanly from `/init`.

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Boot Sequence — Step by Step](#2-boot-sequence)
3. [Kernel Layer](#3-kernel-layer)
4. [The `/init` Script (PID 1)](#4-the-init-script)
5. [moss-bridge — The System Daemon](#5-moss-bridge)
6. [moss-shell — The Native TUI](#6-moss-shell)
7. [CRT Renderer — ncurses Post-Processor](#7-crt-renderer)
8. [Window Manager](#8-window-manager)
9. [libmoss SDK — Programming mOSs](#9-libmoss-sdk)
10. [Programming Guide — Hello World](#10-hello-world)
11. [Persistence & Database](#11-persistence--database)
12. [Asset Pipeline](#12-asset-pipeline)
13. [CLI Reference](#13-cli-reference)
14. [Build System](#14-build-system)
15. [Porting web→native translation map](#15-webtonative-translation-map)
16. [Internals Deep Dive — CRT Math](#16-crt-math)
17. [Adding Apps to the WM](#17-adding-apps)
18. [Security Model](#18-security-model)
19. [Troubleshooting](#19-troubleshooting)

---

## 1. System Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                         mOSs-Native                             │
│                                                                 │
│  Hardware                                                       │
│    CPU + GPU + RAM + Disk + Audio                               │
│         │                                                       │
│  Linux Kernel 6.6 LTS                                           │
│    DRM/KMS → framebuffer │ ALSA/HDA → audio │ evdev → input    │
│         │                                                       │
│  /init  (custom PID 1 — BusyBox ash script)                    │
│    mounts: proc sys dev run tmp                                  │
│    modprobes: GPU, sound, input                                  │
│    starts:  moss-bridge  │  plays chime  │  launches moss-shell │
│         │                │                        │             │
│  moss-bridge             │                moss-shell            │
│    /proc + /sys polls    │                  ncurses TUI         │
│    SQLite3 (scores/AMA)  │                  Terminal mode       │
│    Unix socket IPC ──────┘                  WM mode            │
│                                              libmoss SDK        │
└─────────────────────────────────────────────────────────────────┘
```

### Why native instead of a browser kiosk?

The web version of mOSs runs inside Chromium, which requires ~300 MB RAM,
a GPU driver stack, and Wayland/X11. The native version boots in **~6 seconds**
on a 512 MB machine, uses ~12 MB of RAM at idle, and gives direct access to
kernel interfaces for a true OS-level experience.

---

## 2. Boot Sequence

```
BIOS/UEFI POST
     │
     ▼
GRUB 2  (/boot/grub/grub.cfg)
  • Loads bg.png + eye.png as graphical theme
  • kernel cmdline: quiet loglevel=3 init=/init
  • 5s countdown → boots default entry
     │
     ▼
Linux Kernel 6.6
  • Mounts rootfs from initramfs (squashfs or cpio)
  • Runs /init as PID 1
     │
     ▼
/init  (ash script)
  1. mount proc, sysfs, devtmpfs, tmpfs
  2. mdev -s  (populate /dev)
  3. modprobe GPU → waits for /dev/fb0
  4. modprobe sound + input
  5. Optional: mount /dev/sda2 as persistent ext4 overlay
  6. /bin/moss-bridge --daemonize
  7. mpg123 -q chime.mp3 &
  8. setsid /bin/moss-shell </dev/tty1 >/dev/tty1
     │
     ▼
moss-shell  (ncurses, replaces display manager)
  Phase 1: Boot splash (eye ASCII art, flashing)
  Phase 2: Module loading list (15 items, animated)
  Phase 3: Login screen (username/password, mode select)
  Phase 4: Terminal or WM mode
```

---

## 3. Kernel Layer

### Configuration choices

| Feature | Config key | Why |
|---|---|---|
| DRM/KMS | `CONFIG_DRM`, `CONFIG_DRM_SIMPLEDRM` | GPU-accelerated framebuffer |
| Intel GPU | `CONFIG_DRM_I915` | Most common x86 hardware |
| AMD GPU | `CONFIG_DRM_AMDGPU` | Modern AMD desktop/laptop |
| virtio-gpu | `CONFIG_DRM_VIRTIO_GPU` | QEMU testing |
| VESA FB | `CONFIG_FB_VESA` | Fallback for all hardware |
| OverlayFS | `CONFIG_OVERLAY_FS` | Persistent data partition |
| SquashFS/zstd | `CONFIG_SQUASHFS_ZSTD` | Compressed rootfs |
| ALSA HDA | `CONFIG_SND_HDA_INTEL` | Built-in audio |
| USB HID | `CONFIG_USB_HID`, `CONFIG_HID_GENERIC` | Keyboard/mouse |
| devtmpfs | `CONFIG_DEVTMPFS_MOUNT` | Auto-mount /dev on boot |

### Silent boot

The kernel is configured with `CONFIG_CONSOLE_LOGLEVEL_DEFAULT=3`
(KERN_ERR only). The GRUB cmdline adds `quiet loglevel=3`. Combined,
the user sees no kernel messages — just the GRUB theme dissolving into
the boot splash.

### Module loading order in `/init`

The GPU driver must be probed **before** ncurses starts, because ncurses
needs the framebuffer size from the TTY. The init script waits up to
3 seconds for `/dev/fb0` to appear, then proceeds regardless (ncurses
works on a VT without a framebuffer, just at lower quality).

---

## 4. The `/init` Script

`/init` is a POSIX sh script that runs as PID 1. It is intentionally
simple — no dependency resolution, no parallel startup. The entire
userspace lives in a single process tree rooted here.

### Persistent overlay

If `/dev/sda2` exists and is ext4, the init mounts it and uses OverlayFS
to make the rootfs writable:

```
lowerdir=/              (squashfs, read-only)
upperdir=/mnt/data/upper  (ext4, writable)
workdir=/mnt/data/work
```

This means changes to `/etc/moss/config.json`, `/var/lib/moss/moss.db`,
and any user-created files in the VFS survive reboots. On ISOs without
a second partition, the system runs entirely in tmpfs (changes lost on reboot).

### Respawn loop

```sh
while true; do
    setsid /bin/moss-shell </dev/tty1 >/dev/tty1 2>&1
    sleep 1
done
```

If moss-shell crashes, it automatically relaunches. PID 1 can never exit.

---

## 5. moss-bridge

`moss-bridge` is a C daemon that is the **nervous system** of mOSs.
It replaces three distinct pieces of the web architecture:

| Web component | Native replacement |
|---|---|
| `fetch_config.json` "Quantum Cortex" mock data | `/proc` + `/sys` polling thread |
| `config.php` + JSONBin API | SQLite3 `config` table + Unix socket |
| Formspree AMA endpoint | SQLite3 `ama` table |
| High score API | SQLite3 `scores` table |

### IPC protocol

Request and response are single-line JSON terminated by `\n`.

```json
// Get system metrics
→ {"cmd": "system"}
← {"ok": true, "data": {"cpu": {"pct": 42, "temp": 61}, "mem": {...}, ...}}

// Submit high score
→ {"cmd": "submit", "game": "snake", "user": "moss", "score": 1234}
← {"ok": true}

// Get top scores
→ {"cmd": "scores", "game": "snake"}
← {"ok": true, "data": [{"user": "moss", "score": 1234, "ts": 1700000000}]}

// AMA submission
→ {"cmd": "ama", "user": "alice", "message": "What is mOSs?"}
← {"ok": true, "data": "Message received."}

// Config read/write
→ {"cmd": "config",  "key": "terminal_color"}
← {"ok": true, "data": "#00ff00"}
→ {"cmd": "setcfg",  "key": "terminal_color", "value": "#ff0000"}
← {"ok": true}
```

### Metrics poll thread

A background `pthread` polls `/proc/stat`, `/proc/meminfo`,
`/proc/net/dev`, `/sys/class/thermal/*/temp`, and `ps` every 1000ms.
It writes into a mutex-protected `MetricCache` struct. The request
handler locks the mutex, copies the cache, and serialises to JSON.
The critical section is intentionally short (memcpy only).

### SQLite WAL mode

The database uses WAL (Write-Ahead Logging) so that the shell and
bridge can read/write concurrently without blocking each other.

---

## 6. moss-shell

`moss-shell` is the native replacement for the entire `index.html`
frontend. It is structured as a state machine:

```
main()
  └─ shell_run()
       ├─ boot_sequence()      ← startup overlay + module loading
       ├─ show_login()         ← login screen (ncurses WINDOW)
       └─ [loop]
            ├─ terminal_loop() ← full-screen shell (MODE_TERMINAL)
            └─ wm_run()        ← window manager   (MODE_WM)
```

### Virtual Filesystem

The shell maintains a flat `VFSEntry[]` array that mirrors the `fileSystem`
object from `index.html`. Default entries:

```
/michal.txt     — "michalface"
/discord.txt    — Discord handle
/mymusic.txt    — Music preferences
```

User-created files (via `touch`, `mkdir`, `nano`) are appended to this
array. On systems with persistent storage, the array is serialised to
`/var/lib/moss/fs.json` and loaded on startup.

### Command synonyms

The shell resolves synonyms before dispatch, matching the web `commandSynonyms`
map:

```
dir → ls      cls → clear    neofetch → fetch
poweroff → shutdown   restart → reboot   colour → color
```

### CRT rendering during shell display

After every `term_redraw()` call, the renderer's `renderer_apply_crt()`
is invoked. This adds the scanline and flicker pass on top of whatever
ncurses has drawn, without touching the logical WINDOW content.

---

## 7. CRT Renderer

The CRT renderer (`moss_renderer.c`) is a post-processor that runs
**after** `update_panels()` but **before** `doupdate()`.

### The three passes

#### Pass 1 — Scanlines

```c
// Every even row gets A_DIM (LIGHT) or A_INVIS (STRONG)
for (int row = scanline_offset & 1; row < rows; row += 2)
    mvchgat(row, 0, cols, A_DIM, 0, NULL);
```

The `scanline_offset` increments each frame, advancing by 1 pixel
per frame. This creates the CSS `translateY(2px)` scroll animation
from `@keyframes scanline`.

#### Pass 2 — Flicker

```c
// Every 150ms (LIGHT) or 120ms (STRONG) toggle flicker_phase
// On dark phase: dim all odd rows
if (flicker_phase == 1)
    for (int row = 1; row < rows; row += 2)
        mvchgat(row, 0, cols, A_DIM, 1, NULL);
```

The 150ms / 120ms intervals directly mirror the CSS:
```css
body.crt-effect-light  { animation: flicker 0.15s infinite }
body.crt-effect-strong { animation: flicker-strong 0.12s infinite }
```

#### Pass 3 — Vignette

```c
// Dim outer ~10% of each edge
int vig_cols = cols / 10;
int vig_rows = rows / 8;
// left/right
for (int r = 0; r < rows; r++) {
    mvchgat(r, 0,              vig_cols, A_DIM, 0, NULL);
    mvchgat(r, cols-vig_cols,  vig_cols, A_DIM, 0, NULL);
}
```

This approximates the CSS radial-gradient vignette on `body::after`.

---

## 8. Window Manager

The WM (`moss_wm.c`) uses ncurses `panel.h` — each window is an
ncurses `PANEL` managed by `show_panel()` / `hide_panel()` / `top_panel()`.

### Mapping from the web WM

| Web (index.html) | Native (moss_wm.c) |
|---|---|
| `.wm-container` | `stdscr` |
| `.wm-desktop` (grid CSS) | `renderer_draw_grid()` on stdscr |
| `.wm-taskbar` | `WINDOW *taskbar` (last 2 rows) |
| `.wm-start-menu` | `WINDOW *start_menu` (pop-up) |
| `.wm-window` | `MOSS_Window` (PANEL pair: border + content) |
| `.wm-window-header` cursor drag | Arrow key + Ctrl-HJKL move |
| `wmOpenApp()` | `wm_open_app(wm, AppID)` |
| Taskbar time display | Right-aligned `strftime` in taskbar |
| Window z-order (layers) | `top_panel()` on focus |

### Window cascade

New windows are placed at `row = 2 + (n%5)*2`, `col = 4 + (n%5)*4`
so they cascade diagonally, matching the web WM's default positioning.

### Built-in apps per-frame rendering

Each built-in app has a `draw_*()` function called every event loop
iteration. Apps that need live data (system info, task manager, clock)
query `moss_sysinfo_get()` each frame. The metrics are fresh because
`moss-bridge` polls every 1 second in the background.

---

## 9. libmoss SDK

### Object model

```
MOSS_Ctx          — one per process; owns all windows
  └─ MOSS_Window  — a panel-managed window
       └─ WINDOW *ncwin   (ncurses WINDOW, content area)
       └─ WINDOW *border  (outer border + title bar)
       └─ PANEL  *panel   (z-order management)

MOSS_Terminal     — embedded terminal inside a MOSS_Window
MOSS_AsciiArt     — parsed ascii_art.json resource
MOSS_SysInfo      — metrics snapshot from moss-bridge
MOSS_Config       — parsed /etc/moss/config.json
```

### Threading model

- **Main thread**: all ncurses calls, event loop, rendering.
- **Background**: moss-bridge runs in its own process; libmoss communicates
  with it over the Unix socket. Each `moss_sysinfo_get()` call opens a
  fresh connection, does one request/response, and closes.
- **No ncurses from threads**: ncurses is not thread-safe. All rendering
  must happen on the main thread. Use `pthread_mutex` to pass data in,
  and trigger a redraw from the main event loop.

### CRT integration

`moss_render_frame()` calls `_apply_scanlines()` → `_apply_flicker()` →
`_draw_vignette()` in that order. This happens automatically inside
`moss_event_loop()`. If you write a custom event loop, call
`moss_render_frame(ctx)` at the end of each iteration.

---

## 10. Hello World

A complete native mOSs application with live system metrics:

```c
/* hello_moss.c
 * Compile: gcc -O2 hello_moss.c -lmoss -lncurses -lm -o hello_moss
 * Run on mOSs: ./hello_moss
 */
#include <moss.h>

static void on_key(MOSS_Ctx *ctx, const MOSS_Event *ev, void *ud) {
    MOSS_Window *win = (MOSS_Window *)ud;
    if (ev->type == MOSS_EVENT_KEY) {
        if (ev->key.key == 'q' || ev->key.key == MOSS_KEY_ESC)
            moss_post_quit(ctx);

        /* 'c' cycles CRT mode */
        if (ev->key.key == 'c') {
            static MOSS_CrtMode m = MOSS_CRT_OFF;
            m = (m + 1) % 3;
            moss_set_crt_mode(ctx, m);
        }
    }

    /* Redraw on every event */
    MOSS_SysInfo *si = moss_sysinfo_get();
    if (!si) return;

    moss_clear_window(win);
    moss_set_color(win, MOSS_COLOR_GREEN, MOSS_COLOR_BLACK);
    moss_set_attr(win, MOSS_ATTR_BOLD);
    moss_print(win, 1, 2, "Hello from mOSs!");

    moss_set_attr(win, MOSS_ATTR_NORMAL);
    moss_printf(win, 3, 2, "CPU  : %s", moss_sysinfo_cpu_model(si));
    moss_printf(win, 4, 2, "Usage: %d%%", moss_sysinfo_cpu_pct(si));
    moss_printf(win, 5, 2, "Temp : %d°C", moss_sysinfo_cpu_temp_c(si));

    char upbuf[64];
    moss_uptime_str(moss_sysinfo_uptime_s(si), upbuf, sizeof upbuf);
    moss_printf(win, 6, 2, "Up   : %s", upbuf);

    moss_draw_progress_bar(win, 8, 2, 36,
                            moss_sysinfo_cpu_pct(si), MOSS_COLOR_GREEN);
    moss_draw_progress_bar(win, 9, 2, 36,
                            moss_sysinfo_mem_pct(si), MOSS_COLOR_CYAN);

    moss_printf(win, 11, 2, "Press 'c' to cycle CRT | 'q' to quit");

    moss_sysinfo_free(si);
}

int main(void) {
    MOSS_Ctx    *ctx = moss_init();
    MOSS_Window *win = moss_create_window(ctx, "Hello mOSs", 44, 14);

    moss_set_crt_mode(ctx, MOSS_CRT_LIGHT);
    moss_set_event_callback(ctx, on_key, win);

    moss_window_show(win);
    moss_window_focus(win);
    moss_event_loop(ctx);

    moss_window_destroy(win);
    moss_destroy(ctx);
    return 0;
}
```

### Building your own app

On mOSs (GCC is bundled):
```sh
gcc -O2 hello_moss.c -lmoss -lncurses -lm -o hello_moss
./hello_moss
```

### SDK headers location
```
/usr/share/moss/moss.h          — main header
/usr/share/moss/moss_window.h   — window types
/usr/share/moss/moss_graphics.h — drawing types
/usr/share/moss/moss_system.h   — system types
```

---

## 11. Persistence & Database

All persistent state lives in `/var/lib/moss/moss.db` (SQLite3).

### Schema

```sql
-- High scores (replaces JSONBin highscores API)
CREATE TABLE scores (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    game     TEXT NOT NULL,          -- 'snake', 'tetris', 'mine', '2048', 'pong'
    username TEXT NOT NULL,
    score    INTEGER NOT NULL,
    ts       INTEGER DEFAULT (strftime('%s','now'))
);

-- AMA submissions (replaces Formspree endpoint)
CREATE TABLE ama (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT NOT NULL,
    message  TEXT NOT NULL,
    ts       INTEGER DEFAULT (strftime('%s','now')),
    read     INTEGER DEFAULT 0
);

-- System-wide config (replaces localStorage + config.php)
CREATE TABLE config (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
```

### Accessing the database directly

```sh
# In the shell:
run sqlite3 /var/lib/moss/moss.db

# View high scores:
.> SELECT * FROM scores ORDER BY score DESC LIMIT 10;

# Read AMA messages (admin only):
.> SELECT ts, username, message FROM ama WHERE read=0;
.> UPDATE ama SET read=1;
```

### Config persistence flow

```
User runs: color red
    → moss-shell updates its in-memory accent
    → moss-shell calls moss_bridge {"cmd":"setcfg","key":"terminal_color","value":"#ff0000"}
    → moss-bridge writes to SQLite config table
    → On next boot: /init starts moss-bridge → moss-shell reads config
      via {"cmd":"config","key":"terminal_color"} → applies saved colour
```

---

## 12. Asset Pipeline

### ascii_art.json → `/usr/share/moss/ascii/`

The build script copies `assets/ascii_art.json` into the rootfs at
`/usr/share/moss/ascii/ascii_art.json`. The libmoss JSON parser loads
this file at runtime. Keys available:

```json
{
  "eye":    "...multi-line eye art...",
  "moss":   "...mOSs logo art...",
  "cat":    "...meow art...",
  "ferris": "...Ferris the crab...",
  "gay":    "...pride flag art..."
}
```

To add new art, add a key to the JSON file and rebuild the ISO, or
copy the file to `/usr/share/moss/ascii/ascii_art.json` on a running
system (takes effect immediately on next `moss_ascii_load()` call).

### chime.mp3 → post-boot audio

`/init` runs `mpg123 -q /usr/share/moss/sounds/chime.mp3 &` asynchronously
so the shell prompt appears immediately while the chime plays. If mpg123
is not available, `aplay` is tried on a `.wav` version.

### bg.png + eye.png → GRUB theme

Both images are copied to `/boot/grub/themes/moss/`. GRUB renders
`bg.png` as the full-screen background and `eye.png` as the logo above
the menu. They must be 24-bit PNG; GRUB does not support indexed or
RGBA with complex alpha.

---

## 13. CLI Reference

### moss-shell built-in commands

| Command | Description | Flags |
|---|---|---|
| `ls` | List directory | `-l` long format, `-a` show hidden |
| `cat <file>` | Print file | |
| `cd <dir>` | Change directory | |
| `pwd` | Working directory | |
| `touch <file>` | Create file | |
| `mkdir <dir>` | Create directory | |
| `rm <file>` | Remove file | `-r` recursive, `-f` force |
| `mv <src> <dst>` | Move / rename | |
| `nano <file>` | Edit file | |
| `echo <text>` | Print text | |
| `date` | Current date/time | |
| `whoami` | Current user | |
| `clear` | Clear scrollback | |
| `fetch` | System neofetch | |
| `color <name>` | Set accent colour | `green red yellow blue cyan white magenta` |
| `crt [mode]` | CRT effect | `off light strong` |
| `bg [on/off]` | Background | |
| `size <N>` | Info about font size | |
| `wm` | Launch WM | |
| `startup [mode]` | Set startup mode | `terminal wm` |
| `login` | Re-authenticate | |
| `logout` | Log out | |
| `reboot` | Reboot | |
| `shutdown` | Power off | |
| `meow` | Cat ASCII art | |
| `ferris` | Ferris the crab | |
| `gay` | Pride flag | |
| `bounce` | Bouncing animation | `stop more random` |
| `hits` | Visit counter | |
| `ama` | Ask Me Anything | |
| `mine` | Minesweeper | |
| `snake` | Snake | |
| `tetris` | Tetris | |
| `2048` | 2048 | |
| `pong` | Pong | |
| `help` | Command list | |
| `man moss` | Full manual | |
| `run <binary>` | Execute a binary | |

### moss-fetch

```
moss-fetch              Standard neofetch layout
moss-fetch --json       Machine-readable JSON output
moss-fetch --cpu        CPU info only
moss-fetch --mem        Memory info only
moss-fetch --color none Monochrome output
```

### moss-config

```
moss-config get terminal_color
moss-config get crt_effect
moss-config set terminal_color '#ff0000'
moss-config set startup_mode wm
moss-config set audio_enabled false
```

### moss-bridge

```
moss-bridge                         # foreground, default socket
moss-bridge --daemonize             # background mode
moss-bridge --socket /run/custom.sock
moss-bridge --db /mnt/data/moss.db
```

---

## 14. Build System

```
make                 # check deps → compile services → build ISO
make dev             # run bridge + serve web locally for testing
make patch           # generate patches/index.patched.html
make services        # npm install only
make iso             # build ISO only (requires sudo)
make flash DEVICE=/dev/sdX   # flash to USB
make clean           # remove build artifacts
make clean-all       # remove everything including /tmp/moss-build
```

### Build variables

```
OUTPUT=moss-native.iso    ISO filename
ARCH=x86_64               Target architecture
KERNEL_VERSION=6.6.30     Linux kernel version
BUSYBOX_VERSION=1.36.1    BusyBox version
JOBS=8                    Parallel compile threads
WORK_DIR=/tmp/moss-build  Build scratch directory
```

### QEMU testing (no real hardware needed)

```sh
qemu-system-x86_64 \
  -m 512M \
  -cdrom moss-native.iso \
  -vga virtio \
  -enable-kvm \
  -serial stdio
```

Drop `-enable-kvm` on macOS / Windows with WSL.

---

## 15. Web→Native Translation Map

| index.html concept | Native implementation |
|---|---|
| `startBootSequence()` | `boot_sequence()` in moss_shell.c |
| `showModuleLoading()` | Module loading loop in `boot_sequence()` |
| `showLoginScreen()` | `show_login()` — ncurses form |
| `modules[]` array | `BOOT_MODULES[]` in moss_shell.c |
| `fileSystem` object | `VFSEntry[]` array in ShellState |
| `commandSynonyms` | `SYN[]` table in `handle_command()` |
| `fetch_config.json` | `moss_sysinfo_get()` → moss-bridge → /proc |
| `config.php` + JSONBin | moss-bridge SQLite `config` table |
| Formspree endpoint | moss-bridge SQLite `ama` table |
| `localStorage` settings | SQLite `config` table (via bridge) |
| `@keyframes flicker` | `_apply_flicker()` — timed A_DIM on odd rows |
| `@keyframes scanline` | `_draw_scanlines()` — A_DIM on even rows |
| `body::after` vignette | `_draw_vignette()` — edge A_DIM |
| `--terminal-color` CSS var | `MOSS_Color` passed to all draw calls |
| `.wm-window` drag | `moss_window_move()` + keyboard shortcuts |
| `wmOpenApp()` | `wm_open_app(wm, AppID)` |
| `chime.mp3` audio | `mpg123 -q chime.mp3 &` in /init |
| `ascii_art.json` | `/usr/share/moss/ascii/ascii_art.json` |
| `bg.png` / `eye.png` | GRUB theme assets + boot splash art |
| `localStorage` high scores | SQLite `scores` table |
| `initializeHitCounter()` | `moss_bridge {"cmd":"hits"}` |
| Konami code | Keyboard sequence tracker in moss_shell.c |
| Games (snake, tetris…) | WM apps (ncurses implementation) |

---

## 16. CRT Math

### Scanline period

The CSS scanline period is 2px — one dark row, one bright row:
```css
background: repeating-linear-gradient(0deg,
    rgba(0,0,0,0.15) 1px, transparent 2px);
```

In ncurses, character cells are approximately 8×16 pixels. A "1px" scanline
at 16px/row means every other character row is dimmed — exactly what
`row += 2` achieves.

### Flicker interval math

```
LIGHT:  opacity oscillates 1.0 → 0.98 every 150ms
STRONG: opacity oscillates 1.0 → 0.97 every 120ms

ncurses A_DIM reduces brightness by ~30% (terminal-dependent).
We apply it for one frame (≈ 100ms at halfdelay(1)) every 150/120ms,
giving a duty cycle of:
  LIGHT:  100/150 = 67% bright
  STRONG: 100/120 = 83% bright, but A_DIM on more rows
```

### Vignette radius

```
CSS: radial-gradient(ellipse at center, transparent 0%, rgba(0,0,0,0.3) 70%)

ncurses approximation:
  vig_cols = cols / 10   (≈ 10% from each side = 20% total edge coverage)
  vig_rows = rows / 8    (≈ 12.5% from each side)

This matches the 70% transparent center; the outer 30% is dimmed.
```

---

## 17. Adding Apps to the WM

1. Add an entry to the `AppID` enum in `moss_wm.c`.
2. Add a row to `START_MENU[]`.
3. Add a default size in `app_default_size()`.
4. Write a `draw_myapp()` function:

```c
static void draw_myapp(MOSS_Window *win, WMApp *app) {
    moss_clear_window(win);
    moss_set_color(win, MOSS_COLOR_GREEN, MOSS_COLOR_BLACK);
    moss_print(win, 1, 2, "My App");
    moss_draw_hline(win, 2, 1, 40, ACS_HLINE);
    // ... your content
}
```

5. Add a `case APP_MYAPP:` in `wm_redraw_apps()`:

```c
case APP_MYAPP: draw_myapp(app->win, app); break;
```

6. Recompile: `gcc ... shell/moss_wm.c ...`

---

## 18. Security Model

- **No network stack by default.** mOSs does not start a DHCP client or
  SSH daemon. The network kernel modules are not loaded unless explicitly
  enabled in `/etc/moss/config.json`.
- **Single-user system.** The `moss` user (uid 1000) owns all processes.
  There is no multi-session support.
- **Admin password.** The web version stores a SHA-256 hash of the admin
  password. The native version uses `crypt(3)` with SHA-512 (via
  `/etc/shadow`). The default password is `moss` — **change it**.
- **AMA messages.** All AMA submissions go into the local SQLite database.
  Only a user with filesystem access to `/var/lib/moss/moss.db` can read
  them. There is no remote submission by default.
- **USB hotplug.** mdev handles USB events. Untrusted USB devices can
  inject keystrokes (HID attack) — for kiosk deployments, disable
  `CONFIG_USB_HID` and use a PS/2 keyboard.

---

## 19. Troubleshooting

### Black screen after GRUB

The GPU driver didn't load. Boot into Recovery entry (verbose),
look for DRM error messages. Try adding `nomodeset` to the kernel
cmdline to force VESA framebuffer.

### `moss-shell: ncurses init failed`

The `TERM` environment variable is not set or the terminal doesn't
support colour. Run `export TERM=linux` in the emergency shell.

### Bridge socket not found

moss-bridge may have failed to start. Check `/var/log/moss-bridge.log`.
Common cause: `/var/lib/moss/` doesn't exist (no persistent partition).
Fix: `mkdir -p /var/lib/moss && /bin/moss-bridge &`

### No audio (chime not playing)

1. Check that ALSA loaded: `cat /proc/asound/cards`
2. Unmute: `amixer sset Master unmute`
3. Check chime file: `ls -la /usr/share/moss/sounds/chime.mp3`
4. Test manually: `mpg123 /usr/share/moss/sounds/chime.mp3`

### Build fails: `grub-mkrescue not found`

```sh
# Debian/Ubuntu:
sudo apt-get install grub-pc-bin grub-efi-amd64-bin xorriso

# Arch:
sudo pacman -S grub xorriso
```

### QEMU shows garbled graphics

Add `-vga virtio` instead of `-vga std`. The virtio-gpu driver provides
a clean KMS framebuffer that ncurses handles correctly.

---

*mOSs-Native v1.0 — built by maseratislick*
*Report bugs on Discord: maseratislick*
