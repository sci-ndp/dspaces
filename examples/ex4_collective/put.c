/* put.c - collectively put an array into DataSpaces
 */

#include "dspaces.h"
#include "mpi.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// Size of array and step count, if changing
// MUST also change in get.c
#define ARRAY_SIZE 128
#define NUM_STEPS 10

int main(int argc, char **argv)
{
    int err;
    int nprocs, rank;
    int my_array_size;
    int *data;
    int timestep;
    MPI_Comm gcomm;
    dspaces_client_t client;
    uint64_t lb, ub;
    int ndim;
    int local_min, local_max, local_sum;
    int min, max, sum;
    float avg;
    int i;

    srand(time(NULL));

    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Barrier(MPI_COMM_WORLD);
    gcomm = MPI_COMM_WORLD;

    // Initalize DataSpaces
    // # MPI communicator for collective bootstrapping
    // # handle to initialize
    dspaces_init_mpi(gcomm, &client);

    // Timestep notation left in to demonstrate how this can be adjusted
    timestep = 0;

    // create domain decomposition
    my_array_size = ARRAY_SIZE / nprocs;
    lb = my_array_size * rank;
    ub = lb + (my_array_size - 1);
    if(ub >= ARRAY_SIZE) {
        // truncate the highest rank if ARRAY_SIZE doesn't evenly divide by nprocs
        ub = ARRAY_SIZE;
        my_array_size = (ub - lb) + 1;
    }

    // Create local segment of array
    data = malloc(my_array_size * sizeof(*data));

     // ndim: Dimensions for application data domain
     // In this case, our data array will be 1 dimensional
     ndim = 1;

    for(timestep = 0; timestep < NUM_STEPS; timestep++) {
        // Name the Data that will be writen
        char var_name[128];
        sprintf(var_name, "ex4_sample_data");

        // Populate array, 128 integer, values 0-64k
        for(i = 0; i < my_array_size; i++) {
            data[i] = rand() % 65536;
            if(i == 0) {
                local_min = local_max = local_sum = data[0];
            } else {
                if(data[i] < local_min) {
                    local_min = data[i];
                }
                if(data[i] > local_max) {
                    local_max = data[i];
                }
                local_sum += data[i];
            }
        }

        // DataSpaces: Put data array into the space
        // 1 integer in each box, fill boxes 0,0,0 to 127,0,0
        dspaces_put(client, var_name, timestep, sizeof(int), ndim, &lb, &ub,
                    data);

        // Reduce all local maximums to find the overall maximum in the data
        MPI_Reduce(&local_max, &max, 1, MPI_INT, MPI_MAX, 0, gcomm);
        // Reduce all local minimums to find the overall minimum in the data
        MPI_Reduce(&local_min, &min, 1, MPI_INT, MPI_MIN, 0, gcomm);
        // Reduce all local avgs into a global sum, then divide by the number
        // of processes to get the global average
        MPI_Reduce(&local_sum, &sum, 1, MPI_INT, MPI_SUM, 0, gcomm);
        avg = sum / ARRAY_SIZE;

        // Report data to user
        if(rank == 0) {
            printf("Written timestep %i Max: %d, Min: %d, Average: %f\n", 
                    timestep, max, min, avg);
        }


    }

    // Signal the server to shutdown (the server must receive this signal n
    // times before it shuts down, where n is num_apps in dataspaces.conf)
    if(rank == 0) {
        dspaces_kill(client);
    }

    // DataSpaces: Finalize and clean up DS process
    dspaces_fini(client);

    MPI_Barrier(gcomm);
    MPI_Finalize();

    free(data);

    return 0;
}
