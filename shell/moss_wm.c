#define _GNU_SOURCE
#include <curses.h>
#include <panel.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/select.h>
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
#include "moss_wm.h"
#include "moss_renderer.h"
#include "moss_shell.h"

typedef struct ShellState ShellState;

#define WM_MAX_WINDOWS 16
#define FILE_MAX_ENTRIES 128
#define NOTES_CAP 8192
#define MOUSE_MAX_FDS 32

typedef enum {
    APP_TERMINAL, APP_SYSTEMINFO, APP_TASKMANAGER, APP_FILEMANAGER,
    APP_NOTES, APP_CALCULATOR, APP_CLOCK, APP_COLORPICKER,
    APP_BROWSER, APP_SETTINGS, APP_GAMES, APP_AMA,
    APP_NETWORKINFO, APP_PROCESSMONITOR,
} AppID;

typedef struct {
    AppID id;
    const char *label;
    int hotkey;
} LauncherItem;

typedef struct {
    MOSS_Window *win;
    AppID        app_id;
    bool         active;
    bool         maximized;
    int          restore_x, restore_y, restore_w, restore_h;
    int          z;
    char         title[64];
    struct {
        char path[256];
        char entries[FILE_MAX_ENTRIES][128];
        unsigned char is_dir[FILE_MAX_ENTRIES];
        int  count;
        int  sel;
    } files;
    struct {
        char buf[NOTES_CAP];
        int  len;
    } notes;
    struct {
        char expr[128];
        char result[128];
    } calc;
    int list_sel;
} WMApp;

struct MossWM {
    MOSS_Ctx     *ctx;
    ShellState   *shell;
    WMApp         apps[WM_MAX_WINDOWS];
    int           focused_app;
    bool          running;
    WINDOW       *taskbar;
    WINDOW       *launcher;
    bool          launcher_open;
    int           launcher_sel;
    bool          help_open;
    MossRenderer *renderer;
    int           cascade_x;
    int           cascade_y;
    int           z_counter;

    pthread_t     mouse_thread;
    bool          mouse_thread_running;
    bool          mouse_thread_stop;
    int           mouse_dx;
    int           mouse_dy;
    int           mouse_wheel;
    bool          mouse_left_down;
    bool          mouse_left_prev;
    int           mouse_x;
    int           mouse_y;

    bool          dragging;
    int           drag_app;
    int           drag_off_x;
    int           drag_off_y;
};

static const LauncherItem LAUNCHER[] = {
    { APP_TERMINAL,       "Terminal",        't' },
    { APP_SYSTEMINFO,     "System Info",     's' },
    { APP_TASKMANAGER,    "Task Manager",    'p' },
    { APP_FILEMANAGER,    "Files",           'f' },
    { APP_NOTES,          "Notes",           'n' },
    { APP_CALCULATOR,     "Calculator",      'c' },
    { APP_CLOCK,          "Clock",           'k' },
    { APP_COLORPICKER,    "Color Picker",    'o' },
    { APP_BROWSER,        "Browser",         'b' },
    { APP_SETTINGS,       "Settings",        'e' },
    { APP_GAMES,          "Games",           'g' },
    { APP_NETWORKINFO,    "Network",         'w' },
    { APP_PROCESSMONITOR, "Processes",       'r' },
    { APP_AMA,            "AMA",             'a' },
};
#define NUM_LAUNCHER ((int)(sizeof LAUNCHER / sizeof LAUNCHER[0]))

static const char *app_title(AppID id) {
    for (int i = 0; i < NUM_LAUNCHER; i++)
        if (LAUNCHER[i].id == id) return LAUNCHER[i].label;
    return "App";
}

static int active_count(MossWM *wm) {
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (wm->apps[i].active) n++;
    return n;
}

static void wm_focus(MossWM *wm, int idx) {
    if (idx < 0 || idx >= WM_MAX_WINDOWS || !wm->apps[idx].active) return;
    wm->focused_app = idx;
    wm->apps[idx].z = ++wm->z_counter;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (wm->apps[i].active && wm->apps[i].win)
            wm->apps[i].win->focused = false;
    }
    if (wm->apps[idx].win) moss_window_focus(wm->apps[idx].win);
}

static int topmost_app_at(MossWM *wm, int x, int y) {
    int best = -1;
    int best_z = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!wm->apps[i].active || !wm->apps[i].win || !wm->apps[i].win->visible) continue;
        MOSS_Window *w = wm->apps[i].win;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) {
            if (wm->apps[i].z >= best_z) { best_z = wm->apps[i].z; best = i; }
        }
    }
    return best;
}

static void close_launcher(MossWM *wm) {
    wm->launcher_open = false;
    if (wm->launcher) { delwin(wm->launcher); wm->launcher = NULL; }
}

static void file_refresh(WMApp *app) {
    DIR *dir = opendir(app->files.path[0] ? app->files.path : "/");
    app->files.count = 0;
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir)) && app->files.count < FILE_MAX_ENTRIES) {
        if (strcmp(de->d_name, ".") == 0) continue;
        strncpy(app->files.entries[app->files.count], de->d_name, sizeof app->files.entries[0] - 1);
        app->files.entries[app->files.count][sizeof app->files.entries[0] - 1] = 0;
        app->files.is_dir[app->files.count] = (de->d_type == DT_DIR);
        app->files.count++;
    }
    closedir(dir);
    if (app->files.sel >= app->files.count) app->files.sel = app->files.count - 1;
    if (app->files.sel < 0) app->files.sel = 0;
}

static double parse_expr_r(const char **s);
static void skip_ws(const char **s) { while (**s && isspace((unsigned char)**s)) (*s)++; }
static double parse_number_r(const char **s) {
    skip_ws(s);
    if (**s == '(') {
        (*s)++;
        double v = parse_expr_r(s);
        skip_ws(s);
        if (**s == ')') (*s)++;
        return v;
    }
    char *end = NULL;
    double v = strtod(*s, &end);
    if (end == *s) return 0.0;
    *s = end;
    return v;
}
static double parse_term_r(const char **s) {
    double v = parse_number_r(s);
    while (1) {
        skip_ws(s);
        if (**s == '*') { (*s)++; v *= parse_number_r(s); }
        else if (**s == '/') { (*s)++; double d = parse_number_r(s); v = (d == 0.0) ? 0.0 : (v / d); }
        else break;
    }
    return v;
}
static double parse_expr_r(const char **s) {
    double v = parse_term_r(s);
    while (1) {
        skip_ws(s);
        if (**s == '+') { (*s)++; v += parse_term_r(s); }
        else if (**s == '-') { (*s)++; v -= parse_term_r(s); }
        else break;
    }
    return v;
}
static int calc_eval(const char *expr, double *out) {
    const char *p = expr;
    *out = parse_expr_r(&p);
    skip_ws(&p);
    return *p == '\0';
}

static void draw_systeminfo(MOSS_Window *win) {
    MOSS_SysInfo *si = moss_sysinfo_get();
    if (!si) { moss_print(win, 1, 2, "System info unavailable."); return; }
    char upbuf[64], total[32], used[32];
    moss_uptime_str(moss_sysinfo_uptime_s(si), upbuf, sizeof upbuf);
    moss_format_bytes(moss_sysinfo_mem_total_kb(si) * 1024ULL, total, sizeof total);
    moss_format_bytes(moss_sysinfo_mem_used_kb(si) * 1024ULL, used, sizeof used);
    moss_set_attr(win, MOSS_ATTR_BOLD);
    moss_printf(win, 1, 2, "System Info");
    moss_set_attr(win, MOSS_ATTR_NORMAL);
    moss_draw_hline(win, 2, 1, win->w - 4, ACS_HLINE);
    moss_printf(win, 3, 2, "Host   : %s", moss_sysinfo_hostname(si));
    moss_printf(win, 4, 2, "Kernel : %s", moss_sysinfo_kernel(si));
    moss_printf(win, 5, 2, "CPU    : %s", moss_sysinfo_cpu_model(si));
    moss_printf(win, 6, 2, "CPU %%  : %d", moss_sysinfo_cpu_pct(si));
    moss_printf(win, 7, 2, "Mem    : %s / %s", used, total);
    moss_printf(win, 8, 2, "Disk %% : %d", moss_sysinfo_disk_pct(si));
    moss_printf(win, 9, 2, "Uptime : %s", upbuf);
    moss_draw_progress_bar(win, 11, 2, win->w - 6, moss_sysinfo_cpu_pct(si), MOSS_COLOR_GREEN);
    moss_draw_progress_bar(win, 12, 2, win->w - 6, moss_sysinfo_mem_pct(si), MOSS_COLOR_CYAN);
    moss_sysinfo_free(si);
}

static void draw_terminal(WMApp *app) {
    MOSS_Window *win = app->win;
    moss_set_attr(win, MOSS_ATTR_BOLD);
    moss_printf(win, 1, 2, "mOSs Desktop Terminal");
    moss_set_attr(win, MOSS_ATTR_NORMAL);
    moss_draw_hline(win, 2, 1, win->w - 4, ACS_HLINE);
    moss_printf(win, 3, 2, "This desktop terminal is a launch pad for the real shell.");
    moss_printf(win, 4, 2, "Press F4 to go back to the full terminal prompt.");
    moss_printf(win, 6, 2, "Mouse: drag title bars, click taskbar buttons, snap to top to maximize.");
    moss_printf(win, 8, 2, "Launcher hotkeys:");
    for (int i = 0; i < NUM_LAUNCHER && 10 + i < win->h - 2; i++)
        moss_printf(win, 10 + i, 4, "%c  %s", LAUNCHER[i].hotkey, LAUNCHER[i].label);
}

static void draw_clock(WMApp *app) {
    MOSS_Window *win = app->win;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char tbuf[32], dbuf[64];
    strftime(tbuf, sizeof tbuf, "%H:%M:%S", tm);
    strftime(dbuf, sizeof dbuf, "%A, %d %B %Y", tm);
    moss_set_attr(win, MOSS_ATTR_BOLD);
    moss_printf(win, 2, MOSS_MAX(1, (win->w - (int)strlen(tbuf)) / 2), "%s", tbuf);
    moss_set_attr(win, MOSS_ATTR_NORMAL);
    moss_printf(win, 4, MOSS_MAX(1, (win->w - (int)strlen(dbuf)) / 2), "%s", dbuf);
}

static void draw_task_or_process(MOSS_Window *win, const char *title) {
    FILE *fp = popen("ps 2>/dev/null", "r");
    moss_set_attr(win, MOSS_ATTR_BOLD);
    moss_printf(win, 1, 2, "%s", title);
    moss_set_attr(win, MOSS_ATTR_NORMAL);
    moss_draw_hline(win, 2, 1, win->w - 4, ACS_HLINE);
    if (!fp) { moss_printf(win, 3, 2, "ps unavailable."); return; }
    char line[256];
    int row = 3;
    while (row < win->h - 2 && fgets(line, sizeof line, fp)) {
        line[strcspn(line, "\r\n")] = 0;
        moss_printf(win, row++, 2, "%.*s", win->w - 4, line);
    }
    pclose(fp);
}

static void draw_files(WMApp *app) {
    MOSS_Window *win = app->win;
    file_refresh(app);
    moss_set_attr(win, MOSS_ATTR_BOLD);
    moss_printf(win, 1, 2, "Files: %s", app->files.path[0] ? app->files.path : "/");
    moss_set_attr(win, MOSS_ATTR_NORMAL);
    moss_draw_hline(win, 2, 1, win->w - 4, ACS_HLINE);
    int split = MOSS_MAX(18, (win->w - 4) / 2);
    for (int i = 0; i < app->files.count && 3 + i < win->h - 2; i++) {
        if (i == app->files.sel) moss_set_attr(win, MOSS_ATTR_REVERSE);
        moss_printf(win, 3 + i, 2, "%c %.*s", app->files.is_dir[i] ? '/' : ' ', split - 4, app->files.entries[i]);
        if (i == app->files.sel) moss_set_attr(win, MOSS_ATTR_NORMAL);
    }
    if (app->files.count > 0 && !app->files.is_dir[app->files.sel]) {
        char full[512];
        snprintf(full, sizeof full, "%s/%s", app->files.path[0] ? app->files.path : "/", app->files.entries[app->files.sel]);
        FILE *fp = fopen(full, "r");
        moss_printf(win, 3, split + 2, "Preview:");
        if (!fp) moss_printf(win, 4, split + 2, "Cannot open file.");
        else {
            char line[256];
            int row = 4;
            while (row < win->h - 2 && fgets(line, sizeof line, fp)) {
                line[strcspn(line, "\r\n")] = 0;
                moss_printf(win, row++, split + 2, "%.*s", win->w - split - 5, line);
            }
            fclose(fp);
        }
    }
}

static void draw_notes(WMApp *app) {
    MOSS_Window *win = app->win;
    moss_set_attr(win, MOSS_ATTR_BOLD);
    moss_printf(win, 1, 2, "Notes  (^S save, Backspace delete)");
    moss_set_attr(win, MOSS_ATTR_NORMAL);
    moss_draw_hline(win, 2, 1, win->w - 4, ACS_HLINE);
    int row = 3, col = 2;
    for (int i = 0; i < app->notes.len && row < win->h - 2; i++) {
        char ch = app->notes.buf[i];
        if (ch == '\n' || col >= win->w - 2) { row++; col = 2; if (ch == '\n') continue; }
        mvwaddch(win->ncwin, row, col++, ch);
    }
}

static void draw_calc(WMApp *app) {
    MOSS_Window *win = app->win;
    moss_set_attr(win, MOSS_ATTR_BOLD);
    moss_printf(win, 1, 2, "Calculator  (Enter evaluate)");
    moss_set_attr(win, MOSS_ATTR_NORMAL);
    moss_draw_hline(win, 2, 1, win->w - 4, ACS_HLINE);
    moss_printf(win, 4, 2, "expr   : %s", app->calc.expr[0] ? app->calc.expr : "0");
    moss_printf(win, 6, 2, "result : %s", app->calc.result[0] ? app->calc.result : "-");
    moss_printf(win, 8, 2, "Supports + - * / and parentheses.");
}

static void draw_games(WMApp *app) {
    static const char *games[] = { "snake", "mine", "tetris", "2048", "pong" };
    MOSS_Window *win = app->win;
    moss_set_attr(win, MOSS_ATTR_BOLD);
    moss_printf(win, 1, 2, "Games  (Enter launch)");
    moss_set_attr(win, MOSS_ATTR_NORMAL);
    moss_draw_hline(win, 2, 1, win->w - 4, ACS_HLINE);
    for (int i = 0; i < 5; i++) {
        if (i == app->list_sel) moss_set_attr(win, MOSS_ATTR_REVERSE);
        moss_printf(win, 4 + i, 4, "%s", games[i]);
        if (i == app->list_sel) moss_set_attr(win, MOSS_ATTR_NORMAL);
    }
    moss_printf(win, 11, 2, "Launches the same terminal game code.");
}

static void draw_settings(WMApp *app) {
    MOSS_Window *win = app->win;
    moss_set_attr(win, MOSS_ATTR_BOLD);
    moss_printf(win, 1, 2, "Desktop help");
    moss_set_attr(win, MOSS_ATTR_NORMAL);
    moss_draw_hline(win, 2, 1, win->w - 4, ACS_HLINE);
    moss_printf(win, 4, 2, "Mouse drag title bar  Move window");
    moss_printf(win, 5, 2, "Drag to top edge      Maximize");
    moss_printf(win, 6, 2, "[_][^][X]             Window controls");
    moss_printf(win, 7, 2, "F1 / Start            Launcher");
    moss_printf(win, 8, 2, "Tab                   Focus next window");
    moss_printf(win, 9, 2, "F2                    Close focused window");
    moss_printf(win,10, 2, "F4                    Back to shell");
}

static void draw_network(WMApp *app) {
    MOSS_Window *win = app->win;
    FILE *fp = fopen("/proc/net/dev", "r");
    moss_set_attr(win, MOSS_ATTR_BOLD);
    moss_printf(win, 1, 2, "Network info");
    moss_set_attr(win, MOSS_ATTR_NORMAL);
    moss_draw_hline(win, 2, 1, win->w - 4, ACS_HLINE);
    if (!fp) { moss_printf(win, 3, 2, "No /proc/net/dev."); return; }
    char line[256];
    int row = 3;
    while (row < win->h - 2 && fgets(line, sizeof line, fp)) {
        line[strcspn(line, "\r\n")] = 0;
        moss_printf(win, row++, 2, "%.*s", win->w - 4, line);
    }
    fclose(fp);
}

static void draw_browser_stub(WMApp *app, const char *title, const char *body) {
    MOSS_Window *win = app->win;
    moss_set_attr(win, MOSS_ATTR_BOLD);
    moss_printf(win, 1, 2, "%s", title);
    moss_set_attr(win, MOSS_ATTR_NORMAL);
    moss_draw_hline(win, 2, 1, win->w - 4, ACS_HLINE);
    moss_printf(win, 4, 2, "%s", body);
}

static void draw_window_chrome(WMApp *app) {
    MOSS_Window *w = app->win;
    if (!w || !w->border) return;
    int bx = w->w - 10;
    if (bx < 1) bx = 1;
    mvwprintw(w->border, 0, bx, "[_][%c][X]", app->maximized ? 'R' : '^');
}

static void draw_app(MossWM *wm, WMApp *app) {
    moss_clear_window(app->win);
    switch (app->app_id) {
        case APP_TERMINAL:       draw_terminal(app); break;
        case APP_SYSTEMINFO:     draw_systeminfo(app->win); break;
        case APP_TASKMANAGER:    draw_task_or_process(app->win, "Task Manager"); break;
        case APP_PROCESSMONITOR: draw_task_or_process(app->win, "Process Monitor"); break;
        case APP_FILEMANAGER:    draw_files(app); break;
        case APP_NOTES:          draw_notes(app); break;
        case APP_CALCULATOR:     draw_calc(app); break;
        case APP_CLOCK:          draw_clock(app); break;
        case APP_COLORPICKER:    draw_browser_stub(app, "Color Picker", "Use the shell's color command to set the terminal accent."); break;
        case APP_BROWSER:        draw_browser_stub(app, "Browser", "Text browser stub. Use the shell for external commands."); break;
        case APP_SETTINGS:       draw_settings(app); break;
        case APP_GAMES:          draw_games(app); break;
        case APP_AMA:            draw_browser_stub(app, "AMA", "AMA is a web feature. In native mode it remains a stub."); break;
        case APP_NETWORKINFO:    draw_network(app); break;
    }
    draw_window_chrome(app);
    wnoutrefresh(app->win->border);
    wnoutrefresh(app->win->ncwin);
}

static int app_find_by_id(MossWM *wm, AppID id) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (wm->apps[i].active && wm->apps[i].app_id == id) return i;
    return -1;
}

static void wm_apply_window_bounds(MossWM *wm, WMApp *app) {
    if (!app || !app->active || !app->win) return;
    int max_h = MOSS_MAX(8, LINES - 2);
    int max_w = MOSS_MAX(18, COLS);
    int x = app->win->x;
    int y = app->win->y;
    int w = app->win->w;
    int h = app->win->h;
    if (app->maximized) {
        x = 0; y = 0; w = max_w; h = max_h;
    } else {
        if (w > max_w) w = max_w;
        if (h > max_h) h = max_h;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + w > COLS) x = MOSS_MAX(0, COLS - w);
        if (y + h > LINES - 2) y = MOSS_MAX(0, LINES - 2 - h);
    }
    moss_window_move(app->win, y, x);
    moss_window_resize(app->win, w, h);
}

static void wm_toggle_maximize(MossWM *wm, int idx) {
    if (idx < 0 || idx >= WM_MAX_WINDOWS || !wm->apps[idx].active) return;
    WMApp *app = &wm->apps[idx];
    if (!app->win) return;
    if (!app->maximized) {
        app->restore_x = app->win->x;
        app->restore_y = app->win->y;
        app->restore_w = app->win->w;
        app->restore_h = app->win->h;
        app->maximized = true;
    } else {
        app->maximized = false;
        moss_window_move(app->win, app->restore_y, app->restore_x);
        moss_window_resize(app->win, app->restore_w, app->restore_h);
    }
    wm_apply_window_bounds(wm, app);
    wm_focus(wm, idx);
}

static void app_close(MossWM *wm, int idx) {
    if (idx < 0 || idx >= WM_MAX_WINDOWS || !wm->apps[idx].active) return;
    if (wm->apps[idx].win) moss_window_destroy(wm->apps[idx].win);
    memset(&wm->apps[idx], 0, sizeof wm->apps[idx]);
    wm->apps[idx].active = false;
    if (wm->focused_app == idx) {
        wm->focused_app = -1;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) if (wm->apps[i].active) { wm_focus(wm, i); break; }
    }
}

static int app_open(MossWM *wm, AppID id) {
    int existing = app_find_by_id(wm, id);
    if (existing >= 0) { wm_focus(wm, existing); return existing; }

    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (!wm->apps[i].active) { slot = i; break; }
    if (slot < 0) return -1;

    WMApp *app = &wm->apps[slot];
    memset(app, 0, sizeof *app);
    app->app_id = id;
    app->active = true;
    strncpy(app->title, app_title(id), sizeof app->title - 1);

    int w = MOSS_MIN(COLS - 6, 56);
    int h = MOSS_MIN(LINES - 4, 18);
    if (id == APP_FILEMANAGER) { w = MOSS_MIN(COLS - 4, 76); h = MOSS_MIN(LINES - 4, 22); }
    if (id == APP_SYSTEMINFO || id == APP_SETTINGS) { w = MOSS_MIN(COLS - 4, 52); h = MOSS_MIN(LINES - 4, 16); }
    if (id == APP_CLOCK) { w = MOSS_MIN(COLS - 4, 42); h = MOSS_MIN(LINES - 4, 10); }
    if (id == APP_GAMES) { w = MOSS_MIN(COLS - 4, 40); h = MOSS_MIN(LINES - 4, 16); }

    app->win = moss_create_window(wm->ctx, app->title, w, h);
    if (!app->win) { memset(app, 0, sizeof *app); return -1; }
    moss_window_show(app->win);
    moss_window_move(app->win, wm->cascade_y, wm->cascade_x);
    wm->cascade_x = (wm->cascade_x + 4) % MOSS_MAX(4, COLS / 3);
    wm->cascade_y = (wm->cascade_y + 2) % MOSS_MAX(2, (LINES - 4) / 3);

    if (id == APP_FILEMANAGER) {
        strcpy(app->files.path, "/");
        file_refresh(app);
    }

    app->restore_x = app->win->x;
    app->restore_y = app->win->y;
    app->restore_w = app->win->w;
    app->restore_h = app->win->h;
    wm_apply_window_bounds(wm, app);
    wm_focus(wm, slot);
    return slot;
}

static void draw_taskbar(MossWM *wm) {
    int cols = COLS;
    if (!wm->taskbar) wm->taskbar = newwin(2, cols, LINES - 2, 0);
    wresize(wm->taskbar, 2, cols);
    mvwin(wm->taskbar, LINES - 2, 0);
    werase(wm->taskbar);
    wattron(wm->taskbar, COLOR_PAIR(1));
    mvwhline(wm->taskbar, 0, 0, ACS_HLINE, cols);
    mvwprintw(wm->taskbar, 1, 1, "[Start]");
    int pos = 10;
    for (int i = 0; i < WM_MAX_WINDOWS && pos < cols - 12; i++) {
        if (!wm->apps[i].active) continue;
        if (i == wm->focused_app) wattron(wm->taskbar, A_REVERSE | COLOR_PAIR(1));
        mvwprintw(wm->taskbar, 1, pos, "[%s]", wm->apps[i].title);
        if (i == wm->focused_app) wattroff(wm->taskbar, A_REVERSE | COLOR_PAIR(1));
        pos += (int)strlen(wm->apps[i].title) + 3;
    }
    char tbuf[32];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(tbuf, sizeof tbuf, "%H:%M:%S", tm);
    mvwprintw(wm->taskbar, 0, MOSS_MAX(1, cols - (int)strlen(tbuf) - 1), "%s", tbuf);
    wattroff(wm->taskbar, COLOR_PAIR(1));
    wnoutrefresh(wm->taskbar);
}

static void draw_launcher(MossWM *wm) {
    int h = NUM_LAUNCHER + 4, w = 28;
    if (!wm->launcher) wm->launcher = newwin(h, w, MOSS_MAX(1, LINES - h - 2), 1);
    wresize(wm->launcher, h, w);
    mvwin(wm->launcher, MOSS_MAX(1, LINES - h - 2), 1);
    werase(wm->launcher);
    wattron(wm->launcher, COLOR_PAIR(1));
    box(wm->launcher, 0, 0);
    mvwaddstr(wm->launcher, 0, 2, "[ launcher ]");
    for (int i = 0; i < NUM_LAUNCHER; i++) {
        if (i == wm->launcher_sel) wattron(wm->launcher, A_REVERSE | COLOR_PAIR(1));
        mvwprintw(wm->launcher, 2 + i, 2, "%c  %-20s", LAUNCHER[i].hotkey, LAUNCHER[i].label);
        if (i == wm->launcher_sel) wattroff(wm->launcher, A_REVERSE | COLOR_PAIR(1));
    }
    wattroff(wm->launcher, COLOR_PAIR(1));
    wnoutrefresh(wm->launcher);
}

static void draw_help(MossWM *wm) {
    int h = 14, w = 68;
    WINDOW *help = newwin(h, w, MOSS_MAX(1, (LINES - h) / 2), MOSS_MAX(1, (COLS - w) / 2));
    wattron(help, COLOR_PAIR(1));
    box(help, 0, 0);
    mvwaddstr(help, 0, 2, "[ mOSs desktop help ]");
    mvwaddstr(help, 2, 2, "Mouse: drag title bars, click taskbar buttons, click [_][^][X].");
    mvwaddstr(help, 3, 2, "Snap: drag a window to the top edge and release to maximize it.");
    mvwaddstr(help, 4, 2, "F1 or Start opens the launcher.");
    mvwaddstr(help, 5, 2, "Tab focuses the next window. F2 closes the focused window.");
    mvwaddstr(help, 6, 2, "F4 returns to the shell prompt.");
    mvwaddstr(help, 7, 2, "Files: arrows move, Enter open, Backspace up.");
    mvwaddstr(help, 8, 2, "Notes: type text, Ctrl-S saves to /tmp/moss-notes.txt.");
    mvwaddstr(help, 9, 2, "Calculator: type expression, Enter evaluates.");
    mvwaddstr(help,10, 2, "Games: choose a game and press Enter.");
    mvwaddstr(help,11, 2, "Press ? or Esc to close this panel.");
    wattroff(help, COLOR_PAIR(1));
    wnoutrefresh(help);
    delwin(help);
}

static void launcher_accept(MossWM *wm) {
    app_open(wm, LAUNCHER[wm->launcher_sel].id);
    close_launcher(wm);
}

static void wm_post_game_reset(void) {
    keypad(stdscr, TRUE);
    cbreak();
    noecho();
    timeout(50);
    clear();
    refresh();
}

static int handle_focused_app_key(MossWM *wm, int ch) {
    if (wm->focused_app < 0 || !wm->apps[wm->focused_app].active) return 0;
    WMApp *app = &wm->apps[wm->focused_app];
    if (app->app_id == APP_FILEMANAGER) {
        if (ch == KEY_UP) { if (app->files.sel > 0) app->files.sel--; return 1; }
        if (ch == KEY_DOWN) { if (app->files.sel + 1 < app->files.count) app->files.sel++; return 1; }
        if (ch == KEY_BACKSPACE || ch == 127) {
            if (strcmp(app->files.path, "/") != 0) {
                char *slash = strrchr(app->files.path, '/');
                if (slash && slash != app->files.path) *slash = 0; else strcpy(app->files.path, "/");
                file_refresh(app);
            }
            return 1;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (app->files.count <= 0) return 1;
            if (app->files.is_dir[app->files.sel]) {
                char next[256];
                if (strcmp(app->files.path, "/") == 0) snprintf(next, sizeof next, "/%s", app->files.entries[app->files.sel]);
                else snprintf(next, sizeof next, "%s/%s", app->files.path, app->files.entries[app->files.sel]);
                strncpy(app->files.path, next, sizeof app->files.path - 1);
                file_refresh(app);
            }
            return 1;
        }
    } else if (app->app_id == APP_NOTES) {
        if (ch == 19) {
            FILE *fp = fopen("/tmp/moss-notes.txt", "w");
            if (fp) { fwrite(app->notes.buf, 1, (size_t)app->notes.len, fp); fclose(fp); }
            return 1;
        }
        if ((ch == KEY_BACKSPACE || ch == 127) && app->notes.len > 0) {
            app->notes.buf[--app->notes.len] = 0;
            return 1;
        }
        if ((ch == '\n' || ch == '\r') && app->notes.len + 1 < NOTES_CAP) {
            app->notes.buf[app->notes.len++] = '\n';
            app->notes.buf[app->notes.len] = 0;
            return 1;
        }
        if (ch >= 32 && ch < 127 && app->notes.len + 1 < NOTES_CAP) {
            app->notes.buf[app->notes.len++] = (char)ch;
            app->notes.buf[app->notes.len] = 0;
            return 1;
        }
    } else if (app->app_id == APP_CALCULATOR) {
        if ((ch == KEY_BACKSPACE || ch == 127) && app->calc.expr[0]) {
            app->calc.expr[strlen(app->calc.expr) - 1] = 0;
            return 1;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            double out = 0.0;
            if (calc_eval(app->calc.expr, &out)) snprintf(app->calc.result, sizeof app->calc.result, "%.6g", out);
            else strncpy(app->calc.result, "parse error", sizeof app->calc.result - 1);
            return 1;
        }
        if (strchr("0123456789.+-*/() ", ch) && strlen(app->calc.expr) + 1 < sizeof app->calc.expr) {
            size_t len = strlen(app->calc.expr);
            app->calc.expr[len] = (char)ch;
            app->calc.expr[len + 1] = 0;
            return 1;
        }
    } else if (app->app_id == APP_GAMES) {
        static const char *games[] = { "snake", "mine", "tetris", "2048", "pong" };
        if (ch == KEY_UP) { if (app->list_sel > 0) app->list_sel--; return 1; }
        if (ch == KEY_DOWN) { if (app->list_sel < 4) app->list_sel++; return 1; }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            moss_shell_run_named_game(wm->shell, games[app->list_sel]);
            wm_post_game_reset();
            return 1;
        }
    }
    return 0;
}

static int open_mouse_devices(int *fds, int maxfds) {
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

static void *wm_mouse_thread_main(void *arg) {
    MossWM *wm = (MossWM *)arg;
    int fds[MOUSE_MAX_FDS];
    int nfds = open_mouse_devices(fds, MOUSE_MAX_FDS);
    int legacy = open("/dev/input/mice", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct input_event ev;
    unsigned char pkt[4];
    while (!wm->mouse_thread_stop) {
        int got = 0;
        for (int i = 0; i < nfds; i++) {
            while (read(fds[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
                got = 1;
                if (ev.type == EV_REL) {
                    if (ev.code == REL_X) wm->mouse_dx += ev.value;
                    else if (ev.code == REL_Y) wm->mouse_dy += ev.value;
                    else if (ev.code == REL_WHEEL || ev.code == REL_HWHEEL) wm->mouse_wheel += ev.value;
                } else if (ev.type == EV_KEY) {
                    if (ev.code == BTN_LEFT) wm->mouse_left_down = ev.value ? true : false;
                }
            }
        }
        if (legacy >= 0) {
            while (read(legacy, pkt, sizeof pkt) == 4) {
                got = 1;
                wm->mouse_dx += (signed char)pkt[1];
                wm->mouse_dy -= (signed char)pkt[2];
                if (pkt[0] & 0x1) wm->mouse_left_down = true;
                else wm->mouse_left_down = false;
                if ((signed char)pkt[3] != 0) wm->mouse_wheel += (signed char)pkt[3];
            }
        }
        if (!got) usleep(12000);
    }
    for (int i = 0; i < nfds; i++) close(fds[i]);
    if (legacy >= 0) close(legacy);
    return NULL;
}

static void wm_start_mouse(MossWM *wm) {
    if (wm->mouse_thread_running) return;
    wm->mouse_thread_stop = false;
    wm->mouse_dx = wm->mouse_dy = wm->mouse_wheel = 0;
    wm->mouse_left_down = false;
    wm->mouse_left_prev = false;
    wm->mouse_x = COLS / 2;
    wm->mouse_y = MOSS_MAX(1, (LINES - 2) / 2);
    if (pthread_create(&wm->mouse_thread, NULL, wm_mouse_thread_main, wm) == 0)
        wm->mouse_thread_running = true;
}

static void wm_stop_mouse(MossWM *wm) {
    if (!wm->mouse_thread_running) return;
    wm->mouse_thread_stop = true;
    pthread_join(wm->mouse_thread, NULL);
    wm->mouse_thread_running = false;
}

static void wm_on_resize(MossWM *wm) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (wm->apps[i].active) wm_apply_window_bounds(wm, &wm->apps[i]);
    if (wm->taskbar) { delwin(wm->taskbar); wm->taskbar = NULL; }
    if (wm->launcher) { delwin(wm->launcher); wm->launcher = NULL; }
    wm->mouse_x = MOSS_CLAMP(wm->mouse_x, 0, COLS - 1);
    wm->mouse_y = MOSS_CLAMP(wm->mouse_y, 0, LINES - 3);
}

static int taskbar_hit(MossWM *wm, int x) {
    int pos = 10;
    for (int i = 0; i < WM_MAX_WINDOWS && pos < COLS - 12; i++) {
        if (!wm->apps[i].active) continue;
        int width = (int)strlen(wm->apps[i].title) + 2;
        if (x >= pos && x < pos + width + 1) return i;
        pos += width + 1;
    }
    return -1;
}

static void handle_mouse(MossWM *wm) {
    wm->mouse_x = MOSS_CLAMP(wm->mouse_x + wm->mouse_dx, 0, COLS - 1);
    wm->mouse_y = MOSS_CLAMP(wm->mouse_y + wm->mouse_dy, 0, LINES - 3);
    wm->mouse_dx = 0;
    wm->mouse_dy = 0;

    int press = wm->mouse_left_down && !wm->mouse_left_prev;
    int release = !wm->mouse_left_down && wm->mouse_left_prev;

    if (press) {
        if (wm->mouse_y >= LINES - 2) {
            if (wm->mouse_x >= 1 && wm->mouse_x <= 7) {
                wm->launcher_open = !wm->launcher_open;
                if (!wm->launcher_open) close_launcher(wm);
                else wm->launcher_sel = 0;
            } else {
                int hit = taskbar_hit(wm, wm->mouse_x);
                if (hit >= 0) wm_focus(wm, hit);
            }
        } else if (wm->launcher_open && wm->launcher &&
                   wm->mouse_x >= getbegx(wm->launcher) && wm->mouse_x < getbegx(wm->launcher) + getmaxx(wm->launcher) &&
                   wm->mouse_y >= getbegy(wm->launcher) && wm->mouse_y < getbegy(wm->launcher) + getmaxy(wm->launcher)) {
            int row = wm->mouse_y - getbegy(wm->launcher) - 2;
            if (row >= 0 && row < NUM_LAUNCHER) {
                wm->launcher_sel = row;
                launcher_accept(wm);
            }
        } else {
            int hit = topmost_app_at(wm, wm->mouse_x, wm->mouse_y);
            if (hit >= 0) {
                WMApp *app = &wm->apps[hit];
                MOSS_Window *w = app->win;
                wm_focus(wm, hit);
                int local_x = wm->mouse_x - w->x;
                int local_y = wm->mouse_y - w->y;
                int bx = w->w - 10;
                if (local_y == 0 && local_x >= bx && local_x < bx + 9) {
                    if (local_x >= bx + 6) app_close(wm, hit);
                    else if (local_x >= bx + 3) wm_toggle_maximize(wm, hit);
                } else if (local_y == 0 && !app->maximized) {
                    wm->dragging = true;
                    wm->drag_app = hit;
                    wm->drag_off_x = local_x;
                    wm->drag_off_y = local_y;
                }
            } else {
                close_launcher(wm);
            }
        }
    }

    if (wm->dragging && wm->drag_app >= 0 && wm->mouse_left_down && wm->apps[wm->drag_app].active) {
        WMApp *app = &wm->apps[wm->drag_app];
        if (!app->maximized) {
            int new_x = wm->mouse_x - wm->drag_off_x;
            int new_y = wm->mouse_y - wm->drag_off_y;
            if (new_y < 0) new_y = 0;
            if (new_x < 0) new_x = 0;
            if (new_x + app->win->w > COLS) new_x = MOSS_MAX(0, COLS - app->win->w);
            if (new_y + app->win->h > LINES - 2) new_y = MOSS_MAX(0, LINES - 2 - app->win->h);
            moss_window_move(app->win, new_y, new_x);
        }
    }

    if (release && wm->dragging && wm->drag_app >= 0) {
        if (wm->mouse_y <= 0) wm_toggle_maximize(wm, wm->drag_app);
        wm->dragging = false;
        wm->drag_app = -1;
    }

    wm->mouse_left_prev = wm->mouse_left_down;
}

void wm_run(MossWM *wm, void *shell) {
    wm->shell = (ShellState *)shell;
    wm->running = true;
    timeout(50);
    keypad(stdscr, TRUE);
    cbreak();
    noecho();

    if (active_count(wm) == 0) app_open(wm, APP_TERMINAL);
    wm_start_mouse(wm);

    while (wm->running) {
        handle_mouse(wm);
        erase();
        renderer_draw_grid(wm->renderer, stdscr);
        for (int i = 0; i < WM_MAX_WINDOWS; i++) if (wm->apps[i].active && wm->apps[i].win) draw_app(wm, &wm->apps[i]);
        draw_taskbar(wm);
        if (wm->launcher_open) draw_launcher(wm);
        if (wm->help_open) draw_help(wm);
        update_panels();
        attron(A_REVERSE | COLOR_PAIR(7));
        mvaddch(wm->mouse_y, wm->mouse_x, ' ');
        attroff(A_REVERSE | COLOR_PAIR(7));
        renderer_apply_crt(wm->renderer, wm->ctx->crt_mode);
        wnoutrefresh(stdscr);
        doupdate();

        int ch = getch();
        if (ch == ERR) continue;
        if (ch == KEY_RESIZE) { wm_on_resize(wm); continue; }
        if (ch == KEY_F(4)) { wm->running = false; break; }
        if (ch == '?') { wm->help_open = !wm->help_open; continue; }
        if (wm->help_open && (ch == 27 || ch == '?')) { wm->help_open = false; continue; }
        if (ch == KEY_F(1)) { wm->launcher_open = !wm->launcher_open; if (wm->launcher_open) wm->launcher_sel = 0; else close_launcher(wm); continue; }
        if (wm->launcher_open) {
            if (ch == KEY_UP) { if (wm->launcher_sel > 0) wm->launcher_sel--; else wm->launcher_sel = NUM_LAUNCHER - 1; }
            else if (ch == KEY_DOWN) { wm->launcher_sel = (wm->launcher_sel + 1) % NUM_LAUNCHER; }
            else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) launcher_accept(wm);
            else if (ch == 27) close_launcher(wm);
            else {
                for (int i = 0; i < NUM_LAUNCHER; i++) if (tolower(ch) == LAUNCHER[i].hotkey) { wm->launcher_sel = i; launcher_accept(wm); break; }
            }
            continue;
        }
        if (ch == KEY_F(2) && wm->focused_app >= 0) { app_close(wm, wm->focused_app); continue; }
        if (ch == '\t') {
            int start = wm->focused_app;
            int idx = start;
            for (int k = 0; k < WM_MAX_WINDOWS; k++) {
                idx = (idx + 1 + WM_MAX_WINDOWS) % WM_MAX_WINDOWS;
                if (wm->apps[idx].active) { wm_focus(wm, idx); break; }
            }
            continue;
        }
        for (int i = 0; i < NUM_LAUNCHER; i++) if (tolower(ch) == LAUNCHER[i].hotkey) { app_open(wm, LAUNCHER[i].id); ch = 0; break; }
        if (!ch) continue;
        if (handle_focused_app_key(wm, ch)) continue;
    }

    wm_stop_mouse(wm);
    close_launcher(wm);
    if (wm->taskbar) { delwin(wm->taskbar); wm->taskbar = NULL; }
    for (int i = 0; i < WM_MAX_WINDOWS; i++) app_close(wm, i);
    wm->focused_app = -1;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    timeout(-1);
}

MossWM *wm_create(MOSS_Ctx *ctx, void *shell) {
    MossWM *wm = calloc(1, sizeof *wm);
    if (!wm) return NULL;
    wm->ctx = ctx;
    wm->shell = (ShellState *)shell;
    wm->renderer = renderer_create(ctx);
    wm->focused_app = -1;
    wm->cascade_x = 2;
    wm->cascade_y = 1;
    wm->mouse_x = 10;
    wm->mouse_y = 5;
    return wm;
}

void wm_destroy(MossWM *wm) {
    if (!wm) return;
    wm_stop_mouse(wm);
    if (wm->taskbar) delwin(wm->taskbar);
    if (wm->launcher) delwin(wm->launcher);
    for (int i = 0; i < WM_MAX_WINDOWS; i++) app_close(wm, i);
    if (wm->renderer) renderer_destroy(wm->renderer);
    free(wm);
}
