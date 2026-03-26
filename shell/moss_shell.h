#ifndef MOSS_SHELL_H
#define MOSS_SHELL_H

#include <stdbool.h>

typedef struct VFSEntry {
    char  path[512];
    char  name[128];
    bool  is_dir;
    char *content;    /* NULL for directories */
} VFSEntry;

/* Exposed so the WM can launch terminal games without shelling out. */
void moss_shell_run_named_game(void *shell_state, const char *name);

#endif /* MOSS_SHELL_H */
