/*
 * Copyright (c) 2024, SCI Institute, University of Utah
 *
 * See COPYRIGHT in top-level directory.
 */

#include<dspaces.h>
#include<stdio.h>
#include<stdlib.h>

int main(int argc, char **argv)
{
    int count;
    char **names;
    dspaces_client_t dsp;
    int ret;
    int i;

    ret = dspaces_init(0, &dsp);

    if(ret != dspaces_SUCCESS) {
        return(1);
    }

    count = dspaces_get_var_names(dsp, &names);

    if(count < 0) {
        return(-1);
    }

    printf("Server has %i variable(s)%s\n", count, count?":":".");
    for(i = 0; i < count; i++) {
        printf("  %s\n", names[i]);
        free(names[i]);
    }
    if(count) {
        free(names);
    }

    dspaces_fini(dsp);

    return(0);
}
