#ifndef __DSPACES_CONF_H__
#define __DSPACES_CONF_H__

#include "bbox.h"
#include "dspaces-remote.h"
#include "list.h"

static char *hash_strings[] = {"Dynamic", "Unitary", "SFC", "Bisection"};

/* Server configuration parameters */
struct ds_conf {
    int ndim;
    struct coord dims;
    int max_versions;
    int hash_version;
    int num_apps;
    struct list_head *dirs;
    struct remote **remotes;
    int nremote;
    int nmod;
};

int parse_conf(const char *fname, struct ds_conf *conf);

int parse_conf_toml(const char *fname, struct ds_conf *conf);

void print_conf(struct ds_conf *conf);

#endif // __DSPACES_CONF_H