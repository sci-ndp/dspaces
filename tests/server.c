/*
 * Copyright (c) 2020, Rutgers Discovery Informatics Institute, Rutgers
 * University
 *
 * See COPYRIGHT in top-level directory.
 */

#include <dspaces-server.h>
#include <margo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    char *listen_addr_str, *conf_file;
    dspaces_provider_t s = dspaces_PROVIDER_NULL;
    int rank, color;
    int ret;

    if(argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <listen-address> [<conffile>]\n", argv[0]);
        return -1;
    }

    listen_addr_str = argv[1];
    if(argc == 3) {
        conf_file = argv[2];
    } else {
        conf_file = "dataspaces.conf";
    }

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm gcomm = MPI_COMM_WORLD;

    color = 1;
    MPI_Comm_split(MPI_COMM_WORLD, color, rank, &gcomm);

    ret = dspaces_server_init(listen_addr_str, gcomm, conf_file, &s);
    if(ret != 0)
        return ret;

    // make margo wait for finalize
    dspaces_server_fini(s);

    if(rank == 0) {
        fprintf(stdout, "Server is all done!\n");
    }

    MPI_Finalize();
    return 0;
}
