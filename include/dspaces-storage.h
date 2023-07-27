#ifndef _DSPACES_STORAGE_H

#include "list.h"

enum { DS_FILE_ALL, DS_FILE_LIST };

enum { DS_FILE_NC, DS_FILE_IDX };

struct dspaces_file {
    struct list_head entry;
    int type;
    char *name;
};

struct dspaces_dir {
    struct list_head entry;
    struct list_head files;
    char *name;
    char *path;
    int cont_type;
};

#endif
