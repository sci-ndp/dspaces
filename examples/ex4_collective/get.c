/* Collectively get an array from dataspaces
 */
#include "dspaces.h"
#include "mpi.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Size of array and step count, if changing
// MUST also change in put.c
#define ARRAY_SIZE 128
#define NUM_STEPS 10

int main(int argc, char **argv)
{
    int err;
    int nprocs, rank;
    int my_array_size;
    MPI_Comm gcomm;
    dspaces_client_t client;
    int timestep;
    int ndim;
    int local_min, local_max, local_sum;
    int min, max, sum;
    float avg;
    uint64_t lb, ub;
    int *data;
    int i;
    
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Barrier(MPI_COMM_WORLD);
    gcomm = MPI_COMM_WORLD;

    
    // Initalize DataSpaces
    // # MPI communicator for collective bootstrapping
    // # handle to initialize
    dspaces_init_mpi(gcomm, &client);

    // Name our data.
    char var_name[128];
    sprintf(var_name, "ex4_sample_data");

    // create domain decomposition
    my_array_size = ARRAY_SIZE / nprocs;
    lb = my_array_size * rank;
    ub = lb + (my_array_size - 1);
    if(ub >= ARRAY_SIZE) {
        // truncate the highest rank if ARRAY_SIZE doesn't evenly divide by nprocs
        ub = ARRAY_SIZE;
        my_array_size = (ub - lb) + 1;
    }

    // Create local segment for read
    data = malloc(my_array_size * sizeof(*data));

    // ndim: Dimensions for application data domain
    // In this case, our data array will be 1 dimensional
    ndim = 1;

    for(timestep = 0; timestep < NUM_STEPS; timestep++) {

        // DataSpaces: Get data array from the space
        // Usage: dspaces_get(Name of variable, version num,
        // size (in bytes of each element), dimensions for bounding box,
        // lower bound coordinates, upper bound coordinates,
        // ptr to data buffer, flag value (-1) means wait for data indefinitely
        dspaces_get(client, var_name, timestep, sizeof(int), ndim, &lb, &ub,
                  data, -1);

        local_max = data[0];
        local_min = data[0];
        local_sum = 0; // for avg

        // Find Max and Min in our local buffer
        // Also, sum the contents of this buffer for averaging purposes
        for(i = 0; i < my_array_size; i++) {

            local_sum += data[i];

            if(local_max < data[i]) {
                local_max = data[i];
            } else if(local_min > data[i]) {
                local_min = data[i];
            }
        }

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
            printf("Read timestep %i Max: %d, Min: %d, Average: %f\n", 
                    timestep, max, min, avg);
        }

    }

    free(data);

    // Signal the server to shutdown (the server must receive this signal n
    // times before it shuts down, where n is num_apps in dataspaces.conf)
    if(rank == 0) {
        dspaces_kill(client);
    }

    // DataSpaces: Finalize and clean up DS process
    dspaces_fini(client);

    MPI_Barrier(gcomm);
    MPI_Finalize();

    return 0;
}
