/*
 * Copyright (c) 2022, Scientific Computing & Imagine Institute, University of Utah
 *
 * See COPYRIGHT in top-level directory.
 */

#include <mpi.h>

#include <dspaces.h>
#include <dspaces-ops.h>

#include<stdint.h>
#include<stdio.h>
#include<stdlib.h>

int main(int argc, char **argv)
{
    dspaces_client_t client;
    uint64_t gdim[2] = {128, 128};
    uint64_t lb[2] = {0, 0};
    uint64_t ub[2] = {127, 127};
    double *a, *b;
    double *result;
    ds_expr_t var_a;
    ds_expr_t var_b;
    ds_expr_t const_1;
    ds_expr_t a_plus_b;
    ds_expr_t one_minus_a_plus_b;
    int i, j;

    MPI_Init(&argc, &argv);

    dspaces_init_mpi(MPI_COMM_WORLD, &client);
    dspaces_define_gdim(client, "var_a", 2, gdim);
    dspaces_define_gdim(client, "var_b", 2, gdim);

    a = malloc(sizeof(*a) * gdim[0] * gdim[1]);
    b = malloc(sizeof(*a) * gdim[0] * gdim[1]);
    for(i = 0; i < gdim[0]; i++) {
        for(j = 0; j < gdim[1]; j++) {
            a[(i * gdim[1]) + j] = 1.1;
            b[(i * gdim[1]) + j] = 2.1;
        }
    }

    dspaces_put(client, "var_a", 0, sizeof(double), 2, lb, ub, a);
    dspaces_put(client, "var_b", 0, sizeof(double), 2, lb, ub, b);

    var_a = dspaces_op_new_obj(client, "var_a", 0, DS_VAL_REAL, 2, lb, ub);
    var_b = dspaces_op_new_obj(client, "var_b", 0, DS_VAL_REAL, 2, lb, ub);
    const_1 = dspaces_op_new_iconst(1);
    a_plus_b = dspaces_op_new_add(var_a, var_b);
    one_minus_a_plus_b = dspaces_op_new_sub(const_1, a_plus_b);
    dspaces_op_calc(client, one_minus_a_plus_b, (void **)&result);
    for(i = 0; i < gdim[0]; i++) {
         for(j = 0; j < gdim[1]; j++) {
            if(result[(i * gdim[1]) + j] != 1 - (a[(i * gdim[1]) + j] + b[(i * gdim[1]) + j])) {
                fprintf(stderr, "Bad value at (%i, %i)\n", i, j);
                fprintf(stderr, " Expected %lf, but got %lf\n", 1 - (a[(i * gdim[1]) + j] + b[(i * gdim[1]) + j]), result[(i * gdim[1]) + j]);
                return(-1);
            }
         }
    }

    fprintf(stderr, "result[100] = %lf\n", result[100]);

    dspaces_kill(client);

    MPI_Finalize();
}
