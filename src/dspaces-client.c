/*
 * Copyright (c) 2020, Rutgers Discovery Informatics Institute, Rutgers
 * University
 *
 * See COPYRIGHT in top-level directory.
 */
#include "dspaces-ops.h"
#include "dspaces.h"
#include "dspacesp.h"
#include "gspace.h"
#include "ss_data.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <lz4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HAVE_DRC
#include <rdmacred.h>
#endif /* HAVE_DRC */

#include <mpi.h>

#ifdef USE_APEX
#include <apex.h>
#define APEX_FUNC_TIMER_START(fn)                                              \
    apex_profiler_handle profiler0 = apex_start(APEX_FUNCTION_ADDRESS, &fn);
#define APEX_NAME_TIMER_START(num, name)                                       \
    apex_profiler_handle profiler##num = apex_start(APEX_NAME_STRING, name);
#define APEX_TIMER_STOP(num) apex_stop(profiler##num);
#else
#define APEX_FUNC_TIMER_START(fn) (void)0;
#define APEX_NAME_TIMER_START(num, name) (void)0;
#define APEX_TIMER_STOP(num) (void)0;
#endif

#define DEBUG_OUT(dstr, ...)                                                   \
    do {                                                                       \
        if(client->f_debug) {                                                  \
            ABT_unit_id tid;                                                   \
            ABT_thread_self_id(&tid);                                          \
            fprintf(stderr,                                                    \
                    "Rank %i: TID: %" PRIu64 " %s, line %i (%s): " dstr,       \
                    client->rank, tid, __FILE__, __LINE__, __func__,           \
                    ##__VA_ARGS__);                                            \
        }                                                                      \
    } while(0);

#define SUB_HASH_SIZE 16
#define MB (1024 * 1024)
#define BULK_TRANSFER_MAX (128 * MB)

static int g_is_initialized = 0;

static enum storage_type st = column_major;

struct dspaces_sub_handle {
    struct dspaces_req *req;
    void *arg;
    int result;
    int status;
    int id;
    dspaces_sub_fn cb;
    obj_descriptor q_odsc;
};

struct sub_list_node {
    struct sub_list_node *next;
    struct dspaces_sub_handle *subh;
    int id;
};

struct dspaces_put_req {
    hg_handle_t handle;
    margo_request req;
    struct dspaces_put_req *next;
    bulk_gdim_t in;
    int finalized;
    void *buffer;
    hg_return_t ret;
};

struct dspaces_client {
    margo_instance_id mid;
    hg_id_t put_id;
    hg_id_t put_local_id;
    hg_id_t put_meta_id;
    hg_id_t get_id;
    hg_id_t get_local_id;
    hg_id_t query_id;
    hg_id_t query_meta_id;
    hg_id_t ss_id;
    hg_id_t drain_id;
    hg_id_t kill_id;
    hg_id_t kill_client_id;
    hg_id_t sub_id;
    hg_id_t notify_id;
    hg_id_t do_ops_id;
    hg_id_t pexec_id;
    hg_id_t mpexec_id;
    hg_id_t cond_id;
    hg_id_t get_vars_id;
    hg_id_t get_var_objs_id;
    hg_id_t reg_id;
    struct dc_gspace *dcg;
    char **server_address;
    char **node_names;
    char my_node_name[HOST_NAME_MAX];
    int my_server;
    int size_sp;
    int rank;
    int local_put_count; // used during finalize
    int f_debug;
    int f_final;
    int listener_init;
    struct dspaces_put_req *put_reqs;
    struct dspaces_put_req *put_reqs_end;

    int sub_serial;
    struct sub_list_node *sub_lists[SUB_HASH_SIZE];
    struct sub_list_node *done_list;
    int pending_sub;

    char *nspace;

#ifdef HAVE_DRC
    uint32_t drc_credential_id;
#endif /* HAVE_DRC */

    ABT_mutex ls_mutex;
    ABT_mutex drain_mutex;
    ABT_mutex sub_mutex;
    ABT_cond drain_cond;
    ABT_cond sub_cond;

    ABT_xstream listener_xs;
};

DECLARE_MARGO_RPC_HANDLER(get_local_rpc)
DECLARE_MARGO_RPC_HANDLER(drain_rpc)
static void drain_rpc(hg_handle_t h);
DECLARE_MARGO_RPC_HANDLER(kill_client_rpc)
static void kill_client_rpc(hg_handle_t h);
DECLARE_MARGO_RPC_HANDLER(notify_rpc)
static void notify_rpc(hg_handle_t h);

// round robin fashion
// based on how many clients processes are connected to the server
static hg_return_t get_server_address(dspaces_client_t client,
                                      hg_addr_t *server_addr)
{
    return (margo_addr_lookup(
        client->mid, client->server_address[client->my_server], server_addr));
}

static hg_return_t get_meta_server_address(dspaces_client_t client,
                                           hg_addr_t *server_addr)
{
    return (
        margo_addr_lookup(client->mid, client->server_address[0], server_addr));
}

static void choose_server(dspaces_client_t client)
{
    int match_count = 0;
    int i;

    gethostname(client->my_node_name, HOST_NAME_MAX);

    for(i = 0; i < client->size_sp; i++) {
        if(strcmp(client->my_node_name, client->node_names[i]) == 0) {
            match_count++;
        }
    }
    if(match_count) {
        DEBUG_OUT("found %i servers that share a node with me.\n", match_count);
        match_count = client->rank % match_count;
        for(i = 0; i < client->size_sp; i++) {
            if(strcmp(client->my_node_name, client->node_names[i]) == 0) {
                if(match_count == 0) {
                    DEBUG_OUT("Attaching to server %i.\n", i);
                    client->my_server = i;
                    break;
                }
                match_count--;
            }
        }
    } else {
        client->my_server = client->rank % client->size_sp;
        DEBUG_OUT(
            "No on-node servers found. Attaching round-robin to server %i.\n",
            client->my_server);
        return;
    }
}

static int get_ss_info(dspaces_client_t client, ss_info_hdr *ss_data)
{
    hg_return_t hret;
    hg_handle_t handle;
    ss_information out;
    hg_addr_t server_addr;
    int ret = dspaces_SUCCESS;

    get_server_address(client, &server_addr);

    /* create handle */
    hret = margo_create(client->mid, server_addr, client->ss_id, &handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
        return dspaces_ERR_MERCURY;
    }

    DEBUG_OUT("Sending ss_rpc\n");
    hret = margo_forward(handle, NULL);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s):  margo_forward() failed\n", __func__);
        margo_destroy(handle);
        return dspaces_ERR_MERCURY;
    }
    DEBUG_OUT("Got ss_rpc reply\n");
    hret = margo_get_output(handle, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_get_output() failed with %i\n",
                __func__, hret);
        margo_destroy(handle);
        return dspaces_ERR_MERCURY;
    }
    memcpy(ss_data, out.ss_buf.raw_odsc, sizeof(ss_info_hdr));

    margo_free_output(handle, &out);
    margo_destroy(handle);
    margo_addr_free(client->mid, server_addr);

    return (ret);
}

static void install_ss_info(dspaces_client_t client, ss_info_hdr *ss_data)
{
    client->dcg->ss_info.num_dims = ss_data->num_dims;
    client->dcg->ss_info.num_space_srv = ss_data->num_space_srv;
    memcpy(&(client->dcg->ss_domain), &(ss_data->ss_domain),
           sizeof(struct bbox));
    client->dcg->max_versions = ss_data->max_versions;
    client->dcg->hash_version = ss_data->hash_version;
    memcpy(&(client->dcg->default_gdim), &(ss_data->default_gdim),
           sizeof(struct global_dimension));
}

static int init_ss_info(dspaces_client_t client)
{
    ss_info_hdr ss_data;
    int ret;

    ret = get_ss_info(client, &ss_data);
    if(ret == dspaces_SUCCESS) {
        install_ss_info(client, &ss_data);
    }

    return (ret);
}

static int init_ss_info_mpi(dspaces_client_t client, MPI_Comm comm)
{
    ss_info_hdr ss_data;
    int rank, ret;

    MPI_Comm_rank(comm, &rank);
    if(!rank) {
        ret = get_ss_info(client, &ss_data);
    }
    MPI_Bcast(&ret, 1, MPI_INT, 0, comm);
    if(ret == dspaces_SUCCESS) {
        MPI_Bcast(&ss_data, sizeof(ss_data), MPI_BYTE, 0, comm);
        install_ss_info(client, &ss_data);
    }

    return (ret);
}

static struct dc_gspace *dcg_alloc(dspaces_client_t client)
{
    struct dc_gspace *dcg_l;

    (void)client;
    dcg_l = calloc(1, sizeof(*dcg_l));
    if(!dcg_l)
        goto err_out;

    INIT_LIST_HEAD(&dcg_l->locks_list);
    init_gdim_list(&dcg_l->gdim_list);
    dcg_l->hash_version = ssd_hash_version_v1; // set default hash versio
    return dcg_l;

err_out:
    fprintf(stderr, "'%s()': failed.\n", __func__);
    return NULL;
}

FILE *open_conf_ds(dspaces_client_t client)
{
    int wait_time, time = 0;
    FILE *fd;

    do {
        fd = fopen("conf.ds", "r");
        if(!fd) {
            if(errno == ENOENT) {
                DEBUG_OUT("unable to find config file 'conf.ds' after %d "
                          "seconds, will try again...\n",
                          time);
            } else {
                fprintf(stderr, "could not open config file 'conf.ds'.\n");
                return (NULL);
            }
        } else {
            break;
        }
        wait_time = (rand() % 3) + 1;
        time += wait_time;
        sleep(wait_time);
    } while(!fd);

    return (fd);
}

static int read_conf(dspaces_client_t client, char **listen_addr_str)
{
    int size;
    FILE *fd;
    fpos_t lstart;
    int i, ret;

    ret = -1;
    fd = open_conf_ds(client);
    if(!fd) {
        goto fini;
    }

    ignore_result(fscanf(fd, "%d\n", &client->size_sp));
    client->server_address =
        malloc(client->size_sp * sizeof(*client->server_address));
    client->node_names = malloc(client->size_sp * sizeof(*client->node_names));
    for(i = 0; i < client->size_sp; i++) {
        fgetpos(fd, &lstart);
        ignore_result(fscanf(fd, "%*s%n", &size));
        client->node_names[i] = malloc(size + 1);
        ignore_result(fscanf(fd, "%*s%n\n", &size));
        client->server_address[i] = malloc(size + 1);
        fsetpos(fd, &lstart);
        ignore_result(fscanf(fd, "%s %s\n", client->node_names[i],
                             client->server_address[i]));
    }
    fgetpos(fd, &lstart);
    ignore_result(fscanf(fd, "%*s%n\n", &size));
    fsetpos(fd, &lstart);
    *listen_addr_str = malloc(size + 1);
    ignore_result(fscanf(fd, "%s\n", *listen_addr_str));

#ifdef HAVE_DRC
    fgetpos(fd, &lstart);
    ignore_result(fscanf(fd, "%" SCNu32, &client->drc_credential_id));
#endif
    fclose(fd);

    ret = 0;

fini:
    return ret;
}

static int read_conf_mpi(dspaces_client_t client, MPI_Comm comm,
                         char **listen_addr_str)
{
    FILE *fd, *conf;
    struct stat st;
    char *file_buf;
    int file_len;
    int rank;
    int size;
    fpos_t lstart;
    int i;

    MPI_Comm_rank(comm, &rank);
    if(rank == 0) {
        fd = open_conf_ds(client);
        if(fd == NULL) {
            file_len = -1;
        } else {
            fstat(fileno(fd), &st);
            file_len = st.st_size;
        }
    }
    MPI_Bcast(&file_len, 1, MPI_INT, 0, comm);
    if(file_len == -1) {
        return (-1);
    }
    file_buf = malloc(file_len);
    if(rank == 0) {
        ignore_result(fread(file_buf, 1, file_len, fd));
        fclose(fd);
    }
    MPI_Bcast(file_buf, file_len, MPI_BYTE, 0, comm);

    conf = fmemopen(file_buf, file_len, "r");
    ignore_result(fscanf(conf, "%d\n", &client->size_sp));
    client->server_address =
        malloc(client->size_sp * sizeof(*client->server_address));
    client->node_names = malloc(client->size_sp * sizeof(*client->node_names));
    for(i = 0; i < client->size_sp; i++) {
        fgetpos(conf, &lstart);
        fgetpos(conf, &lstart);
        ignore_result(fscanf(conf, "%*s%n", &size));
        client->node_names[i] = malloc(size + 1);
        ignore_result(fscanf(conf, "%*s%n\n", &size));
        client->server_address[i] = malloc(size + 1);
        fsetpos(conf, &lstart);
        ignore_result(fscanf(conf, "%s %s\n", client->node_names[i],
                             client->server_address[i]));
    }
    fgetpos(conf, &lstart);
    ignore_result(fscanf(conf, "%*s%n\n", &size));
    fsetpos(conf, &lstart);
    *listen_addr_str = malloc(size + 1);
    ignore_result(fscanf(conf, "%s\n", *listen_addr_str));

#ifdef HAVE_DRC
    fgetpos(conf, &lstart);
    fscanf(conf, "%" SCNu32, &client->drc_credential_id);
#endif

    fclose(conf);
    free(file_buf);

    return (0);
}

static int dspaces_init_internal(int rank, dspaces_client_t *c)
{
    const char *envdebug = getenv("DSPACES_DEBUG");

    if(g_is_initialized) {
        fprintf(stderr,
                "DATASPACES: WARNING: %s: multiple instantiations of the "
                "dataspaces client are not supported.\n",
                __func__);
        return (dspaces_ERR_ALLOCATION);
    }
    dspaces_client_t client = (dspaces_client_t)calloc(1, sizeof(*client));
    if(!client)
        return dspaces_ERR_ALLOCATION;

    if(envdebug) {
        client->f_debug = 1;
    }

    client->rank = rank;

    // now do dcg_alloc and store gid
    client->dcg = dcg_alloc(client);

    if(!(client->dcg))
        return dspaces_ERR_ALLOCATION;

    g_is_initialized = 1;

    *c = client;

    return dspaces_SUCCESS;
}

static int dspaces_init_margo(dspaces_client_t client,
                              const char *listen_addr_str)
{
    hg_class_t *hg;
    struct hg_init_info hii = {0};
    char margo_conf[1024];
    struct margo_init_info mii = {0};

    int i;

    margo_set_environment(NULL);
    sprintf(margo_conf,
            "{ \"use_progress_thread\" : false, \"rpc_thread_count\" : 0, "
            "\"handle_cache_size\" : 64}");
    hii.request_post_init = 1024;
    hii.auto_sm = 0;
    mii.hg_init_info = &hii;
    mii.json_config = margo_conf;
    ABT_init(0, NULL);

#ifdef HAVE_DRC
    int ret = 0;
    drc_info_handle_t drc_credential_info;
    uint32_t drc_cookie;
    char drc_key_str[256] = {0};

    ret = drc_access(client->drc_credential_id, 0, &drc_credential_info);
    if(ret != DRC_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): drc_access failure %d\n", __func__, ret);
        return ret;
    }

    drc_cookie = drc_get_first_cookie(drc_credential_info);
    sprintf(drc_key_str, "%u", drc_cookie);

    hii.na_init_info.auth_key = drc_key_str;

    client->mid = margo_init_ext(listen_addr_str, MARGO_SERVER_MODE, &mii);

#else

    client->mid = margo_init_ext(listen_addr_str, MARGO_SERVER_MODE, &mii);
    if(client->mid && client->f_debug) {
        if(!client->rank) {
            char *margo_json = margo_get_config(client->mid);
            fprintf(stderr, "%s", margo_json);
            free(margo_json);
        }
        margo_set_log_level(client->mid, MARGO_LOG_WARNING);
    }

#endif /* HAVE_DRC */
    if(!client->mid) {
        fprintf(stderr, "ERROR: %s: margo_init() failed.\n", __func__);
        return (dspaces_ERR_MERCURY);
    }

    hg = margo_get_class(client->mid);

    ABT_mutex_create(&client->ls_mutex);
    ABT_mutex_create(&client->drain_mutex);
    ABT_mutex_create(&client->sub_mutex);
    ABT_cond_create(&client->drain_cond);
    ABT_cond_create(&client->sub_cond);

    for(i = 0; i < SUB_HASH_SIZE; i++) {
        client->sub_lists[i] = NULL;
    }
    client->done_list = NULL;
    client->sub_serial = 0;
    client->pending_sub = 0;

    /* check if RPCs have already been registered */
    hg_bool_t flag;
    hg_id_t id;
    margo_registered_name(client->mid, "put_rpc", &id, &flag);

    if(flag == HG_TRUE) { /* RPCs already registered */
        margo_registered_name(client->mid, "put_rpc", &client->put_id, &flag);
        margo_registered_name(client->mid, "put_local_rpc",
                              &client->put_local_id, &flag);
        margo_registered_name(client->mid, "put_meta_rpc", &client->put_meta_id,
                              &flag);
        margo_registered_name(client->mid, "get_rpc", &client->get_id, &flag);
        margo_registered_name(client->mid, "get_local_rpc",
                              &client->get_local_id, &flag);
        DS_HG_REGISTER(hg, client->get_local_id, bulk_in_t, bulk_out_t,
                       get_local_rpc);
        margo_registered_name(client->mid, "query_rpc", &client->query_id,
                              &flag);
        margo_registered_name(client->mid, "ss_rpc", &client->ss_id, &flag);
        margo_registered_name(client->mid, "drain_rpc", &client->drain_id,
                              &flag);
        DS_HG_REGISTER(hg, client->drain_id, bulk_in_t, bulk_out_t, drain_rpc);
        margo_registered_name(client->mid, "kill_rpc", &client->kill_id, &flag);
        margo_registered_name(client->mid, "kill_client_rpc",
                              &client->kill_client_id, &flag);
        DS_HG_REGISTER(hg, client->kill_client_id, int32_t, void,
                       kill_client_rpc);
        margo_registered_name(client->mid, "sub_rpc", &client->sub_id, &flag);
        margo_registered_name(client->mid, "notify_rpc", &client->notify_id,
                              &flag);
        DS_HG_REGISTER(hg, client->notify_id, odsc_list_t, void, notify_rpc);
        margo_registered_name(client->mid, "query_meta_rpc",
                              &client->query_meta_id, &flag);
        margo_registered_name(client->mid, "do_ops_rpc", &client->do_ops_id,
                              &flag);
        margo_registered_name(client->mid, "pexec_rpc", &client->pexec_id,
                              &flag);
        margo_registered_name(client->mid, "mpexec_rpc", &client->pexec_id,
                              &flag);
        margo_registered_name(client->mid, "cond_rpc", &client->cond_id, &flag);
        margo_registered_name(client->mid, "get_vars_rpc", &client->get_vars_id,
                              &flag);
        margo_registered_name(client->mid, "get_var_objs_rpc",
                              &client->get_var_objs_id, &flag);
        margo_registered_name(client->mid, "reg_rpc", &client->reg_id, &flag);
    } else {
        client->put_id = MARGO_REGISTER(client->mid, "put_rpc", bulk_gdim_t,
                                        bulk_out_t, NULL);
        client->put_local_id = MARGO_REGISTER(client->mid, "put_local_rpc",
                                              odsc_gdim_t, bulk_out_t, NULL);
        client->put_meta_id = MARGO_REGISTER(client->mid, "put_meta_rpc",
                                             put_meta_in_t, bulk_out_t, NULL);
        margo_register_data(client->mid, client->put_meta_id, (void *)client,
                            NULL);
        client->get_id =
            MARGO_REGISTER(client->mid, "get_rpc", bulk_in_t, bulk_out_t, NULL);
        margo_register_data(client->mid, client->get_id, (void *)client, NULL);
        client->get_local_id = MARGO_REGISTER(
            client->mid, "get_local_rpc", bulk_in_t, bulk_out_t, get_local_rpc);
        margo_register_data(client->mid, client->get_local_id, (void *)client,
                            NULL);
        client->query_id = MARGO_REGISTER(client->mid, "query_rpc", odsc_gdim_t,
                                          odsc_list_t, NULL);
        client->query_meta_id =
            MARGO_REGISTER(client->mid, "query_meta_rpc", query_meta_in_t,
                           query_meta_out_t, NULL);
        client->ss_id =
            MARGO_REGISTER(client->mid, "ss_rpc", void, ss_information, NULL);
        client->drain_id = MARGO_REGISTER(client->mid, "drain_rpc", bulk_in_t,
                                          bulk_out_t, drain_rpc);
        margo_register_data(client->mid, client->drain_id, (void *)client,
                            NULL);
        client->kill_id =
            MARGO_REGISTER(client->mid, "kill_rpc", int32_t, void, NULL);
        margo_registered_disable_response(client->mid, client->kill_id,
                                          HG_TRUE);
        margo_register_data(client->mid, client->kill_id, (void *)client, NULL);
        client->kill_client_id = MARGO_REGISTER(client->mid, "kill_client_rpc",
                                                int32_t, void, kill_client_rpc);
        margo_registered_disable_response(client->mid, client->kill_client_id,
                                          HG_TRUE);
        margo_register_data(client->mid, client->kill_client_id, (void *)client,
                            NULL);
        client->sub_id =
            MARGO_REGISTER(client->mid, "sub_rpc", odsc_gdim_t, void, NULL);
        margo_registered_disable_response(client->mid, client->sub_id, HG_TRUE);
        client->notify_id = MARGO_REGISTER(client->mid, "notify_rpc",
                                           odsc_list_t, void, notify_rpc);
        margo_register_data(client->mid, client->notify_id, (void *)client,
                            NULL);
        margo_registered_disable_response(client->mid, client->notify_id,
                                          HG_TRUE);
        client->do_ops_id = MARGO_REGISTER(client->mid, "do_ops_rpc",
                                           do_ops_in_t, bulk_out_t, NULL);
        client->pexec_id = MARGO_REGISTER(client->mid, "pexec_rpc", pexec_in_t,
                                          pexec_out_t, NULL);
        client->mpexec_id = MARGO_REGISTER(client->mid, "mpexec_rpc",
                                           pexec_in_t, pexec_out_t, NULL);
        client->cond_id =
            MARGO_REGISTER(client->mid, "cond_rpc", cond_in_t, void, NULL);
        margo_registered_disable_response(client->mid, client->cond_id,
                                          HG_TRUE);
        client->get_vars_id = MARGO_REGISTER(client->mid, "get_vars_rpc",
                                             int32_t, name_list_t, NULL);
        client->get_var_objs_id = MARGO_REGISTER(
            client->mid, "get_var_objs_rpc", get_var_objs_in_t, odsc_hdr, NULL);
        client->reg_id =
            MARGO_REGISTER(client->mid, "reg_rpc", reg_in_t, uint64_t, NULL);
    }

    return (dspaces_SUCCESS);
}

static int dspaces_post_init(dspaces_client_t client)
{
    choose_server(client);

    DEBUG_OUT("Total max versions on the client side is %d\n",
              client->dcg->max_versions);

    client->dcg->ls = ls_alloc(client->dcg->max_versions);
    client->local_put_count = 0;
    client->f_final = 0;

    return (dspaces_SUCCESS);
}

int dspaces_init(int rank, dspaces_client_t *c)
{
    dspaces_client_t client;
    char *listen_addr_str;
    int ret;

    ret = dspaces_init_internal(rank, &client);
    if(ret != dspaces_SUCCESS) {
        return (ret);
    }

    ret = read_conf(client, &listen_addr_str);
    if(ret != 0) {
        return (ret);
    }

    ret = dspaces_init_margo(client, listen_addr_str);

    free(listen_addr_str);
    if(ret != 0) {
        return (ret);
    }

    choose_server(client);
    ret = init_ss_info(client);
    if(ret != dspaces_SUCCESS) {
        return (ret);
    }
    dspaces_post_init(client);

    *c = client;

    return dspaces_SUCCESS;
}

int dspaces_init_mpi(MPI_Comm comm, dspaces_client_t *c)
{
    dspaces_client_t client;
    int rank;
    char *listen_addr_str;
    int ret;

    MPI_Comm_rank(comm, &rank);

    ret = dspaces_init_internal(rank, &client);
    if(ret != dspaces_SUCCESS) {
        return (ret);
    }

    ret = read_conf_mpi(client, comm, &listen_addr_str);
    if(ret != 0) {
        return (ret);
    }
    ret = dspaces_init_margo(client, listen_addr_str);
    free(listen_addr_str);
    if(ret != 0) {
        return (ret);
    }

    choose_server(client);
    ret = init_ss_info_mpi(client, comm);
    if(ret != dspaces_SUCCESS) {
        return (ret);
    }
    dspaces_post_init(client);

    *c = client;

    return (dspaces_SUCCESS);
}

static int arg_conf(dspaces_client_t client, const char *conn_str)
{
    client->size_sp = 1;
    client->server_address = malloc(sizeof(*client->server_address));
    client->node_names = malloc(sizeof(*client->node_names));
    client->node_names[0] = strdup("remote");
    client->server_address[0] = strdup(conn_str);

    return (0);
}

int dspaces_init_wan(const char *listen_addr_str, const char *conn_str,
                     int rank, dspaces_client_t *c)
{
    dspaces_client_t client;
    int ret;

    ret = dspaces_init_internal(rank, &client);
    if(ret != dspaces_SUCCESS) {
        return (ret);
    }

    ret = arg_conf(client, conn_str);
    if(ret != 0) {
        return (ret);
    }
    dspaces_init_margo(client, listen_addr_str);
    if(ret != 0) {
        return (ret);
    }

    choose_server(client);
    ret =init_ss_info(client);
    if(ret != dspaces_SUCCESS) {
        return (ret);
    }
    dspaces_post_init(client);

    *c = client;

    return (dspaces_SUCCESS);
}

int dspaces_init_wan_mpi(const char *listen_addr_str, const char *conn_str,
                         MPI_Comm comm, dspaces_client_t *c)
{
    dspaces_client_t client;
    int rank;
    int ret;

    MPI_Comm_rank(comm, &rank);

    ret = dspaces_init_internal(rank, &client);
    if(ret != dspaces_SUCCESS) {
        return (ret);
    }

    ret = arg_conf(client, conn_str);
    if(ret != 0) {
        return (ret);
    }
    ret = dspaces_init_margo(client, listen_addr_str);
    if(ret != 0) {
        return (ret);
    }

    choose_server(client);
    ret = init_ss_info_mpi(client, comm);
    if(ret != dspaces_SUCCESS) {
        return (ret);
    }
    dspaces_post_init(client);

    *c = client;

    return (dspaces_SUCCESS);
}

int dspaces_server_count(dspaces_client_t client) { return (client->size_sp); }

static void free_done_list(dspaces_client_t client)
{
    struct sub_list_node *node;

    while(client->done_list) {
        node = client->done_list;
        client->done_list = node->next;
        free(node->subh);
        free(node);
    }
}

int dspaces_fini(dspaces_client_t client)
{
    DEBUG_OUT("finalizing.\n");

    ABT_mutex_lock(client->sub_mutex);
    while(client->pending_sub > 0) {
        DEBUG_OUT("Pending subscriptions: %d\n", client->pending_sub);
        ABT_cond_wait(client->sub_cond, client->sub_mutex);
    }
    ABT_mutex_unlock(client->sub_mutex);

    free_done_list(client);

    do { // watch out for spurious wake
        ABT_mutex_lock(client->drain_mutex);
        client->f_final = 1;

        if(client->local_put_count > 0) {
            DEBUG_OUT("waiting for pending drainage. %d object remain.\n",
                      client->local_put_count);
            ABT_cond_wait(client->drain_cond, client->drain_mutex);
            DEBUG_OUT("received drainage signal.\n");
        }
        ABT_mutex_unlock(client->drain_mutex);
    } while(client->local_put_count > 0);

    while(client->put_reqs) {
        dspaces_check_put(client, client->put_reqs, 1);
    }

    DEBUG_OUT("all objects drained. Finalizing...\n");

    free_gdim_list(&client->dcg->gdim_list);
    free(client->server_address[0]);
    free(client->server_address);
    ls_free(client->dcg->ls);
    free(client->dcg);

    margo_finalize(client->mid);

    free(client);

    g_is_initialized = 0;

    return dspaces_SUCCESS;
}

void dspaces_define_gdim(dspaces_client_t client, const char *var_name,
                         int ndim, uint64_t *gdim)
{
    char *long_vname = NULL;
    const char *fq_vname = NULL;

    if(client->nspace) {
        long_vname = malloc(strlen(client->nspace + strlen(var_name) + 2));
        sprintf(long_vname, "%s\\%s", client->nspace, var_name);
        fq_vname = long_vname;
    } else {
        fq_vname = var_name;
    }

    if(ndim > BBOX_MAX_NDIM) {
        fprintf(stderr, "ERROR: %s: maximum object dimensionality is %d\n",
                __func__, BBOX_MAX_NDIM);
    } else {
        update_gdim_list(&(client->dcg->gdim_list), fq_vname, ndim, gdim);
    }

    if(long_vname) {
        free(long_vname);
    }
}

void dspaces_get_gdim(dspaces_client_t client, const char *var_name, int *ndim,
                      uint64_t *gdims)
{
    char *long_vname = NULL;
    const char *fq_vname = NULL;
    struct global_dimension gdim;

    if(client->nspace) {
        long_vname = malloc(strlen(client->nspace + strlen(var_name) + 2));
        sprintf(long_vname, "%s\\%s", client->nspace, var_name);
        fq_vname = long_vname;
    } else {
        fq_vname = var_name;
    }

    set_global_dimension(&(client->dcg->gdim_list), fq_vname,
                         &(client->dcg->default_gdim), &gdim);
    get_global_dimensions(&gdim, ndim, gdims);

    if(long_vname) {
        free(long_vname);
    }
}

static void copy_var_name_to_odsc(dspaces_client_t client, const char *var_name,
                                  obj_descriptor *odsc)
{
    if(client->nspace) {
        strncpy(odsc->name, client->nspace, sizeof(odsc->name) - 1);
        odsc->name[strlen(client->nspace)] = '\\';
        strncpy(&odsc->name[strlen(client->nspace) + 1], var_name,
                (sizeof(odsc->name) - strlen(client->nspace)) - 1);
    } else {
        strncpy(odsc->name, var_name, sizeof(odsc->name) - 1);
    }
    odsc->name[sizeof(odsc->name) - 1] = '\0';
}

static int setup_put(dspaces_client_t client, const char *var_name,
                     unsigned int ver, int elem_size, int ndim, uint64_t *lb,
                     uint64_t *ub, const void *data, hg_addr_t *server_addr,
                     hg_handle_t *handle, bulk_gdim_t *in)
{
    hg_return_t hret;
    int ret = dspaces_SUCCESS;

    obj_descriptor odsc = {.version = ver,
                           .owner = {0},
                           .st = st,
                           .flags = 0,
                           .size = elem_size,
                           .bb = {
                               .num_dims = ndim,
                           }};

    memset(odsc.bb.lb.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    memset(odsc.bb.ub.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);

    memcpy(odsc.bb.lb.c, lb, sizeof(uint64_t) * ndim);
    memcpy(odsc.bb.ub.c, ub, sizeof(uint64_t) * ndim);

    copy_var_name_to_odsc(client, var_name, &odsc);

    struct global_dimension odsc_gdim;
    set_global_dimension(&(client->dcg->gdim_list), odsc.name,
                         &(client->dcg->default_gdim), &odsc_gdim);

    in->odsc.size = sizeof(odsc);
    in->odsc.raw_odsc = (char *)(&odsc);
    in->odsc.gdim_size = sizeof(struct global_dimension);
    in->odsc.raw_gdim = (char *)(&odsc_gdim);
    hg_size_t rdma_size = (elem_size)*bbox_volume(&odsc.bb);

    DEBUG_OUT("sending object %s \n", obj_desc_sprint(&odsc));

    hret = margo_bulk_create(client->mid, 1, (void **)&data, &rdma_size,
                             HG_BULK_READ_ONLY, &in->handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_bulk_create() failed\n", __func__);
        return dspaces_ERR_MERCURY;
    }

    get_server_address(client, server_addr);
    /* create handle */
    hret = margo_create(client->mid, *server_addr, client->put_id, handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
        margo_bulk_free(in->handle);
        return dspaces_ERR_MERCURY;
    }

    return (0);
}

int dspaces_put(dspaces_client_t client, const char *var_name, unsigned int ver,
                int elem_size, int ndim, uint64_t *lb, uint64_t *ub,
                const void *data)
{
    return (dspaces_put_tag(client, var_name, ver, elem_size, 0, ndim, lb, ub,
                            data));
}

int dspaces_put_tag(dspaces_client_t client, const char *var_name,
                    unsigned int ver, int elem_size, int tag, int ndim,
                    uint64_t *lb, uint64_t *ub, const void *data)
{
    hg_addr_t server_addr;
    hg_handle_t handle;
    hg_return_t hret;
    bulk_gdim_t in;
    bulk_out_t out;
    int type;
    int ret = dspaces_SUCCESS;

    if(elem_size < 0) {
        type = elem_size;
        elem_size = type_to_size(type);
    }

    obj_descriptor odsc = {.version = ver,
                           .owner = {0},
                           .st = st,
                           .flags = 0,
                           .tag = tag,
                           .type = type,
                           .size = elem_size,
                           .bb = {
                               .num_dims = ndim,
                           }};

    memset(odsc.bb.lb.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    memset(odsc.bb.ub.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);

    memcpy(odsc.bb.lb.c, lb, sizeof(uint64_t) * ndim);
    memcpy(odsc.bb.ub.c, ub, sizeof(uint64_t) * ndim);

    copy_var_name_to_odsc(client, var_name, &odsc);

    struct global_dimension odsc_gdim;
    set_global_dimension(&(client->dcg->gdim_list), odsc.name,
                         &(client->dcg->default_gdim), &odsc_gdim);

    in.odsc.size = sizeof(odsc);
    in.odsc.raw_odsc = (char *)(&odsc);
    in.odsc.gdim_size = sizeof(struct global_dimension);
    in.odsc.raw_gdim = (char *)(&odsc_gdim);
    hg_size_t rdma_size = (elem_size)*bbox_volume(&odsc.bb);

    DEBUG_OUT("sending object %s \n", obj_desc_sprint(&odsc));

    hret = margo_bulk_create(client->mid, 1, (void **)&data, &rdma_size,
                             HG_BULK_READ_ONLY, &in.handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_bulk_create() failed\n", __func__);
        return dspaces_ERR_MERCURY;
    }

    get_server_address(client, &server_addr);

    hret = margo_create(client->mid, server_addr, client->put_id, &handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
        margo_bulk_free(in.handle);
        return dspaces_ERR_MERCURY;
    }

    hret = margo_forward(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_forward() failed\n", __func__);
        margo_bulk_free(in.handle);
        margo_destroy(handle);
        return dspaces_ERR_MERCURY;
    }

    hret = margo_get_output(handle, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_get_output() failed\n", __func__);
        margo_bulk_free(in.handle);
        margo_destroy(handle);
        return dspaces_ERR_MERCURY;
    }

    ret = out.ret;
    margo_free_output(handle, &out);
    margo_bulk_free(in.handle);
    margo_destroy(handle);
    margo_addr_free(client->mid, server_addr);
    return ret;
}

static int finalize_req(struct dspaces_put_req *req)
{
    bulk_out_t out;
    int ret;
    hg_return_t hret;

    hret = margo_get_output(req->handle, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_get_output() failed\n", __func__);
        margo_bulk_free(req->in.handle);
        margo_destroy(req->handle);
        return dspaces_ERR_MERCURY;
    }
    ret = out.ret;
    margo_free_output(req->handle, &out);
    margo_bulk_free(req->in.handle);
    margo_destroy(req->handle);

    if(req->buffer) {
        free(req->buffer);
        req->buffer = NULL;
    }

    req->finalized = 1;
    req->ret = ret;

    return ret;
}

struct dspaces_put_req *dspaces_iput(dspaces_client_t client,
                                     const char *var_name, unsigned int ver,
                                     int elem_size, int ndim, uint64_t *lb,
                                     uint64_t *ub, void *data, int alloc,
                                     int check, int free)
{
    hg_addr_t server_addr;
    hg_return_t hret;
    struct dspaces_put_req *ds_req, *ds_req_prev, **ds_req_p;
    int ret = dspaces_SUCCESS;
    const void *buffer;
    int flag;

    if(check) {
        // Check for comleted iputs
        ds_req_prev = NULL;
        ds_req_p = &client->put_reqs;
        while(*ds_req_p) {
            ds_req = *ds_req_p;
            flag = 0;
            if(!ds_req->finalized) {
                margo_test(ds_req->req, &flag);
                if(flag) {
                    finalize_req(ds_req);
                    if(!ds_req->next) {
                        client->put_reqs_end = ds_req_prev;
                    }
                    *ds_req_p = ds_req->next;
                    // do not free ds_req yet - user might do a
                    // dspaces_check_put later
                }
            }
            ds_req_prev = ds_req;
            ds_req_p = &ds_req->next;
        }
    }

    ds_req = calloc(1, sizeof(*ds_req));
    obj_descriptor odsc = {.version = ver,
                           .owner = {0},
                           .st = st,
                           .flags = 0,
                           .size = elem_size,
                           .bb = {
                               .num_dims = ndim,
                           }};

    memset(odsc.bb.lb.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    memset(odsc.bb.ub.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);

    memcpy(odsc.bb.lb.c, lb, sizeof(uint64_t) * ndim);
    memcpy(odsc.bb.ub.c, ub, sizeof(uint64_t) * ndim);

    copy_var_name_to_odsc(client, var_name, &odsc);

    struct global_dimension odsc_gdim;
    set_global_dimension(&(client->dcg->gdim_list), odsc.name,
                         &(client->dcg->default_gdim), &odsc_gdim);

    ds_req->in.odsc.size = sizeof(odsc);
    ds_req->in.odsc.raw_odsc = (char *)(&odsc);
    ds_req->in.odsc.gdim_size = sizeof(struct global_dimension);
    ds_req->in.odsc.raw_gdim = (char *)(&odsc_gdim);
    hg_size_t rdma_size = (elem_size)*bbox_volume(&odsc.bb);

    if(alloc) {
        ds_req->buffer = malloc(rdma_size);
        memcpy(ds_req->buffer, data, rdma_size);
        buffer = ds_req->buffer;
    } else {
        buffer = data;
        if(free) {
            ds_req->buffer = data;
        }
    }

    DEBUG_OUT("sending object %s \n", obj_desc_sprint(&odsc));

    hret = margo_bulk_create(client->mid, 1, (void **)&buffer, &rdma_size,
                             HG_BULK_READ_ONLY, &ds_req->in.handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_bulk_create() failed\n", __func__);
        return dspaces_PUT_NULL;
    }

    get_server_address(client, &server_addr);

    hret =
        margo_create(client->mid, server_addr, client->put_id, &ds_req->handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
        margo_bulk_free(ds_req->in.handle);
        return dspaces_PUT_NULL;
    }

    hret = margo_iforward(ds_req->handle, &ds_req->in, &ds_req->req);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_forward() failed\n", __func__);
        margo_bulk_free(ds_req->in.handle);
        margo_destroy(ds_req->handle);
        return dspaces_PUT_NULL;
    }

    margo_addr_free(client->mid, server_addr);

    ds_req->next = NULL;
    if(client->put_reqs_end) {
        client->put_reqs_end->next = ds_req;
        client->put_reqs_end = ds_req;
    } else {
        client->put_reqs = client->put_reqs_end = ds_req;
    }

    return ds_req;
}

int dspaces_check_put(dspaces_client_t client, struct dspaces_put_req *req,
                      int wait)
{
    int flag;
    struct dspaces_put_req **ds_req_p, *ds_req_prev;
    int ret;
    hg_return_t hret;

    if(req->finalized) {
        ret = req->ret;
        free(req);
        return ret;
    }

    if(wait) {
        hret = margo_wait(req->req);
        if(hret == HG_SUCCESS) {
            ds_req_prev = NULL;
            ds_req_p = &client->put_reqs;
            while(*ds_req_p && *ds_req_p != req) {
                ds_req_prev = *ds_req_p;
                ds_req_p = &((*ds_req_p)->next);
            }
            if(!ds_req_p) {
                fprintf(stderr,
                        "ERROR: put req finished, but was not saved.\n");
                return (-1);
            } else {
                ret = finalize_req(req);
                if(req->next == NULL) {
                    client->put_reqs_end = ds_req_prev;
                }
                *ds_req_p = req->next;
                free(req);
                return ret;
            }
        }
        return (hret);
    } else {
        margo_test(req->req, &flag);
        if(flag) {
            ds_req_prev = NULL;
            ds_req_p = &client->put_reqs;
            while(*ds_req_p && *ds_req_p != req) {
                ds_req_prev = *ds_req_p;
                ds_req_p = &((*ds_req_p)->next);
            }
            if(!ds_req_p) {
                fprintf(stderr,
                        "ERROR: put req finished, but was not saved.\n");
                return (-1);
            } else {
                ret = finalize_req(req);
                if(req->next == NULL) {
                    client->put_reqs_end = ds_req_prev;
                }
                *ds_req_p = req->next;
                free(req);
            }
        }
        return flag;
    }
}

static int get_data(dspaces_client_t client, int num_odscs,
                    obj_descriptor req_obj, obj_descriptor *odsc_tab,
                    void *data)
{
    struct timeval start, stop;
    bulk_in_t *in;
    struct obj_data **od;
    margo_request *serv_req;
    hg_handle_t *hndl;
    hg_size_t *rdma_size;
    size_t max_size = 0;
    void *ucbuffer;
    int ret;
    hg_return_t hret;

    in = (bulk_in_t *)calloc(sizeof(bulk_in_t), num_odscs);
    od = malloc(num_odscs * sizeof(struct obj_data *));
    hndl = (hg_handle_t *)malloc(sizeof(hg_handle_t) * num_odscs);
    serv_req = (margo_request *)malloc(sizeof(margo_request) * num_odscs);
    rdma_size = (hg_size_t *)malloc(sizeof(*rdma_size) * num_odscs);

    gettimeofday(&start, NULL);

    for(int i = 0; i < num_odscs; ++i) {
        od[i] = obj_data_alloc(&odsc_tab[i]);
        in[i].odsc.size = sizeof(obj_descriptor);
        in[i].odsc.raw_odsc = (char *)(&odsc_tab[i]);

        rdma_size[i] = (req_obj.size) * bbox_volume(&odsc_tab[i].bb);

        DEBUG_OUT("For odsc %i, element size is %zi, and there are %" PRIu64
                  " elements to fetch.\n",
                  i, req_obj.size, bbox_volume(&odsc_tab[i].bb));

        DEBUG_OUT("creating bulk handle for buffer %p of size %" PRIu64 ".\n",
                  od[i]->data, rdma_size[i]);
        hret =
            margo_bulk_create(client->mid, 1, (void **)(&(od[i]->data)),
                              &rdma_size[i], HG_BULK_WRITE_ONLY, &in[i].handle);
        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: %s: margo_bulk_create failed with %i\n",
                    __func__, hret);
        }
        if(rdma_size[i] > max_size) {
            max_size = rdma_size[i];
        }

        hg_addr_t server_addr;
        margo_addr_lookup(client->mid, odsc_tab[i].owner, &server_addr);

        hg_handle_t handle;
        if(odsc_tab[i].flags & DS_CLIENT_STORAGE) {
            DEBUG_OUT("retrieving object from client-local storage.\n");
            margo_create(client->mid, server_addr, client->get_local_id,
                         &handle);
        } else {
            DEBUG_OUT("retrieving object from server storage.\n");
            margo_create(client->mid, server_addr, client->get_id, &handle);
        }
        margo_request req;
        // forward get requests
        margo_iforward(handle, &in[i], &req);
        hndl[i] = handle;
        serv_req[i] = req;
        margo_addr_free(client->mid, server_addr);
    }

    struct obj_data *return_od = obj_data_alloc_no_data(&req_obj, data);
    ucbuffer = malloc(max_size);

    // TODO: rewrite with margo_wait_any()
    for(int i = 0; i < num_odscs; ++i) {
        margo_wait(serv_req[i]);
        bulk_out_t resp;
        margo_get_output(hndl[i], &resp);

        if((!(odsc_tab[i].flags & DS_CLIENT_STORAGE)) && resp.len) {
            // decompress into buffer and copy back
            ret = LZ4_decompress_safe(od[i]->data, ucbuffer, resp.len,
                                      rdma_size[i]);
            DEBUG_OUT("decompressed from %" PRIu64 " to %i bytes\n", resp.len,
                      ret);
            if(ret != rdma_size[i]) {
                fprintf(stderr, "LZ4 decompression failed with %i.\n", ret);
            }
            memcpy(od[i]->data, ucbuffer, rdma_size[i]);
        } else if(!resp.len) {
            DEBUG_OUT("receive buffer is not compressed.\n");
        }

        // copy received data into user return buffer
        ssd_copy(return_od, od[i]);
        obj_data_free(od[i]);
        margo_free_output(hndl[i], &resp);
        margo_destroy(hndl[i]);
    }
    free(hndl);
    free(serv_req);
    free(in);
    free(return_od);
    free(rdma_size);
    free(ucbuffer);

    gettimeofday(&stop, NULL);

    if(client->f_debug) {
        uint64_t req_size = obj_data_size(&req_obj);
        long dsec = stop.tv_sec - start.tv_sec;
        long dusec = stop.tv_usec - start.tv_usec;
        float transfer_time = (float)dsec + (dusec / 1000000.0);
        DEBUG_OUT("got %" PRIu64 " bytes in %f sec\n", req_size, transfer_time);
    }

    return 0;
}

static int dspaces_init_listener(dspaces_client_t client)
{

    ABT_pool margo_pool;
    hg_return_t hret;
    int ret = dspaces_SUCCESS;

    hret = margo_get_handler_pool(client->mid, &margo_pool);
    if(hret != HG_SUCCESS || margo_pool == ABT_POOL_NULL) {
        fprintf(stderr, "ERROR: %s: could not get handler pool (%d).\n",
                __func__, hret);
        return (dspaces_ERR_ARGOBOTS);
    }
    client->listener_xs = ABT_XSTREAM_NULL;
    ret = ABT_xstream_create_basic(ABT_SCHED_BASIC_WAIT, 1, &margo_pool,
                                   ABT_SCHED_CONFIG_NULL, &client->listener_xs);
    if(ret != ABT_SUCCESS) {
        char err_str[1000];
        ABT_error_get_str(ret, err_str, NULL);
        fprintf(stderr, "ERROR: %s: could not launch handler thread: %s\n",
                __func__, err_str);
        return (dspaces_ERR_ARGOBOTS);
    }

    client->listener_init = 1;

    return (ret);
}

int dspaces_put_meta(dspaces_client_t client, const char *name, int version,
                     const void *data, unsigned int len)
{
    hg_addr_t server_addr;
    hg_handle_t handle;
    hg_size_t rdma_length = len;
    hg_return_t hret;
    put_meta_in_t in;
    bulk_out_t out;

    int ret = dspaces_SUCCESS;

    DEBUG_OUT("posting metadata for `%s`, version %d with length %i bytes.\n",
              name, version, len);

    in.name = strdup(name);
    in.length = len;
    in.version = version;
    hret = margo_bulk_create(client->mid, 1, (void **)&data, &rdma_length,
                             HG_BULK_READ_ONLY, &in.handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_bulk_create() failed\n", __func__);
        return dspaces_ERR_MERCURY;
    }

    get_meta_server_address(client, &server_addr);
    hret = margo_create(client->mid, server_addr, client->put_meta_id, &handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
        margo_bulk_free(in.handle);
        return dspaces_ERR_MERCURY;
    }

    hret = margo_forward(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_forward() failed\n", __func__);
        margo_bulk_free(in.handle);
        margo_destroy(handle);
        return dspaces_ERR_MERCURY;
    }

    hret = margo_get_output(handle, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_get_output() failed\n", __func__);
        margo_bulk_free(in.handle);
        margo_destroy(handle);
        return dspaces_ERR_MERCURY;
    }

    DEBUG_OUT("metadata posted successfully.\n");

    ret = out.ret;
    margo_free_output(handle, &out);
    margo_bulk_free(in.handle);
    margo_destroy(handle);
    margo_addr_free(client->mid, server_addr);

    return (ret);
}

int dspaces_put_local(dspaces_client_t client, const char *var_name,
                      unsigned int ver, int elem_size, int ndim, uint64_t *lb,
                      uint64_t *ub, void *data)
{
    hg_addr_t server_addr;
    hg_handle_t handle;
    hg_return_t hret;
    int ret = dspaces_SUCCESS;

    if(client->listener_init == 0) {
        ret = dspaces_init_listener(client);
        if(ret != dspaces_SUCCESS) {
            return (ret);
        }
    }

    client->local_put_count++;

    obj_descriptor odsc = {.version = ver,
                           .st = st,
                           .flags = DS_CLIENT_STORAGE,
                           .size = elem_size,
                           .bb = {
                               .num_dims = ndim,
                           }};

    hg_addr_t owner_addr;
    hg_size_t owner_addr_size = 128;

    margo_addr_self(client->mid, &owner_addr);
    margo_addr_to_string(client->mid, odsc.owner, &owner_addr_size, owner_addr);
    margo_addr_free(client->mid, owner_addr);

    memset(odsc.bb.lb.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    memset(odsc.bb.ub.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);

    memcpy(odsc.bb.lb.c, lb, sizeof(uint64_t) * ndim);
    memcpy(odsc.bb.ub.c, ub, sizeof(uint64_t) * ndim);

    copy_var_name_to_odsc(client, var_name, &odsc);

    odsc_gdim_t in;
    bulk_out_t out;
    struct obj_data *od;
    od = obj_data_alloc_with_data(&odsc, data);

    set_global_dimension(&(client->dcg->gdim_list), odsc.name,
                         &(client->dcg->default_gdim), &od->gdim);

    ABT_mutex_lock(client->ls_mutex);
    ls_add_obj(client->dcg->ls, od);
    DEBUG_OUT("Added into local_storage\n");
    ABT_mutex_unlock(client->ls_mutex);

    in.odsc_gdim.size = sizeof(odsc);
    in.odsc_gdim.raw_odsc = (char *)(&odsc);
    in.odsc_gdim.gdim_size = sizeof(struct global_dimension);
    in.odsc_gdim.raw_gdim = (char *)(&od->gdim);

    DEBUG_OUT("sending object information %s \n", obj_desc_sprint(&odsc));

    get_server_address(client, &server_addr);
    /* create handle */
    hret =
        margo_create(client->mid, server_addr, client->put_local_id, &handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
        return dspaces_ERR_MERCURY;
    }

    hret = margo_forward(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_forward() failed\n", __func__);
        margo_destroy(handle);
        return dspaces_ERR_MERCURY;
    }
    DEBUG_OUT("RPC sent, awaiting response.\n");

    hret = margo_get_output(handle, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s):  margo_get_output() failed\n", __func__);
        margo_destroy(handle);
        return dspaces_ERR_MERCURY;
    }

    ret = out.ret;
    margo_free_output(handle, &out);
    margo_destroy(handle);
    margo_addr_free(client->mid, server_addr);

    return ret;
}

static int get_odscs(dspaces_client_t client, obj_descriptor *odsc, int timeout,
                     obj_descriptor **odsc_tab)
{
    struct global_dimension od_gdim;
    int num_odscs;
    hg_addr_t server_addr;
    hg_return_t hret;
    hg_handle_t handle;

    odsc_gdim_t in;
    odsc_list_t out;

    in.odsc_gdim.size = sizeof(*odsc);
    in.odsc_gdim.raw_odsc = (char *)odsc;
    in.param = timeout;

    DEBUG_OUT("starting query.\n");
    set_global_dimension(&(client->dcg->gdim_list), odsc->name,
                         &(client->dcg->default_gdim), &od_gdim);
    in.odsc_gdim.gdim_size = sizeof(od_gdim);
    in.odsc_gdim.raw_gdim = (char *)(&od_gdim);
    DEBUG_OUT("Found gdims.\n");

    get_server_address(client, &server_addr);

    hret = margo_create(client->mid, server_addr, client->query_id, &handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: %s: margo_create() failed with %d.\n", __func__,
                hret);
        return (0);
    }
    DEBUG_OUT("Forwarding RPC.\n");
    hret = margo_forward(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: %s: margo_forward() failed with %d.\n",
                __func__, hret);
        margo_destroy(handle);
        return (0);
    }
    DEBUG_OUT("RPC sent, awaiting reply.\n");
    hret = margo_get_output(handle, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: %s: margo_get_output() failed with %d.\n",
                __func__, hret);
        margo_destroy(handle);
        return (0);
    }

    num_odscs = (out.odsc_list.size) / sizeof(obj_descriptor);
    *odsc_tab = malloc(out.odsc_list.size);
    memcpy(*odsc_tab, out.odsc_list.raw_odsc, out.odsc_list.size);
    margo_free_output(handle, &out);
    margo_addr_free(client->mid, server_addr);
    margo_destroy(handle);

    return (num_odscs);
}

static void fill_odsc(dspaces_client_t client, const char *var_name,
                      unsigned int ver, int elem_size, int ndim, uint64_t *lb,
                      uint64_t *ub, obj_descriptor *odsc)
{
    odsc->version = ver;
    memset(odsc->owner, 0, sizeof(odsc->owner));
    odsc->st = st;
    odsc->size = elem_size;
    odsc->bb.num_dims = ndim;

    memset(odsc->bb.lb.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    memset(odsc->bb.ub.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);

    memcpy(odsc->bb.lb.c, lb, sizeof(uint64_t) * ndim);
    memcpy(odsc->bb.ub.c, ub, sizeof(uint64_t) * ndim);

    copy_var_name_to_odsc(client, var_name, odsc);
}

static void odsc_from_req(dspaces_client_t client, struct dspaces_req *req,
                          obj_descriptor *odsc)
{
    fill_odsc(client, req->var_name, req->ver, req->elem_size, req->ndim,
              req->lb, req->ub, odsc);
}

static void fill_req(dspaces_client_t client, obj_descriptor *odsc, void *data,
                     struct dspaces_req *req)
{
    int i;

    if(client->nspace) {
        req->var_name = strdup(&odsc->name[strlen(client->nspace) + 2]);
    } else {
        req->var_name = strdup(odsc->name);
    }
    req->ver = odsc->version;
    req->elem_size = odsc->size;
    req->ndim = odsc->bb.num_dims;
    req->lb = malloc(sizeof(*req->lb) * req->ndim);
    req->ub = malloc(sizeof(*req->ub) * req->ndim);
    for(i = 0; i < req->ndim; i++) {
        req->lb[i] = odsc->bb.lb.c[i];
        req->ub[i] = odsc->bb.ub.c[i];
    }
    req->buf = data;
    req->tag = odsc->tag;
}

int dspaces_get_req(dspaces_client_t client, struct dspaces_req *in_req,
                    struct dspaces_req *out_req, int timeout)
{
    obj_descriptor odsc = {0};
    obj_descriptor *odsc_tab;
    int num_odscs;
    int elem_size;
    int num_elem = 1;
    int i, j;
    int ret = dspaces_SUCCESS;
    void *data;

    odsc_from_req(client, in_req, &odsc);

    DEBUG_OUT("Querying %s with timeout %d\n", obj_desc_sprint(&odsc), timeout);

    num_odscs = get_odscs(client, &odsc, timeout, &odsc_tab);

    DEBUG_OUT("Finished query - need to fetch %d objects\n", num_odscs);
    for(int i = 0; i < num_odscs; i++) {
        if(odsc_tab[i].flags & DS_OBJ_RESIZE) {
            DEBUG_OUT("the result is cropped.\n");
            memcpy(&odsc.bb, &odsc_tab[i].bb, sizeof(odsc_tab[i].bb));
        }
        DEBUG_OUT("%s\n", obj_desc_sprint(&odsc_tab[i]));
    }

    // send request to get the obj_desc
    if(num_odscs != 0) {
        elem_size = odsc_tab[0].size;
    } else {
        DEBUG_OUT("not setting element size because there are no result "
                  "descriptors.\n");
        data = NULL;
        return (ret);
    }

    odsc.size = elem_size;
    DEBUG_OUT("element size is %zi\n", odsc.size);
    for(i = 0; i < odsc.bb.num_dims; i++) {
        num_elem *= (odsc.bb.ub.c[i] - odsc.bb.lb.c[i]) + 1;
    }
    DEBUG_OUT("data buffer size is %d\n", num_elem * elem_size);

    odsc.tag = odsc_tab[0].tag;
    for(i = 1; i < num_odscs; i++) {
        if(odsc_tab[i].tag != odsc_tab[0].tag) {
            fprintf(stderr, "WARNING: multiple distinct tag values returned in "
                            "query result. Returning first one.\n");
            break;
        }
    }

    if(in_req->buf == NULL) {
        data = malloc(num_elem * elem_size);
    } else {
        data = in_req->buf;
    }

    get_data(client, num_odscs, odsc, odsc_tab, data);
    if(out_req) {
        fill_req(client, &odsc, data, out_req);
    }

    return ret;
}

int dspaces_aget(dspaces_client_t client, const char *var_name,
                 unsigned int ver, int ndim, uint64_t *lb, uint64_t *ub,
                 void **data, int *tag, int timeout)
{
    obj_descriptor odsc = {0};
    obj_descriptor *odsc_tab;
    int num_odscs;
    int elem_size;
    int num_elem = 1;
    int i, j;
    int ret = dspaces_SUCCESS;

    fill_odsc(client, var_name, ver, 0, ndim, lb, ub, &odsc);

    DEBUG_OUT("Querying %s with timeout %d\n", obj_desc_sprint(&odsc), timeout);

    num_odscs = get_odscs(client, &odsc, timeout, &odsc_tab);

    DEBUG_OUT("Finished query - need to fetch %d objects\n", num_odscs);
    for(int i = 0; i < num_odscs; i++) {
        if(odsc_tab[i].flags & DS_OBJ_RESIZE) {
            DEBUG_OUT("the result is cropped.\n");
            memcpy(&odsc.bb, &odsc_tab[i].bb, sizeof(odsc_tab[i].bb));
            for(j = 0; j < odsc.bb.num_dims; j++) {
                lb[j] = odsc.bb.lb.c[j];
                ub[j] = odsc.bb.ub.c[j];
            }
        }
        DEBUG_OUT("%s\n", obj_desc_sprint(&odsc_tab[i]));
    }

    // send request to get the obj_desc
    if(num_odscs != 0)
        elem_size = odsc_tab[0].size;
    else {
        DEBUG_OUT("not setting element size because there are no result "
                  "descriptors.");
        *data = NULL;
        return (ret);
    }
    odsc.size = elem_size;
    DEBUG_OUT("element size is %zi\n", odsc.size);
    for(i = 0; i < ndim; i++) {
        num_elem *= (ub[i] - lb[i]) + 1;
    }
    DEBUG_OUT("data buffer size is %d\n", num_elem * elem_size);

    if(tag) {
        *tag = odsc_tab[0].tag;
        for(i = 1; i < num_odscs; i++) {
            if(odsc_tab[i].tag != *tag) {
                fprintf(stderr,
                        "WARNING: multiple distinct tag values returned in "
                        "query result. Returning first one.\n");
                break;
            }
        }
    }
    *data = malloc(num_elem * elem_size);
    get_data(client, num_odscs, odsc, odsc_tab, *data);

    return ret;
}

int dspaces_get(dspaces_client_t client, const char *var_name, unsigned int ver,
                int elem_size, int ndim, uint64_t *lb, uint64_t *ub, void *data,
                int timeout)
{
    obj_descriptor odsc;
    obj_descriptor *odsc_tab;
    int num_odscs;
    int ret = dspaces_SUCCESS;

    fill_odsc(client, var_name, ver, elem_size, ndim, lb, ub, &odsc);

    DEBUG_OUT("Querying %s with timeout %d\n", obj_desc_sprint(&odsc), timeout);

    num_odscs = get_odscs(client, &odsc, timeout, &odsc_tab);

    DEBUG_OUT("Finished query - need to fetch %d objects\n", num_odscs);
    for(int i = 0; i < num_odscs; ++i) {
        DEBUG_OUT("%s\n", obj_desc_sprint(&odsc_tab[i]));
    }

    // send request to get the obj_desc
    if(num_odscs != 0) {
        get_data(client, num_odscs, odsc, odsc_tab, data);
        free(odsc_tab);
    } else {
        return (-1);
    }

    return (ret);
}

int dspaces_get_meta(dspaces_client_t client, const char *name, int mode,
                     int current, int *version, void **data, unsigned int *len)
{
    query_meta_in_t in;
    query_meta_out_t out;
    hg_addr_t server_addr;
    hg_handle_t handle;
    hg_bulk_t bulk_handle;
    hg_return_t hret;

    in.name = strdup(name);
    in.version = current;
    in.mode = mode;

    DEBUG_OUT("querying meta data '%s' version %d (mode %d).\n", name, current,
              mode);

    get_meta_server_address(client, &server_addr);
    hret =
        margo_create(client->mid, server_addr, client->query_meta_id, &handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: %s: margo_create() failed with %d.\n", __func__,
                hret);
        goto err_hg;
    }
    hret = margo_forward(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: %s: margo_forward() failed with %d.\n",
                __func__, hret);
        goto err_hg_handle;
    }
    hret = margo_get_output(handle, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: %s: margo_get_output() failed with %d.\n",
                __func__, hret);
        goto err_hg_output;
    }

    DEBUG_OUT("Replied with version %d.\n", out.version);

    if(out.mdata.len) {
        DEBUG_OUT("fetching %" PRIu64 " bytes.\n", out.mdata.len);
        *data = malloc(out.mdata.len);
        /*
        hret = margo_bulk_create(client->mid, 1, data, &out.size,
                                 HG_BULK_WRITE_ONLY, &bulk_handle);
        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: %s: margo_bulk_create() failed with %d.\n",
                    __func__, hret);
            goto err_free;
        }
        hret = margo_bulk_transfer(client->mid, HG_BULK_PULL, server_addr,
                                   out.handle, 0, bulk_handle, 0, out.size);
        if(hret != HG_SUCCESS) {
            fprintf(stderr,
                    "ERROR: %s: margo_bulk_transfer() failed with %d.\n",
                    __func__, hret);
            goto err_bulk;
        }
        */
        memcpy(*data, out.mdata.buf, out.mdata.len);
        DEBUG_OUT("metadata for '%s', version %d retrieved successfully.\n",
                  name, out.version);
    } else {
        DEBUG_OUT("Metadata is empty.\n");
        *data = NULL;
    }

    *len = out.mdata.len;
    *version = out.version;

    // margo_bulk_free(bulk_handle);
    margo_free_output(handle, &out);
    margo_destroy(handle);

    return dspaces_SUCCESS;

err_bulk:
    margo_bulk_free(bulk_handle);
err_free:
    free(*data);
err_hg_output:
    margo_free_output(handle, &out);
err_hg_handle:
    margo_destroy(handle);
err_hg:
    free(in.name);
    return dspaces_ERR_MERCURY;
}

int dspaces_mpexec(dspaces_client_t client, int num_args,
                   struct dspaces_req *args, const char *fn, unsigned int fnsz,
                   const char *fn_name, void **data, int *size)
{
    obj_descriptor *arg_odscs;
    hg_addr_t server_addr;
    hg_return_t hret;
    hg_handle_t handle, cond_handle;
    pexec_in_t in;
    pexec_out_t out;
    cond_in_t in2;
    uint64_t mtxp, condp;
    hg_bulk_t bulk_handle;
    hg_size_t rdma_size;
    margo_request req;
    int i;

    in.odsc.size = num_args * sizeof(*arg_odscs);
    in.odsc.raw_odsc = calloc(1, in.odsc.size);
    arg_odscs = (obj_descriptor *)in.odsc.raw_odsc;
    for(i = 0; i < num_args; i++) {
        odsc_from_req(client, &args[i], &arg_odscs[i]);
        DEBUG_OUT("Remote args %i is %s\n", i, obj_desc_sprint(&arg_odscs[i]));
    }

    in.fn_name = strdup(fn_name);

    rdma_size = fnsz;
    in.length = fnsz;
    if(rdma_size > 0) {
        DEBUG_OUT("sending fn\n");
        hret = margo_bulk_create(client->mid, 1, (void **)&fn, &rdma_size,
                                 HG_BULK_READ_ONLY, &in.handle);
        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): margo_bulk_create() failed\n",
                    __func__);
            return dspaces_ERR_MERCURY;
        }
        DEBUG_OUT("created fn tranfer buffer\n");
    }

    get_server_address(client, &server_addr);

    hret = margo_create(client->mid, server_addr, client->mpexec_id, &handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
        margo_bulk_free(in.handle);
        return dspaces_ERR_MERCURY;
    }

    hret = margo_forward(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_forward() failed\n", __func__);
        margo_bulk_free(in.handle);
        margo_destroy(handle);
        return dspaces_ERR_MERCURY;
    }

    hret = margo_get_output(handle, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_get_output() failed\n", __func__);
        margo_bulk_free(in.handle);
        margo_destroy(handle);
        return dspaces_ERR_MERCURY;
    }
    margo_bulk_free(in.handle);

    DEBUG_OUT("received result with size %" PRIu32 "\n", out.length);
    rdma_size = out.length;
    if(rdma_size > 0) {
        *data = malloc(out.length);
        hret = margo_bulk_create(client->mid, 1, (void **)data, &rdma_size,
                                 HG_BULK_WRITE_ONLY, &bulk_handle);
        if(hret != HG_SUCCESS) {
            // TODO notify server of failure (server is waiting)
            margo_free_output(handle, &out);
            margo_destroy(handle);
            return dspaces_ERR_MERCURY;
        }
        hret = margo_bulk_transfer(client->mid, HG_BULK_PULL, server_addr,
                                   out.handle, 0, bulk_handle, 0, rdma_size);
        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): margo_bulk_transfer failed!\n",
                    __func__);
            margo_free_output(handle, &out);
            margo_destroy(handle);
            return dspaces_ERR_MERCURY;
        }
        margo_bulk_free(bulk_handle);
        in2.mtxp = out.mtxp;
        in2.condp = out.condp;
        hret = margo_create(client->mid, server_addr, client->cond_id,
                            &cond_handle);

        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
            return dspaces_ERR_MERCURY;
        }

        DEBUG_OUT("sending cond_rpc with condp = %" PRIu64 ", mtxp = %" PRIu64
                  "\n",
                  in2.condp, in2.mtxp);
        hret = margo_iforward(cond_handle, &in2, &req);
        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): margo_iforward() failed\n", __func__);
            margo_destroy(cond_handle);
            return dspaces_ERR_MERCURY;
        }
        DEBUG_OUT("sent\n");
        *size = rdma_size;
        margo_destroy(cond_handle);
    } else {
        *size = 0;
        *data = NULL;
    }
    margo_free_output(handle, &out);
    margo_destroy(handle);
    free(in.odsc.raw_odsc);
    DEBUG_OUT("done with handling pexec\n");
    return (dspaces_SUCCESS);
}

int dspaces_pexec(dspaces_client_t client, const char *var_name,
                  unsigned int ver, int ndim, uint64_t *lb, uint64_t *ub,
                  const char *fn, unsigned int fnsz, const char *fn_name,
                  void **data, int *size)
{
    obj_descriptor odsc = {0};
    hg_addr_t server_addr;
    hg_return_t hret;
    hg_handle_t handle, cond_handle;
    pexec_in_t in;
    pexec_out_t out;
    cond_in_t in2;
    uint64_t mtxp, condp;
    hg_bulk_t bulk_handle;
    hg_size_t rdma_size;
    margo_request req;

    fill_odsc(client, var_name, ver, 0, ndim, lb, ub, &odsc);

    DEBUG_OUT("Doing remote pexec on %s\n", obj_desc_sprint(&odsc));

    in.odsc.size = sizeof(odsc);
    in.odsc.raw_odsc = (char *)&odsc;

    in.fn_name = strdup(fn_name);

    rdma_size = fnsz;
    in.length = fnsz;
    if(rdma_size > 0) {
        DEBUG_OUT("sending fn\n");
        hret = margo_bulk_create(client->mid, 1, (void **)&fn, &rdma_size,
                                 HG_BULK_READ_ONLY, &in.handle);
        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): margo_bulk_create() failed\n",
                    __func__);
            return dspaces_ERR_MERCURY;
        }
        DEBUG_OUT("created fn tranfer buffer\n");
    }

    get_server_address(client, &server_addr);

    hret = margo_create(client->mid, server_addr, client->pexec_id, &handle);

    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
        margo_bulk_free(in.handle);
        return dspaces_ERR_MERCURY;
    }

    hret = margo_forward(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_forward() failed\n", __func__);
        margo_bulk_free(in.handle);
        margo_destroy(handle);
        return dspaces_ERR_MERCURY;
    }

    hret = margo_get_output(handle, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_get_output() failed\n", __func__);
        margo_bulk_free(in.handle);
        margo_destroy(handle);
        return dspaces_ERR_MERCURY;
    }
    margo_bulk_free(in.handle);

    DEBUG_OUT("received result with size %" PRIu32 "\n", out.length);
    rdma_size = out.length;
    if(rdma_size > 0) {
        *data = malloc(out.length);
        hret = margo_bulk_create(client->mid, 1, (void **)data, &rdma_size,
                                 HG_BULK_WRITE_ONLY, &bulk_handle);
        if(hret != HG_SUCCESS) {
            // TODO notify server of failure (server is waiting)
            margo_free_output(handle, &out);
            margo_destroy(handle);
            return dspaces_ERR_MERCURY;
        }
        hret = margo_bulk_transfer(client->mid, HG_BULK_PULL, server_addr,
                                   out.handle, 0, bulk_handle, 0, rdma_size);
        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): margo_bulk_transfer failed!\n",
                    __func__);
            margo_free_output(handle, &out);
            margo_destroy(handle);
            return dspaces_ERR_MERCURY;
        }
        margo_bulk_free(bulk_handle);
        in2.mtxp = out.mtxp;
        in2.condp = out.condp;
        hret = margo_create(client->mid, server_addr, client->cond_id,
                            &cond_handle);

        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
            return dspaces_ERR_MERCURY;
        }

        DEBUG_OUT("sending cond_rpc with condp = %" PRIu64 ", mtxp = %" PRIu64
                  "\n",
                  in2.condp, in2.mtxp);
        hret = margo_iforward(cond_handle, &in2, &req);
        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): margo_iforward() failed\n", __func__);
            margo_destroy(cond_handle);
            return dspaces_ERR_MERCURY;
        }
        DEBUG_OUT("sent\n");
        *size = rdma_size;
        margo_destroy(cond_handle);
    } else {
        *size = 0;
        *data = NULL;
    }
    margo_free_output(handle, &out);
    margo_destroy(handle);
    DEBUG_OUT("done with handling pexec\n");
    return (dspaces_SUCCESS);
}

long dspaces_register_simple(dspaces_client_t client, const char *type,
                             const char *name, const char *reg_data,
                             char **nspace)
{
    hg_addr_t server_addr;
    hg_return_t hret;
    reg_in_t in;
    uint64_t out;
    hg_handle_t handle;
    long reg_handle;

    in.type = strdup(type);
    in.name = strdup(name);
    in.reg_data = strdup(reg_data);
    in.src = -1;

    get_server_address(client, &server_addr);

    hret = margo_create(client->mid, server_addr, client->reg_id, &handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
        return (DS_MOD_ECLIENT);
    }

    hret = margo_forward(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_forward() failed\n", __func__);
        margo_destroy(handle);
        return (DS_MOD_ECLIENT);
    }

    hret = margo_get_output(handle, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_get_output() failed\n", __func__);
        margo_destroy(handle);
        return (DS_MOD_ECLIENT);
    }

    reg_handle = out;
    if(nspace) {
        if(reg_handle < 0) {
            *nspace = NULL;
        } else {
            *nspace = strdup("ds_reg");
        }
    }

    margo_free_output(handle, &out);
    margo_destroy(handle);

    return (reg_handle);
}

static void get_local_rpc(hg_handle_t handle)
{
    hg_return_t hret;
    bulk_in_t in;
    bulk_out_t out;
    hg_size_t remaining, xfer_size;
    margo_request *reqs;
    hg_bulk_t bulk_handle;
    int i;

    APEX_FUNC_TIMER_START(get_local_rpc);
    margo_instance_id mid = margo_hg_handle_get_instance(handle);

    const struct hg_info *info = margo_get_info(handle);
    dspaces_client_t client =
        (dspaces_client_t)margo_registered_data(mid, info->id);

    DEBUG_OUT("Received rpc to get data\n");

    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s. margo_get_input() failed with "
                "%d.\n",
                __func__, hret);
        return;
    }

    obj_descriptor in_odsc;
    memcpy(&in_odsc, in.odsc.raw_odsc, sizeof(in_odsc));

    DEBUG_OUT("%s\n", obj_desc_sprint(&in_odsc));

    struct obj_data *od, *from_obj;
    APEX_NAME_TIMER_START(1, "get_local_ls_find");
    from_obj = ls_find(client->dcg->ls, &in_odsc);
    if(!from_obj)
        fprintf(stderr,
                "DATASPACES: WARNING handling %s: Object not found in local "
                "storage\n",
                __func__);
    APEX_TIMER_STOP(1);
    APEX_NAME_TIMER_START(2, "get_local_obj_alloc");
    od = obj_data_alloc(&in_odsc);
    if(!od)
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: object allocation failed\n",
                __func__);

    if(from_obj->data == NULL)
        fprintf(
            stderr,
            "DATASPACES: ERROR handling %s: object data allocation failed\n",
            __func__);
    APEX_TIMER_STOP(2);
    APEX_NAME_TIMER_START(3, "get_local_ssd_copy");
    ssd_copy(od, from_obj);
    APEX_TIMER_STOP(3);
    DEBUG_OUT("After ssd_copy\n");

    hg_size_t size = (in_odsc.size) * bbox_volume(&(in_odsc.bb));
    void *buffer = (void *)od->data;

    DEBUG_OUT("creating buffer of size %" PRIu64 "\n", size);
    APEX_NAME_TIMER_START(4, "get_local_bulk_create");
    hret = margo_bulk_create(mid, 1, (void **)&buffer, &size, HG_BULK_READ_ONLY,
                             &bulk_handle);

    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_bulk_create() failed\n",
                __func__);
        out.ret = dspaces_ERR_MERCURY;
        margo_respond(handle, &out);
        margo_free_input(handle, &in);
        margo_destroy(handle);
        return;
    }
    APEX_TIMER_STOP(4);

    APEX_NAME_TIMER_START(5, "get_local_bulk_transfer");
    if(size <= BULK_TRANSFER_MAX) {
        hret = margo_bulk_transfer(mid, HG_BULK_PUSH, info->addr, in.handle, 0,
                                   bulk_handle, 0, size);
    } else {
        remaining = size;
        int num_reqs = (size + BULK_TRANSFER_MAX - 1) / BULK_TRANSFER_MAX;
        DEBUG_OUT("transferring in %i steps\n", num_reqs);
        reqs = malloc(sizeof(*reqs) * num_reqs);
        int req_id = 0;
        size_t offset = 0;
        while(remaining) {
            DEBUG_OUT("%" PRIu64 " bytes left to transfer\n", remaining);
            offset = size - remaining;
            xfer_size =
                (remaining > BULK_TRANSFER_MAX) ? BULK_TRANSFER_MAX : remaining;
            hret = margo_bulk_itransfer(mid, HG_BULK_PUSH, info->addr,
                                        in.handle, offset, bulk_handle, offset,
                                        xfer_size, &reqs[req_id++]);
            if(hret != HG_SUCCESS) {
                fprintf(stderr, "margo_bulk_itransfer %i failed!\n",
                        req_id - 1);
                break;
            }
            remaining -= xfer_size;
        }
        // if anything is left, we bailed with an error
        if(!remaining) {
            for(i = 0; i < num_reqs; i++) {
                hret = margo_wait(reqs[i]);
                if(hret != HG_SUCCESS) {
                    fprintf(stderr, "margo_wait %i failed\n", i);
                    break;
                }
            }
        }
    }
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_bulk_transfer() failed "
                "(%d)\n",
                __func__, hret);
        out.ret = dspaces_ERR_MERCURY;
        margo_respond(handle, &out);
        margo_free_input(handle, &in);
        margo_bulk_free(bulk_handle);
        margo_destroy(handle);
        return;
    }
    APEX_TIMER_STOP(5);
    margo_bulk_free(bulk_handle);
    out.ret = dspaces_SUCCESS;
    obj_data_free(od);
    margo_respond(handle, &out);
    margo_free_input(handle, &in);
    margo_destroy(handle);
    DEBUG_OUT("complete\n");
    APEX_TIMER_STOP(0);
}
DEFINE_MARGO_RPC_HANDLER(get_local_rpc)

static void drain_rpc(hg_handle_t handle)
{
    hg_return_t hret;
    bulk_in_t in;
    bulk_out_t out;
    hg_bulk_t bulk_handle;

    margo_instance_id mid = margo_hg_handle_get_instance(handle);

    const struct hg_info *info = margo_get_info(handle);
    dspaces_client_t client =
        (dspaces_client_t)margo_registered_data(mid, info->id);

    DEBUG_OUT("Received rpc to drain data\n");

    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_get_input() failed with "
                "%d.\n",
                __func__, hret);
        return;
    }

    obj_descriptor in_odsc;
    memcpy(&in_odsc, in.odsc.raw_odsc, sizeof(in_odsc));

    DEBUG_OUT("%s\n", obj_desc_sprint(&in_odsc));

    struct obj_data *from_obj;

    from_obj = ls_find(client->dcg->ls, &in_odsc);
    if(!from_obj) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s:"
                "Object not found in client's local storage.\n Make sure MAX "
                "version is set appropriately in dataspaces.conf\n",
                __func__);
        out.ret = dspaces_ERR_MERCURY;
        margo_respond(handle, &out);
        return;
    }

    hg_size_t size = (in_odsc.size) * bbox_volume(&(in_odsc.bb));
    void *buffer = (void *)from_obj->data;

    hret = margo_bulk_create(mid, 1, (void **)&buffer, &size, HG_BULK_READ_ONLY,
                             &bulk_handle);

    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_bulk_create() failed\n",
                __func__);
        out.ret = dspaces_ERR_MERCURY;
        margo_respond(handle, &out);
        margo_free_input(handle, &in);
        margo_destroy(handle);
        return;
    }

    hret = margo_bulk_transfer(mid, HG_BULK_PUSH, info->addr, in.handle, 0,
                               bulk_handle, 0, size);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_bulk_transfer() failed\n",
                __func__);
        out.ret = dspaces_ERR_MERCURY;
        margo_respond(handle, &out);
        margo_free_input(handle, &in);
        margo_bulk_free(bulk_handle);
        margo_destroy(handle);
        return;
    }
    margo_bulk_free(bulk_handle);

    out.ret = dspaces_SUCCESS;
    margo_respond(handle, &out);
    margo_free_input(handle, &in);
    margo_destroy(handle);
    // delete object from local storage
    DEBUG_OUT("Finished draining %s\n", obj_desc_sprint(&from_obj->obj_desc));
    ABT_mutex_lock(client->ls_mutex);
    ls_try_remove_free(client->dcg->ls, from_obj);
    ABT_mutex_unlock(client->ls_mutex);

    ABT_mutex_lock(client->drain_mutex);
    client->local_put_count--;
    if(client->local_put_count == 0 && client->f_final) {
        DEBUG_OUT("signaling all objects drained.\n");
        ABT_cond_signal(client->drain_cond);
    }
    ABT_mutex_unlock(client->drain_mutex);
    DEBUG_OUT("%d objects left to drain...\n", client->local_put_count);
}
DEFINE_MARGO_RPC_HANDLER(drain_rpc)

static struct dspaces_sub_handle *dspaces_get_sub(dspaces_client_t client,
                                                  int sub_id)
{
    int listidx = sub_id % SUB_HASH_SIZE;
    struct sub_list_node *node;

    node = client->sub_lists[listidx];
    while(node) {
        if(node->id == sub_id) {
            return (node->subh);
        }
    }

    fprintf(stderr,
            "WARNING: received notification for unknown subscription id %d. "
            "This shouldn't happen.\n",
            sub_id);
    return (NULL);
}

static void dspaces_move_sub(dspaces_client_t client, int sub_id)
{
    int listidx = sub_id % SUB_HASH_SIZE;
    struct sub_list_node *node, **nodep;

    nodep = &client->sub_lists[listidx];
    while(*nodep && (*nodep)->id != sub_id) {
        nodep = &((*nodep)->next);
    }

    if(!*nodep) {
        fprintf(stderr,
                "WARNING: trying to mark unknown sub %d done. This shouldn't "
                "happen.\n",
                sub_id);
        return;
    }

    node = *nodep;
    *nodep = node->next;
    node->next = client->done_list;
    client->done_list = node;
}

static void free_sub_req(struct dspaces_req *req)
{
    if(!req) {
        return;
    }

    free(req->var_name);
    free(req->lb);
    free(req->ub);
    free(req);
}

static void notify_rpc(hg_handle_t handle)
{
    margo_instance_id mid = margo_hg_handle_get_instance(handle);
    const struct hg_info *info = margo_get_info(handle);
    dspaces_client_t client =
        (dspaces_client_t)margo_registered_data(mid, info->id);
    odsc_list_t in;
    struct dspaces_sub_handle *subh;
    int sub_id;
    int num_odscs;
    obj_descriptor *odsc_tab;
    void *data;
    size_t data_size;
    int i;

    margo_get_input(handle, &in);
    sub_id = in.param;

    DEBUG_OUT("Received notification for sub %d\n", sub_id);
    ABT_mutex_lock(client->sub_mutex);
    subh = dspaces_get_sub(client, sub_id);
    if(subh->status == DSPACES_SUB_WAIT) {
        ABT_mutex_unlock(client->sub_mutex);
        subh->status = DSPACES_SUB_TRANSFER;
        num_odscs = (in.odsc_list.size) / sizeof(obj_descriptor);
        odsc_tab = malloc(in.odsc_list.size);
        memcpy(odsc_tab, in.odsc_list.raw_odsc, in.odsc_list.size);

        DEBUG_OUT("Satisfying subscription requires fetching %d objects:\n",
                  num_odscs);
        for(i = 0; i < num_odscs; i++) {
            DEBUG_OUT("%s\n", obj_desc_sprint(&odsc_tab[i]));
        }

        data_size = subh->q_odsc.size;
        for(i = 0; i < subh->q_odsc.bb.num_dims; i++) {
            data_size *=
                (subh->q_odsc.bb.ub.c[i] - subh->q_odsc.bb.lb.c[i]) + 1;
        }
        data = malloc(data_size);

        if(num_odscs) {
            get_data(client, num_odscs, subh->q_odsc, odsc_tab, data);
        }
        if(!data) {
            fprintf(stderr, "ERROR: %s: data allocated, but is null.\n",
                    __func__);
        }
    } else {
        fprintf(stderr,
                "WARNING: got notification, but sub status was not "
                "DSPACES_SUB_WAIT (%i)\n",
                subh->status);
        ABT_mutex_unlock(client->sub_mutex);
        odsc_tab = NULL;
        data = NULL;
    }

    margo_free_input(handle, &in);
    margo_destroy(handle);

    ABT_mutex_lock(client->sub_mutex);
    if(subh->status == DSPACES_SUB_TRANSFER) {
        subh->req->buf = data;
        subh->status = DSPACES_SUB_RUNNING;
    } else if(data) {
        // subscription was cancelled
        DEBUG_OUT("transfer complete, but sub was cancelled? (status %d)\n",
                  subh->status);
        free(data);
        data = NULL;
    }
    ABT_mutex_unlock(client->sub_mutex);

    if(data) {
        subh->result = subh->cb(client, subh->req, subh->arg);
    } else {
        DEBUG_OUT("no data, skipping callback.\n");
    }

    ABT_mutex_lock(client->sub_mutex);
    client->pending_sub--;
    dspaces_move_sub(client, sub_id);
    subh->status = DSPACES_SUB_DONE;
    ABT_cond_signal(client->sub_cond);
    ABT_mutex_unlock(client->sub_mutex);

    if(odsc_tab) {
        free(odsc_tab);
    }
    free_sub_req(subh->req);

    DEBUG_OUT("finished notification handling.\n");
}
DEFINE_MARGO_RPC_HANDLER(notify_rpc)

static void register_client_sub(dspaces_client_t client,
                                struct dspaces_sub_handle *subh)
{
    int listidx = subh->id % SUB_HASH_SIZE;
    struct sub_list_node **node = &client->sub_lists[listidx];

    while(*node) {
        node = &((*node)->next);
    }

    *node = malloc(sizeof(**node));
    (*node)->next = NULL;
    (*node)->subh = subh;
    (*node)->id = subh->id;
}

struct dspaces_sub_handle *dspaces_sub(dspaces_client_t client,
                                       const char *var_name, unsigned int ver,
                                       int elem_size, int ndim, uint64_t *lb,
                                       uint64_t *ub, dspaces_sub_fn sub_cb,
                                       void *arg)
{
    hg_addr_t my_addr, server_addr;
    hg_handle_t handle;
    margo_request req;
    hg_return_t hret;
    struct dspaces_sub_handle *subh;
    odsc_gdim_t in;
    struct global_dimension od_gdim;
    hg_size_t owner_addr_size = 128;
    int ret;

    if(client->listener_init == 0) {
        ret = dspaces_init_listener(client);
        if(ret != dspaces_SUCCESS) {
            return (DSPACES_SUB_FAIL);
        }
    }

    subh = malloc(sizeof(*subh));

    subh->req = malloc(sizeof(*subh->req));
    subh->req->var_name = strdup(var_name);
    subh->req->ver = ver;
    subh->req->elem_size = elem_size;
    subh->req->ndim = ndim;
    subh->req->lb = malloc(sizeof(*subh->req->lb) * ndim);
    subh->req->ub = malloc(sizeof(*subh->req->ub) * ndim);
    memcpy(subh->req->lb, lb, ndim * sizeof(*lb));
    memcpy(subh->req->ub, ub, ndim * sizeof(*ub));

    subh->q_odsc.version = ver;
    subh->q_odsc.st = st;
    subh->q_odsc.size = elem_size;
    subh->q_odsc.bb.num_dims = ndim;

    subh->arg = arg;
    subh->cb = sub_cb;

    ABT_mutex_lock(client->sub_mutex);
    client->pending_sub++;
    subh->id = client->sub_serial++;
    register_client_sub(client, subh);
    subh->status = DSPACES_SUB_WAIT;
    ABT_mutex_unlock(client->sub_mutex);

    memset(subh->q_odsc.bb.lb.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    memset(subh->q_odsc.bb.ub.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    memcpy(subh->q_odsc.bb.lb.c, lb, sizeof(uint64_t) * ndim);
    memcpy(subh->q_odsc.bb.ub.c, ub, sizeof(uint64_t) * ndim);
    strncpy(subh->q_odsc.name, var_name, sizeof(subh->q_odsc.name) - 1);
    copy_var_name_to_odsc(client, var_name, &subh->q_odsc);

    // A hack to send our address to the server without using more space. This
    // field is ignored in a normal query.
    margo_addr_self(client->mid, &my_addr);
    margo_addr_to_string(client->mid, subh->q_odsc.owner, &owner_addr_size,
                         my_addr);
    margo_addr_free(client->mid, my_addr);

    in.odsc_gdim.size = sizeof(subh->q_odsc);
    in.odsc_gdim.raw_odsc = (char *)(&subh->q_odsc);
    in.param = subh->id;

    DEBUG_OUT("registered data subscription for %s with id %d\n",
              obj_desc_sprint(&subh->q_odsc), subh->id);

    set_global_dimension(&(client->dcg->gdim_list), subh->q_odsc.name,
                         &(client->dcg->default_gdim), &od_gdim);
    in.odsc_gdim.gdim_size = sizeof(struct global_dimension);
    in.odsc_gdim.raw_gdim = (char *)(&od_gdim);

    get_server_address(client, &server_addr);

    hret = margo_create(client->mid, server_addr, client->sub_id, &handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: %s: margo_create() failed with %d.\n", __func__,
                hret);
        return (DSPACES_SUB_FAIL);
    }
    hret = margo_iforward(handle, &in, &req);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: %s: margo_forward() failed with %d.\n",
                __func__, hret);
        margo_destroy(handle);
        return (DSPACES_SUB_FAIL);
    }

    DEBUG_OUT("subscription %d sent.\n", subh->id);

    margo_addr_free(client->mid, server_addr);
    margo_destroy(handle);

    return (subh);
}

int dspaces_check_sub(dspaces_client_t client, dspaces_sub_t subh, int wait,
                      int *result)
{
    if(subh == DSPACES_SUB_FAIL) {
        fprintf(stderr,
                "WARNING: %s: status check on invalid subscription handle.\n",
                __func__);
        return DSPACES_SUB_INVALID;
    }

    DEBUG_OUT("checking status of subscription %d\n", subh->id);

    if(wait) {
        DEBUG_OUT("blocking on notification for subscription %d.\n", subh->id);
        ABT_mutex_lock(client->sub_mutex);
        while(subh->status == DSPACES_SUB_WAIT ||
              subh->status == DSPACES_SUB_TRANSFER ||
              subh->status == DSPACES_SUB_RUNNING) {
            ABT_cond_wait(client->sub_cond, client->sub_mutex);
        }
        ABT_mutex_unlock(client->sub_mutex);
    }

    // This is a concurrency bug. The status could change to DONE with result
    // unset.
    if(subh->status == DSPACES_SUB_DONE) {
        *result = subh->result;
    }

    return (subh->status);
}

static void kill_client_rpc(hg_handle_t handle)
{
    margo_instance_id mid = margo_hg_handle_get_instance(handle);
    const struct hg_info *info = margo_get_info(handle);
    dspaces_client_t client =
        (dspaces_client_t)margo_registered_data(mid, info->id);

    DEBUG_OUT("Received kill message.\n");

    ABT_mutex_lock(client->drain_mutex);
    client->local_put_count = 0;
    ABT_cond_signal(client->drain_cond);
    ABT_mutex_unlock(client->drain_mutex);

    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(kill_client_rpc)

int dspaces_cancel_sub(dspaces_client_t client, dspaces_sub_t subh)
{
    if(subh == DSPACES_SUB_FAIL) {
        return (DSPACES_SUB_INVALID);
    }
    ABT_mutex_lock(client->sub_mutex);
    if(subh->status == DSPACES_SUB_WAIT ||
       subh->status == DSPACES_SUB_TRANSFER) {
        subh->status = DSPACES_SUB_CANCELLED;
    }
    ABT_mutex_unlock(client->sub_mutex);

    return (0);
}

void dspaces_kill(dspaces_client_t client)
{
    uint32_t in = -1;
    hg_addr_t server_addr;
    hg_handle_t h;
    margo_request req;
    hg_return_t hret;

    DEBUG_OUT("sending kill signal to servers.\n");

    margo_addr_lookup(client->mid, client->server_address[0], &server_addr);
    hret = margo_create(client->mid, server_addr, client->kill_id, &h);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
        margo_addr_free(client->mid, server_addr);
        return;
    }
    margo_iforward(h, &in, &req);

    DEBUG_OUT("kill signal sent.\n");

    margo_addr_free(client->mid, server_addr);
    margo_destroy(h);
}

int dspaces_op_calc(dspaces_client_t client, struct ds_data_expr *expr,
                    void **buf)
{
    hg_handle_t handle;
    do_ops_in_t in;
    bulk_out_t out;
    hg_addr_t server_addr;
    hg_size_t rdma_size = expr->size;
    void *cbuf;
    hg_return_t hret;
    int ret;

    DEBUG_OUT("Sending expression type %i\n", expr->type);

    in.expr = expr;
    cbuf = malloc(expr->size);

    get_server_address(client, &server_addr);

    margo_bulk_create(client->mid, 1, &cbuf, &rdma_size, HG_BULK_WRITE_ONLY,
                      &in.handle);
    hret = margo_create(client->mid, server_addr, client->do_ops_id, &handle);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: %s: margo_create() failed with %d.\n", __func__,
                hret);
        return (dspaces_ERR_MERCURY);
    }
    hret = margo_forward(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: %s: margo_forward() failed with %d.\n",
                __func__, hret);
        margo_destroy(handle);
        return (dspaces_ERR_MERCURY);
    }
    hret = margo_get_output(handle, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: %s: margo_get_output() failed with %d.\n",
                __func__, hret);
        margo_destroy(handle);
        return (dspaces_ERR_MERCURY);
    }

    if(out.len) {
        *buf = malloc(rdma_size);
        ret = LZ4_decompress_safe(cbuf, *buf, out.len, rdma_size);
        DEBUG_OUT("decompressed results from %" PRIu64 " to %" PRIu64
                  " bytes.\n",
                  out.len, rdma_size);
        free(cbuf);
    } else {
        *buf = cbuf;
    }

    margo_free_output(handle, &out);
    margo_destroy(handle);

    return (dspaces_SUCCESS);
}

void dspaces_set_namespace(dspaces_client_t client, const char *nspace)
{
    if(client->nspace) {
        free(client->nspace);
    }

    client->nspace = strdup(nspace);
}

int dspaces_get_var_names(dspaces_client_t client, char ***var_names)
{
    uint32_t in = -1;
    hg_addr_t server_addr;
    hg_handle_t h;
    name_list_t out;
    int i, ret;
    hg_return_t hret;

    DEBUG_OUT("requesting variables names from server.\n");

    get_server_address(client, &server_addr);
    hret = margo_create(client->mid, server_addr, client->get_vars_id, &h);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
        margo_addr_free(client->mid, server_addr);
        return (-1);
    }

    hret = margo_forward(h, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_forward() failed\n", __func__);
        margo_destroy(h);
        return (-1);
    }

    hret = margo_get_output(h, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_get_output() failed\n", __func__);
        margo_destroy(h);
        return (-1);
    }

    DEBUG_OUT("Received %" PRIu64 " variables in reply\n", out.count);

    *var_names = malloc(sizeof(*var_names) * out.count);
    for(i = 0; i < out.count; i++) {
        (*var_names)[i] = strdup(out.names[i]);
    }
    ret = out.count;

    margo_free_output(h, &out);
    margo_addr_free(client->mid, server_addr);
    margo_destroy(h);

    return (ret);
}

int dspaces_get_var_objs(dspaces_client_t client, const char *name,
                         struct dspaces_obj **objs)
{
    get_var_objs_in_t in;
    hg_addr_t server_addr;
    hg_handle_t h;
    odsc_hdr out;
    obj_descriptor *odscs, *odsc;
    int num_odsc;
    int ndim;
    struct dspaces_obj *obj;
    int i, j, ret;
    hg_return_t hret;

    DEBUG_OUT("Retrieving description of available objects for %s\n", name);

    in.src = -1;
    in.var_name = strdup(name);

    get_server_address(client, &server_addr);
    hret = margo_create(client->mid, server_addr, client->get_var_objs_id, &h);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_create() failed\n", __func__);
        margo_addr_free(client->mid, server_addr);
        free(in.var_name);
        return (-1);
    }

    ret = margo_forward(h, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_forward() failed\n", __func__);
        margo_destroy(h);
        free(in.var_name);
        return (-1);
    }

    hret = margo_get_output(h, &out);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_get_output() failed\n", __func__);
        margo_destroy(h);
        free(in.var_name);
        return (-1);
    }

    odscs = (obj_descriptor *)out.raw_odsc;
    num_odsc = out.size / sizeof(*odscs);
    *objs = malloc(sizeof(**objs) * num_odsc);
    for(i = 0; i < num_odsc; i++) {
        obj = &((*objs)[i]);
        odsc = &odscs[i];
        obj->name = strdup(name);
        obj->version = odsc->version;
        obj->ndim = odsc->bb.num_dims;
        ndim = obj->ndim;
        obj->lb = malloc(sizeof(*obj->lb) * ndim);
        obj->ub = malloc(sizeof(*obj->ub) * ndim);
        for(j = 0; j < ndim; j++) {
            obj->lb[j] = odsc->bb.lb.c[j];
            obj->ub[j] = odsc->bb.ub.c[j];
        }
    }
    ret = num_odsc;

    margo_free_output(h, &out);
    margo_addr_free(client->mid, server_addr);
    margo_destroy(h);
    free(in.var_name);

    return (ret);
}
