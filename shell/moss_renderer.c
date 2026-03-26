#define _GNU_SOURCE
#include <curses.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "moss.h"
#include "moss_renderer.h"

struct MossRenderer {
    MOSS_Ctx *ctx;
    struct timespec last_tick;
    int flicker_phase;
    int scanline_offset;
};

static long ms_since(const struct timespec *last) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - last->tv_sec) * 1000L + (now.tv_nsec - last->tv_nsec) / 1000000L;
}

MossRenderer *renderer_create(MOSS_Ctx *ctx) {
    MossRenderer *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->ctx = ctx;
    clock_gettime(CLOCK_MONOTONIC, &r->last_tick);
    return r;
}

void renderer_destroy(MossRenderer *r) {
    free(r);
}

static void apply_scanlines(MossRenderer *r, MOSS_CrtMode mode) {
    int rows, cols, row;
    getmaxyx(stdscr, rows, cols);
    for (row = (r->scanline_offset & 1); row < rows; row += 2) {
        int col;
        if (mode == MOSS_CRT_STRONG) {
            move(row, 0);
            attron(A_DIM);
            for (col = 0; col < cols; ++col) addch(' ');
            attroff(A_DIM);
        } else {
            for (col = 0; col < cols; ++col) {
                chtype ch = mvwinch(stdscr, row, col);
                mvaddch(row, col, (ch & ~A_ATTRIBUTES) | ((ch & A_ATTRIBUTES) | A_DIM));
            }
        }
    }
}

static void apply_flicker(MossRenderer *r, MOSS_CrtMode mode) {
    long interval = (mode == MOSS_CRT_STRONG) ? 120L : 150L;
    if (ms_since(&r->last_tick) >= interval) {
        r->flicker_phase ^= 1;
        r->scanline_offset ^= 1;
        clock_gettime(CLOCK_MONOTONIC, &r->last_tick);
    }
    if (r->flicker_phase) {
        int rows, cols, row, col;
        getmaxyx(stdscr, rows, cols);
        for (row = 1; row < rows; row += 2) {
            for (col = 0; col < cols; ++col) {
                chtype ch = mvwinch(stdscr, row, col);
                mvaddch(row, col, (ch & ~A_ATTRIBUTES) | ((ch & A_ATTRIBUTES) | A_DIM));
            }
        }
    }
}

static void apply_vignette(void) {
    int rows, cols, r;
    int vig_cols, vig_rows;
    getmaxyx(stdscr, rows, cols);
    vig_cols = cols / 10;
    vig_rows = rows / 8;
    if (vig_cols < 1 || vig_rows < 1) return;
    for (r = 0; r < rows; ++r) {
        int c;
        for (c = 0; c < vig_cols; ++c) {
            chtype ch1 = mvwinch(stdscr, r, c);
            chtype ch2 = mvwinch(stdscr, r, cols - 1 - c);
            mvaddch(r, c, (ch1 & ~A_ATTRIBUTES) | ((ch1 & A_ATTRIBUTES) | A_DIM));
            mvaddch(r, cols - 1 - c, (ch2 & ~A_ATTRIBUTES) | ((ch2 & A_ATTRIBUTES) | A_DIM));
        }
    }
    for (r = 0; r < vig_rows; ++r) {
        int c;
        for (c = 0; c < cols; ++c) {
            chtype ch1 = mvwinch(stdscr, r, c);
            chtype ch2 = mvwinch(stdscr, rows - 1 - r, c);
            mvaddch(r, c, (ch1 & ~A_ATTRIBUTES) | ((ch1 & A_ATTRIBUTES) | A_DIM));
            mvaddch(rows - 1 - r, c, (ch2 & ~A_ATTRIBUTES) | ((ch2 & A_ATTRIBUTES) | A_DIM));
        }
    }
}

void renderer_apply_crt(MossRenderer *r, MOSS_CrtMode mode) {
    if (!r || mode == MOSS_CRT_OFF) return;
    apply_scanlines(r, mode);
    apply_flicker(r, mode);
    apply_vignette();
}

void renderer_draw_grid(MossRenderer *r, WINDOW *win) {
    int rows, cols, row, col;
    (void)r;
    if (!win) return;
    getmaxyx(win, rows, cols);
    wattron(win, A_DIM | COLOR_PAIR(8));
    for (row = 15; row < rows; row += 15) mvwhline(win, row, 0, ACS_HLINE, cols);
    for (col = 15; col < cols; col += 15) mvwvline(win, 0, col, ACS_VLINE, rows);
    wattroff(win, A_DIM | COLOR_PAIR(8));
}
