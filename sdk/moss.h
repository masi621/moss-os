/* =============================================================================
 *  mOSs SDK  —  moss.h
 *  Public C API for writing native mOSs applications.
 *
 *  Link with:  -lmoss -lncurses -lm
 *  Include:    #include <moss.h>
 *
 *  Design principles:
 *    - Every object is heap-allocated and returned as an opaque pointer.
 *    - The caller owns its objects and must free them with the paired
 *      moss_*_destroy() function.
 *    - Thread safety: the rendering context (MOSS_Ctx) is NOT thread-safe.
 *      Spawn worker threads for blocking I/O; render exclusively from the
 *      main thread.
 *    - Error handling: functions that can fail return MOSS_OK (0) or a
 *      negative MOSS_E_* error code.  Pointers return NULL on failure.
 *
 *  Quick-start:
 *    #include <moss.h>
 *    int main(void) {
 *        MOSS_Ctx *ctx = moss_init();
 *        MOSS_Window *win = moss_create_window(ctx, "Hello", 40, 12);
 *        moss_print(win, 1, 1, "Hello from mOSs!");
 *        moss_window_show(win);
 *        moss_event_loop(ctx);
 *        moss_destroy(ctx);
 *        return 0;
 *    }
 * =============================================================================
 */
#ifndef MOSS_H
#define MOSS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ncurses.h>
#include <panel.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <time.h>

/* ── Version ─────────────────────────────────────────────────────────────── */
#define MOSS_VERSION_MAJOR 1
#define MOSS_VERSION_MINOR 0
#define MOSS_VERSION_PATCH 0
#define MOSS_VERSION_STRING "1.0.0"

/* ── Error codes ─────────────────────────────────────────────────────────── */
#define MOSS_OK             0
#define MOSS_E_NOMEM       -1   /* malloc / realloc failed              */
#define MOSS_E_BOUNDS      -2   /* coordinate out of window bounds      */
#define MOSS_E_NOTERM      -3   /* terminal not initialised             */
#define MOSS_E_IO          -4   /* file / socket / read error           */
#define MOSS_E_NOTFOUND    -5   /* resource (ASCII art, config) missing */
#define MOSS_E_BADARG      -6   /* invalid argument                     */
#define MOSS_E_PERM        -7   /* insufficient permissions             */
#define MOSS_E_BRIDGE      -8   /* moss-bridge socket unreachable       */

/* ── Colour palette ──────────────────────────────────────────────────────── */
/* Named constants map to the terminal-colour CSS variable from index.html.  */
typedef enum {
    MOSS_COLOR_BLACK   = 0,
    MOSS_COLOR_GREEN   = 1,   /* default #00ff00  */
    MOSS_COLOR_RED     = 2,
    MOSS_COLOR_YELLOW  = 3,
    MOSS_COLOR_BLUE    = 4,
    MOSS_COLOR_MAGENTA = 5,
    MOSS_COLOR_CYAN    = 6,
    MOSS_COLOR_WHITE   = 7,
    MOSS_COLOR_DARK_GREEN  = 8,  /* #007700 — dim/secondary text   */
    MOSS_COLOR_BRIGHT_GREEN = 9, /* #55ff55 — success messages      */
    MOSS_COLOR_ORANGE  = 10,
    MOSS_COLOR_CUSTOM  = 255  /* Use moss_set_custom_color() first  */
} MOSS_Color;

/* ── Text attributes (may be OR'd together) ──────────────────────────────── */
typedef enum {
    MOSS_ATTR_NORMAL    = 0,
    MOSS_ATTR_BOLD      = (1 << 0),
    MOSS_ATTR_DIM       = (1 << 1),
    MOSS_ATTR_UNDERLINE = (1 << 2),
    MOSS_ATTR_BLINK     = (1 << 3),
    MOSS_ATTR_REVERSE   = (1 << 4),
    MOSS_ATTR_ITALIC    = (1 << 5),
} MOSS_Attr;

/* ── CRT effect levels  (mirrors index.html's crtEffectMode) ────────────── */
typedef enum {
    MOSS_CRT_OFF    = 0,
    MOSS_CRT_LIGHT  = 1,    /* 0.15s flicker, subtle scanlines */
    MOSS_CRT_STRONG = 2,    /* 0.12s flicker, heavy scanlines  */
} MOSS_CrtMode;

/* ── Event types ─────────────────────────────────────────────────────────── */
typedef enum {
    MOSS_EVENT_NONE    = 0,
    MOSS_EVENT_KEY     = 1,
    MOSS_EVENT_RESIZE  = 2,
    MOSS_EVENT_TIMER   = 3,
    MOSS_EVENT_BRIDGE  = 4,   /* system metrics update from moss-bridge */
    MOSS_EVENT_QUIT    = 5,
} MOSS_EventType;

/* ── Key codes ───────────────────────────────────────────────────────────── */
#define MOSS_KEY_NONE      0
#define MOSS_KEY_ESC       27
#define MOSS_KEY_ENTER     10
#define MOSS_KEY_BACKSPACE 127
#define MOSS_KEY_TAB       9
#define MOSS_KEY_UP        0x10001
#define MOSS_KEY_DOWN      0x10002
#define MOSS_KEY_LEFT      0x10003
#define MOSS_KEY_RIGHT     0x10004
#define MOSS_KEY_F1        0x10010
#define MOSS_KEY_F2        0x10011
#define MOSS_KEY_F3        0x10012
#define MOSS_KEY_F4        0x10013
#define MOSS_KEY_F5        0x10014
#define MOSS_KEY_F10       0x10019
#define MOSS_KEY_CTRL(c)   ((c) & 0x1f)

/* ── Forward / public types ─────────────────────────────────────────────── */
typedef struct MOSS_Ctx       MOSS_Ctx;       /* global display context    */
typedef struct MOSS_Window    MOSS_Window;    /* a floating window / panel */
typedef struct MOSS_Terminal  MOSS_Terminal;  /* an in-window terminal     */
typedef struct MOSS_Canvas    MOSS_Canvas;    /* off-screen drawing buffer */
typedef struct MOSS_SysInfo   MOSS_SysInfo;   /* snapshot from moss-bridge */
typedef struct MOSS_AsciiArt  MOSS_AsciiArt;  /* loaded ASCII art entry    */
typedef struct MOSS_Config    MOSS_Config;    /* parsed /etc/moss/config   */

/* ── Event structure ─────────────────────────────────────────────────────── */
typedef struct {
    MOSS_EventType type;
    union {
        struct { int key; int mod; }  key;       /* MOSS_EVENT_KEY    */
        struct { int rows; int cols; } resize;   /* MOSS_EVENT_RESIZE */
        struct { uint64_t id; }        timer;    /* MOSS_EVENT_TIMER  */
        struct { MOSS_SysInfo *info; } bridge;   /* MOSS_EVENT_BRIDGE */
    };
} MOSS_Event;

/* ── Callback types ──────────────────────────────────────────────────────── */
typedef void (*MOSS_EventCb)(MOSS_Ctx *ctx, const MOSS_Event *ev, void *userdata);
typedef void (*MOSS_DrawCb)(MOSS_Window *win, void *userdata);
typedef void (*MOSS_InputCb)(MOSS_Terminal *term, const char *cmd, void *ud);
typedef int  (*MOSS_CompleteCb)(const char *partial, char *buf, int n, void *ud);

/* ── Public struct layouts used by shell / wm internals ─────────────────── */
struct MOSS_Window {
    MOSS_Ctx     *ctx;
    WINDOW       *ncwin;
    WINDOW       *border;
    PANEL        *panel;
    char          title[64];
    int           x, y, w, h;
    bool          visible;
    bool          focused;
    MOSS_Color    fg, bg;
    MOSS_Attr     attr;
    MOSS_DrawCb   draw_cb;
    void         *draw_ud;
};

struct MOSS_Ctx {
    int           rows, cols;
    MOSS_CrtMode  crt_mode;
    MOSS_Color    accent;
    MOSS_Window  *windows[64];
    int           win_count;
    MOSS_Window  *focused;
    MOSS_EventCb  event_cb;
    void         *event_ud;
    bool          quit;
    int           bridge_fd;
    MOSS_Config  *config;
    SCREEN       *scr;
    struct timespec last_flicker;
    int           flicker_phase;
};

/* =============================================================================
 *  CONTEXT  — init / destroy / event loop
 * ============================================================================= */

/**
 * moss_init() — Initialise the mOSs rendering context.
 *
 * Calls ncurses initscr(), sets up colour pairs, loads /etc/moss/config.json,
 * connects to the moss-bridge Unix socket, and seeds the CRT renderer.
 *
 * Returns NULL if the terminal cannot be initialised (MOSS_E_NOTERM).
 * Must be called once before any other moss_* function.
 */
MOSS_Ctx *moss_init(void);

/**
 * moss_destroy() — Tear down the context, restore the terminal, free memory.
 * Always call this before exit(), even on error paths.
 */
void moss_destroy(MOSS_Ctx *ctx);

/**
 * moss_event_loop() — Block until the user quits (ESC or Ctrl-Q).
 *
 * Internally calls:
 *   - getch() for keyboard events
 *   - poll() on the bridge socket for metrics updates
 *   - select() for timer callbacks
 * Dispatches to registered callbacks, then calls moss_render_frame().
 */
void moss_event_loop(MOSS_Ctx *ctx);

/**
 * moss_set_event_callback() — Register a global event handler.
 * Only one global handler is active at a time; subsequent calls overwrite.
 */
void moss_set_event_callback(MOSS_Ctx *ctx, MOSS_EventCb cb, void *userdata);

/**
 * moss_post_quit() — Signal the event loop to exit cleanly.
 */
void moss_post_quit(MOSS_Ctx *ctx);

/**
 * moss_get_dims() — Get current terminal dimensions.
 */
void moss_get_dims(const MOSS_Ctx *ctx, int *rows, int *cols);

/* =============================================================================
 *  WINDOWS  — create / configure / destroy
 * ============================================================================= */

/**
 * moss_create_window() — Create a floating window with a green border.
 *
 * @param ctx    The active rendering context.
 * @param title  Title shown in the window header bar  (max 64 chars).
 * @param w      Width  in columns  (0 = full screen width).
 * @param h      Height in rows     (0 = full screen height).
 *
 * Windows start hidden. Call moss_window_show() to make them visible.
 * The window manager allows dragging via the header bar (mouse + keyboard).
 *
 * Returns NULL on MOSS_E_NOMEM or MOSS_E_NOTERM.
 */
MOSS_Window *moss_create_window(MOSS_Ctx *ctx,
                                 const char *title,
                                 int w, int h);

/** moss_window_show() — Make the window visible and bring it to the front. */
void moss_window_show(MOSS_Window *win);

/** moss_window_hide() — Hide but do not destroy the window. */
void moss_window_hide(MOSS_Window *win);

/** moss_window_focus() — Raise and focus the window (highlighted border). */
void moss_window_focus(MOSS_Window *win);

/**
 * moss_window_move() — Set the window's top-left corner.
 * Negative values are clamped to 0. Out-of-screen values are clamped to
 * screen edges.
 */
int moss_window_move(MOSS_Window *win, int row, int col);

/**
 * moss_window_resize() — Resize the window content area.
 * Existing content is clipped or padded as needed.
 */
int moss_window_resize(MOSS_Window *win, int new_w, int new_h);

/**
 * moss_window_set_draw_callback() — Register a per-frame draw function.
 * Called every time moss_render_frame() refreshes the window content.
 */
void moss_window_set_draw_callback(MOSS_Window *win, MOSS_DrawCb cb, void *ud);

/** moss_window_destroy() — Destroy and free the window. */
void moss_window_destroy(MOSS_Window *win);

/* =============================================================================
 *  DRAWING  — print text and shapes inside a window
 * ============================================================================= */

/**
 * moss_print() — Print a string at (row, col) inside the window's content area.
 *
 * @param win   Target window.
 * @param row   Row relative to the window's content area (0-indexed).
 * @param col   Column relative to the window's content area (0-indexed).
 * @param text  Null-terminated UTF-8 string.  \n advances to the next row.
 *
 * Returns MOSS_E_BOUNDS if (row, col) is outside the content area.
 */
int moss_print(MOSS_Window *win, int row, int col, const char *text);

/**
 * moss_printf() — printf-style version of moss_print().
 */
int moss_printf(MOSS_Window *win, int row, int col, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/**
 * moss_set_color() — Set the active colour for subsequent draw calls.
 * Does not affect already-drawn content.
 */
void moss_set_color(MOSS_Window *win, MOSS_Color fg, MOSS_Color bg);

/**
 * moss_set_attr() — Set text attributes for subsequent draw calls.
 * Pass MOSS_ATTR_NORMAL to reset.
 */
void moss_set_attr(MOSS_Window *win, MOSS_Attr attr);

/**
 * moss_clear_window() — Erase all content inside the window (not the border).
 */
void moss_clear_window(MOSS_Window *win);

/**
 * moss_draw_hline() — Draw a horizontal line using the given character.
 * @param ch  Character to draw (0 = use ACS_HLINE box-drawing).
 */
int moss_draw_hline(MOSS_Window *win, int row, int col, int len, chtype ch);

/**
 * moss_draw_vline() — Draw a vertical line.
 */
int moss_draw_vline(MOSS_Window *win, int row, int col, int len, chtype ch);

/**
 * moss_draw_box() — Draw a rectangle border inside the window.
 */
int moss_draw_box(MOSS_Window *win, int row, int col, int w, int h);

/**
 * moss_draw_progress_bar() — Render a [====    ] progress bar.
 *
 * @param pct  0–100 percent fill.
 */
int moss_draw_progress_bar(MOSS_Window *win, int row, int col,
                            int width, int pct, MOSS_Color color);

/* =============================================================================
 *  TERMINAL  — embedded terminal emulator inside a window
 * ============================================================================= */

/**
 * moss_create_terminal() — Embed a scrollback terminal inside a window.
 *
 * The terminal provides:
 *   - Scrollback buffer (default 500 lines)
 *   - Colour output via ANSI escape codes
 *   - Command history (↑↓)
 *   - Tab completion hook (set via moss_terminal_set_complete_cb)
 *
 * @param win   Host window (must already be created).
 * Returns NULL on failure.
 */
MOSS_Terminal *moss_create_terminal(MOSS_Window *win);

/**
 * moss_terminal_write() — Append text to the terminal output buffer.
 * Supports a subset of ANSI escape codes: \033[32m (colour), \033[0m (reset).
 * Newlines scroll the buffer.
 */
int moss_terminal_write(MOSS_Terminal *term, const char *text);

/**
 * moss_terminal_writef() — printf-style terminal output.
 */
int moss_terminal_writef(MOSS_Terminal *term, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * moss_terminal_set_prompt() — Change the shell prompt string.
 * Default: "moss@mOSs:~$ "
 */
void moss_terminal_set_prompt(MOSS_Terminal *term, const char *prompt);

/**
 * moss_terminal_set_input_cb() — Set a callback invoked on Enter.
 * The callback receives the trimmed command string.
 */
void moss_terminal_set_input_cb(MOSS_Terminal *term, MOSS_InputCb cb, void *ud);

/**
 * moss_terminal_set_complete_cb() — Tab-completion hook.
 * Fill buf[0..n-1] with the completed string; return actual length or 0.
 */
void moss_terminal_set_complete_cb(MOSS_Terminal *term,
                                    MOSS_CompleteCb cb, void *ud);

/** moss_terminal_destroy() — Free the terminal (does not destroy the window). */
void moss_terminal_destroy(MOSS_Terminal *term);

/* =============================================================================
 *  ASCII ART  — load and render from /usr/share/moss/ascii/ascii_art.json
 * ============================================================================= */

/**
 * moss_ascii_load() — Load the system ASCII art resource file.
 *
 * Parses /usr/share/moss/ascii/ascii_art.json (or the path from config)
 * and returns a handle that can be queried for individual entries.
 *
 * Returns NULL on MOSS_E_NOTFOUND or MOSS_E_IO.
 */
MOSS_AsciiArt *moss_ascii_load(const MOSS_Config *cfg);

/**
 * moss_ascii_get() — Retrieve a named art entry as a null-terminated string.
 *
 * @param name   Key from ascii_art.json (e.g. "eye", "moss", "ferris")
 * Returns NULL if the key does not exist.
 */
const char *moss_ascii_get(const MOSS_AsciiArt *art, const char *name);

/**
 * moss_draw_ascii() — Render ASCII art into a window at (row, col).
 *
 * Handles multi-line art correctly. Each line is drawn at col, advancing
 * the row by 1. Clips at window boundaries.
 *
 * @param art    Art resource handle.
 * @param name   Art key.
 * @param color  Colour to render the art in.
 */
int moss_draw_ascii(MOSS_Window *win, const MOSS_AsciiArt *art,
                    const char *name, int row, int col, MOSS_Color color);

/** moss_ascii_destroy() — Free the art handle. */
void moss_ascii_destroy(MOSS_AsciiArt *art);

/* =============================================================================
 *  SYSTEM BRIDGE  — real-time hardware metrics from moss-bridge
 * ============================================================================= */

/**
 * moss_sysinfo_get() — Query the moss-bridge daemon for a live metrics snapshot.
 *
 * Opens /run/moss-bridge.sock, sends a JSON request, receives a MOSS_SysInfo.
 * Blocks for at most 200 ms. Returns NULL on MOSS_E_BRIDGE.
 *
 * The caller must free the result with moss_sysinfo_free().
 */
MOSS_SysInfo *moss_sysinfo_get(void);

/**
 * moss_sysinfo_subscribe() — Register a callback invoked every `interval_ms`.
 *
 * moss-bridge pushes updates over the socket; the event loop calls this
 * callback after each update.  Returns an opaque subscription ID, or 0
 * on failure.
 *
 * Cancel with moss_sysinfo_unsubscribe(id).
 */
uint64_t moss_sysinfo_subscribe(MOSS_Ctx *ctx, uint32_t interval_ms,
                                 MOSS_EventCb cb, void *userdata);
void     moss_sysinfo_unsubscribe(MOSS_Ctx *ctx, uint64_t sub_id);

/* Accessors for MOSS_SysInfo fields (avoids exposing struct layout) */
const char *moss_sysinfo_hostname(const MOSS_SysInfo *si);
const char *moss_sysinfo_kernel(const MOSS_SysInfo *si);
uint64_t    moss_sysinfo_uptime_s(const MOSS_SysInfo *si);
int         moss_sysinfo_cpu_pct(const MOSS_SysInfo *si);      /* 0–100 */
int         moss_sysinfo_cpu_temp_c(const MOSS_SysInfo *si);   /* °C, -1 if N/A */
int         moss_sysinfo_cpu_freq_mhz(const MOSS_SysInfo *si); /* MHz, -1 if N/A */
const char *moss_sysinfo_cpu_model(const MOSS_SysInfo *si);
uint64_t    moss_sysinfo_mem_total_kb(const MOSS_SysInfo *si);
uint64_t    moss_sysinfo_mem_used_kb(const MOSS_SysInfo *si);
int         moss_sysinfo_mem_pct(const MOSS_SysInfo *si);      /* 0–100 */
uint64_t    moss_sysinfo_disk_total_mb(const MOSS_SysInfo *si);
uint64_t    moss_sysinfo_disk_used_mb(const MOSS_SysInfo *si);
int         moss_sysinfo_disk_pct(const MOSS_SysInfo *si);     /* 0–100 */

void moss_sysinfo_free(MOSS_SysInfo *si);

/* =============================================================================
 *  CONFIGURATION
 * ============================================================================= */

/**
 * moss_config_load() — Parse /etc/moss/config.json.
 * Returns NULL on MOSS_E_NOTFOUND.  The returned pointer must be freed
 * with moss_config_free().
 */
MOSS_Config *moss_config_load(void);
MOSS_Config *moss_config_load_path(const char *path);

const char  *moss_config_get_str(const MOSS_Config *cfg, const char *key,
                                  const char *default_val);
int          moss_config_get_int(const MOSS_Config *cfg, const char *key,
                                  int default_val);
bool         moss_config_get_bool(const MOSS_Config *cfg, const char *key,
                                   bool default_val);

/** moss_config_set_*() — Persist a setting to /etc/moss/config.json. */
int moss_config_set_str(MOSS_Config *cfg,  const char *key, const char *val);
int moss_config_set_int(MOSS_Config *cfg,  const char *key, int val);
int moss_config_set_bool(MOSS_Config *cfg, const char *key, bool val);
int moss_config_save(const MOSS_Config *cfg);
void moss_config_free(MOSS_Config *cfg);

/* =============================================================================
 *  CRT RENDERER
 * ============================================================================= */

/**
 * moss_set_crt_mode() — Set the CRT effect globally.
 * Mirrors index.html's crtEffectMode: OFF / LIGHT / STRONG.
 * In LIGHT mode: every even row is rendered at half brightness.
 * In STRONG mode: even rows are blank (true scanline simulation).
 */
void moss_set_crt_mode(MOSS_Ctx *ctx, MOSS_CrtMode mode);

/**
 * moss_render_frame() — Flush all dirty windows to the terminal.
 * Applies the CRT scanline pass after all window content is composed.
 * Called automatically by moss_event_loop(); call manually for custom loops.
 */
void moss_render_frame(MOSS_Ctx *ctx);

/**
 * moss_set_terminal_color() — Change the global accent colour (#00ff00 default).
 * Updates all window borders and the terminal prompt on the next render.
 */
void moss_set_terminal_color(MOSS_Ctx *ctx, MOSS_Color color);
void moss_set_custom_color(MOSS_Ctx *ctx, short pair_id, short fg, short bg);

/* =============================================================================
 *  HIGH-SCORE / PERSISTENCE  — talks to the local SQLite DB via moss-bridge
 * ============================================================================= */

typedef struct {
    char     username[64];
    int64_t  score;
    char     game[32];
    uint64_t ts;   /* Unix timestamp */
} MOSS_Score;

/**
 * moss_scores_get() — Fetch the top-N scores for a given game name.
 *
 * @param game   Game identifier ("snake", "tetris", "mine", "2048", "pong").
 * @param n      Maximum entries to return.
 * @param out    Caller-allocated array of at least n MOSS_Score entries.
 * Returns the number of entries written, or a negative error code.
 */
int moss_scores_get(const char *game, int n, MOSS_Score *out);

/**
 * moss_scores_submit() — Add a new score entry.
 */
int moss_scores_submit(const char *game, const char *username, int64_t score);

/* =============================================================================
 *  UTILITY
 * ============================================================================= */

/** moss_sleep_ms() — Millisecond-resolution sleep. */
void moss_sleep_ms(uint32_t ms);

/** moss_uptime_str() — Format seconds as "Xd Xh Xm Xs". Writes into buf. */
char *moss_uptime_str(uint64_t seconds, char *buf, size_t buflen);

/** moss_format_bytes() — Format a byte count as "1.23 MiB". */
char *moss_format_bytes(uint64_t bytes, char *buf, size_t buflen);

/** moss_strlcpy() — Safe bounded string copy (always NUL-terminates). */
size_t moss_strlcpy(char *dst, const char *src, size_t size);

/** moss_err_str() — Human-readable error message for a MOSS_E_* code. */
const char *moss_err_str(int err);

/* ── Convenience macros ───────────────────────────────────────────────────── */
#define MOSS_ARRAY_LEN(arr)  (sizeof(arr) / sizeof((arr)[0]))
#define MOSS_MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MOSS_MAX(a, b)       ((a) > (b) ? (a) : (b))
#define MOSS_CLAMP(x, lo, hi) MOSS_MAX((lo), MOSS_MIN((x), (hi)))

/* Print to window at a computed row/col using the current color + attr */
#define MOSS_PRINT_FMT(win, r, c, fmt, ...) \
    moss_printf((win), (r), (c), (fmt), ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
#endif /* MOSS_H */
