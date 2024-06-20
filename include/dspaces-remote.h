#ifndef __DSPACES_REMOTE_H
#define __DSPACES_REMOTE_H

#include "dspaces.h"

struct remote {
    char *name;
    char addr_str[128];
    dspaces_client_t conn;
};

#endif // __DSPACES_REMOTE_H