#define _GNU_SOURCE
#include "moss.h"

#include <curses.h>
#include <panel.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MOSS_BRIDGE_SOCK    "/run/moss-bridge.sock"
#define MOSS_CONFIG_PATH    "/etc/moss/config.json"
#define MOSS_ASCII_PATH     "/usr/share/moss/ascii/ascii_art.json"
#define MOSS_MAX_WINDOWS    64
#define MOSS_TERM_LINES     500
#define MOSS_TERM_HISTORY   128
#define MOSS_INPUT_BUF      4096
#define MOSS_JSON_MAX       (256 * 1024)

typedef struct {
    char key[128];
    char *value;
} KVPair;

struct MOSS_Config {
    char path[256];
    KVPair *pairs;
    int count;
};

struct MOSS_AsciiArt {
    KVPair *pairs;
    int count;
};

struct MOSS_SysInfo {
    char     hostname[64];
    char     kernel[128];
    char     cpu_model[128];
    uint64_t uptime_s;
    int      cpu_pct;
    int      cpu_temp_c;
    int      cpu_freq_mhz;
    uint64_t mem_total_kb;
    uint64_t mem_used_kb;
    int      mem_pct;
    uint64_t disk_total_mb;
    uint64_t disk_used_mb;
    int      disk_pct;
};

struct MOSS_Terminal {
    MOSS_Window     *win;
    char           **lines;
    int              line_count;
    int              scroll_top;
    char             prompt[128];
    char             input_buf[MOSS_INPUT_BUF];
    int              input_len;
    int              cursor_pos;
    char           **history;
    int              history_len;
    int              history_idx;
    MOSS_InputCb     input_cb;
    void            *input_ud;
    MOSS_CompleteCb  complete_cb;
    void            *complete_ud;
};

static size_t moss_strlcat(char *dst, const char *src, size_t sz) {
    size_t dlen = strlen(dst);
    size_t slen = strlen(src);
    if (dlen < sz) {
        size_t n = MOSS_MIN(slen, sz - dlen - 1);
        memcpy(dst + dlen, src, n);
        dst[dlen + n] = '\0';
    }
    return dlen + slen;
}

static int color_to_pair(MOSS_Color fg, MOSS_Color bg) {
    (void)bg;
    switch (fg) {
        case MOSS_COLOR_GREEN:        return 1;
        case MOSS_COLOR_RED:          return 2;
        case MOSS_COLOR_YELLOW:       return 3;
        case MOSS_COLOR_BLUE:         return 4;
        case MOSS_COLOR_MAGENTA:      return 5;
        case MOSS_COLOR_CYAN:         return 6;
        case MOSS_COLOR_WHITE:        return 7;
        case MOSS_COLOR_DARK_GREEN:   return 8;
        case MOSS_COLOR_BRIGHT_GREEN: return 9;
        case MOSS_COLOR_ORANGE:       return 10;
        default:                      return 1;
    }
}

static void init_color_pairs(void) {
    if (!has_colors()) return;
    start_color();
#ifdef NCURSES_VERSION
    use_default_colors();
#endif
    init_pair(1, COLOR_GREEN,   -1);
    init_pair(2, COLOR_RED,     -1);
    init_pair(3, COLOR_YELLOW,  -1);
    init_pair(4, COLOR_BLUE,    -1);
    init_pair(5, COLOR_MAGENTA, -1);
    init_pair(6, COLOR_CYAN,    -1);
    init_pair(7, COLOR_WHITE,   -1);
    init_pair(8, COLOR_GREEN,   -1);
    init_pair(9, COLOR_GREEN,   -1);
    init_pair(10, COLOR_YELLOW, -1);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    char *buf;
    long len;
    size_t nread;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    if (len > MOSS_JSON_MAX) {
        fclose(f);
        return NULL;
    }
    buf = calloc(1, (size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    nread = fread(buf, 1, (size_t)len, f);
    buf[nread] = '\0';
    fclose(f);
    return buf;
}

static char *json_unescape_dup(const char *src, size_t len) {
    char *out = calloc(1, len + 1);
    size_t i, o = 0;
    if (!out) return NULL;
    for (i = 0; i < len; ++i) {
        if (src[i] == '\\' && i + 1 < len) {
            ++i;
            switch (src[i]) {
                case 'n': out[o++] = '\n'; break;
                case 'r': out[o++] = '\r'; break;
                case 't': out[o++] = '\t'; break;
                case '\\': out[o++] = '\\'; break;
                case '"': out[o++] = '"'; break;
                default: out[o++] = src[i]; break;
            }
        } else {
            out[o++] = src[i];
        }
    }
    out[o] = '\0';
    return out;
}

static int kv_add(KVPair **pairs, int *count, const char *key, const char *value) {
    KVPair *tmp = realloc(*pairs, (size_t)(*count + 1) * sizeof(**pairs));
    if (!tmp) return MOSS_E_NOMEM;
    *pairs = tmp;
    moss_strlcpy((*pairs)[*count].key, key, sizeof((*pairs)[*count].key));
    (*pairs)[*count].value = strdup(value ? value : "");
    if (!(*pairs)[*count].value) return MOSS_E_NOMEM;
    (*count)++;
    return MOSS_OK;
}

static void kv_free(KVPair *pairs, int count) {
    int i;
    for (i = 0; i < count; ++i) free(pairs[i].value);
    free(pairs);
}

static const char *kv_get(const KVPair *pairs, int count, const char *key) {
    int i;
    for (i = 0; i < count; ++i) {
        if (strcmp(pairs[i].key, key) == 0) return pairs[i].value;
    }
    return NULL;
}

static int parse_simple_json_object(const char *src, KVPair **pairs, int *count) {
    const char *p = src;
    *pairs = NULL;
    *count = 0;

    while (*p) {
        const char *k0, *k1, *v0, *v1;
        char key[128];
        char *val;
        while (*p && *p != '"') ++p;
        if (!*p) break;
        k0 = ++p;
        while (*p && (*p != '"' || (*(p - 1) == '\\' && p > k0))) ++p;
        if (!*p) break;
        k1 = p;
        if ((size_t)(k1 - k0) >= sizeof(key)) return MOSS_E_IO;
        memcpy(key, k0, (size_t)(k1 - k0));
        key[k1 - k0] = '\0';
        ++p;
        while (*p && *p != ':') ++p;
        if (!*p) break;
        ++p;
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        if (*p == '"') {
            v0 = ++p;
            while (*p && (*p != '"' || (*(p - 1) == '\\' && p > v0))) ++p;
            if (!*p) break;
            v1 = p;
            val = json_unescape_dup(v0, (size_t)(v1 - v0));
            if (!val) return MOSS_E_NOMEM;
            if (kv_add(pairs, count, key, val) != MOSS_OK) {
                free(val);
                return MOSS_E_NOMEM;
            }
            free(val);
            ++p;
        } else {
            v0 = p;
            while (*p && *p != ',' && *p != '}' && *p != '\n' && *p != '\r') ++p;
            v1 = p;
            while (v1 > v0 && isspace((unsigned char)*(v1 - 1))) --v1;
            val = strndup(v0, (size_t)(v1 - v0));
            if (!val) return MOSS_E_NOMEM;
            if (kv_add(pairs, count, key, val) != MOSS_OK) {
                free(val);
                return MOSS_E_NOMEM;
            }
            free(val);
        }
    }

    return MOSS_OK;
}

static char *bridge_request(const char *req) {
    int fd;
    struct sockaddr_un addr;
    char *buf;
    ssize_t n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    moss_strlcpy(addr.sun_path, MOSS_BRIDGE_SOCK, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }
    if (send(fd, req, strlen(req), 0) < 0) {
        close(fd);
        return NULL;
    }
    buf = calloc(1, 65536);
    if (!buf) {
        close(fd);
        return NULL;
    }
    n = recv(fd, buf, 65535, 0);
    close(fd);
    if (n <= 0) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

static int json_find_string(const char *json, const char *key, char *out, size_t out_sz) {
    char needle[128];
    const char *p;
    const char *s;
    if (!json || !key || !out || out_sz == 0) return 0;
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    s = p;
    while (*p && (*p != '"' || (*(p - 1) == '\\' && p > s))) ++p;
    if (!*p) return 0;
    {
        char *tmp = json_unescape_dup(s, (size_t)(p - s));
        if (!tmp) return 0;
        moss_strlcpy(out, tmp, out_sz);
        free(tmp);
    }
    return 1;
}

static long long json_find_int(const char *json, const char *key) {
    char needle[128];
    const char *p;
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p && isspace((unsigned char)*p)) ++p;
    return atoll(p);
}

static void fill_sysinfo_from_proc(MOSS_SysInfo *si) {
    FILE *f;
    char line[256];

    f = fopen("/proc/sys/kernel/hostname", "r");
    if (f) {
        if (fgets(si->hostname, sizeof(si->hostname), f))
            si->hostname[strcspn(si->hostname, "\n")] = '\0';
        fclose(f);
    }

    f = fopen("/proc/version", "r");
    if (f) {
        if (fgets(si->kernel, sizeof(si->kernel), f))
            si->kernel[strcspn(si->kernel, "\n")] = '\0';
        fclose(f);
    }

    f = fopen("/proc/uptime", "r");
    if (f) {
        double up = 0.0;
        if (fscanf(f, "%lf", &up) == 1) si->uptime_s = (uint64_t)up;
        fclose(f);
    }

    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    ++colon;
                    while (*colon == ' ') ++colon;
                    moss_strlcpy(si->cpu_model, colon, sizeof(si->cpu_model));
                    si->cpu_model[strcspn(si->cpu_model, "\n")] = '\0';
                    break;
                }
            }
        }
        fclose(f);
    }

    f = fopen("/proc/meminfo", "r");
    if (f) {
        uint64_t total = 0, avail = 0;
        while (fgets(line, sizeof(line), f)) {
            unsigned long long v;
            if (sscanf(line, "MemTotal: %llu kB", &v) == 1) total = (uint64_t)v;
            else if (sscanf(line, "MemAvailable: %llu kB", &v) == 1) avail = (uint64_t)v;
        }
        fclose(f);
        si->mem_total_kb = total;
        si->mem_used_kb = (avail <= total) ? (total - avail) : 0;
        si->mem_pct = total ? (int)((si->mem_used_kb * 100ULL) / total) : 0;
    }

    f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (f) {
        int temp = 0;
        if (fscanf(f, "%d", &temp) == 1) si->cpu_temp_c = temp / 1000;
        fclose(f);
    } else {
        si->cpu_temp_c = -1;
    }

    f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (f) {
        int freq = 0;
        if (fscanf(f, "%d", &freq) == 1) si->cpu_freq_mhz = freq / 1000;
        fclose(f);
    } else {
        si->cpu_freq_mhz = -1;
    }

    {
        struct statvfs st;
        if (statvfs("/", &st) == 0 && st.f_frsize) {
            uint64_t total = (uint64_t)st.f_blocks * (uint64_t)st.f_frsize;
            uint64_t avail = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
            uint64_t used = total >= avail ? total - avail : 0;
            si->disk_total_mb = total / (1024ULL * 1024ULL);
            si->disk_used_mb = used / (1024ULL * 1024ULL);
            si->disk_pct = total ? (int)((used * 100ULL) / total) : 0;
        }
    }
}

MOSS_Ctx *moss_init(void) {
    MOSS_Ctx *ctx;
    SCREEN *scr;

    scr = newterm(NULL, stdout, stdin);
    if (!scr) return NULL;
    set_term(scr);
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, FALSE);
    curs_set(0);
    init_color_pairs();

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        endwin();
        delscreen(scr);
        return NULL;
    }

    getmaxyx(stdscr, ctx->rows, ctx->cols);
    ctx->crt_mode = MOSS_CRT_LIGHT;
    ctx->accent = MOSS_COLOR_GREEN;
    ctx->bridge_fd = -1;
    ctx->scr = scr;
    clock_gettime(CLOCK_MONOTONIC, &ctx->last_flicker);
    return ctx;
}

void moss_destroy(MOSS_Ctx *ctx) {
    int i;
    if (!ctx) return;
    while (ctx->win_count > 0) moss_window_destroy(ctx->windows[0]);
    if (ctx->bridge_fd >= 0) close(ctx->bridge_fd);
    moss_config_free(ctx->config);
    endwin();
    if (ctx->scr) delscreen(ctx->scr);
    free(ctx);
}

void moss_set_event_callback(MOSS_Ctx *ctx, MOSS_EventCb cb, void *userdata) {
    if (!ctx) return;
    ctx->event_cb = cb;
    ctx->event_ud = userdata;
}

void moss_post_quit(MOSS_Ctx *ctx) {
    if (ctx) ctx->quit = true;
}

void moss_get_dims(const MOSS_Ctx *ctx, int *rows, int *cols) {
    if (!ctx) return;
    if (rows) *rows = ctx->rows;
    if (cols) *cols = ctx->cols;
}

void moss_set_crt_mode(MOSS_Ctx *ctx, MOSS_CrtMode mode) {
    if (ctx) ctx->crt_mode = mode;
}

void moss_set_terminal_color(MOSS_Ctx *ctx, MOSS_Color color) {
    if (!ctx) return;
    ctx->accent = color;
}

void moss_render_frame(MOSS_Ctx *ctx) {
    if (!ctx) return;
    getmaxyx(stdscr, ctx->rows, ctx->cols);
    update_panels();
    doupdate();
}

void moss_event_loop(MOSS_Ctx *ctx) {
    if (!ctx) return;
    timeout(100);
    while (!ctx->quit) {
        int ch = getch();
        MOSS_Event ev;
        int nr, nc;

        memset(&ev, 0, sizeof(ev));
        getmaxyx(stdscr, nr, nc);
        if (nr != ctx->rows || nc != ctx->cols) {
            ctx->rows = nr;
            ctx->cols = nc;
            resizeterm(nr, nc);
            ev.type = MOSS_EVENT_RESIZE;
            ev.resize.rows = nr;
            ev.resize.cols = nc;
            if (ctx->event_cb) ctx->event_cb(ctx, &ev, ctx->event_ud);
        }

        if (ch == ERR) {
            moss_render_frame(ctx);
            continue;
        }
        if (ch == MOSS_KEY_ESC || ch == MOSS_KEY_CTRL('q')) {
            ctx->quit = true;
            break;
        }
        ev.type = MOSS_EVENT_KEY;
        ev.key.key = ch;
        if (ctx->event_cb) ctx->event_cb(ctx, &ev, ctx->event_ud);
        moss_render_frame(ctx);
    }
}

static void draw_border(MOSS_Window *win) {
    int cp;
    char tb[80];
    int title_col;
    if (!win || !win->border) return;
    cp = color_to_pair(win->focused ? win->ctx->accent : win->fg, win->bg);
    wattron(win->border, COLOR_PAIR(cp));
    box(win->border, 0, 0);
    if (win->title[0]) {
        snprintf(tb, sizeof(tb), "[ %s ]", win->title);
        title_col = MOSS_MAX(1, (win->w - (int)strlen(tb)) / 2);
        mvwprintw(win->border, 0, title_col, "%s", tb);
    }
    wattroff(win->border, COLOR_PAIR(cp));
    wnoutrefresh(win->border);
    wnoutrefresh(win->ncwin);
}

MOSS_Window *moss_create_window(MOSS_Ctx *ctx, const char *title, int w, int h) {
    MOSS_Window *win;
    int rows, cols;
    if (!ctx || ctx->win_count >= MOSS_MAX_WINDOWS) return NULL;
    getmaxyx(stdscr, rows, cols);
    if (w <= 0) w = cols;
    if (h <= 0) h = rows;
    w = MOSS_CLAMP(w, 4, cols);
    h = MOSS_CLAMP(h, 3, rows);

    win = calloc(1, sizeof(*win));
    if (!win) return NULL;
    win->ctx = ctx;
    win->w = w;
    win->h = h;
    win->x = MOSS_MAX(0, (cols - w) / 2);
    win->y = MOSS_MAX(0, (rows - h) / 2);
    win->fg = MOSS_COLOR_GREEN;
    win->bg = MOSS_COLOR_BLACK;
    moss_strlcpy(win->title, title ? title : "", sizeof(win->title));

    win->border = newwin(h, w, win->y, win->x);
    if (!win->border) { free(win); return NULL; }
    win->ncwin = derwin(win->border, h - 2, w - 2, 1, 1);
    if (!win->ncwin) {
        delwin(win->border);
        free(win);
        return NULL;
    }
    win->panel = new_panel(win->border);
    if (!win->panel) {
        delwin(win->ncwin);
        delwin(win->border);
        free(win);
        return NULL;
    }
    keypad(win->ncwin, TRUE);
    scrollok(win->ncwin, TRUE);
    hide_panel(win->panel);
    ctx->windows[ctx->win_count++] = win;
    return win;
}

void moss_window_show(MOSS_Window *win) {
    if (!win) return;
    win->visible = true;
    show_panel(win->panel);
    draw_border(win);
    if (win->draw_cb) win->draw_cb(win, win->draw_ud);
}

void moss_window_hide(MOSS_Window *win) {
    if (!win) return;
    win->visible = false;
    hide_panel(win->panel);
}

void moss_window_focus(MOSS_Window *win) {
    if (!win || !win->ctx) return;
    if (win->ctx->focused) {
        win->ctx->focused->focused = false;
        draw_border(win->ctx->focused);
    }
    win->focused = true;
    win->ctx->focused = win;
    top_panel(win->panel);
    draw_border(win);
}

int moss_window_move(MOSS_Window *win, int row, int col) {
    int max_row, max_col;
    if (!win) return MOSS_E_BADARG;
    getmaxyx(stdscr, max_row, max_col);
    win->y = MOSS_CLAMP(row, 0, MOSS_MAX(0, max_row - win->h));
    win->x = MOSS_CLAMP(col, 0, MOSS_MAX(0, max_col - win->w));
    move_panel(win->panel, win->y, win->x);
    return MOSS_OK;
}

int moss_window_resize(MOSS_Window *win, int nw, int nh) {
    if (!win) return MOSS_E_BADARG;
    nw = MOSS_MAX(4, nw);
    nh = MOSS_MAX(3, nh);
    win->w = nw;
    win->h = nh;
    wresize(win->border, nh, nw);
    wresize(win->ncwin, nh - 2, nw - 2);
    replace_panel(win->panel, win->border);
    draw_border(win);
    return MOSS_OK;
}

void moss_window_set_draw_callback(MOSS_Window *win, MOSS_DrawCb cb, void *ud) {
    if (!win) return;
    win->draw_cb = cb;
    win->draw_ud = ud;
}

void moss_window_destroy(MOSS_Window *win) {
    int i;
    MOSS_Ctx *ctx;
    if (!win) return;
    ctx = win->ctx;
    if (ctx) {
        for (i = 0; i < ctx->win_count; ++i) {
            if (ctx->windows[i] == win) {
                memmove(&ctx->windows[i], &ctx->windows[i + 1],
                        (size_t)(ctx->win_count - i - 1) * sizeof(ctx->windows[0]));
                ctx->win_count--;
                break;
            }
        }
        if (ctx->focused == win) ctx->focused = NULL;
    }
    if (win->panel) del_panel(win->panel);
    if (win->ncwin) delwin(win->ncwin);
    if (win->border) delwin(win->border);
    free(win);
}

int moss_print(MOSS_Window *win, int row, int col, const char *text) {
    int h, w;
    if (!win || !text) return MOSS_E_BADARG;
    getmaxyx(win->ncwin, h, w);
    if (row < 0 || col < 0 || row >= h || col >= w) return MOSS_E_BOUNDS;
    mvwaddnstr(win->ncwin, row, col, text, w - col);
    return MOSS_OK;
}

int moss_printf(MOSS_Window *win, int row, int col, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return moss_print(win, row, col, buf);
}

void moss_set_color(MOSS_Window *win, MOSS_Color fg, MOSS_Color bg) {
    if (!win) return;
    win->fg = fg;
    win->bg = bg;
    wattron(win->ncwin, COLOR_PAIR(color_to_pair(fg, bg)));
}

void moss_set_attr(MOSS_Window *win, MOSS_Attr attr) {
    int bits = A_NORMAL;
    if (!win) return;
    if (attr & MOSS_ATTR_BOLD) bits |= A_BOLD;
    if (attr & MOSS_ATTR_DIM) bits |= A_DIM;
    if (attr & MOSS_ATTR_UNDERLINE) bits |= A_UNDERLINE;
    if (attr & MOSS_ATTR_BLINK) bits |= A_BLINK;
    if (attr & MOSS_ATTR_REVERSE) bits |= A_REVERSE;
    wattrset(win->ncwin, bits | COLOR_PAIR(color_to_pair(win->fg, win->bg)));
    win->attr = attr;
}

void moss_clear_window(MOSS_Window *win) {
    if (!win) return;
    werase(win->ncwin);
    draw_border(win);
}

int moss_draw_hline(MOSS_Window *win, int row, int col, int len, chtype ch) {
    if (!win) return MOSS_E_BADARG;
    if (!ch) ch = ACS_HLINE;
    mvwhline(win->ncwin, row, col, ch, len);
    return MOSS_OK;
}

int moss_draw_vline(MOSS_Window *win, int row, int col, int len, chtype ch) {
    if (!win) return MOSS_E_BADARG;
    if (!ch) ch = ACS_VLINE;
    mvwvline(win->ncwin, row, col, ch, len);
    return MOSS_OK;
}

int moss_draw_box(MOSS_Window *win, int row, int col, int w, int h) {
    if (!win) return MOSS_E_BADARG;
    if (w < 2 || h < 2) return MOSS_E_BADARG;
    mvwaddch(win->ncwin, row, col, ACS_ULCORNER);
    mvwaddch(win->ncwin, row, col + w - 1, ACS_URCORNER);
    mvwaddch(win->ncwin, row + h - 1, col, ACS_LLCORNER);
    mvwaddch(win->ncwin, row + h - 1, col + w - 1, ACS_LRCORNER);
    mvwhline(win->ncwin, row, col + 1, ACS_HLINE, w - 2);
    mvwhline(win->ncwin, row + h - 1, col + 1, ACS_HLINE, w - 2);
    mvwvline(win->ncwin, row + 1, col, ACS_VLINE, h - 2);
    mvwvline(win->ncwin, row + 1, col + w - 1, ACS_VLINE, h - 2);
    return MOSS_OK;
}

int moss_draw_progress_bar(MOSS_Window *win, int row, int col, int width, int pct, MOSS_Color color) {
    int i, fill;
    if (!win || width < 3) return MOSS_E_BADARG;
    pct = MOSS_CLAMP(pct, 0, 100);
    fill = (width - 2) * pct / 100;
    moss_set_color(win, color, MOSS_COLOR_BLACK);
    mvwaddch(win->ncwin, row, col, '[');
    for (i = 0; i < width - 2; ++i) {
        mvwaddch(win->ncwin, row, col + 1 + i, i < fill ? '=' : ' ');
    }
    mvwaddch(win->ncwin, row, col + width - 1, ']');
    return MOSS_OK;
}

MOSS_Config *moss_config_load_path(const char *path) {
    char *src;
    MOSS_Config *cfg;
    int rc;
    if (!path) return NULL;
    src = read_file(path);
    if (!src) return NULL;
    cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        free(src);
        return NULL;
    }
    moss_strlcpy(cfg->path, path, sizeof(cfg->path));
    rc = parse_simple_json_object(src, &cfg->pairs, &cfg->count);
    free(src);
    if (rc != MOSS_OK) {
        moss_config_free(cfg);
        return NULL;
    }
    return cfg;
}

MOSS_Config *moss_config_load(void) {
    return moss_config_load_path(MOSS_CONFIG_PATH);
}

const char *moss_config_get_str(const MOSS_Config *cfg, const char *key, const char *default_val) {
    const char *v;
    if (!cfg || !key) return default_val;
    v = kv_get(cfg->pairs, cfg->count, key);
    return v ? v : default_val;
}

int moss_config_get_int(const MOSS_Config *cfg, const char *key, int default_val) {
    const char *v = moss_config_get_str(cfg, key, NULL);
    return v ? atoi(v) : default_val;
}

bool moss_config_get_bool(const MOSS_Config *cfg, const char *key, bool default_val) {
    const char *v = moss_config_get_str(cfg, key, NULL);
    if (!v) return default_val;
    if (strcmp(v, "true") == 0 || strcmp(v, "1") == 0) return true;
    if (strcmp(v, "false") == 0 || strcmp(v, "0") == 0) return false;
    return default_val;
}

static int config_set_value(MOSS_Config *cfg, const char *key, const char *val) {
    int i;
    if (!cfg || !key || !val) return MOSS_E_BADARG;
    for (i = 0; i < cfg->count; ++i) {
        if (strcmp(cfg->pairs[i].key, key) == 0) {
            char *dup = strdup(val);
            if (!dup) return MOSS_E_NOMEM;
            free(cfg->pairs[i].value);
            cfg->pairs[i].value = dup;
            return MOSS_OK;
        }
    }
    return kv_add(&cfg->pairs, &cfg->count, key, val);
}

int moss_config_set_str(MOSS_Config *cfg, const char *key, const char *val) {
    return config_set_value(cfg, key, val ? val : "");
}

int moss_config_set_int(MOSS_Config *cfg, const char *key, int val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", val);
    return config_set_value(cfg, key, buf);
}

int moss_config_set_bool(MOSS_Config *cfg, const char *key, bool val) {
    return config_set_value(cfg, key, val ? "true" : "false");
}

int moss_config_save(const MOSS_Config *cfg) {
    FILE *f;
    int i;
    if (!cfg || !cfg->path[0]) return MOSS_E_BADARG;
    f = fopen(cfg->path, "w");
    if (!f) return MOSS_E_IO;
    fputs("{\n", f);
    for (i = 0; i < cfg->count; ++i) {
        fprintf(f, "  \"%s\": ", cfg->pairs[i].key);
        if (strcmp(cfg->pairs[i].value, "true") == 0 || strcmp(cfg->pairs[i].value, "false") == 0 ||
            strspn(cfg->pairs[i].value, "-0123456789") == strlen(cfg->pairs[i].value)) {
            fprintf(f, "%s", cfg->pairs[i].value);
        } else {
            fputc('"', f);
            for (const char *p = cfg->pairs[i].value; *p; ++p) {
                if (*p == '"' || *p == '\\') fputc('\\', f);
                if (*p == '\n') { fputs("\\n", f); continue; }
                fputc(*p, f);
            }
            fputc('"', f);
        }
        fputs(i + 1 == cfg->count ? "\n" : ",\n", f);
    }
    fputs("}\n", f);
    fclose(f);
    return MOSS_OK;
}

void moss_config_free(MOSS_Config *cfg) {
    if (!cfg) return;
    kv_free(cfg->pairs, cfg->count);
    free(cfg);
}

MOSS_AsciiArt *moss_ascii_load(const MOSS_Config *cfg) {
    const char *path = cfg ? moss_config_get_str(cfg, "ascii_path", MOSS_ASCII_PATH) : MOSS_ASCII_PATH;
    char *src = read_file(path);
    MOSS_AsciiArt *art;
    int rc;
    if (!src) return NULL;
    art = calloc(1, sizeof(*art));
    if (!art) {
        free(src);
        return NULL;
    }
    rc = parse_simple_json_object(src, &art->pairs, &art->count);
    free(src);
    if (rc != MOSS_OK) {
        moss_ascii_destroy(art);
        return NULL;
    }
    return art;
}

const char *moss_ascii_get(const MOSS_AsciiArt *art, const char *name) {
    if (!art || !name) return NULL;
    return kv_get(art->pairs, art->count, name);
}

int moss_draw_ascii(MOSS_Window *win, const MOSS_AsciiArt *art, const char *name, int row, int col, MOSS_Color color) {
    const char *text = moss_ascii_get(art, name);
    const char *p = text;
    int r = row;
    if (!text) return MOSS_E_NOTFOUND;
    moss_set_color(win, color, MOSS_COLOR_BLACK);
    while (*p) {
        const char *nl = strchr(p, '\n');
        if (!nl) {
            moss_print(win, r, col, p);
            break;
        }
        {
            char line[512];
            size_t len = (size_t)(nl - p);
            if (len >= sizeof(line)) len = sizeof(line) - 1;
            memcpy(line, p, len);
            line[len] = '\0';
            moss_print(win, r++, col, line);
        }
        p = nl + 1;
    }
    return MOSS_OK;
}

void moss_ascii_destroy(MOSS_AsciiArt *art) {
    if (!art) return;
    kv_free(art->pairs, art->count);
    free(art);
}

MOSS_SysInfo *moss_sysinfo_get(void) {
    MOSS_SysInfo *si = calloc(1, sizeof(*si));
    char *json;
    if (!si) return NULL;
    fill_sysinfo_from_proc(si);
    json = bridge_request("{\"cmd\":\"system\"}\n");
    if (!json) return si;

    json_find_string(json, "hostname", si->hostname, sizeof(si->hostname));
    json_find_string(json, "kernel", si->kernel, sizeof(si->kernel));
    if (strstr(json, "\"cpu\":{")) {
        const char *cpu = strstr(json, "\"cpu\":{");
        if (cpu) {
            json_find_string(cpu, "model", si->cpu_model, sizeof(si->cpu_model));
            si->cpu_pct = (int)json_find_int(cpu, "pct");
            si->cpu_temp_c = (int)json_find_int(cpu, "temp");
            si->cpu_freq_mhz = (int)json_find_int(cpu, "freq");
        }
    }
    if (strstr(json, "\"mem\":{")) {
        const char *mem = strstr(json, "\"mem\":{");
        if (mem) {
            si->mem_total_kb = (uint64_t)json_find_int(mem, "total");
            si->mem_used_kb = (uint64_t)json_find_int(mem, "used");
            si->mem_pct = (int)json_find_int(mem, "pct");
        }
    }
    if (strstr(json, "\"disk\":{")) {
        const char *disk = strstr(json, "\"disk\":{");
        if (disk) {
            si->disk_total_mb = (uint64_t)json_find_int(disk, "total");
            si->disk_used_mb = (uint64_t)json_find_int(disk, "used");
            si->disk_pct = (int)json_find_int(disk, "pct");
        }
    }
    si->uptime_s = (uint64_t)json_find_int(json, "uptime");

    free(json);
    return si;
}

uint64_t moss_sysinfo_subscribe(MOSS_Ctx *ctx, uint32_t interval_ms, MOSS_EventCb cb, void *userdata) {
    (void)ctx; (void)interval_ms; (void)cb; (void)userdata;
    return 0;
}

void moss_sysinfo_unsubscribe(MOSS_Ctx *ctx, uint64_t sub_id) {
    (void)ctx; (void)sub_id;
}

const char *moss_sysinfo_hostname(const MOSS_SysInfo *si) { return si ? si->hostname : ""; }
const char *moss_sysinfo_kernel(const MOSS_SysInfo *si) { return si ? si->kernel : ""; }
const char *moss_sysinfo_cpu_model(const MOSS_SysInfo *si) { return si ? si->cpu_model : ""; }
uint64_t moss_sysinfo_uptime_s(const MOSS_SysInfo *si) { return si ? si->uptime_s : 0; }
int moss_sysinfo_cpu_pct(const MOSS_SysInfo *si) { return si ? si->cpu_pct : 0; }
int moss_sysinfo_cpu_temp_c(const MOSS_SysInfo *si) { return si ? si->cpu_temp_c : -1; }
int moss_sysinfo_cpu_freq_mhz(const MOSS_SysInfo *si) { return si ? si->cpu_freq_mhz : -1; }
uint64_t moss_sysinfo_mem_total_kb(const MOSS_SysInfo *si) { return si ? si->mem_total_kb : 0; }
uint64_t moss_sysinfo_mem_used_kb(const MOSS_SysInfo *si) { return si ? si->mem_used_kb : 0; }
int moss_sysinfo_mem_pct(const MOSS_SysInfo *si) { return si ? si->mem_pct : 0; }
uint64_t moss_sysinfo_disk_total_mb(const MOSS_SysInfo *si) { return si ? si->disk_total_mb : 0; }
uint64_t moss_sysinfo_disk_used_mb(const MOSS_SysInfo *si) { return si ? si->disk_used_mb : 0; }
int moss_sysinfo_disk_pct(const MOSS_SysInfo *si) { return si ? si->disk_pct : 0; }
void moss_sysinfo_free(MOSS_SysInfo *si) { free(si); }

MOSS_Terminal *moss_create_terminal(MOSS_Window *win) {
    MOSS_Terminal *term = calloc(1, sizeof(*term));
    if (!term) return NULL;
    term->win = win;
    moss_strlcpy(term->prompt, "moss@mOSs:~$ ", sizeof(term->prompt));
    return term;
}

int moss_terminal_write(MOSS_Terminal *term, const char *text) {
    if (!term || !text) return MOSS_E_BADARG;
    if (term->win) return moss_print(term->win, 0, 0, text);
    return MOSS_OK;
}

int moss_terminal_writef(MOSS_Terminal *term, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return moss_terminal_write(term, buf);
}

void moss_terminal_set_prompt(MOSS_Terminal *term, const char *prompt) {
    if (term && prompt) moss_strlcpy(term->prompt, prompt, sizeof(term->prompt));
}

void moss_terminal_set_input_cb(MOSS_Terminal *term, MOSS_InputCb cb, void *ud) {
    if (!term) return;
    term->input_cb = cb;
    term->input_ud = ud;
}

void moss_terminal_set_complete_cb(MOSS_Terminal *term, MOSS_CompleteCb cb, void *ud) {
    if (!term) return;
    term->complete_cb = cb;
    term->complete_ud = ud;
}

void moss_terminal_destroy(MOSS_Terminal *term) {
    int i;
    if (!term) return;
    for (i = 0; i < term->line_count; ++i) free(term->lines[i]);
    free(term->lines);
    for (i = 0; i < term->history_len; ++i) free(term->history[i]);
    free(term->history);
    free(term);
}

int moss_scores_get(const char *game, int n, MOSS_Score *out) {
    (void)game; (void)n; (void)out;
    return 0;
}

int moss_scores_submit(const char *game, const char *username, int64_t score) {
    (void)game; (void)username; (void)score;
    return MOSS_OK;
}

void moss_sleep_ms(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000U;
    ts.tv_nsec = (long)(ms % 1000U) * 1000000L;
    nanosleep(&ts, NULL);
}

char *moss_uptime_str(uint64_t seconds, char *buf, size_t buflen) {
    uint64_t d = seconds / 86400ULL;
    uint64_t h = (seconds % 86400ULL) / 3600ULL;
    uint64_t m = (seconds % 3600ULL) / 60ULL;
    uint64_t s = seconds % 60ULL;
    if (!buf || buflen == 0) return buf;
    if (d) snprintf(buf, buflen, "%llud %lluh %llum", (unsigned long long)d, (unsigned long long)h, (unsigned long long)m);
    else snprintf(buf, buflen, "%lluh %llum %llus", (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
    return buf;
}

char *moss_format_bytes(uint64_t bytes, char *buf, size_t buflen) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = (double)bytes;
    int unit = 0;
    if (!buf || buflen == 0) return buf;
    while (v >= 1024.0 && unit < 4) {
        v /= 1024.0;
        ++unit;
    }
    snprintf(buf, buflen, unit == 0 ? "%.0f %s" : "%.2f %s", v, units[unit]);
    return buf;
}

size_t moss_strlcpy(char *dst, const char *src, size_t size) {
    size_t slen = src ? strlen(src) : 0;
    if (size > 0 && dst) {
        size_t n = MOSS_MIN(slen, size - 1);
        if (src && n) memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return slen;
}

void moss_set_custom_color(MOSS_Ctx *ctx, short pair_id, short fg, short bg) {
    (void)ctx;
    init_pair(pair_id, fg, bg);
}

const char *moss_err_str(int err) {
    switch (err) {
        case MOSS_OK: return "OK";
        case MOSS_E_NOMEM: return "Out of memory";
        case MOSS_E_BOUNDS: return "Out of bounds";
        case MOSS_E_NOTERM: return "Terminal not initialised";
        case MOSS_E_IO: return "I/O error";
        case MOSS_E_NOTFOUND: return "Resource not found";
        case MOSS_E_BADARG: return "Bad argument";
        case MOSS_E_PERM: return "Permission denied";
        case MOSS_E_BRIDGE: return "Bridge unreachable";
        default: return "Unknown error";
    }
}
