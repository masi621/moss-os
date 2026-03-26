#ifndef MOSS_RENDERER_H
#define MOSS_RENDERER_H

#include <curses.h>
#include "moss.h"

typedef struct MossRenderer MossRenderer;

MossRenderer *renderer_create(MOSS_Ctx *ctx);
void renderer_destroy(MossRenderer *r);
void renderer_apply_crt(MossRenderer *r, MOSS_CrtMode mode);
void renderer_draw_grid(MossRenderer *r, WINDOW *win);

#endif /* MOSS_RENDERER_H */
