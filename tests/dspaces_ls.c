/*
 * Copyright (c) 2024, SCI Institute, University of Utah
 *
 * See COPYRIGHT in top-level directory.
 */

#include <dspaces.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

int ls_all(dspaces_client_t dsp)
{
    char **names;
    int count;
    int i;

    count = dspaces_get_var_names(dsp, &names);

    if(count < 0) {
        return (-1);
    }

    printf("Server has %i variable(s)%s\n", count, count ? ":" : ".");
    for(i = 0; i < count; i++) {
        printf("  %s\n", names[i]);
        free(names[i]);
    }
    if(count) {
        free(names);
    }
}

int ls_one(dspaces_client_t dsp, const char *name)
{
    dspaces_obj_t *found_objs = NULL;
    dspaces_obj_t *obj;
    int count;
    int ndim;
    int i, j;

    count = dspaces_get_var_objs(dsp, name, &found_objs);

    printf("Found %i object(s) stored for variable '%s'%s\n", count, name,
           count ? ":" : ".");
    for(i = 0; i < count; i++) {
        obj = &(found_objs[i]);
        printf(" name: %s, version: %i, lb: (", obj->name, obj->version);
        ndim = obj->ndim;
        for(j = 0; j < ndim; j++) {
            printf("%" PRIu64 "%s", obj->lb[j], j == ndim - 1 ? ") " : ", ");
        }
        printf("ub: (");
        for(j = 0; j < ndim; j++) {
            printf("%" PRIu64 "%s", obj->ub[j], j == ndim - 1 ? ")\n" : ", ");
        }
    }

    if(count) {
        free(found_objs);
    }
}

int main(int argc, char **argv)
{
    dspaces_client_t dsp;
    int ret;
    char *var_name = NULL;
    int i;

    if(argc == 2) {
        var_name = argv[1];
    } else if(argc != 1) {
        fprintf(stderr, "Usage: %s [<variable_name>]\n", argv[0]);
        return (-1);
    }

    ret = dspaces_init(0, &dsp);

    if(ret != dspaces_SUCCESS) {
        return (1);
    }

    if(var_name) {
        ret = ls_one(dsp, var_name);
    } else {
        ret = ls_all(dsp);
    }

    if(ret != dspaces_SUCCESS) {
        return (1);
    }

    dspaces_fini(dsp);

    return (0);
}
