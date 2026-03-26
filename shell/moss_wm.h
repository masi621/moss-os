#ifndef MOSS_WM_H
#define MOSS_WM_H

#include "moss.h"

typedef struct MossWM MossWM;

MossWM *wm_create(MOSS_Ctx *ctx, void *shell);
void wm_run(MossWM *wm, void *shell);
void wm_destroy(MossWM *wm);

#endif /* MOSS_WM_H */
