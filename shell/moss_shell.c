/* =============================================================================
 *  mOSs-Native :: moss_shell.c
 *  The primary user-facing shell process.  This is what runs as the
 *  "desktop" - it is the direct native translation of index.html.
 *
 *  Modes (mirrors index.html's startup_mode):
 *    TERMINAL  - full-screen ncurses shell (like the web terminal view)
 *    WM        - floating-window manager (like the web WM view)
 *
 *  Boot sequence  (mirrors startBootSequence()):
 *    1. Draw eye.png ASCII art on black screen (startup overlay)
 *    2. Flash + glow animation  -> play chime (aplay async)
 *    3. Show module loading list  (system.core ... graphics.renderer)
 *    4. Show login screen
 *    5. Enter chosen mode
 *
 *  Commands implemented (complete list from commandPrefixes in index.html):
 *    ls  cat  cd  pwd  touch  mkdir  mv  rm  nano  echo  date  whoami
 *    clear  help  about  fetch  color  crt  bg  bounce  size  wm
 *    startup  login  logout  shutdown  reboot  run  say  meow  ferris
 *    gay  hits  ama  mine  snake  tetris  2048  pong  bounce
 *
 *  Compile:
 *    gcc -O2 moss_shell.c moss_renderer.c moss_wm.c \
 *        -Isdk -lncurses -lpanel -lmenu -lm -lpthread \
 *        -L../build/lib -lmoss -Wl,-rpath,/lib \
 *        -o moss-shell
 * =============================================================================
 */

#define _GNU_SOURCE
#include <curses.h>
#include <panel.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/reboot.h>  /* RB_AUTOBOOT, RB_POWER_OFF */
#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <sys/time.h>

#ifndef EV_KEY
#define EV_KEY 0x01
#endif
#ifndef EV_REL
#define EV_REL 0x02
#endif
#ifndef REL_X
#define REL_X 0x00
#endif
#ifndef REL_Y
#define REL_Y 0x01
#endif
#ifndef REL_HWHEEL
#define REL_HWHEEL 0x06
#endif
#ifndef REL_WHEEL
#define REL_WHEEL 0x08
#endif
#ifndef BTN_LEFT
#define BTN_LEFT 0x110
#endif
struct input_event {
    struct timeval time;
    unsigned short type;
    unsigned short code;
    int value;
};

#include "moss.h"
#include "moss_shell.h"
#include "moss_renderer.h"
#include "moss_wm.h"
#include "moss_ascii_art_embed.h"

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Boot module list  (mirrors modules[] in index.html)                         */
/* ─────────────────────────────────────────────────────────────────────────── */
static const struct { const char *name; int delay_ms; } BOOT_MODULES[] = {
    { "system.core",        200 },
    { "kernel.scheduler",   150 },
    { "memory.manager",     180 },
    { "device.drivers",     220 },
    { "network.stack",      190 },
    { "filesystem.vfs",     170 },
    { "security.module",    210 },
    { "terminal.emulator",  160 },
    { "user.interface",     140 },
    { "bounce.engine",      130 },
    { "audio.system",       250 },
    { "graphics.renderer",  200 },
    { "input.handler",      120 },
    { "power.management",   180 },
    { "system.monitor",     160 },
};
#define NUM_MODULES (sizeof BOOT_MODULES / sizeof BOOT_MODULES[0])

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Global shell state                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */
typedef enum { MODE_TERMINAL, MODE_WM } ShellMode;
typedef enum { USER_GUEST, USER_MOSS, USER_ADMIN } UserRole;

typedef struct {
    /* --- display --- */
    MOSS_Ctx      *ctx;
    MOSS_AsciiArt *art;
    MOSS_Config   *cfg;
    ShellMode      mode;
    MossRenderer  *renderer;
    MossWM        *wm;

    /* --- session --- */
    UserRole  user;
    char      username[64];
    bool      logged_in;
    int       login_attempts;

    /* --- terminal state --- */
    char    **scrollback;       /* ring buffer of output lines     */
    int       sb_count;
    int       sb_top;           /* viewport top                    */
    char      input_buf[4096];
    int       input_len;
    int       input_pos;
    char    **history;
    int       history_len;
    int       history_idx;
    char      cwd[512];

    /* --- virtual filesystem (mirrors fileSystem in index.html) --- */
    /* Flat array of entries; directories just hold name + children  */
    /* Full VFS would be a tree; here we keep a flat list for clarity */
    VFSEntry *vfs;
    int       vfs_count;
    int       vfs_capacity;

    /* --- color + CRT --- */
    MOSS_Color    accent;
    MOSS_CrtMode  crt_mode;
    bool          bg_enabled;
    int           font_size;

    /* --- hits counter --- */
    int  hit_count;

    /* --- bounce --- */
    bool bounce_active;
    pthread_t bounce_thread;

    /* --- optional raw mouse wheel support --- */
    bool mouse_thread_running;
    bool mouse_thread_stop;
    int  mouse_scroll_pending;
    pthread_t mouse_thread;
} ShellState;

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Forward declarations                                                         */
/* ─────────────────────────────────────────────────────────────────────────── */
static void shell_run(ShellState *s);
static void boot_sequence(ShellState *s);
static bool show_login(ShellState *s);
static void terminal_loop(ShellState *s);
static void handle_command(ShellState *s, const char *line);
static void term_print(ShellState *s, const char *text, MOSS_Color color, MOSS_Attr attr);
static void term_printf(ShellState *s, MOSS_Color color, const char *fmt, ...);
static void term_redraw(ShellState *s);
static void term_scroll(ShellState *s, int delta);
static void play_chime(void);
static bool login_accepts_char(int ch, bool password);
static void login_read_field(int y, int x, char *buf, size_t bufsz, bool password);
static void cmd_size(ShellState *s, const char *arg);
static void cmd_man(ShellState *s, const char *arg);
static void cmd_setres(ShellState *s, const char *arg);
static void cmd_font(ShellState *s, const char *arg);
static void cmd_mv(ShellState *s, const char *arg);
static void shell_relogin(ShellState *s);
static void shell_launch_wm(ShellState *s);
static void shell_stop_mouse_thread(ShellState *s);
static void shell_start_mouse_thread(ShellState *s);
static void run_snake_game(ShellState *s);
static void run_pong_game(ShellState *s);
static void run_2048_game(ShellState *s);
static void run_minesweeper_game(ShellState *s);
static void run_tetris_game(ShellState *s);
static void term_print_dot_art(ShellState *s, const char **lines, int nlines, MOSS_Color color, const char *dot_on);
static void term_print_rainbow_bar(ShellState *s, MOSS_Color color);
static void term_print_manual_topic(ShellState *s, const char *topic, const char *body);
static int try_write_text_file(const char *path, const char *value);
static int find_external_executable(const char *cmd, char *out, size_t outsz);
static void shell_init_palette(void);
static int try_setres_sysfs(int w, int h, int hz, char *msg, size_t msgsz);
static const char *embedded_art_lookup(const char *key);
static const char *resolve_art_alias(const char *content);

/* ─────────────────────────────────────────────────────────────────────────── */
/*  VFS helpers  (matches fileSystem in index.html)                             */
/* ─────────────────────────────────────────────────────────────────────────── */
static VFSEntry DEFAULT_VFS[] = {
    { "/",                "/",         true,  NULL },
    { "/michal.txt",      "michal.txt", false, "ART:michal" },
    { "/discord.txt",     "discord.txt",false,
      "Add me on discord, my @ is maseratislick" },
    { "/mymusic.txt",     "mymusic.txt",false,
      "My music taste is all over the place,\n"
      "but i mainly enjoy rap or cloud rap, incl. artists such as\n"
      "$uicideboy$, Sematary, Bladee, Ecco2K, Yung Lean, smokedope2016,\n"
      "Lil Darkie and Lil Peep.\n"
      "I also like some stuff besides rap, such as 100 gecs,\n"
      "Femtanyl or Gorillaz" },
};

static void vfs_init(ShellState *s) {
    int n = sizeof DEFAULT_VFS / sizeof DEFAULT_VFS[0];
    s->vfs_capacity = n + 256;
    s->vfs = calloc(s->vfs_capacity, sizeof(VFSEntry));
    if (!s->vfs) return;
    memcpy(s->vfs, DEFAULT_VFS, n * sizeof(VFSEntry));
    s->vfs_count = n;
}

static VFSEntry *vfs_find(ShellState *s, const char *path) {
    for (int i = 0; i < s->vfs_count; i++)
        if (strcmp(s->vfs[i].path, path) == 0) return &s->vfs[i];
    return NULL;
}

/* Resolve relative path against cwd */
static void vfs_resolve(const ShellState *s, const char *arg, char *out, size_t n) {
    if (arg[0] == '/') { strncpy(out, arg, n-1); out[n-1]=0; return; }
    if (strcmp(s->cwd, "/") == 0)
        snprintf(out, n, "/%s", arg);
    else
        snprintf(out, n, "%s/%s", s->cwd, arg);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Scrollback                                                                   */
/* ─────────────────────────────────────────────────────────────────────────── */
#define SB_MAX 500

static void sb_push(ShellState *s, const char *line) {
    if (!s->scrollback) {
        s->scrollback = calloc(SB_MAX, sizeof(char*));
        s->sb_count = 0;
    }
    if (s->sb_count < SB_MAX) {
        s->scrollback[s->sb_count++] = strdup(line);
    } else {
        free(s->scrollback[0]);
        memmove(s->scrollback, s->scrollback+1, (SB_MAX-1)*sizeof(char*));
        s->scrollback[SB_MAX-1] = strdup(line);
    }
    /* Auto-scroll to bottom */
    s->sb_top = MOSS_MAX(0, s->sb_count - (LINES - 3));
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Output helpers                                                               */
/* ─────────────────────────────────────────────────────────────────────────── */
static void sanitize_display_line(const char *src, char *dst, size_t n) {
    size_t i = 0, j = 0;
    while (src && src[i] && j + 1 < n) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == 9) {
            if (j + 4 >= n) break;
            dst[j++] = ' '; dst[j++] = ' '; dst[j++] = ' '; dst[j++] = ' ';
            i++;
            continue;
        }
        if (ch == 13) { i++; continue; }
        if (ch >= 32 && ch < 127) {
            dst[j++] = (char)ch;
            i++;
            continue;
        }
        if (ch < 32) { i++; continue; }
        if (ch == 0xE2 && src[i+1] && src[i+2] && (unsigned char)src[i+1] == 0xA0 && (unsigned char)src[i+2] == 0x80) {
            dst[j++] = ' ';
            i += 3;
            continue;
        }
        int len = 0;
        if ((ch & 0xE0) == 0xC0 && src[i+1] && ((unsigned char)src[i+1] & 0xC0) == 0x80) len = 2;
        else if ((ch & 0xF0) == 0xE0 && src[i+1] && src[i+2] && ((unsigned char)src[i+1] & 0xC0) == 0x80 && ((unsigned char)src[i+2] & 0xC0) == 0x80) len = 3;
        else if ((ch & 0xF8) == 0xF0 && src[i+1] && src[i+2] && src[i+3] && ((unsigned char)src[i+1] & 0xC0) == 0x80 && ((unsigned char)src[i+2] & 0xC0) == 0x80 && ((unsigned char)src[i+3] & 0xC0) == 0x80) len = 4;
        if (len > 0 && j + (size_t)len < n) {
            for (int k = 0; k < len; k++) dst[j++] = src[i++];
            continue;
        }
        dst[j++] = '?';
        i++;
        while (src[i] && ((unsigned char)src[i] & 0xC0) == 0x80) i++;
    }
    dst[j] = 0;
}

static const char *embedded_art_lookup(const char *key) {
    if (!key) return NULL;
    if (strcmp(key, "ferris") == 0) return MOSS_ART_FERRIS;
    if (strcmp(key, "michal") == 0 || strcmp(key, "michal.txt") == 0 || strcmp(key, "michalface") == 0) return MOSS_ART_MICHAL_TXT;
    if (strcmp(key, "bluescreen") == 0) return MOSS_ART_BLUESCREEN;
    return NULL;
}

static const char *resolve_art_alias(const char *content) {
    if (!content) return NULL;
    if (strncmp(content, "ART:", 4) == 0) return embedded_art_lookup(content + 4);
    return embedded_art_lookup(content);
}

static short curses_fg_from_moss(MOSS_Color col) {
    switch (col) {
        case MOSS_COLOR_GREEN: return COLOR_GREEN;
        case MOSS_COLOR_RED: return COLOR_RED;
        case MOSS_COLOR_YELLOW: return COLOR_YELLOW;
        case MOSS_COLOR_BLUE: return COLOR_BLUE;
        case MOSS_COLOR_MAGENTA: return COLOR_MAGENTA;
        case MOSS_COLOR_CYAN: return COLOR_CYAN;
        case MOSS_COLOR_WHITE: return COLOR_WHITE;
        case MOSS_COLOR_DARK_GREEN: return COLOR_GREEN;
        case MOSS_COLOR_BRIGHT_GREEN: return COLOR_GREEN;
        case MOSS_COLOR_ORANGE: return COLOR_YELLOW;
        default: return COLOR_GREEN;
    }
}

static void shell_init_palette(void) {
    if (!has_colors()) return;
    start_color();
#ifdef NCURSES_VERSION
    use_default_colors();
#endif
    init_pair(MOSS_COLOR_GREEN, curses_fg_from_moss(MOSS_COLOR_GREEN), COLOR_BLACK);
    init_pair(MOSS_COLOR_RED, curses_fg_from_moss(MOSS_COLOR_RED), COLOR_BLACK);
    init_pair(MOSS_COLOR_YELLOW, curses_fg_from_moss(MOSS_COLOR_YELLOW), COLOR_BLACK);
    init_pair(MOSS_COLOR_BLUE, curses_fg_from_moss(MOSS_COLOR_BLUE), COLOR_BLACK);
    init_pair(MOSS_COLOR_MAGENTA, curses_fg_from_moss(MOSS_COLOR_MAGENTA), COLOR_BLACK);
    init_pair(MOSS_COLOR_CYAN, curses_fg_from_moss(MOSS_COLOR_CYAN), COLOR_BLACK);
    init_pair(MOSS_COLOR_WHITE, curses_fg_from_moss(MOSS_COLOR_WHITE), COLOR_BLACK);
    init_pair(MOSS_COLOR_DARK_GREEN, curses_fg_from_moss(MOSS_COLOR_DARK_GREEN), COLOR_BLACK);
    init_pair(MOSS_COLOR_BRIGHT_GREEN, curses_fg_from_moss(MOSS_COLOR_BRIGHT_GREEN), COLOR_BLACK);
    init_pair(MOSS_COLOR_ORANGE, curses_fg_from_moss(MOSS_COLOR_ORANGE), COLOR_BLACK);
}

static void term_print(ShellState *s, const char *text,
                        MOSS_Color color, MOSS_Attr attr) {
    char buf[8192];
    strncpy(buf, text ? text : "", sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char clean[4096];
        sanitize_display_line(line, clean, sizeof clean);
        MOSS_Color out_color = (color == MOSS_COLOR_GREEN) ? s->accent : color;
        char encoded[4200];
        snprintf(encoded, sizeof encoded, "\x1b[%d;%dm%s\x1b[0m",
                 (int)attr, (int)out_color, clean);
        sb_push(s, encoded);
    }
    if (text && text[0] == '\n') sb_push(s, "");
    term_redraw(s);
}

static void term_printf(ShellState *s, MOSS_Color color, const char *fmt, ...) {
    char buf[4096]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    term_print(s, buf, color, MOSS_ATTR_NORMAL);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Terminal redraw - the ncurses rendering of the scrollback + input line      */
/* ─────────────────────────────────────────────────────────────────────────── */
static void term_redraw(ShellState *s) {
    if (!s->ctx) return;
    int rows = LINES, cols = COLS;

    /* Background: solid dark */
    attron(COLOR_PAIR(1));
    for (int r = 0; r < rows; r++) {
        move(r, 0);
        for (int c = 0; c < cols; c++) addch(' ');
    }
    attroff(COLOR_PAIR(1));

    /* Scrollback area: rows 0..rows-3 */
    int display_rows = rows - 3;
    for (int i = 0; i < display_rows; i++) {
        int sb_idx = s->sb_top + i;
        if (sb_idx >= s->sb_count) break;
        const char *line = s->scrollback[sb_idx];
        /* Strip our simple encoding prefix for display */
        const char *text = line;
        int cp = s->accent;
        if (line[0] == '\x1b' && line[1] == '[') {
            /* Parse \x1b[attr;colorpm */
            int attr_v = 0, color_v = 1;
            if (sscanf(line+2, "%d;%dm", &attr_v, &color_v) == 2) {
                cp = MOSS_CLAMP(color_v, 1, 10);
                if (cp == MOSS_COLOR_GREEN) cp = s->accent;
                const char *m = strchr(line+2, 'm');
                if (m) text = m+1;
            }
        }
        /* Trim the reset suffix */
        char display[512]; strncpy(display, text, sizeof display-1);
        char *esc = strstr(display, "\x1b[0m");
        if (esc) *esc = 0;

        char safe_display[1024]; sanitize_display_line(display, safe_display, sizeof safe_display);
        move(i, 0);
        attron(COLOR_PAIR(cp));
        addstr(safe_display);
        attroff(COLOR_PAIR(cp));
    }

    /* Separator line */
    move(rows-3, 0);
    attron(COLOR_PAIR(s->accent) | A_DIM);
    for (int c = 0; c < cols; c++) addch(ACS_HLINE);
    attroff(COLOR_PAIR(s->accent) | A_DIM);

    /* Prompt + input */
    move(rows-2, 0);
    attron(COLOR_PAIR(s->accent) | A_BOLD);
    char prompt[128];
    const char *user = s->logged_in ? s->username : "guest";
    snprintf(prompt, sizeof prompt, "%s@mOSs:%s$ ", user, s->cwd);
    addstr(prompt);
    attroff(COLOR_PAIR(s->accent) | A_BOLD);

    attron(COLOR_PAIR(s->accent));
    addnstr(s->input_buf, s->input_len);
    /* Cursor */
    int px = (int)strlen(prompt) + s->input_pos;
    if (px < cols) {
        move(rows-2, px);
        attron(A_REVERSE | COLOR_PAIR(s->accent));
        addch(s->input_pos < s->input_len ? s->input_buf[s->input_pos] : ' ');
        attroff(A_REVERSE | COLOR_PAIR(s->accent));
    }
    attroff(COLOR_PAIR(s->accent));

    /* Status bar */
    move(rows-1, 0);
    attron(COLOR_PAIR(s->accent) | A_DIM);
    char status[512];
    time_t t = time(NULL); struct tm *tm = localtime(&t);
    char tstr[32]; strftime(tstr, sizeof tstr, "%H:%M:%S", tm);
    snprintf(status, sizeof status,
             " mOSs v1.0 | %s | CRT:%s | Tab:complete  F1:help  F5:wm",
             tstr,
             s->crt_mode == MOSS_CRT_OFF ? "off" :
             s->crt_mode == MOSS_CRT_LIGHT ? "light" : "strong");
    addnstr(status, cols-1);
    attroff(COLOR_PAIR(s->accent) | A_DIM);

    /* CRT pass */
    renderer_apply_crt(s->renderer, s->crt_mode);

    refresh();
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Boot sequence                                                                */
/* ─────────────────────────────────────────────────────────────────────────── */
static void draw_boot_splash(const char *art) {
    clear();
    int rows = LINES, cols = COLS;
    attron(COLOR_PAIR(1) | A_BOLD);

    /* Centre the ASCII art */
    char buf[4096]; strncpy(buf, art ? art : "mOSs", sizeof buf-1); buf[sizeof buf-1] = 0;
    char *lines[64]; int nlines = 0;
    char *tok = strtok(buf, "\n");
    while (tok && nlines < 64) { lines[nlines++] = tok; tok = strtok(NULL,"\n"); }

    int start_row = (rows - nlines) / 2;
    int max_w = 0;
    for (int i = 0; i < nlines; i++) {
        int l = (int)strlen(lines[i]); if (l > max_w) max_w = l;
    }
    for (int i = 0; i < nlines; i++) {
        int c = (cols - (int)strlen(lines[i])) / 2;
        if (c < 0) c = 0;
        mvaddstr(start_row + i, c, lines[i]);
    }

    /* "mOSs" title below art */
    const char *title = "[ mOSs - NATIVE SYSTEM ENVIRONMENT ]";
    mvaddstr(start_row + nlines + 2, (cols-(int)strlen(title))/2, title);
    attroff(COLOR_PAIR(1) | A_BOLD);
    refresh();
}

static void boot_sequence(ShellState *s) {
    const char *eye_art = moss_ascii_get(s->art, "eye");
    if (!eye_art) eye_art =
        "      .  .  .  .\n"
        "   .           .\n"
        " .    ______    .\n"
        ".   /  @@@@  \\   .\n"
        "   | @ |__| @ |  \n"
        ".   \\  @@@@  /   .\n"
        " .    ------    .\n"
        "   .           .\n"
        "      .  .  .  .";

    /* Phase 1: fade in */
    draw_boot_splash(eye_art);
    moss_sleep_ms(500);

    /* Phase 2: flash + chime */
    attron(A_BOLD | COLOR_PAIR(1));
    for (int i = 0; i < 3; i++) {
        attron(A_BLINK); refresh(); moss_sleep_ms(80);
        attroff(A_BLINK); refresh(); moss_sleep_ms(80);
    }
    play_chime();

    /* Phase 3: glow (simulate with bold + status text) */
    int rows = LINES, cols = COLS;
    const char *booting = "INITIALIZING mOSs ENVIRONMENT...";
    attron(COLOR_PAIR(1) | A_BOLD);
    mvaddstr(rows-3, (cols-(int)strlen(booting))/2, booting);
    refresh();
    moss_sleep_ms(800);
    attroff(COLOR_PAIR(1) | A_BOLD);

    /* Phase 4: module loading */
    clear();
    attron(COLOR_PAIR(1));
    mvaddstr(1, 2, "mOSs :: Module Loader");
    mvhline(2, 2, ACS_HLINE, 41);
    attroff(COLOR_PAIR(1));

    for (int i = 0; i < (int)NUM_MODULES; i++) {
        moss_sleep_ms(BOOT_MODULES[i].delay_ms);

        /* Print module line */
        attron(COLOR_PAIR(1));
        mvprintw(4 + i, 2, "[ LOADING ] %s", BOOT_MODULES[i].name);
        refresh();
        moss_sleep_ms(60);

        /* Update status to OK */
        attron(COLOR_PAIR(9) | A_BOLD);  /* bright green */
        mvprintw(4 + i, 2, "[   OK    ] %s", BOOT_MODULES[i].name);
        attroff(COLOR_PAIR(9) | A_BOLD);
        refresh();
    }

    mvprintw(4 + (int)NUM_MODULES + 1, 2, "All modules loaded.");
    attron(COLOR_PAIR(1) | A_BOLD);
    mvprintw(4 + (int)NUM_MODULES + 2, 2, "Launching mOSs shell...");
    attroff(COLOR_PAIR(1) | A_BOLD);
    refresh();
    moss_sleep_ms(500);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Login screen                                                                 */
/* ─────────────────────────────────────────────────────────────────────────── */
/* Simple SHA-256 would be ideal; here we use a djb2-based hash as a stand-in.
   The web version stores: ADMIN_PASSWORD_HASH = 'e97624552d...'
   For the native shell we accept "moss" as the admin password,
   anything else as guest. A real deployment would use crypt(3).          */
static bool show_login(ShellState *s) {
    clear();
    int rows = LINES, cols = COLS;

    /* Draw login box */
    int bw = 50, bh = 12;
    int bx = (cols - bw) / 2, by = (rows - bh) / 2;

    attron(COLOR_PAIR(1) | A_BOLD);
    for (int r = by; r < by+bh; r++) {
        move(r, bx);
        if (r == by || r == by+bh-1) {
            for (int c = 0; c < bw; c++) addch(ACS_HLINE);
        } else {
            addch(ACS_VLINE);
            for (int c = 1; c < bw-1; c++) addch(' ');
            addch(ACS_VLINE);
        }
    }
    mvaddch(by, bx, ACS_ULCORNER);
    mvaddch(by, bx+bw-1, ACS_URCORNER);
    mvaddch(by+bh-1, bx, ACS_LLCORNER);
    mvaddch(by+bh-1, bx+bw-1, ACS_LRCORNER);

    const char *title = "[ mOSs LOGIN ]";
    mvaddstr(by, bx + (bw-(int)strlen(title))/2, title);
    attroff(COLOR_PAIR(1) | A_BOLD);

    attron(COLOR_PAIR(1));
    mvaddstr(by+2, bx+3, "Username:");
    mvaddstr(by+5, bx+3, "Password:");
    mvaddstr(by+7, bx+3, "Mode: [t]erminal  [w]m  (default: terminal)");
    mvaddstr(by+9, bx+3, "Tip: guest / moss (admin)");
    attroff(COLOR_PAIR(1));

    attron(COLOR_PAIR(1) | A_DIM);
    mvaddstr(by+10, bx+3, "ASCII login only; Enter confirms each field.");
    attroff(COLOR_PAIR(1) | A_DIM);

    char uname[64] = {0};
    char pw[64] = {0};
    login_read_field(by+3, bx+3, uname, sizeof uname, false);
    login_read_field(by+6, bx+3, pw, sizeof pw, true);

    s->login_attempts++;
    strncpy(s->username, uname[0] ? uname : "guest", sizeof s->username - 1);

    if (strcmp(pw, "moss") == 0) {
        s->user = USER_ADMIN;
        s->logged_in = true;
    } else if (strlen(uname) > 0) {
        s->user = USER_GUEST;
        s->logged_in = true;
    } else {
        strncpy(s->username, "guest", sizeof s->username - 1);
        s->user = USER_GUEST;
        s->logged_in = true;
    }

    move(by+7, bx+48);
    timeout(1500);
    int mode_ch;
    do {
        mode_ch = getch();
    } while (mode_ch != ERR && mode_ch != 't' && mode_ch != 'T' && mode_ch != 'w' && mode_ch != 'W' &&
             mode_ch != 10 && mode_ch != 13 && mode_ch != KEY_ENTER);
    timeout(-1);
    s->mode = (mode_ch == 'w' || mode_ch == 'W') ? MODE_WM : MODE_TERMINAL;

    clear();
    refresh();
    return s->logged_in;
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Command handling - the full command set from index.html                      */
/* ─────────────────────────────────────────────────────────────────────────── */

static void cmd_ls(ShellState *s, const char *args) {
    bool long_fmt = strstr(args, "-l") != NULL;
    bool show_all = strstr(args, "-a") != NULL;
    /* Find entries under cwd */
    int found = 0;
    for (int i = 0; i < s->vfs_count; i++) {
        VFSEntry *e = &s->vfs[i];
        if (strcmp(e->path, s->cwd) == 0) continue;
        /* Check direct child */
        char parent[512];
        strncpy(parent, e->path, sizeof parent - 1);
        char *last = strrchr(parent, '/');
        if (last) { *last = 0; if (last == parent) strcpy(parent, "/"); }
        else continue;
        if (strcmp(parent, s->cwd) != 0) continue;
        if (!show_all && e->name[0] == '.') continue;
        if (long_fmt) {
            term_printf(s, MOSS_COLOR_CYAN,
                        "%s  %s  %s",
                        e->is_dir ? "drwxr-xr-x" : "-rw-r--r--",
                        e->is_dir ? "<DIR>" : "     ",
                        e->name);
        } else {
            term_printf(s, e->is_dir ? MOSS_COLOR_YELLOW : MOSS_COLOR_CYAN,
                        "%s%s", e->name, e->is_dir ? "/" : "");
        }
        found++;
    }
    if (found == 0)
        term_printf(s, MOSS_COLOR_GREEN, "(empty directory)");
}

static void cmd_cat(ShellState *s, const char *arg) {
    if (!arg || !*arg) { term_printf(s, MOSS_COLOR_RED, "cat: missing filename"); return; }
    char path[512]; vfs_resolve(s, arg, path, sizeof path);
    VFSEntry *e = vfs_find(s, path);
    if (!e) { term_printf(s, MOSS_COLOR_RED, "cat: %s: No such file", arg); return; }
    if (e->is_dir) { term_printf(s, MOSS_COLOR_RED, "cat: %s: Is a directory", arg); return; }
    const char *art = resolve_art_alias(e->content);
    if (art) { term_print(s, art, s->accent, MOSS_ATTR_BOLD); return; }
    term_printf(s, MOSS_COLOR_GREEN, "%s", e->content ? e->content : "");
}

static void cmd_fetch(ShellState *s) {
    MOSS_SysInfo *si = moss_sysinfo_get();
    const char *eye = moss_ascii_get(s->art, "eye");

    char upbuf[64]; moss_uptime_str(si ? moss_sysinfo_uptime_s(si) : 0, upbuf, sizeof upbuf);
    char membuf[64];
    if (si) {
        char total[32], used[32];
        moss_format_bytes(moss_sysinfo_mem_total_kb(si)*1024, total, sizeof total);
        moss_format_bytes(moss_sysinfo_mem_used_kb(si)*1024,  used,  sizeof used);
        snprintf(membuf, sizeof membuf, "%s / %s (%d%%)",
                 used, total, moss_sysinfo_mem_pct(si));
    } else {
        strcpy(membuf, "N/A");
    }

    /* Mimic neofetch layout */
    if (eye) term_print(s, eye, MOSS_COLOR_GREEN, MOSS_ATTR_BOLD);

    term_printf(s, MOSS_COLOR_BRIGHT_GREEN, "%s@mOSs", s->username);
    term_printf(s, MOSS_COLOR_GREEN, "----------------------------");
    term_printf(s, MOSS_COLOR_GREEN, "OS     : mOSs Native v1.0");
    term_printf(s, MOSS_COLOR_GREEN, "Kernel : %s",
                si ? moss_sysinfo_kernel(si) : "Linux");
    term_printf(s, MOSS_COLOR_GREEN, "Host   : %s",
                si ? moss_sysinfo_hostname(si) : "mOSs");
    term_printf(s, MOSS_COLOR_GREEN, "Uptime : %s", upbuf);
    term_printf(s, MOSS_COLOR_GREEN, "CPU    : %s (%d%%)",
                si ? moss_sysinfo_cpu_model(si) : "Unknown",
                si ? moss_sysinfo_cpu_pct(si) : 0);
    if (si && moss_sysinfo_cpu_temp_c(si) >= 0)
        term_printf(s, MOSS_COLOR_GREEN, "Temp   : %d C",
                    moss_sysinfo_cpu_temp_c(si));
    term_printf(s, MOSS_COLOR_GREEN, "Memory : %s", membuf);
    term_printf(s, MOSS_COLOR_GREEN, "Shell  : moss-shell v1.0");
    term_printf(s, MOSS_COLOR_GREEN, "CRT    : %s",
                s->crt_mode==MOSS_CRT_OFF ? "off" :
                s->crt_mode==MOSS_CRT_LIGHT ? "light" : "strong");
    term_printf(s, MOSS_COLOR_GREEN, "Hits   : %d", s->hit_count);

    if (si) moss_sysinfo_free(si);
}

static void cmd_color(ShellState *s, const char *arg) {
    if (!arg || !*arg) {
        term_printf(s, MOSS_COLOR_GREEN,
                    "Usage: color <green|red|yellow|blue|cyan|white|magenta>");
        return;
    }
    static const struct { const char *name; MOSS_Color col; } MAP[] = {
        {"green",MOSS_COLOR_GREEN},{"red",MOSS_COLOR_RED},
        {"yellow",MOSS_COLOR_YELLOW},{"blue",MOSS_COLOR_BLUE},
        {"cyan",MOSS_COLOR_CYAN},{"white",MOSS_COLOR_WHITE},
        {"magenta",MOSS_COLOR_MAGENTA},
    };
    for (int i = 0; i < 7; i++) {
        if (strcasecmp(arg, MAP[i].name) == 0) {
            s->accent = MAP[i].col;
            moss_set_terminal_color(s->ctx, s->accent);
            term_printf(s, MAP[i].col, "Terminal color set to %s", arg);
            term_redraw(s);
            return;
        }
    }
    term_printf(s, MOSS_COLOR_RED, "Unknown color: %s", arg);
}

static void cmd_crt(ShellState *s, const char *arg) {
    if (!arg || !*arg || strcmp(arg,"status")==0) {
        const char *modes[]={"off","light","strong"};
        term_printf(s, MOSS_COLOR_GREEN, "CRT effect: %s", modes[s->crt_mode]);
        return;
    }
    if (strcmp(arg,"off")==0)    { s->crt_mode=MOSS_CRT_OFF;   }
    else if (strcmp(arg,"light")==0)  { s->crt_mode=MOSS_CRT_LIGHT;  }
    else if (strcmp(arg,"strong")==0) { s->crt_mode=MOSS_CRT_STRONG; }
    else { term_printf(s, MOSS_COLOR_RED, "crt: unknown mode '%s'", arg); return; }
    moss_set_crt_mode(s->ctx, s->crt_mode);
    const char *modes[]={"off","light","strong"};
    term_printf(s, MOSS_COLOR_GREEN, "CRT effect set to: %s", modes[s->crt_mode]);
}


static void cmd_size(ShellState *s, const char *arg) {
    if (!arg || !*arg) {
        term_printf(s, MOSS_COLOR_GREEN,
                    "Terminal size: %dx%d cells | preferred font size: %d",
                    COLS, LINES, s->font_size > 0 ? s->font_size : 16);
        return;
    }
    int n = atoi(arg);
    if (n < 8 || n > 72) {
        term_printf(s, MOSS_COLOR_RED, "size: expected a value between 8 and 72");
        return;
    }
    s->font_size = n;
    term_printf(s, MOSS_COLOR_GREEN,
                "Preferred font size set to %d. This is informational only on the Linux console.",
                s->font_size);
}

static void cmd_font(ShellState *s, const char *arg) {
    const char *dir = "/usr/share/consolefonts";
    if (!arg || !*arg || strcasecmp(arg, "list") == 0) {
        DIR *d = opendir(dir);
        term_printf(s, MOSS_COLOR_GREEN, "Available console fonts in %s:", dir);
        if (!d) {
            term_printf(s, MOSS_COLOR_RED, "font: could not open %s", dir);
            return;
        }
        struct dirent *de;
        int shown = 0;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            if (!strstr(de->d_name, ".psf")) continue;
            term_printf(s, MOSS_COLOR_DARK_GREEN, "  %s", de->d_name);
            shown++;
        }
        closedir(d);
        if (!shown) term_printf(s, MOSS_COLOR_YELLOW, "  (no console font files found)");
        term_printf(s, MOSS_COLOR_DARK_GREEN, "Use: font <name>   Example: font spleen-8x16.psfu.gz");
        return;
    }

    char path[512];
    if (arg[0] == '/') snprintf(path, sizeof path, "%s", arg);
    else {
        snprintf(path, sizeof path, "%s/%s", dir, arg);
        if (access(path, R_OK) != 0) {
            snprintf(path, sizeof path, "%s/%s.psfu.gz", dir, arg);
            if (access(path, R_OK) != 0)
                snprintf(path, sizeof path, "%s/%s.psfu", dir, arg);
        }
    }

    if (access(path, R_OK) != 0) {
        term_printf(s, MOSS_COLOR_RED, "font: file not found: %s", arg);
        return;
    }
    const char *setfont = access("/bin/setfont", X_OK) == 0 ? "/bin/setfont" :
                          (access("/usr/bin/setfont", X_OK) == 0 ? "/usr/bin/setfont" : NULL);
    if (!setfont) {
        term_printf(s, MOSS_COLOR_RED, "font: setfont is not available in this build");
        return;
    }

    char cmd[768];
    snprintf(cmd, sizeof cmd, "%s '%s' </dev/tty1 >/dev/tty1 2>/dev/null", setfont, path);
    def_prog_mode();
    endwin();
    int rc = system(cmd);
    reset_prog_mode();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    refresh();

    if (rc == 0) {
        FILE *fp = fopen("/var/lib/moss/consolefont.next", "w");
        if (!fp) fp = fopen("/tmp/moss-consolefont.next", "w");
        if (fp) { fprintf(fp, "%s\n", path); fclose(fp); }
        term_printf(s, MOSS_COLOR_GREEN, "Console font set to %s", path);
        term_printf(s, MOSS_COLOR_DARK_GREEN, "It will be re-applied automatically on next boot.");
    } else {
        term_printf(s, MOSS_COLOR_RED, "font: failed to apply %s", path);
    }
}

static void cmd_man(ShellState *s, const char *arg) {
    static const struct { const char *topic; const char *body; } TOPICS[] = {
        {"moss",
         "mOSs manual\n"
         "-----------\n"
         "help              Show the short command list.\n"
         "man moss          Show this manual.\n"
         "man <topic>       Show a built-in topic or a VFS file.\n"
         "login/logout      Return to the login screen.\n"
         "wm                Open the draggable desktop window manager.\n"
         "setres WxH[@R]    Try runtime framebuffer mode changes.\n"
         "size [N]          Show or store a preferred font size.\n"
         "font [name]       List/apply console fonts from /usr/share/consolefonts.\n"
         "\nNavigation\n"
         "----------\n"
         "PgUp / PgDn       Scroll terminal history.\n"
         "Mouse wheel       Scroll terminal history when mouse events exist.\n"
         "F1                Show help.\n"
         "F5                Enter the draggable desktop window manager.\n"
         "Ctrl-C            Clear the current input line.\n"
         "Ctrl-L            Clear the terminal scrollback.\n"
         "\nWM quick keys\n"
         "-------------\n"
         "F1 launcher, Tab focus, F2 close, F4 back to shell, mouse drag windows, click taskbar buttons, and drag to top to maximize.\n"},
        {"wm", "The WM is now a mouse-driven desktop with draggable windows, taskbar buttons, maximize/close controls, and snap-to-top maximize. Open it with 'wm' or F5."},
        {"setres", "setres first tries DRM connector mode files, then framebuffer sysfs, then FBIOPUT_VSCREENINFO, then fbset. If live switching still fails, it saves the requested mode for the next boot so /init can try it earlier."},
        {"games", "Games currently available in terminal mode: snake, mine, tetris, 2048, pong. The WM Games app can launch them too."},
        {"ferris", "Ferris uses the original art embedded from ascii_art.json. Blank braille cells are normalized to spaces before rendering."},
        {"font", "font lists console fonts from /usr/share/consolefonts and applies one using setfont. The selected font is saved and /init reapplies it on the next boot."},
    };

    if (!arg || !*arg) arg = "moss";

    for (size_t i = 0; i < sizeof TOPICS / sizeof TOPICS[0]; i++) {
        if (strcmp(arg, TOPICS[i].topic) == 0) {
            term_print_manual_topic(s, TOPICS[i].topic, TOPICS[i].body);
            return;
        }
    }

    char path[512];
    vfs_resolve(s, arg, path, sizeof path);
    VFSEntry *e = vfs_find(s, path);
    if (!e && arg[0] == '/') e = vfs_find(s, arg);
    if (e && !e->is_dir) {
        const char *art = resolve_art_alias(e->content);
        term_print_manual_topic(s, arg, art ? art : (e->content ? e->content : "(empty file)"));
        return;
    }

    term_printf(s, MOSS_COLOR_RED, "man: no manual entry for %s", arg);
}

static void cmd_setres(ShellState *s, const char *arg) {
    (void)s;
    if (!arg || !*arg) {
        term_printf(s, MOSS_COLOR_GREEN, "Usage: setres <width>x<height>[@refresh]");
        return;
    }

    int w = 0, h = 0, hz = 0;
    if (sscanf(arg, "%dx%d@%d", &w, &h, &hz) < 2 &&
        sscanf(arg, "%dX%d@%d", &w, &h, &hz) < 2 &&
        sscanf(arg, "%dx%d", &w, &h) < 2 &&
        sscanf(arg, "%dX%d", &w, &h) < 2) {
        term_printf(s, MOSS_COLOR_RED, "setres: expected format like 1920x1080@60");
        return;
    }
    if (w < 320 || h < 200) {
        term_printf(s, MOSS_COLOR_RED, "setres: resolution too small");
        return;
    }

    char msg[256];
    if (try_setres_sysfs(w, h, hz, msg, sizeof msg) == 0) {
        clear();
        refresh();
        term_printf(s, MOSS_COLOR_GREEN, "%s", msg);
        return;
    }

    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        struct fb_var_screeninfo var;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0) {
            var.xres = (unsigned int)w;
            var.yres = (unsigned int)h;
            var.xres_virtual = (unsigned int)w;
            var.yres_virtual = (unsigned int)h;
            var.activate = FB_ACTIVATE_NOW;
            if (ioctl(fd, FBIOPUT_VSCREENINFO, &var) == 0) {
                close(fd);
                clear();
                refresh();
                if (hz > 0)
                    term_printf(s, MOSS_COLOR_GREEN, "Requested framebuffer mode %dx%d@%d via FBIOPUT_VSCREENINFO.", w, h, hz);
                else
                    term_printf(s, MOSS_COLOR_GREEN, "Requested framebuffer mode %dx%d via FBIOPUT_VSCREENINFO.", w, h);
                return;
            }
        }
        close(fd);
    }

    char cmd[256];
    snprintf(cmd, sizeof cmd, "fbset -g %d %d %d %d 32 >/dev/null 2>&1", w, h, w, h);
    if (system(cmd) == 0) {
        clear();
        refresh();
        term_printf(s, MOSS_COLOR_GREEN, "Requested framebuffer mode %dx%d using fbset.", w, h);
        return;
    }

    char req[64];
    if (hz > 0) snprintf(req, sizeof req, "%dx%d@%d", w, h, hz);
    else snprintf(req, sizeof req, "%dx%d", w, h);
    FILE *persist = fopen("/var/lib/moss/setres.next", "w");
    if (!persist) persist = fopen("/tmp/moss-setres.next", "w");
    if (persist) { fprintf(persist, "%s\n", req); fclose(persist); }
    term_printf(s, MOSS_COLOR_YELLOW,
                "setres: live switching was not available. Saved %s for next boot; /init will try DRM connector mode files and framebuffer paths early during boot.", req);
}

static void cmd_mv(ShellState *s, const char *arg) {
    if (!arg || !*arg) {
        term_printf(s, MOSS_COLOR_RED, "mv: missing source and destination");
        return;
    }
    char tmp[1024];
    strncpy(tmp, arg, sizeof tmp - 1);
    tmp[sizeof tmp - 1] = '\0';
    char *src = strtok(tmp, " ");
    char *dst = strtok(NULL, "");
    if (!src || !dst || !*dst) {
        term_printf(s, MOSS_COLOR_RED, "mv: usage: mv <source> <dest>");
        return;
    }
    char src_path[512], dst_path[512];
    vfs_resolve(s, src, src_path, sizeof src_path);
    vfs_resolve(s, dst, dst_path, sizeof dst_path);
    VFSEntry *e = vfs_find(s, src_path);
    if (!e) {
        term_printf(s, MOSS_COLOR_RED, "mv: %s: No such file or directory", src);
        return;
    }
    if (vfs_find(s, dst_path)) {
        term_printf(s, MOSS_COLOR_RED, "mv: %s: target already exists", dst);
        return;
    }
    strncpy(e->path, dst_path, sizeof e->path - 1);
    const char *base = strrchr(dst_path, '/');
    strncpy(e->name, base ? base + 1 : dst_path, sizeof e->name - 1);
    term_printf(s, MOSS_COLOR_GREEN, "Moved %s -> %s", src, dst);
}

static void cmd_help(ShellState *s) {
    term_printf(s, MOSS_COLOR_GREEN, "mOSs Shell v1.0 - Available Commands");
    term_printf(s, MOSS_COLOR_DARK_GREEN, "----------------------------------------------------");
    term_printf(s, MOSS_COLOR_GREEN,
        "  ls [-l/-a]     List directory\n"
        "  cat <file>     Print file contents\n"
        "  cd <dir>       Change directory\n"
        "  pwd            Print working directory\n"
        "  touch <file>   Create empty file\n"
        "  mkdir <dir>    Create directory\n"
        "  rm [-rf] <f>   Remove file/directory\n"
        "  mv <src> <dst> Move/rename\n"
        "  echo <text>    Print text\n"
        "  nano <file>    Edit file\n"
        "  date           Current date/time\n"
        "  whoami         Current user\n"
        "  clear          Clear screen\n"
        "  fetch          System info (neofetch)\n"
        "  color <name>   Set accent color\n"
        "  crt [mode]     CRT effect (off/light/strong)\n"
        "  bg [on/off]    Background toggle\n"
        "  size [N]       Show/set preferred font size\n"
        "  setres WxH[@R] Try runtime framebuffer resolution\n"
        "  wm             Launch desktop WM\n"
        "  pkg ...        Package manager (search/install/remove/list)\n"
        "  say <text>     Print text (alias echo)\n"
        "  meow           Meow!\n"
        "  ferris         Ferris the crab (Unicode art)\n"
        "  gay            Pride flag\n"
        "  hits           Visit counter\n"
        "  about          About mOSs\n"
        "  ama            Ask Me Anything\n"
        "  mine           Minesweeper\n"
        "  snake          Snake game\n"
        "  tetris         Tetris\n"
        "  2048           2048 game\n"
        "  pong           Pong\n"
        "  login          Login as different user\n"
        "  logout         Log out\n"
        "  reboot         Reboot system\n"
        "  shutdown       Shutdown system\n"
        "  help           Show this help\n"
    );
    term_printf(s, MOSS_COLOR_DARK_GREEN, "  man moss       Full manual page");
    term_printf(s, MOSS_COLOR_DARK_GREEN, "  BusyBox tools  grep/find/ps/kill/df/free/top/ip/tar/wget... available externally");
    term_printf(s, MOSS_COLOR_DARK_GREEN, "  man <topic>    Built-in topic or VFS file");
}


static void term_print_dot_art(ShellState *s, const char **lines, int nlines,
                               MOSS_Color color, const char *dot_on) {
    char out[1024];
    if (!dot_on || !*dot_on) dot_on = "o";
    for (int i = 0; i < nlines; i++) {
        const char *src = lines[i] ? lines[i] : "";
        size_t j = 0;
        out[0] = '\0';
        for (size_t k = 0; src[k] && j + 4 < sizeof out; k++) {
            if (src[k] == '#' || src[k] == '@' || src[k] == 'o' || src[k] == 'O' || src[k] == '*') {
                out[j++] = dot_on[0];
            } else {
                out[j++] = ' ';
            }
        }
        out[j] = '\0';
        term_print(s, out, color, MOSS_ATTR_BOLD);
    }
}

static void term_print_rainbow_bar(ShellState *s, MOSS_Color color) {
    char bar[96];
    int w = MOSS_CLAMP(COLS - 8, 24, (int)sizeof(bar) - 1);
    memset(bar, '#', (size_t)w);
    bar[w] = '\0';
    term_print(s, bar, color, MOSS_ATTR_BOLD);
}

static void term_print_manual_topic(ShellState *s, const char *topic, const char *body) {
    term_printf(s, MOSS_COLOR_BRIGHT_GREEN, "[ %s ]", topic);
    term_print(s, body, MOSS_COLOR_GREEN, MOSS_ATTR_NORMAL);
}

static int try_write_text_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = write(fd, value, strlen(value));
    close(fd);
    return n == (ssize_t)strlen(value) ? 0 : -1;
}

static int find_external_executable(const char *cmd, char *out, size_t outsz) {
    static const char *dirs[] = { "/bin", "/usr/bin", "/usr/local/bin", "/sbin", "/usr/sbin", NULL };
    if (!cmd || !*cmd || !out || outsz == 0) return -1;
    if (strchr(cmd, '/')) {
        if (access(cmd, X_OK) == 0) {
            snprintf(out, outsz, "%s", cmd);
            return 0;
        }
        return -1;
    }
    for (int i = 0; dirs[i]; i++) {
        char candidate[512];
        snprintf(candidate, sizeof candidate, "%s/%s", dirs[i], cmd);
        if (access(candidate, X_OK) == 0) {
            snprintf(out, outsz, "%s", candidate);
            return 0;
        }
    }
    return -1;
}

static int try_setres_sysfs(int w, int h, int hz, char *msg, size_t msgsz) {
    DIR *drm = opendir("/sys/class/drm");
    if (drm) {
        struct dirent *de;
        while ((de = readdir(drm))) {
            if (de->d_name[0] == '.') continue;
            char base[256], status[320], modes[320], mode[320], enabled[320];
            snprintf(base, sizeof base, "/sys/class/drm/%s", de->d_name);
            snprintf(status, sizeof status, "%s/status", base);
            snprintf(modes, sizeof modes, "%s/modes", base);
            snprintf(mode, sizeof mode, "%s/mode", base);
            snprintf(enabled, sizeof enabled, "%s/enabled", base);

            FILE *sf = fopen(status, "r");
            if (sf) {
                char st[64] = {0};
                if (fgets(st, sizeof st, sf) && strncmp(st, "connected", 9) != 0) { fclose(sf); continue; }
                fclose(sf);
            }

            FILE *fp = fopen(modes, "r");
            if (!fp) continue;
            char line[128];
            char chosen[128] = {0};
            while (fgets(line, sizeof line, fp)) {
                int mw = 0, mh = 0;
                if (sscanf(line, "%dx%d", &mw, &mh) == 2 && mw == w && mh == h) {
                    strncpy(chosen, line, sizeof chosen - 1);
                    break;
                }
            }
            fclose(fp);
            if (chosen[0]) {
                chosen[strcspn(chosen, "\r\n")] = 0;
                try_write_text_file(enabled, "on\n");
                if (try_write_text_file(mode, chosen) == 0) {
                    snprintf(msg, msgsz, "Applied %s via DRM connector mode file.", chosen);
                    closedir(drm);
                    return 0;
                }
                char chosen_nl[128];
                snprintf(chosen_nl, sizeof chosen_nl, "%s\n", chosen);
                if (try_write_text_file(mode, chosen_nl) == 0) {
                    snprintf(msg, msgsz, "Applied %s via DRM connector mode file.", chosen);
                    closedir(drm);
                    return 0;
                }
            }
        }
        closedir(drm);
    }

    FILE *fp = fopen("/sys/class/graphics/fb0/modes", "r");
    if (fp) {
        char line[128];
        char chosen[128] = {0};
        while (fgets(line, sizeof line, fp)) {
            int mw = 0, mh = 0, mhz = 0;
            if (sscanf(line, "U:%dx%dp-%d", &mw, &mh, &mhz) >= 2 && mw == w && mh == h) {
                if (hz == 0 || mhz == hz) {
                    strncpy(chosen, line, sizeof chosen - 1);
                    break;
                }
                if (!chosen[0]) strncpy(chosen, line, sizeof chosen - 1);
            }
        }
        fclose(fp);
        if (chosen[0]) {
            chosen[strcspn(chosen, "\r\n")] = 0;
            if (try_write_text_file("/sys/class/graphics/fb0/mode", chosen) == 0) {
                snprintf(msg, msgsz, "Applied %s via /sys/class/graphics/fb0/mode.", chosen);
                return 0;
            }
        }
    }

    char value[64];
    snprintf(value, sizeof value, "%d,%d", w, h);
    if (try_write_text_file("/sys/class/graphics/fb0/virtual_size", value) == 0) {
        snprintf(msg, msgsz, "Requested %s via /sys/class/graphics/fb0/virtual_size.", value);
        return 0;
    }

    return -1;
}

static void cmd_about(ShellState *s) {
    static const char *LOGO =
        "mOSs Native\n"
        "===========\n";
    term_print(s, LOGO, MOSS_COLOR_GREEN, MOSS_ATTR_BOLD);
    term_printf(s, MOSS_COLOR_GREEN,
        "mOSs - Native System Environment v1.0\n"
        "A custom Linux distribution built around the mOSs aesthetic.\n"
        "Kernel: Linux 6.6 LTS  |  Init: custom /init\n"
        "Shell:  moss-shell (ncurses TUI)\n"
        "WM:     moss-wm (draggable desktop with taskbar)\n"
        "SDK:    libmoss v1.0 (C/Python)\n"
        "\nCreated by maseratislick\n"
        "Report bugs on Discord: maseratislick\n"
        "\nType 'man moss' for the full reference manual.");
}

static void cmd_meow(ShellState *s) {
    const char *art = moss_ascii_get(s->art, "cat");
    if (!art) art =
        " /\\_/\\\n"
        "( o.o )\n"
        " > ^ <\n"
        "meow!";
    term_print(s, art, MOSS_COLOR_GREEN, MOSS_ATTR_NORMAL);
}

static void cmd_ferris(ShellState *s) {
    const char *art = s && s->art ? moss_ascii_get(s->art, "ferris") : NULL;
    if (!art || !*art) art = MOSS_ART_FERRIS;
    term_print(s, art, MOSS_COLOR_RED, MOSS_ATTR_BOLD);
}

static void cmd_gay(ShellState *s) {
    MOSS_Color colors[] = {
        MOSS_COLOR_RED, MOSS_COLOR_YELLOW, MOSS_COLOR_GREEN,
        MOSS_COLOR_CYAN, MOSS_COLOR_BLUE, MOSS_COLOR_MAGENTA
    };
    for (int i = 0; i < 6; i++)
        term_print_rainbow_bar(s, colors[i]);
}

static void cmd_say(ShellState *s, const char *text) {
    if (!text || !*text) { term_printf(s, MOSS_COLOR_RED, "say: missing text"); return; }
    term_printf(s, MOSS_COLOR_GREEN, "%s", text);
}

static void cmd_reboot(ShellState *s) {
    (void)s;
    term_printf(s, MOSS_COLOR_YELLOW, "System rebooting...");
    term_redraw(s);
    moss_sleep_ms(1000);
    sync();
    reboot(RB_AUTOBOOT);
}

static void cmd_shutdown(ShellState *s) {
    term_printf(s, MOSS_COLOR_YELLOW, "System shutting down...");
    term_redraw(s);
    moss_sleep_ms(1000);
    sync();
    reboot(RB_POWER_OFF);
}

static void cmd_nano(ShellState *s, const char *arg) {
    if (!arg || !*arg) { term_printf(s, MOSS_COLOR_RED, "nano: missing filename"); return; }
    char path[512]; vfs_resolve(s, arg, path, sizeof path);
    /* Launch a minimal fullscreen editor using the VFS */
    VFSEntry *e = vfs_find(s, path);
    char *content = e && !e->is_dir && e->content ? strdup(e->content) : strdup("");
    if (!content) return;

    /* Simple line editor - press Ctrl+X to exit, Ctrl+S to save */
    WINDOW *ew = newwin(LINES, COLS, 0, 0);
    box(ew, 0, 0);
    wattron(ew, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(ew, 0, 2, "[ nano: %s ]  ^X=exit  ^S=save", arg);
    wattroff(ew, COLOR_PAIR(1) | A_BOLD);

    WINDOW *ec = derwin(ew, LINES-3, COLS-2, 1, 1);
    scrollok(ec, TRUE); keypad(ec, TRUE);

    wattron(ec, COLOR_PAIR(1));
    mvwaddstr(ec, 0, 0, content);
    wmove(ec, 0, 0);
    wattroff(ec, COLOR_PAIR(1));
    wrefresh(ew); wrefresh(ec);

    /* Minimal edit loop - 64 KB heap buffer (not stack: would overflow
     * BusyBox getty's constrained stack environment). */
    char *edit_buf = malloc(65536);
    if (!edit_buf) { delwin(ec); delwin(ew); free(content); return; }
    strncpy(edit_buf, content, 65535); edit_buf[65535] = 0;
    int ch2;
    while ((ch2 = wgetch(ec)) != MOSS_KEY_CTRL('x')) {
        if (ch2 == MOSS_KEY_CTRL('s')) {
            /* Save to VFS */
            if (!e) {
                /* Create new entry */
                if (s->vfs_count < s->vfs_count + 1) {
                    VFSEntry ne = {0};
                    strncpy(ne.path, path, sizeof ne.path - 1);
                    strncpy(ne.name, arg, sizeof ne.name - 1);
                    ne.is_dir = false;
                    ne.content = strdup(edit_buf);
                    s->vfs[s->vfs_count++] = ne;
                }
            } else {
                free(e->content); e->content = strdup(edit_buf);
            }
            wattron(ec, COLOR_PAIR(9) | A_BOLD);
            mvwaddstr(ew, LINES-2, 2, " [Saved] ");
            wattroff(ec, COLOR_PAIR(9) | A_BOLD);
            wrefresh(ew);
        }
    }

    delwin(ec); delwin(ew);
    free(edit_buf);
    free(content);
    term_printf(s, MOSS_COLOR_GREEN, "nano: closed %s", arg);
    term_redraw(s);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Command dispatcher                                                           */
/* ─────────────────────────────────────────────────────────────────────────── */
static void handle_command(ShellState *s, const char *line) {
    if (!line || !*line) return;

    /* Push to history */
    if (s->history_len < 500) {
        s->history = realloc(s->history, (s->history_len+1)*sizeof(char*));
        s->history[s->history_len++] = strdup(line);
    }
    s->history_idx = s->history_len;

    /* Echo the command itself */
    char prompt_echo[512];
    snprintf(prompt_echo, sizeof prompt_echo,
             "%s@mOSs:%s$ %s", s->username, s->cwd, line);
    term_print(s, prompt_echo, MOSS_COLOR_GREEN, MOSS_ATTR_BOLD);

    /* Tokenise */
    char buf[4096]; strncpy(buf, line, sizeof buf - 1);
    char *cmd = strtok(buf, " ");
    char *args = strtok(NULL, "");
    if (!cmd) return;
    for (char *p = cmd; *p; ++p) *p = (char)tolower((unsigned char)*p);

    /* Synonym resolution */
    static const struct { const char *from, *to; } SYN[] = {
        {"dir","ls"},{"list","ls"},{"cls","clear"},{"time","date"},
        {"who","whoami"},{"reboot","reboot"},{"restart","reboot"},
        {"poweroff","shutdown"},{"type","cat"},{"display","cat"},
        {"neofetch","fetch"},{"info","about"},{"colour","color"},
        {"bouncing","bounce"},{"windowmanager","wm"},{"startup","startup"},
    };
    for (int i=0;i<(int)(sizeof SYN/sizeof SYN[0]);i++)
        if (strcmp(cmd,SYN[i].from)==0) { cmd=(char*)SYN[i].to; break; }

    /* Dispatch */
    if      (!strcmp(cmd,"ls"))       cmd_ls(s, args ? args : "");
    else if (!strcmp(cmd,"cat"))      cmd_cat(s, args);
    else if (!strcmp(cmd,"cd"))       {
        if (!args || !strcmp(args,"/")) {
            strcpy(s->cwd, "/");
        } else if (!strcmp(args,"..")) {
            char *sl = strrchr(s->cwd,'/');
            if (sl && sl!=s->cwd) *sl=0; else strcpy(s->cwd,"/");
        } else {
            char path[512]; vfs_resolve(s, args, path, sizeof path);
            if (vfs_find(s, path)) strncpy(s->cwd, path, sizeof s->cwd-1);
            else term_printf(s, MOSS_COLOR_RED, "cd: %s: No such directory", args);
        }
    }
    else if (!strcmp(cmd,"pwd"))      term_printf(s, MOSS_COLOR_GREEN, "%s", s->cwd);
    else if (!strcmp(cmd,"mv"))       cmd_mv(s, args);
    else if (!strcmp(cmd,"touch"))    {
        if (!args) { term_printf(s, MOSS_COLOR_RED, "touch: missing filename"); }
        else {
            char path[512]; vfs_resolve(s, args, path, sizeof path);
            if (!vfs_find(s, path)) {
                VFSEntry e={0};
                strncpy(e.path,path,sizeof e.path-1);
                strncpy(e.name,args,sizeof e.name-1);
                e.content=strdup(""); s->vfs[s->vfs_count++]=e;
                term_printf(s, MOSS_COLOR_GREEN, "Created: %s", args);
            }
        }
    }
    else if (!strcmp(cmd,"mkdir"))    {
        if (!args) { term_printf(s, MOSS_COLOR_RED, "mkdir: missing name"); }
        else {
            char path[512]; vfs_resolve(s, args, path, sizeof path);
            VFSEntry e={0}; strncpy(e.path,path,sizeof e.path-1);
            strncpy(e.name,args,sizeof e.name-1); e.is_dir=true;
            s->vfs[s->vfs_count++]=e;
            term_printf(s, MOSS_COLOR_GREEN, "Directory created: %s", args);
        }
    }
    else if (!strcmp(cmd,"rm"))       {
        if (!args) { term_printf(s, MOSS_COLOR_RED, "rm: missing filename"); return; }
        char path[512]; vfs_resolve(s, strstr(args,"-rf ")?args+4:
                                     strstr(args,"-r ")?args+3:args,
                                     path, sizeof path);
        VFSEntry *e = vfs_find(s, path);
        if (!e) { term_printf(s, MOSS_COLOR_RED, "rm: cannot remove '%s'", args); }
        else {
            int idx = (int)(e - s->vfs);
            free(e->content);
            memmove(s->vfs+idx, s->vfs+idx+1, (s->vfs_count-idx-1)*sizeof(VFSEntry));
            s->vfs_count--;
            term_printf(s, MOSS_COLOR_GREEN, "Removed: %s", args);
        }
    }
    else if (!strcmp(cmd,"echo"))     term_printf(s, MOSS_COLOR_GREEN, "%s", args?args:"");
    else if (!strcmp(cmd,"say"))      cmd_say(s, args);
    else if (!strcmp(cmd,"nano"))     cmd_nano(s, args);
    else if (!strcmp(cmd,"clear"))    { s->sb_count=0; s->sb_top=0; }
    else if (!strcmp(cmd,"date"))     {
        time_t t=time(NULL); term_printf(s, MOSS_COLOR_GREEN, "%s", ctime(&t));
    }
    else if (!strcmp(cmd,"whoami"))   term_printf(s, MOSS_COLOR_GREEN, "%s", s->username);
    else if (!strcmp(cmd,"help"))     cmd_help(s);
    else if (!strcmp(cmd,"about"))    cmd_about(s);
    else if (!strcmp(cmd,"fetch"))    cmd_fetch(s);
    else if (!strcmp(cmd,"color"))    cmd_color(s, args);
    else if (!strcmp(cmd,"crt"))      cmd_crt(s, args);
    else if (!strcmp(cmd,"bg"))       {
        if (!args||!strcmp(args,"on"))  s->bg_enabled=true;
        else if(!strcmp(args,"off"))    s->bg_enabled=false;
        term_printf(s, MOSS_COLOR_GREEN, "Background: %s", s->bg_enabled?"on":"off");
    }
    else if (!strcmp(cmd,"size"))     cmd_size(s, args);
    else if (!strcmp(cmd,"font"))     cmd_font(s, args);
    else if (!strcmp(cmd,"setres"))   cmd_setres(s, args);
    else if (!strcmp(cmd,"meow"))     cmd_meow(s);
    else if (!strcmp(cmd,"ferris"))   cmd_ferris(s);
    else if (!strcmp(cmd,"gay"))      cmd_gay(s);
    else if (!strcmp(cmd,"hits"))     term_printf(s, MOSS_COLOR_GREEN, "Total hits: %d", ++s->hit_count);
    else if (!strcmp(cmd,"pkg"))      {
        char exe[512];
        if (find_external_executable("pkg", exe, sizeof exe) == 0 || find_external_executable("moss-pkg", exe, sizeof exe) == 0) {
            char full[4096]; snprintf(full, sizeof full, "%s %s", exe, args?args:"");
            def_prog_mode(); endwin(); system(full); reset_prog_mode(); raw(); noecho(); keypad(stdscr, TRUE); refresh();
        } else {
            term_printf(s, MOSS_COLOR_RED, "pkg: package manager is not installed in this build");
        }
    }
    else if (!strcmp(cmd,"wm"))       { shell_launch_wm(s); }
    else if (!strcmp(cmd,"startup"))  term_printf(s, MOSS_COLOR_GREEN, "Mode: %s", s->mode==MODE_WM?"wm":"terminal");
    else if (!strcmp(cmd,"login"))    { shell_relogin(s); }
    else if (!strcmp(cmd,"logout"))   { shell_relogin(s); }
    else if (!strcmp(cmd,"reboot"))   cmd_reboot(s);
    else if (!strcmp(cmd,"shutdown")) cmd_shutdown(s);
    else if (!strcmp(cmd,"mine"))     { run_minesweeper_game(s); }
    else if (!strcmp(cmd,"snake"))    { run_snake_game(s); }
    else if (!strcmp(cmd,"tetris"))   { run_tetris_game(s); }
    else if (!strcmp(cmd,"2048"))     { run_2048_game(s); }
    else if (!strcmp(cmd,"pong"))     { run_pong_game(s); }
    else if (!strcmp(cmd,"ama"))      {
        term_printf(s, MOSS_COLOR_YELLOW,
            "AMA is a web feature. In native mode, messages are written to\n"
            "/var/lib/moss/ama.log - the admin can read them after reboot.");
    }
    else if (!strcmp(cmd,"man"))      cmd_man(s, args);
    else {
        /* Try exec from /bin */
        char exe[512]; snprintf(exe, sizeof exe, "/bin/%s", cmd);
        if (access(exe, X_OK) == 0) {
            char full[4096]; snprintf(full, sizeof full, "%s %s", exe, args?args:"");
            def_prog_mode();
            endwin();
            system(full);
            reset_prog_mode();
            raw();
            noecho();
            keypad(stdscr, TRUE);
            refresh();
        } else {
            term_printf(s, MOSS_COLOR_RED,
                "moss: command not found: %s\nType 'help' for a list of commands.", cmd);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Terminal input loop                                                          */
/* ─────────────────────────────────────────────────────────────────────────── */
static void terminal_loop(ShellState *s) {
    raw();
    noecho();
    keypad(stdscr, TRUE);
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);

    /* Welcome banner */
    term_printf(s, MOSS_COLOR_GREEN, "");
    term_printf(s, MOSS_COLOR_GREEN, "Welcome to mOSs, %s!", s->username);
    term_printf(s, MOSS_COLOR_DARK_GREEN, "Type 'help' for commands, 'wm' for desktop, 'pkg' for packages.");
    term_printf(s, MOSS_COLOR_GREEN, "");
    term_redraw(s);

    halfdelay(1);
    shell_start_mouse_thread(s);

    while (!s->ctx->quit) {
        if (s->mouse_scroll_pending != 0) {
            int delta = s->mouse_scroll_pending;
            s->mouse_scroll_pending = 0;
            term_scroll(s, -delta * MOSS_MAX(1, LINES / 6));
            term_redraw(s);
        }
        int ch = getch();
        if (ch == ERR) { term_redraw(s); continue; }

        /* Handle special keys */
        if (ch == '\n' || ch == '\r') {
            s->input_buf[s->input_len] = 0;
            handle_command(s, s->input_buf);
            s->input_len = 0; s->input_pos = 0;
            memset(s->input_buf, 0, sizeof s->input_buf);
        } else if (ch == KEY_BACKSPACE || ch == 127) {
            if (s->input_pos > 0) {
                memmove(s->input_buf + s->input_pos - 1,
                        s->input_buf + s->input_pos,
                        s->input_len - s->input_pos);
                s->input_len--; s->input_pos--;
                s->input_buf[s->input_len] = 0;
            }
        } else if (ch == KEY_LEFT)  { if (s->input_pos > 0) s->input_pos--; }
        else if (ch == KEY_RIGHT) { if (s->input_pos < s->input_len) s->input_pos++; }
        else if (ch == KEY_UP) {
            if (s->history_idx > 0) {
                s->history_idx--;
                strncpy(s->input_buf, s->history[s->history_idx], sizeof s->input_buf-1);
                s->input_len = (int)strlen(s->input_buf);
                s->input_pos = s->input_len;
            }
        } else if (ch == KEY_DOWN) {
            if (s->history_idx < s->history_len-1) {
                s->history_idx++;
                strncpy(s->input_buf, s->history[s->history_idx], sizeof s->input_buf-1);
                s->input_len = (int)strlen(s->input_buf);
                s->input_pos = s->input_len;
            } else {
                s->history_idx = s->history_len;
                s->input_buf[0] = 0; s->input_len = 0; s->input_pos = 0;
            }
        } else if (ch == KEY_PPAGE) { term_scroll(s, -(LINES/2)); }
        else if (ch == KEY_NPAGE)   { term_scroll(s,  (LINES/2)); }
        else if (ch == KEY_MOUSE) {
            MEVENT ev;
            if (getmouse(&ev) == OK) {
#ifdef BUTTON4_PRESSED
                if (ev.bstate & BUTTON4_PRESSED) term_scroll(s, -(LINES/3));
#endif
#ifdef BUTTON5_PRESSED
                if (ev.bstate & BUTTON5_PRESSED) term_scroll(s,  (LINES/3));
#endif
            }
        }
        else if (ch == KEY_RESIZE) {
            term_redraw(s);
        }
        else if (ch == '\t') {
            /* Tab completion: try to complete command / filename */
            /* Stub: print available commands starting with input */
        } else if (ch == KEY_F(1)) { cmd_help(s); }
        else if (ch == KEY_F(5))   { shell_launch_wm(s); }
        else if (ch == MOSS_KEY_CTRL('l')) { s->sb_count=0; s->sb_top=0; }
        else if (ch == MOSS_KEY_CTRL('c')) {
            term_printf(s, MOSS_COLOR_RED, "^C");
            s->input_len=0; s->input_pos=0; memset(s->input_buf,0,sizeof s->input_buf);
        } else if (ch >= 32 && ch < 127 && s->input_len < 4000) {
            /* Insert at cursor */
            memmove(s->input_buf + s->input_pos + 1,
                    s->input_buf + s->input_pos,
                    s->input_len - s->input_pos);
            s->input_buf[s->input_pos++] = (char)ch;
            s->input_len++;
        }

        term_redraw(s);
    }

    shell_stop_mouse_thread(s);
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
}

static void term_scroll(ShellState *s, int delta) {
    s->sb_top = MOSS_CLAMP(s->sb_top + delta, 0,
                            MOSS_MAX(0, s->sb_count - (LINES-3)));
}

static void play_chime(void) {
    /* Non-blocking: fork + aplay / mpg123 */
    if (fork() == 0) {
        close(STDOUT_FILENO); close(STDERR_FILENO);
        execlp("mpg123", "mpg123", "-q",
               "/usr/share/moss/sounds/chime.mp3", NULL);
        execlp("aplay", "aplay", "-q",
               "/usr/share/moss/sounds/chime.wav", NULL);
        _exit(0);
    }
}

static bool login_accepts_char(int ch, bool password) {
    if (ch < 0 || ch > 255) return false;
    if (isalnum((unsigned char)ch)) return true;
    if (password) return ch == '_' || ch == '-' || ch == '.' || ch == '@';
    return ch == '_' || ch == '-' || ch == '.';
}

static void login_read_field(int y, int x, char *buf, size_t bufsz, bool password) {
    size_t len = 0;
    int ch;
    memset(buf, 0, bufsz);
    keypad(stdscr, TRUE);
    curs_set(1);
    move(y, x);
    clrtoeol();
    refresh();

    while (1) {
        move(y, x + (int)len);
        refresh();
        ch = getch();
        if (ch == ERR) continue;
        if (ch == KEY_ENTER || ch == 10 || ch == 13 || ch == 9) break;
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                len--;
                buf[len] = '\0';
                mvaddch(y, x + (int)len, ' ');
                move(y, x + (int)len);
            }
            continue;
        }
        if (ch == 27 || ch == KEY_UP || ch == KEY_DOWN || ch == KEY_LEFT || ch == KEY_RIGHT) continue;
        if (!login_accepts_char(ch, password)) continue;
        if (len + 1 >= bufsz) continue;
        buf[len++] = (char)ch;
        mvaddch(y, x + (int)len - 1, password ? '*' : ch);
    }

    buf[len] = '\0';
    curs_set(0);
}


static int open_shell_mouse_fds(int *fds, int maxfds) {
    int count = 0;
    DIR *dir = opendir("/dev/input");
    if (!dir) return 0;
    struct dirent *de;
    while ((de = readdir(dir)) && count < maxfds) {
        if (strncmp(de->d_name, "event", 5) != 0) continue;
        char path[256];
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) fds[count++] = fd;
    }
    closedir(dir);
    return count;
}

static void *mouse_wheel_thread_main(void *arg) {
    ShellState *s = (ShellState *)arg;
    int fds[32];
    int nfds = open_shell_mouse_fds(fds, 32);
    int legacy = open("/dev/input/mice", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct input_event ev;
    unsigned char pkt[4];
    while (!s->mouse_thread_stop) {
        int got = 0;
        for (int i = 0; i < nfds; i++) {
            while (read(fds[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
                got = 1;
                if (ev.type == EV_REL && (ev.code == REL_WHEEL || ev.code == REL_HWHEEL))
                    s->mouse_scroll_pending += ev.value;
            }
        }
        if (legacy >= 0) {
            while (read(legacy, pkt, sizeof pkt) == 4) {
                got = 1;
                signed char wheel = (signed char)pkt[3];
                if (wheel != 0) s->mouse_scroll_pending += (int)wheel;
            }
        }
        if (!got) usleep(12000);
    }
    for (int i = 0; i < nfds; i++) close(fds[i]);
    if (legacy >= 0) close(legacy);
    return NULL;
}

static void shell_start_mouse_thread(ShellState *s) {
    if (s->mouse_thread_running) return;
    s->mouse_thread_stop = false;
    s->mouse_scroll_pending = 0;
    if (pthread_create(&s->mouse_thread, NULL, mouse_wheel_thread_main, s) == 0)
        s->mouse_thread_running = true;
}

static void shell_stop_mouse_thread(ShellState *s) {
    if (!s->mouse_thread_running) return;
    s->mouse_thread_stop = true;
    pthread_join(s->mouse_thread, NULL);
    s->mouse_thread_running = false;
}

static void shell_launch_wm(ShellState *s) {
    shell_stop_mouse_thread(s);
    s->mode = MODE_WM;
    wm_run(s->wm, s);
    s->mode = MODE_TERMINAL;
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    keypad(stdscr, TRUE);
    raw();
    noecho();
    clear();
    term_redraw(s);
    shell_start_mouse_thread(s);
}

static void shell_relogin(ShellState *s) {
    shell_stop_mouse_thread(s);
    s->logged_in = false;
    memset(s->input_buf, 0, sizeof s->input_buf);
    s->input_len = 0;
    s->input_pos = 0;
    clear();
    refresh();
    if (!show_login(s)) {
        s->logged_in = true;
    }
    if (s->mode == MODE_WM) {
        shell_launch_wm(s);
    } else {
        clear();
        term_redraw(s);
        shell_start_mouse_thread(s);
    }
}

static void game_finish_screen(const char *title, const char *line1, const char *line2) {
    timeout(-1);
    clear();
    attron(COLOR_PAIR(1) | A_BOLD);
    mvprintw(2, MOSS_MAX(0, (COLS - (int)strlen(title)) / 2), "%s", title);
    attroff(COLOR_PAIR(1) | A_BOLD);
    attron(COLOR_PAIR(1));
    mvprintw(4, 2, "%s", line1 ? line1 : "");
    mvprintw(5, 2, "%s", line2 ? line2 : "Press any key to return.");
    attroff(COLOR_PAIR(1));
    refresh();
    getch();
}

static void run_snake_game(ShellState *s) {
    (void)s;
    enum { MAX_SEG = 2048 };
    typedef struct { int r, c; } Pt;
    Pt snake[MAX_SEG];
    int len = 5;
    int dir = KEY_RIGHT;
    int nr = LINES - 4, nc = COLS - 2;
    if (nr < 10 || nc < 20) { game_finish_screen("Snake", "Terminal too small.", NULL); return; }
    for (int i = 0; i < len; i++) { snake[i].r = nr / 2; snake[i].c = nc / 2 - i; }
    srand((unsigned int)time(NULL));
    Pt food = { 2 + rand() % (nr - 4), 2 + rand() % (nc - 4) };
    int score = 0;
    timeout(100);
    keypad(stdscr, TRUE);
    while (1) {
        clear();
        attron(COLOR_PAIR(1));
        box(stdscr, 0, 0);
        mvprintw(0, 2, "[ Snake ] score=%d  arrows/WASD move  q quits", score);
        mvaddch(food.r, food.c, '@');
        for (int i = 0; i < len; i++) mvaddch(snake[i].r, snake[i].c, i == 0 ? 'O' : 'o');
        attroff(COLOR_PAIR(1));
        refresh();
        int ch = getch();
        if (ch == 'q' || ch == 27) break;
        if ((ch == KEY_UP || ch == 'w' || ch == 'W') && dir != KEY_DOWN) dir = KEY_UP;
        else if ((ch == KEY_DOWN || ch == 's' || ch == 'S') && dir != KEY_UP) dir = KEY_DOWN;
        else if ((ch == KEY_LEFT || ch == 'a' || ch == 'A') && dir != KEY_RIGHT) dir = KEY_LEFT;
        else if ((ch == KEY_RIGHT || ch == 'd' || ch == 'D') && dir != KEY_LEFT) dir = KEY_RIGHT;
        Pt next = snake[0];
        if (dir == KEY_UP) next.r--; else if (dir == KEY_DOWN) next.r++; else if (dir == KEY_LEFT) next.c--; else next.c++;
        if (next.r <= 0 || next.r >= nr || next.c <= 0 || next.c >= nc) break;
        for (int i = 0; i < len; i++) if (snake[i].r == next.r && snake[i].c == next.c) goto snake_done;
        for (int i = len; i > 0; i--) snake[i] = snake[i-1];
        snake[0] = next;
        if (next.r == food.r && next.c == food.c) {
            if (len < MAX_SEG - 1) len++;
            score += 10;
            int ok = 0;
            while (!ok) {
                ok = 1;
                food.r = 2 + rand() % (nr - 4); food.c = 2 + rand() % (nc - 4);
                for (int i = 0; i < len; i++) if (snake[i].r == food.r && snake[i].c == food.c) ok = 0;
            }
        }
    }
snake_done:
    timeout(-1);
    char msg[64]; snprintf(msg, sizeof msg, "Game over. Score: %d", score);
    game_finish_screen("Snake", msg, "Press any key to return.");
    clear(); term_redraw(s);
}

static int slide_line_2048(int line[4], int *score) {
    int old[4]; memcpy(old, line, sizeof old);
    int tmp[4] = {0}; int pos = 0;
    for (int i = 0; i < 4; i++) if (line[i]) tmp[pos++] = line[i];
    for (int i = 0; i < 3; i++) if (tmp[i] && tmp[i] == tmp[i+1]) { tmp[i] *= 2; *score += tmp[i]; tmp[i+1] = 0; }
    int out[4] = {0}; pos = 0;
    for (int i = 0; i < 4; i++) if (tmp[i]) out[pos++] = tmp[i];
    memcpy(line, out, sizeof out);
    return memcmp(old, line, sizeof old) != 0;
}

static void add_random_tile_2048(int b[4][4]) {
    int empty[16][2], n = 0;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) if (!b[r][c]) { empty[n][0] = r; empty[n][1] = c; n++; }
    if (!n) return;
    int pick = rand() % n;
    b[empty[pick][0]][empty[pick][1]] = (rand() % 10 == 0) ? 4 : 2;
}

static int board_has_moves_2048(int b[4][4]) {
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        if (!b[r][c]) return 1;
        if (r < 3 && b[r][c] == b[r+1][c]) return 1;
        if (c < 3 && b[r][c] == b[r][c+1]) return 1;
    }
    return 0;
}

static void run_2048_game(ShellState *s) {
    int b[4][4] = {{0}}; int score = 0;
    srand((unsigned int)time(NULL) ^ 0x2048U);
    add_random_tile_2048(b); add_random_tile_2048(b);
    keypad(stdscr, TRUE); timeout(-1);
    while (1) {
        clear();
        attron(COLOR_PAIR(1));
        box(stdscr, 0, 0);
        mvprintw(0, 2, "[ 2048 ] score=%d  arrows move  q quits", score);
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                int y = 2 + r * 3, x = 4 + c * 8;
                mvprintw(y, x, "+------+" );
                mvprintw(y+1, x, "|%6s|", "");
                mvprintw(y+2, x, "+------+" );
                if (b[r][c]) mvprintw(y+1, x+1, "%6d", b[r][c]);
            }
        }
        attroff(COLOR_PAIR(1));
        refresh();
        int ch = getch();
        if (ch == 'q' || ch == 27) break;
        int moved = 0;
        if (ch == KEY_LEFT || ch == 'a' || ch == 'A') {
            for (int r = 0; r < 4; r++) moved |= slide_line_2048(b[r], &score);
        } else if (ch == KEY_RIGHT || ch == 'd' || ch == 'D') {
            for (int r = 0; r < 4; r++) { int line[4]; for (int i = 0; i < 4; i++) line[i] = b[r][3-i]; moved |= slide_line_2048(line, &score); for (int i = 0; i < 4; i++) b[r][3-i] = line[i]; }
        } else if (ch == KEY_UP || ch == 'w' || ch == 'W') {
            for (int c = 0; c < 4; c++) { int line[4]; for (int i = 0; i < 4; i++) line[i] = b[i][c]; moved |= slide_line_2048(line, &score); for (int i = 0; i < 4; i++) b[i][c] = line[i]; }
        } else if (ch == KEY_DOWN || ch == 's' || ch == 'S') {
            for (int c = 0; c < 4; c++) { int line[4]; for (int i = 0; i < 4; i++) line[i] = b[3-i][c]; moved |= slide_line_2048(line, &score); for (int i = 0; i < 4; i++) b[3-i][c] = line[i]; }
        }
        if (moved) add_random_tile_2048(b);
        int won = 0; for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) if (b[r][c] >= 2048) won = 1;
        if (won) { game_finish_screen("2048", "You reached 2048.", "Press any key to return."); break; }
        if (!board_has_moves_2048(b)) { char msg[64]; snprintf(msg, sizeof msg, "No more moves. Score: %d", score); game_finish_screen("2048", msg, "Press any key to return."); break; }
    }
    clear(); term_redraw(s);
}

static void flood_ms(int rows, int cols, int bombs[16][16], int vis[16][16], int r, int c) {
    if (r < 0 || c < 0 || r >= rows || c >= cols || vis[r][c]) return;
    vis[r][c] = 1;
    if (bombs[r][c] == -1) return;
    if (bombs[r][c] != 0) return;
    for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) if (dr || dc) flood_ms(rows, cols, bombs, vis, r+dr, c+dc);
}

static void run_minesweeper_game(ShellState *s) {
    const int rows = 9, cols = 9, mines = 10;
    int bombs[16][16] = {{0}}, vis[16][16] = {{0}}, flag[16][16] = {{0}};
    int cur_r = 0, cur_c = 0, placed = 0, revealed = 0;
    srand((unsigned int)time(NULL) ^ 0x51A2U);
    while (placed < mines) { int r = rand() % rows, c = rand() % cols; if (bombs[r][c] == -1) continue; bombs[r][c] = -1; placed++; }
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) if (bombs[r][c] != -1) {
        int n = 0; for (int dr = -1; dr <= 1; dr++) for (int dc = -1; dc <= 1; dc++) { int rr = r + dr, cc = c + dc; if (rr >= 0 && cc >= 0 && rr < rows && cc < cols && bombs[rr][cc] == -1) n++; }
        bombs[r][c] = n;
    }
    keypad(stdscr, TRUE); timeout(-1);
    while (1) {
        clear(); attron(COLOR_PAIR(1)); box(stdscr, 0, 0);
        mvprintw(0, 2, "[ Minesweeper ] arrows move  Enter reveal  f flag  q quits");
        for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
            int y = 2 + r, x = 4 + c * 2;
            if (r == cur_r && c == cur_c) attron(A_REVERSE | COLOR_PAIR(1));
            char ch = '#';
            if (flag[r][c]) ch = 'F';
            if (vis[r][c]) ch = bombs[r][c] == -1 ? '*' : (bombs[r][c] ? '0' + bombs[r][c] : '.');
            mvaddch(y, x, ch);
            if (r == cur_r && c == cur_c) attroff(A_REVERSE | COLOR_PAIR(1));
        }
        attroff(COLOR_PAIR(1)); refresh();
        int ch = getch();
        if (ch == 'q' || ch == 27) break;
        if (ch == KEY_UP) cur_r = (cur_r + rows - 1) % rows; else if (ch == KEY_DOWN) cur_r = (cur_r + 1) % rows;
        else if (ch == KEY_LEFT) cur_c = (cur_c + cols - 1) % cols; else if (ch == KEY_RIGHT) cur_c = (cur_c + 1) % cols;
        else if (ch == 'f' || ch == 'F') flag[cur_r][cur_c] = !flag[cur_r][cur_c];
        else if (ch == KEY_ENTER || ch == 10 || ch == 13 || ch == ' ') {
            if (flag[cur_r][cur_c]) continue;
            if (bombs[cur_r][cur_c] == -1) { for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) if (bombs[r][c] == -1) vis[r][c] = 1; game_finish_screen("Minesweeper", "Boom. You hit a mine.", "Press any key to return."); break; }
            flood_ms(rows, cols, bombs, vis, cur_r, cur_c);
            revealed = 0; for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) if (vis[r][c] && bombs[r][c] != -1) revealed++;
            if (revealed == rows * cols - mines) { game_finish_screen("Minesweeper", "You cleared the board.", "Press any key to return."); break; }
        }
    }
    clear(); term_redraw(s);
}

static int tetris_rotate(int px, int py, int r) {
    switch (r % 4) {
        case 0: return py * 4 + px;
        case 1: return 12 + py - (px * 4);
        case 2: return 15 - (py * 4) - px;
        default: return 3 - py + (px * 4);
    }
}

static void run_tetris_game(ShellState *s) {
    const char *tet[7] = {
        "..X...X...X...X.",
        "..X..XX...X.....",
        ".X...XX...X.....",
        ".....XX..XX.....",
        "..X..XX..X......",
        ".X...XX...X.....",
        ".X...X...XX....."
    };
    int field_w = 12, field_h = 18;
    int field[12 * 18];
    for (int x = 0; x < field_w; x++) for (int y = 0; y < field_h; y++) field[y * field_w + x] = (x == 0 || x == field_w - 1 || y == field_h - 1) ? 9 : 0;
    int cur = rand() % 7, rot = 0, px = field_w / 2, py = 0, score = 0;
    int speed = 20, tick = 0, force = 0, game_over = 0;
    keypad(stdscr, TRUE); timeout(50);
    while (!game_over) {
        int ch = getch();
        if (ch == 'q' || ch == 27) break;
        int can = 1;
        if (ch == KEY_LEFT) { for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) if (tet[cur][tetris_rotate(x,y,rot)] == 'X' && field[(py+y)*field_w + (px+x-1)]) can = 0; if (can) px--; }
        if (ch == KEY_RIGHT) { can = 1; for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) if (tet[cur][tetris_rotate(x,y,rot)] == 'X' && field[(py+y)*field_w + (px+x+1)]) can = 0; if (can) px++; }
        if (ch == KEY_UP) { int nr = rot + 1; can = 1; for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) if (tet[cur][tetris_rotate(x,y,nr)] == 'X' && field[(py+y)*field_w + (px+x)]) can = 0; if (can) rot = nr; }
        if (ch == KEY_DOWN || ch == ' ') speed = 1;
        tick++; force = (tick % speed) == 0;
        if (force) {
            can = 1;
            for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) if (tet[cur][tetris_rotate(x,y,rot)] == 'X' && field[(py+y+1)*field_w + (px+x)]) can = 0;
            if (can) py++;
            else {
                for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) if (tet[cur][tetris_rotate(x,y,rot)] == 'X') field[(py+y)*field_w + (px+x)] = cur + 1;
                for (int y = 0; y < 4; y++) if (py + y < field_h - 1) {
                    int line = 1; for (int x = 1; x < field_w - 1; x++) line &= field[(py+y)*field_w + x] != 0;
                    if (line) {
                        for (int x = 1; x < field_w - 1; x++) field[(py+y)*field_w + x] = 8;
                        for (int yy = py + y; yy > 0; yy--) for (int x = 1; x < field_w - 1; x++) field[yy*field_w + x] = field[(yy-1)*field_w + x];
                        score += 100;
                    }
                }
                cur = rand() % 7; rot = 0; px = field_w / 2; py = 0; speed = 20;
                for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) if (tet[cur][tetris_rotate(x,y,rot)] == 'X' && field[(py+y)*field_w + (px+x)]) game_over = 1;
            }
        }
        clear(); attron(COLOR_PAIR(1)); box(stdscr, 0, 0); mvprintw(0, 2, "[ Tetris ] score=%d  arrows move/rotate  q quits", score);
        for (int y = 0; y < field_h; y++) for (int x = 0; x < field_w; x++) {
            char out = " .########"[field[y*field_w + x]];
            mvaddch(2 + y, 4 + x, out);
        }
        for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) if (tet[cur][tetris_rotate(x,y,rot)] == 'X') mvaddch(2 + py + y, 4 + px + x, 'X');
        attroff(COLOR_PAIR(1)); refresh();
    }
    timeout(-1); char msg[64]; snprintf(msg, sizeof msg, "Game over. Score: %d", score); game_finish_screen("Tetris", msg, "Press any key to return."); clear(); term_redraw(s);
}

static void run_pong_game(ShellState *s) {
    int paddle_w = 10, paddle_x = COLS / 2 - 5, score = 0;
    int bx = COLS / 2, by = LINES / 2, vx = 1, vy = -1;
    keypad(stdscr, TRUE); timeout(40);
    while (1) {
        clear(); attron(COLOR_PAIR(1)); box(stdscr, 0, 0);
        mvprintw(0, 2, "[ Pong ] score=%d  arrows move  q quits", score);
        for (int i = 0; i < paddle_w; i++) mvaddch(LINES - 3, paddle_x + i, '=');
        mvaddch(by, bx, 'O');
        attroff(COLOR_PAIR(1)); refresh();
        int ch = getch();
        if (ch == 'q' || ch == 27) break;
        if ((ch == KEY_LEFT || ch == 'a') && paddle_x > 1) paddle_x--;
        if ((ch == KEY_RIGHT || ch == 'd') && paddle_x + paddle_w < COLS - 1) paddle_x++;
        bx += vx; by += vy;
        if (bx <= 1 || bx >= COLS - 2) vx = -vx;
        if (by <= 1) vy = 1;
        if (by == LINES - 4 && bx >= paddle_x && bx < paddle_x + paddle_w) { vy = -1; score++; }
        if (by >= LINES - 2) break;
    }
    timeout(-1); char msg[64]; snprintf(msg, sizeof msg, "Ball dropped. Score: %d", score); game_finish_screen("Pong", msg, "Press any key to return."); clear(); term_redraw(s);
}


void moss_shell_run_named_game(void *shell_state, const char *name) {
    ShellState *s = (ShellState *)shell_state;
    if (!s || !name) return;
    if (strcmp(name, "snake") == 0) run_snake_game(s);
    else if (strcmp(name, "mine") == 0 || strcmp(name, "minesweeper") == 0) run_minesweeper_game(s);
    else if (strcmp(name, "tetris") == 0) run_tetris_game(s);
    else if (strcmp(name, "2048") == 0) run_2048_game(s);
    else if (strcmp(name, "pong") == 0) run_pong_game(s);
}

/* ─────────────────────────────────────────────────────────────────────────── */
/*  Main shell entry                                                             */
/* ─────────────────────────────────────────────────────────────────────────── */
static void shell_run(ShellState *s) {
    boot_sequence(s);

    while (true) {
        if (!s->logged_in) {
            if (!show_login(s)) continue;
        }

        s->ctx->quit = false;
        ShellMode launched = s->mode;

        if (launched == MODE_TERMINAL)
            terminal_loop(s);
        else
            wm_run(s->wm, s);

        if (!s->logged_in) {
            s->ctx->quit = false;
            continue;
        }

        if (launched == MODE_TERMINAL && s->mode == MODE_WM) {
            s->ctx->quit = false;
            continue;
        }

        if (launched == MODE_WM) {
            s->mode = MODE_TERMINAL;
            s->ctx->quit = false;
            continue;
        }

        break;
    }
}

int main(void) {
    setlocale(LC_ALL, "");

    /* Ignore child signals so play_chime() children don't zombie */
    signal(SIGCHLD, SIG_IGN);

    ShellState s = {0};
    strcpy(s.cwd, "/");
    s.accent   = MOSS_COLOR_GREEN;
    s.crt_mode = MOSS_CRT_LIGHT;
    s.mode     = MODE_TERMINAL;
    s.bg_enabled = true;
    s.font_size = 16;

    s.ctx = moss_init();
    if (!s.ctx) {
        /* ncurses init failed: TERM not set or terminfo missing.
         * Write directly to the TTY so the message is visible on the screen
         * even though ncurses never started. */
        const char *msg =
            "\r\n[mOSs] FATAL: ncurses init failed.\r\n"
            "  Check that TERM=linux is exported in /init\r\n"
            "  and /usr/share/terminfo/l/linux exists in the rootfs.\r\n";
        write(STDERR_FILENO, msg, strlen(msg));
        return 1;
    }

    shell_init_palette();

    s.cfg      = moss_config_load();
    s.art      = moss_ascii_load(s.cfg);
    s.renderer = renderer_create(s.ctx);
    s.wm       = wm_create(s.ctx, &s);

    vfs_init(&s);

    /* Read startup preferences from config */
    if (s.cfg) {
        const char *sm = moss_config_get_str(s.cfg, "startup_mode", "terminal");
        s.mode = strcmp(sm,"wm")==0 ? MODE_WM : MODE_TERMINAL;
        const char *crt = moss_config_get_str(s.cfg, "crt_effect", "light");
        if (!strcmp(crt,"off"))    s.crt_mode = MOSS_CRT_OFF;
        if (!strcmp(crt,"strong")) s.crt_mode = MOSS_CRT_STRONG;
        moss_set_crt_mode(s.ctx, s.crt_mode);
    }

    shell_run(&s);

    wm_destroy(s.wm);
    renderer_destroy(s.renderer);
    moss_ascii_destroy(s.art);
    moss_config_free(s.cfg);
    moss_destroy(s.ctx);
    return 0;
}
