/*
 * Copyright (c) 2020, Rutgers Discovery Informatics Institute, Rutgers
 * University
 *
 * See COPYRIGHT in top-level directory.
 */

#ifndef __DSPACES_SERVER_H
#define __DSPACES_SERVER_H

#include <dspaces-common.h>
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define dspaces_ABT_POOL_DEFAULT ABT_POOL_NULL

typedef struct dspaces_provider *dspaces_provider_t;
#define dspaces_PROVIDER_NULL ((dspaces_provider_t)NULL)

struct dspaces_data_obj {
    const char *var_name;
    int version;
    int ndim;
    int size;
    uint64_t *lb;
    uint64_t *ub;
};

/**
 * @brief Creates a MESSAGING server.
 *
 * @param[in] listen_addr_str mercury-interpeted network string
 * @param[in] comm MPI Communicator
 * @param[in] conf_file DataSpaces configuration file *.toml parsed as TOML,
 * else legacy
 * @param[out] server DataSpaces server handle
 */
int dspaces_server_init(const char *listen_addr_str, MPI_Comm comm,
                        const char *conf_file, dspaces_provider_t *server);

/**
 * @brief Waits for the dataspaces server to finish (be killed.)
 *
 * @param[in] server server handle
 *
 */
void dspaces_server_fini(dspaces_provider_t server);

/**
 * @brief Retrieves list of data objects for a given variable/version pair.
 *
 * @param[in] server DataSpaces server handle
 * @param[in] var_name Name of variable to query
 * @param[in] version version for which to query
 * @param[out] objs list data objects in local storage that match the query.
 *
 * @return Number of objs found. Negative numbers indicate failure.
 */
int dspaces_server_find_objs(dspaces_provider_t server, const char *var_name,
                             int version, struct dspaces_data_obj **objs);

/**
 * @brief Retrieves data from a local storage object
 *
 * @param[in] server DataSpaces server handle
 * @param[in] obj Data object for which to retrieve the data
 * @param[out] buffer Buffer into which to copy the object data. Must be large
 * enough receive all the data of obj
 *
 * @return 0 for success, non-zero if the data cannot be retrieved.
 */
int dspaces_server_get_objdata(dspaces_provider_t server,
                               struct dspaces_data_obj *obj, void *buffer);

#if defined(__cplusplus)
}
#endif

#endif
