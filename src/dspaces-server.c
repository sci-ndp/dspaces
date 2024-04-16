/*
 * Copyright (c) 2020, Rutgers Discovery Informatics Institute, Rutgers
 * University
 *
 * See COPYRIGHT in top-level directory.
 */
#include "dspaces-server.h"
#include "dspaces-ops.h"
#include "dspaces-storage.h"
#include "dspaces.h"
#include "dspacesp.h"
#include "gspace.h"
#include "ss_data.h"
#include "str_hash.h"
#include "toml.h"
#include <abt.h>
#include <errno.h>
#include <fcntl.h>
#include <lz4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef OPS_USE_OPENMP
#include <omp.h>
#endif

#ifdef DSPACES_HAVE_PYTHON
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#define PY_ARRAY_UNIQUE_SYMBOL dsm
#include <Python.h>
#include <numpy/ndarrayobject.h>
#include <numpy/ndarraytypes.h>
#endif

#ifdef HAVE_DRC
#include <rdmacred.h>
#endif /* HAVE_DRC */

#define DEBUG_OUT(dstr, ...)                                                   \
    do {                                                                       \
        if(server->f_debug) {                                                  \
            ABT_unit_id tid;                                                   \
            ABT_thread_self_id(&tid);                                          \
            fprintf(stderr,                                                    \
                    "Rank %i: TID: %" PRIu64 " %s, line %i (%s): " dstr,       \
                    server->rank, tid, __FILE__, __LINE__, __func__,           \
                    ##__VA_ARGS__);                                            \
        }                                                                      \
    } while(0);

#define DSPACES_DEFAULT_NUM_HANDLERS 4

#define xstr(s) str(s)
#define str(s) #s

// TODO !
// static enum storage_type st = column_major;

typedef enum obj_update_type { DS_OBJ_NEW, DS_OBJ_OWNER } obj_update_t;

int cond_num = 0;

struct addr_list_entry {
    struct list_head entry;
    char *addr;
};

struct remote {
    char *name;
    char addr_str[128];
    dspaces_client_t conn;
};

#define DSPACES_ARG_REAL 0
#define DSPACES_ARG_INT 1
#define DSPACES_ARG_STR 2
#define DSPACES_ARG_NONE 3
struct dspaces_module_args {
    char *name;
    int type;
    int len;
    union {
        double rval;
        long ival;
        double *rarray;
        long *iarray;
        char *strval;
    };
};

#define DSPACES_MOD_RET_ARRAY 0
struct dspaces_module_ret {
    int type;
    int len;
    uint64_t *dim;
    int ndim;
    int tag;
    int elem_size;
    void *data;
};

#define DSPACES_MOD_PY 0
struct dspaces_module {
    char *name;
    int type;
    union {
#ifdef DSPACES_HAVE_PYTHON
        PyObject *pModule;
#endif // DSPACES_HAVE_PYTHON
    };
};

struct dspaces_provider {
    struct list_head dirs;
    margo_instance_id mid;
    hg_id_t put_id;
    hg_id_t put_local_id;
    hg_id_t put_meta_id;
    hg_id_t query_id;
    hg_id_t peek_meta_id;
    hg_id_t query_meta_id;
    hg_id_t get_id;
    hg_id_t get_local_id;
    hg_id_t obj_update_id;
    hg_id_t odsc_internal_id;
    hg_id_t ss_id;
    hg_id_t drain_id;
    hg_id_t kill_id;
    hg_id_t kill_client_id;
    hg_id_t sub_id;
    hg_id_t notify_id;
    hg_id_t do_ops_id;
    hg_id_t pexec_id;
    hg_id_t cond_id;
    hg_id_t get_vars_id;
    hg_id_t get_var_objs_id;
    int nmods;
    struct dspaces_module *mods;
    struct ds_gspace *dsg;
    char **server_address;
    char **node_names;
    char *listen_addr_str;
    int rank;
    int comm_size;
    int f_debug;
    int f_drain;
    int f_kill;

#ifdef HAVE_DRC
    uint32_t drc_credential_id;
#endif

    MPI_Comm comm;

    ABT_mutex odsc_mutex;
    ABT_mutex ls_mutex;
    ABT_mutex dht_mutex;
    ABT_mutex sspace_mutex;
    ABT_mutex kill_mutex;

    ABT_xstream drain_xstream;
    ABT_pool drain_pool;
    ABT_thread drain_t;

    const char *pub_ip;
    const char *priv_ip;

    struct remote *remotes;
    int nremote;
};

DECLARE_MARGO_RPC_HANDLER(put_rpc)
DECLARE_MARGO_RPC_HANDLER(put_local_rpc)
DECLARE_MARGO_RPC_HANDLER(put_meta_rpc)
DECLARE_MARGO_RPC_HANDLER(get_rpc)
DECLARE_MARGO_RPC_HANDLER(query_rpc)
DECLARE_MARGO_RPC_HANDLER(peek_meta_rpc)
DECLARE_MARGO_RPC_HANDLER(query_meta_rpc)
DECLARE_MARGO_RPC_HANDLER(obj_update_rpc)
DECLARE_MARGO_RPC_HANDLER(odsc_internal_rpc)
DECLARE_MARGO_RPC_HANDLER(ss_rpc)
DECLARE_MARGO_RPC_HANDLER(kill_rpc)
DECLARE_MARGO_RPC_HANDLER(sub_rpc)
DECLARE_MARGO_RPC_HANDLER(do_ops_rpc)
#ifdef DSPACES_HAVE_PYTHON
DECLARE_MARGO_RPC_HANDLER(pexec_rpc)
#endif // DSPACES_HAVE_PYTHON
DECLARE_MARGO_RPC_HANDLER(cond_rpc)
DECLARE_MARGO_RPC_HANDLER(get_vars_rpc);
DECLARE_MARGO_RPC_HANDLER(get_var_objs_rpc);

static void put_rpc(hg_handle_t h);
static void put_local_rpc(hg_handle_t h);
static void put_meta_rpc(hg_handle_t h);
static void get_rpc(hg_handle_t h);
static void query_rpc(hg_handle_t h);
static void query_meta_rpc(hg_handle_t h);
static void obj_update_rpc(hg_handle_t h);
static void odsc_internal_rpc(hg_handle_t h);
static void ss_rpc(hg_handle_t h);
static void kill_rpc(hg_handle_t h);
static void sub_rpc(hg_handle_t h);
static void do_ops_rpc(hg_handle_t h);
static void pexec_rpc(hg_handle_t h);
static void cond_rpc(hg_handle_t h);
static void get_vars_rpc(hg_handle_t h);
static void get_vars_obj_rpc(hg_handle_t h);

/* Server configuration parameters */
static struct {
    int ndim;
    struct coord dims;
    int max_versions;
    int hash_version; /* 1 - ssd_hash_version_v1, 2 - ssd_hash_version_v2 */
    int num_apps;
} ds_conf;

static struct {
    const char *opt;
    int *pval;
} options[] = {{"ndim", &ds_conf.ndim},
               {"dims", (int *)&ds_conf.dims},
               {"max_versions", &ds_conf.max_versions},
               {"hash_version", &ds_conf.hash_version},
               {"num_apps", &ds_conf.num_apps}};

static void eat_spaces(char *line)
{
    char *t = line;

    while(t && *t) {
        if(*t != ' ' && *t != '\t' && *t != '\n')
            *line++ = *t;
        t++;
    }
    if(line)
        *line = '\0';
}

static int parse_line(int lineno, char *line)
{
    char *t;
    int i, n;

    /* Comment line ? */
    if(line[0] == '#')
        return 0;

    t = strstr(line, "=");
    if(!t) {
        eat_spaces(line);
        if(strlen(line) == 0)
            return 0;
        else
            return -EINVAL;
    }

    t[0] = '\0';
    eat_spaces(line);
    t++;

    n = sizeof(options) / sizeof(options[0]);

    for(i = 0; i < n; i++) {
        if(strcmp(line, options[1].opt) == 0) { /**< when "dims" */
            // get coordinates
            int idx = 0;
            char *crd;
            crd = strtok(t, ",");
            while(crd != NULL) {
                ((struct coord *)options[1].pval)->c[idx] = atoll(crd);
                crd = strtok(NULL, ",");
                idx++;
            }
            if(idx != *(int *)options[0].pval) {
                fprintf(stderr, "ERROR: (%s): dimensionality mismatch.\n",
                        __func__);
                fprintf(stderr, "ERROR: index=%d, ndims=%d\n", idx,
                        *(int *)options[0].pval);
                return -EINVAL;
            }
            break;
        }
        if(strcmp(line, options[i].opt) == 0) {
            eat_spaces(line);
            *(int *)options[i].pval = atoi(t);
            break;
        }
    }

    if(i == n) {
        fprintf(stderr, "WARNING: (%s): unknown option '%s' at line %d.\n",
                __func__, line, lineno);
    }
    return 0;
}

static int parse_conf(const char *fname)
{
    FILE *fin;
    char buff[1024];
    int lineno = 1, err;

    fin = fopen(fname, "rt");
    if(!fin) {
        fprintf(stderr, "ERROR: could not open configuration file '%s'.\n",
                fname);
        return -errno;
    }

    while(fgets(buff, sizeof(buff), fin) != NULL) {
        err = parse_line(lineno++, buff);
        if(err < 0) {
            fclose(fin);
            return err;
        }
    }

    fclose(fin);
    return 0;
}

static int parse_conf_toml(const char *fname, struct list_head *dir_list,
                           struct remote **rem_array, int *nremote)
{
    FILE *fin;
    toml_table_t *conf;
    toml_table_t *storage;
    toml_table_t *conf_dir;
    toml_table_t *server;
    toml_table_t *remotes;
    toml_table_t *remote;
    toml_datum_t dat;
    toml_array_t *arr;
    struct dspaces_dir *dir;
    struct dspaces_file *file;
    char errbuf[200];
    char *ip;
    int port;
    int ndim = 0;
    int ndir, nfile;
    int i, j, n;

    fin = fopen(fname, "r");
    if(!fin) {
        fprintf(stderr, "ERROR: could not open configuration file '%s'.\n",
                fname);
        return -errno;
    }

    conf = toml_parse_file(fin, errbuf, sizeof(errbuf));
    fclose(fin);

    if(!conf) {
        fprintf(stderr, "could not parse %s, %s.\n", fname, errbuf);
        return -1;
    }

    server = toml_table_in(conf, "server");
    if(!server) {
        fprintf(stderr, "missing [server] block from %s\n", fname);
        return -1;
    }

    n = sizeof(options) / sizeof(options[0]);
    for(i = 0; i < n; i++) {
        if(strcmp(options[i].opt, "dims") == 0) {
            arr = toml_array_in(server, "dims");
            if(arr) {
                while(1) {
                    dat = toml_int_at(arr, ndim);
                    if(!dat.ok) {
                        break;
                    }
                    ((struct coord *)options[i].pval)->c[ndim] = dat.u.i;
                    ndim++;
                }
            }
            for(j = 0; j < n; j++) {
                if(strcmp(options[j].opt, "ndim") == 0) {
                    *(int *)options[j].pval = ndim;
                }
            }
        } else {
            dat = toml_int_in(server, options[i].opt);
            if(dat.ok) {
                *(int *)options[i].pval = dat.u.i;
            }
        }
    }

    remotes = toml_table_in(conf, "remotes");
    if(remotes) {
        *nremote = toml_table_ntab(remotes);
        *rem_array = malloc(sizeof(**rem_array) * *nremote);
        for(i = 0; i < *nremote; i++) {
            // remote = toml_table_at(remotes, i);
            (*rem_array)[i].name = strdup(toml_key_in(remotes, i));
            remote = toml_table_in(remotes, (*rem_array)[i].name);
            dat = toml_string_in(remote, "ip");
            ip = dat.u.s;
            dat = toml_int_in(remote, "port");
            port = dat.u.i;
            sprintf((*rem_array)[i].addr_str, "sockets://%s:%i", ip, port);
            free(ip);
        }
    }

    storage = toml_table_in(conf, "storage");
    if(storage) {
        ndir = toml_table_ntab(storage);
        for(i = 0; i < ndir; i++) {
            dir = malloc(sizeof(*dir));
            dir->name = strdup(toml_key_in(storage, i));
            conf_dir = toml_table_in(storage, dir->name);
            dat = toml_string_in(conf_dir, "directory");
            dir->path = strdup(dat.u.s);
            free(dat.u.s);
            if(0 != (arr = toml_array_in(conf_dir, "files"))) {
                INIT_LIST_HEAD(&dir->files);
                nfile = toml_array_nelem(arr);
                for(j = 0; j < nfile; j++) {
                    dat = toml_string_at(arr, j);
                    file = malloc(sizeof(*file));
                    file->type = DS_FILE_NC;
                    file->name = strdup(dat.u.s);
                    free(dat.u.s);
                    list_add(&file->entry, &dir->files);
                }
            } else {
                dat = toml_string_in(conf_dir, "files");
                if(dat.ok) {
                    if(strcmp(dat.u.s, "all") == 0) {
                        dir->cont_type = DS_FILE_ALL;
                    } else {
                        fprintf(stderr,
                                "ERROR: %s: invalid value for "
                                "storage.%s.files: %s\n",
                                __func__, dir->name, dat.u.s);
                    }
                    free(dat.u.s);
                } else {
                    fprintf(stderr,
                            "ERROR: %s: no readable 'files' key for '%s'.\n",
                            __func__, dir->name);
                }
            }
            list_add(&dir->entry, dir_list);
        }
    }

    toml_free(conf);
}

static int init_sspace(dspaces_provider_t server, struct bbox *default_domain,
                       struct ds_gspace *dsg_l)
{
    int err = -ENOMEM;
    dsg_l->ssd = ssd_alloc(default_domain, dsg_l->size_sp, ds_conf.max_versions,
                           ds_conf.hash_version);
    if(!dsg_l->ssd)
        goto err_out;

    if(ds_conf.hash_version == ssd_hash_version_auto) {
        DEBUG_OUT("server selected hash version %i for default space\n",
                  dsg_l->ssd->hash_version);
    }

    err = ssd_init(dsg_l->ssd, dsg_l->rank);
    if(err < 0)
        goto err_out;

    dsg_l->default_gdim.ndim = ds_conf.ndim;
    int i;
    for(i = 0; i < ds_conf.ndim; i++) {
        dsg_l->default_gdim.sizes.c[i] = ds_conf.dims.c[i];
    }

    INIT_LIST_HEAD(&dsg_l->sspace_list);
    return 0;
err_out:
    fprintf(stderr, "%s(): ERROR failed\n", __func__);
    return err;
}

static int write_conf(dspaces_provider_t server, MPI_Comm comm)
{
    hg_addr_t my_addr = HG_ADDR_NULL;
    char *my_addr_str = NULL;
    char my_node_str[HOST_NAME_MAX];
    hg_size_t my_addr_size = 0;
    int my_node_name_len = 0;
    int *str_sizes;
    hg_return_t hret = HG_SUCCESS;
    int buf_size = 0;
    int *sizes_psum;
    char *str_buf;
    FILE *fd;
    int i;
    int ret = 0;

    hret = margo_addr_self(server->mid, &my_addr);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_addr_self() returned %d\n",
                __func__, hret);
        ret = -1;
        goto error;
    }

    hret = margo_addr_to_string(server->mid, NULL, &my_addr_size, my_addr);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_addr_to_string() returned %d\n",
                __func__, hret);
        ret = -1;
        goto errorfree;
    }

    my_addr_str = malloc(my_addr_size);
    hret =
        margo_addr_to_string(server->mid, my_addr_str, &my_addr_size, my_addr);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_addr_to_string() returned %d\n",
                __func__, hret);
        ret = -1;
        goto errorfree;
    }

    MPI_Comm_size(comm, &server->comm_size);
    str_sizes = malloc(server->comm_size * sizeof(*str_sizes));
    sizes_psum = malloc(server->comm_size * sizeof(*sizes_psum));
    MPI_Allgather(&my_addr_size, 1, MPI_INT, str_sizes, 1, MPI_INT, comm);
    sizes_psum[0] = 0;
    for(i = 0; i < server->comm_size; i++) {
        buf_size += str_sizes[i];
        if(i) {
            sizes_psum[i] = sizes_psum[i - 1] + str_sizes[i - 1];
        }
    }
    str_buf = malloc(buf_size);
    MPI_Allgatherv(my_addr_str, my_addr_size, MPI_CHAR, str_buf, str_sizes,
                   sizes_psum, MPI_CHAR, comm);

    server->server_address =
        malloc(server->comm_size * sizeof(*server->server_address));
    for(i = 0; i < server->comm_size; i++) {
        server->server_address[i] = &str_buf[sizes_psum[i]];
    }

    gethostname(my_node_str, HOST_NAME_MAX);
    my_node_str[HOST_NAME_MAX - 1] = '\0';
    my_node_name_len = strlen(my_node_str) + 1;
    MPI_Allgather(&my_node_name_len, 1, MPI_INT, str_sizes, 1, MPI_INT, comm);
    sizes_psum[0] = 0;
    buf_size = 0;
    for(i = 0; i < server->comm_size; i++) {
        buf_size += str_sizes[i];
        if(i) {
            sizes_psum[i] = sizes_psum[i - 1] + str_sizes[i - 1];
        }
    }
    str_buf = malloc(buf_size);
    MPI_Allgatherv(my_node_str, my_node_name_len, MPI_CHAR, str_buf, str_sizes,
                   sizes_psum, MPI_CHAR, comm);
    server->node_names =
        malloc(server->comm_size * sizeof(*server->node_names));
    for(i = 0; i < server->comm_size; i++) {
        server->node_names[i] = &str_buf[sizes_psum[i]];
    }

    MPI_Comm_rank(comm, &server->rank);
    if(server->rank == 0) {
        fd = fopen("conf.ds", "w");
        if(!fd) {
            fprintf(stderr,
                    "ERROR: %s: unable to open 'conf.ds' for writing.\n",
                    __func__);
            ret = -1;
            goto errorfree;
        }
        fprintf(fd, "%d\n", server->comm_size);
        for(i = 0; i < server->comm_size; i++) {
            fprintf(fd, "%s %s\n", server->node_names[i],
                    server->server_address[i]);
        }
        fprintf(fd, "%s\n", server->listen_addr_str);
#ifdef HAVE_DRC
        fprintf(fd, "%" PRIu32 "\n", server->drc_credential_id);
#endif
        fclose(fd);
    }

    free(my_addr_str);
    free(str_sizes);
    free(sizes_psum);
    margo_addr_free(server->mid, my_addr);

    return (ret);

errorfree:
    margo_addr_free(server->mid, my_addr);
error:
    margo_finalize(server->mid);
    return (ret);
}

const char *hash_strings[] = {"Dynamic", "SFC", "Bisection"};

void print_conf()
{
    int i;

    printf("DataSpaces server config:\n");
    printf("=========================\n");
    printf(" Default global dimensions: (");
    printf("%" PRIu64, ds_conf.dims.c[0]);
    for(i = 1; i < ds_conf.ndim; i++) {
        printf(", %" PRIu64, ds_conf.dims.c[i]);
    }
    printf(")\n");
    printf(" MAX STORED VERSIONS: %i\n", ds_conf.max_versions);
    printf(" HASH TYPE: %s\n", hash_strings[ds_conf.hash_version]);
    if(ds_conf.num_apps >= 0) {
        printf(" APPS EXPECTED: %i\n", ds_conf.num_apps);
    } else {
        printf(" RUN UNTIL KILLED\n");
    }
    printf("=========================\n");
}

static int dsg_alloc(dspaces_provider_t server, const char *conf_name,
                     MPI_Comm comm)
{
    struct ds_gspace *dsg_l;
    char *ext;
    int err = -ENOMEM;

    /* Default values */
    ds_conf.max_versions = 255;
    ds_conf.hash_version = ssd_hash_version_auto;
    ds_conf.num_apps = -1;

    INIT_LIST_HEAD(&server->dirs);

    ext = strrchr(conf_name, '.');
    if(!ext || strcmp(ext, ".toml") != 0) {
        err = parse_conf(conf_name);
    } else {
        err = parse_conf_toml(conf_name, &server->dirs, &server->remotes,
                              &server->nremote);
    }
    if(err < 0) {
        goto err_out;
    }

    // Check number of dimension
    if(ds_conf.ndim > BBOX_MAX_NDIM) {
        fprintf(
            stderr,
            "%s(): ERROR maximum number of array dimension is %d but ndim is %d"
            " in file '%s'\n",
            __func__, BBOX_MAX_NDIM, ds_conf.ndim, conf_name);
        err = -EINVAL;
        goto err_out;
    } else if(ds_conf.ndim == 0) {
        DEBUG_OUT(
            "no global coordinates provided. Setting trivial placeholder.\n");
        ds_conf.ndim = 1;
        ds_conf.dims.c[0] = 1;
    }

    // Check hash version
    if((ds_conf.hash_version < ssd_hash_version_auto) ||
       (ds_conf.hash_version >= _ssd_hash_version_count)) {
        fprintf(stderr, "%s(): ERROR unknown hash version %d in file '%s'\n",
                __func__, ds_conf.hash_version, conf_name);
        err = -EINVAL;
        goto err_out;
    }

    struct bbox domain;
    memset(&domain, 0, sizeof(struct bbox));
    domain.num_dims = ds_conf.ndim;
    int i;
    for(i = 0; i < domain.num_dims; i++) {
        domain.lb.c[i] = 0;
        domain.ub.c[i] = ds_conf.dims.c[i] - 1;
    }

    dsg_l = malloc(sizeof(*dsg_l));
    if(!dsg_l)
        goto err_out;

    MPI_Comm_size(comm, &(dsg_l->size_sp));

    MPI_Comm_rank(comm, &dsg_l->rank);

    if(dsg_l->rank == 0) {
        print_conf();
    }

    write_conf(server, comm);

    err = init_sspace(server, &domain, dsg_l);
    if(err < 0) {
        goto err_free;
    }
    dsg_l->ls = ls_alloc(ds_conf.max_versions);
    if(!dsg_l->ls) {
        fprintf(stderr, "%s(): ERROR ls_alloc() failed\n", __func__);
        goto err_free;
    }

    // proxy storage
    dsg_l->ps = ls_alloc(ds_conf.max_versions);
    if(!dsg_l->ps) {
        fprintf(stderr, "%s(): ERROR ls_alloc() failed\n", __func__);
        goto err_free;
    }

    dsg_l->num_apps = ds_conf.num_apps;

    INIT_LIST_HEAD(&dsg_l->obj_desc_drain_list);

    server->dsg = dsg_l;
    return 0;
err_free:
    free(dsg_l);
err_out:
    fprintf(stderr, "'%s()': failed with %d.\n", __func__, err);
    return err;
}

static int free_sspace(struct ds_gspace *dsg_l)
{
    ssd_free(dsg_l->ssd);
    struct sspace_list_entry *ssd_entry, *temp;
    list_for_each_entry_safe(ssd_entry, temp, &dsg_l->sspace_list,
                             struct sspace_list_entry, entry)
    {
        ssd_free(ssd_entry->ssd);
        list_del(&ssd_entry->entry);
        free(ssd_entry);
    }

    return 0;
}

static struct sspace *lookup_sspace(dspaces_provider_t server,
                                    const char *var_name,
                                    const struct global_dimension *gd)
{
    struct global_dimension gdim;
    struct ds_gspace *dsg_l = server->dsg;
    int i;

    memcpy(&gdim, gd, sizeof(struct global_dimension));

    if(server->f_debug) {
        DEBUG_OUT("global dimensions for %s:\n", var_name);
        for(i = 0; i < gdim.ndim; i++) {
            DEBUG_OUT(" dim[%i] = %" PRIu64 "\n", i, gdim.sizes.c[i]);
        }
    }

    // Return the default shared space created based on
    // global data domain specified in dataspaces.conf
    if(global_dimension_equal(&gdim, &dsg_l->default_gdim)) {
        DEBUG_OUT("uses default gdim\n");
        return dsg_l->ssd;
    }

    // Otherwise, search for shared space based on the
    // global data domain specified by application in put()/get().
    struct sspace_list_entry *ssd_entry = NULL;
    list_for_each_entry(ssd_entry, &dsg_l->sspace_list,
                        struct sspace_list_entry, entry)
    {
        // compare global dimension
        if(gdim.ndim != ssd_entry->gdim.ndim)
            continue;

        if(global_dimension_equal(&gdim, &ssd_entry->gdim))
            return ssd_entry->ssd;
    }

    DEBUG_OUT("didn't find an existing shared space. Make a new one.\n");

    // If not found, add new shared space
    int err;
    struct bbox domain;
    memset(&domain, 0, sizeof(struct bbox));
    domain.num_dims = gdim.ndim;
    DEBUG_OUT("global dimmensions being allocated:\n");
    for(i = 0; i < gdim.ndim; i++) {
        domain.lb.c[i] = 0;
        domain.ub.c[i] = gdim.sizes.c[i] - 1;
        DEBUG_OUT("dim %i: lb = %" PRIu64 ", ub = %" PRIu64 "\n", i,
                  domain.lb.c[i], domain.ub.c[i]);
    }

    ssd_entry = malloc(sizeof(struct sspace_list_entry));
    memcpy(&ssd_entry->gdim, &gdim, sizeof(struct global_dimension));

    DEBUG_OUT("allocate the ssd.\n");
    ssd_entry->ssd = ssd_alloc(&domain, dsg_l->size_sp, ds_conf.max_versions,
                               ds_conf.hash_version);
    if(!ssd_entry->ssd) {
        fprintf(stderr, "%s(): ssd_alloc failed for '%s'\n", __func__,
                var_name);
        return dsg_l->ssd;
    }

    if(ds_conf.hash_version == ssd_hash_version_auto) {
        DEBUG_OUT("server selected hash version %i for var %s\n",
                  ssd_entry->ssd->hash_version, var_name);
    }

    DEBUG_OUT("doing ssd init\n");
    err = ssd_init(ssd_entry->ssd, dsg_l->rank);
    if(err < 0) {
        fprintf(stderr, "%s(): ssd_init failed\n", __func__);
        return dsg_l->ssd;
    }

    list_add(&ssd_entry->entry, &dsg_l->sspace_list);
    return ssd_entry->ssd;
}

static void obj_update_local_dht(dspaces_provider_t server,
                                 obj_descriptor *odsc, struct sspace *ssd,
                                 obj_update_t type)
{
    DEBUG_OUT("Add in local_dht %d\n", server->dsg->rank);
    ABT_mutex_lock(server->dht_mutex);
    switch(type) {
    case DS_OBJ_NEW:
        dht_add_entry(ssd->ent_self, odsc);
        break;
    case DS_OBJ_OWNER:
        dht_update_owner(ssd->ent_self, odsc, 1);
        break;
    default:
        fprintf(stderr, "ERROR: (%s): unknown object update type.\n", __func__);
    }
    ABT_mutex_unlock(server->dht_mutex);
}

static int obj_update_dht(dspaces_provider_t server, struct obj_data *od,
                          obj_update_t type)
{
    obj_descriptor *odsc = &od->obj_desc;
    DEBUG_OUT("getting sspace lock.\n");
    ABT_mutex_lock(server->sspace_mutex);
    DEBUG_OUT("got sspace lock.\n");
    struct sspace *ssd = lookup_sspace(server, odsc->name, &od->gdim);
    DEBUG_OUT("realeasing sspace lock.\n");
    ABT_mutex_unlock(server->sspace_mutex);
    struct dht_entry *dht_tab[ssd->dht->num_entries];

    int num_de, i;

    /* Compute object distribution to nodes in the space. */
    num_de = ssd_hash(ssd, &odsc->bb, dht_tab);
    if(num_de == 0) {
        DEBUG_OUT("Could not distribute the object in a spatial index. Storing "
                  "locally.\n");
        obj_update_local_dht(server, odsc, ssd, type);
    }

    for(i = 0; i < num_de; i++) {
        if(dht_tab[i]->rank == server->dsg->rank) {
            obj_update_local_dht(server, odsc, ssd, type);
            continue;
        }

        // now send rpc to the server for dht_update
        hg_return_t hret;
        odsc_gdim_t in;
        margo_request req;
        DEBUG_OUT("Server %d sending object %s to dht server %d \n",
                  server->dsg->rank, obj_desc_sprint(odsc), dht_tab[i]->rank);

        in.odsc_gdim.size = sizeof(*odsc);
        in.odsc_gdim.gdim_size = sizeof(struct global_dimension);
        in.odsc_gdim.raw_odsc = (char *)(odsc);
        in.odsc_gdim.raw_gdim = (char *)(&od->gdim);
        in.param = type;

        hg_addr_t svr_addr;
        margo_addr_lookup(server->mid, server->server_address[dht_tab[i]->rank],
                          &svr_addr);

        hg_handle_t h;
        margo_create(server->mid, svr_addr, server->obj_update_id, &h);
        margo_iforward(h, &in, &req);
        DEBUG_OUT("sent obj server %d to update dht %s in \n", dht_tab[i]->rank,
                  obj_desc_sprint(odsc));

        margo_addr_free(server->mid, svr_addr);
        hret = margo_destroy(h);
        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): could not destroy handle!\n",
                    __func__);
            return (dspaces_ERR_MERCURY);
        }
    }

    return dspaces_SUCCESS;
}

static int get_client_data(obj_descriptor odsc, dspaces_provider_t server)
{
    bulk_in_t in;
    bulk_out_t out;
    struct obj_data *od;
    od = malloc(sizeof(struct obj_data));
    int ret;

    od = obj_data_alloc(&odsc);
    in.odsc.size = sizeof(obj_descriptor);
    in.odsc.raw_odsc = (char *)(&odsc);

    hg_addr_t owner_addr;
    size_t owner_addr_size = 128;

    margo_addr_self(server->mid, &owner_addr);
    margo_addr_to_string(server->mid, od->obj_desc.owner, &owner_addr_size,
                         owner_addr);
    margo_addr_free(server->mid, owner_addr);

    hg_size_t rdma_size = (odsc.size) * bbox_volume(&odsc.bb);

    margo_bulk_create(server->mid, 1, (void **)(&(od->data)), &rdma_size,
                      HG_BULK_WRITE_ONLY, &in.handle);
    hg_addr_t client_addr;
    margo_addr_lookup(server->mid, odsc.owner, &client_addr);

    hg_handle_t handle;
    margo_create(server->mid, client_addr, server->drain_id, &handle);
    margo_forward(handle, &in);
    margo_get_output(handle, &out);
    if(out.ret == dspaces_SUCCESS) {
        ABT_mutex_lock(server->ls_mutex);
        ls_add_obj(server->dsg->ls, od);
        ABT_mutex_unlock(server->ls_mutex);
    }
    ret = out.ret;
    // now update the dht with new owner information
    DEBUG_OUT("Inside get_client_data\n");
    margo_addr_free(server->mid, client_addr);
    margo_bulk_free(in.handle);
    margo_free_output(handle, &out);
    margo_destroy(handle);
    obj_update_dht(server, od, DS_OBJ_OWNER);
    return ret;
}

// thread to move data between layers
static void drain_thread(void *arg)
{
    dspaces_provider_t server = arg;

    while(server->f_kill != 0) {
        int counter = 0;
        DEBUG_OUT("Thread WOKEUP\n");
        do {
            counter = 0;
            obj_descriptor odsc;
            struct obj_desc_list *odscl;
            // requires better way to get the obj_descriptor
            ABT_mutex_lock(server->odsc_mutex);
            DEBUG_OUT("Inside odsc mutex\n");
            list_for_each_entry(odscl, &(server->dsg->obj_desc_drain_list),
                                struct obj_desc_list, odsc_entry)
            {
                memcpy(&odsc, &(odscl->odsc), sizeof(obj_descriptor));
                DEBUG_OUT("Found %s in odsc_list\n", obj_desc_sprint(&odsc));
                counter = 1;
                break;
            }
            if(counter == 1) {
                list_del(&odscl->odsc_entry);
                ABT_mutex_unlock(server->odsc_mutex);
                int ret = get_client_data(odsc, server);
                DEBUG_OUT("Finished draining %s\n", obj_desc_sprint(&odsc));
                if(ret != dspaces_SUCCESS) {
                    ABT_mutex_lock(server->odsc_mutex);
                    DEBUG_OUT("Drain failed, returning object to queue...\n");
                    list_add_tail(&odscl->odsc_entry,
                                  &server->dsg->obj_desc_drain_list);
                    ABT_mutex_unlock(server->odsc_mutex);
                }
                sleep(1);
            } else {
                ABT_mutex_unlock(server->odsc_mutex);
            }

        } while(counter == 1);

        sleep(10);

        ABT_thread_yield();
    }
}

#ifdef DSPACES_HAVE_PYTHON
static void *bootstrap_python()
{
    Py_Initialize();
    import_array();
}

static int dspaces_init_py_mods(dspaces_provider_t server,
                                struct dspaces_module **pmodsp)
{
    char *pypath = getenv("PYTHONPATH");
    char *new_pypath;
    int pypath_len;
    struct dspaces_module *pmods;
    int npmods = 1;
    PyObject *pName;

    pypath_len = strlen(xstr(DSPACES_MOD_DIR)) + 1;
    if(pypath) {
        pypath_len += strlen(pypath) + 1;
    }
    new_pypath = malloc(pypath_len);
    if(pypath) {
        sprintf(new_pypath, "%s:%s", xstr(DSPACES_MOD_DIR), pypath);
    } else {
        strcpy(new_pypath, xstr(DSPACES_MOD_DIR));
    }
    setenv("PYTHONPATH", new_pypath, 1);
    DEBUG_OUT("New PYTHONPATH is %s\n", new_pypath);

    bootstrap_python();

    pmods = malloc(sizeof(*pmods) * npmods);
    pmods[0].name = strdup("goes17");
    pmods[0].type = DSPACES_MOD_PY;
    pName = PyUnicode_DecodeFSDefault("s3nc_mod");
    pmods[0].pModule = PyImport_Import(pName);
    if(pmods[0].pModule == NULL) {
        fprintf(stderr,
                "WARNING: could not load s3nc mod from %s. File missing? Any "
                "s3nc accesses will fail.\n",
                xstr(DSPACES_MOD_DIR));
    }
    Py_DECREF(pName);

    free(new_pypath);

    *pmodsp = pmods;

    return (npmods);
}
#endif // DSPACES_HAVE_PYTHON

void dspaces_init_mods(dspaces_provider_t server)
{
#ifdef DSPACES_HAVE_PYTHON
    server->nmods = dspaces_init_py_mods(server, &server->mods);
#endif // DSPACES_HAVE_PYTHON
}

int dspaces_server_init(const char *listen_addr_str, MPI_Comm comm,
                        const char *conf_file, dspaces_provider_t *sv)
{
    const char *envdebug = getenv("DSPACES_DEBUG");
    const char *envnthreads = getenv("DSPACES_NUM_HANDLERS");
    const char *envdrain = getenv("DSPACES_DRAIN");
    dspaces_provider_t server;
    hg_class_t *hg;
    static int is_initialized = 0;
    hg_bool_t flag;
    hg_id_t id;
    int num_handlers = DSPACES_DEFAULT_NUM_HANDLERS;
    struct hg_init_info hii = {0};
    char margo_conf[1024];
    struct margo_init_info mii = {0};
    int i, ret;

    if(is_initialized) {
        fprintf(stderr,
                "DATASPACES: WARNING: %s: multiple instantiations of the "
                "dataspaces server is not supported.\n",
                __func__);
        return (dspaces_ERR_ALLOCATION);
    }

    server = (dspaces_provider_t)calloc(1, sizeof(*server));
    if(server == NULL)
        return dspaces_ERR_ALLOCATION;

    if(envdebug) {
        server->f_debug = 1;
    }

    if(envnthreads) {
        num_handlers = atoi(envnthreads);
    }

    if(envdrain) {
        DEBUG_OUT("enabling data draining.\n");
        server->f_drain = 1;
    }

    MPI_Comm_dup(comm, &server->comm);
    MPI_Comm_rank(comm, &server->rank);

    dspaces_init_mods(server);

    const char *mod_dir_str = xstr(DSPACES_MOD_DIR);
    DEBUG_OUT("module directory is %s\n", mod_dir_str);

    margo_set_environment(NULL);
    sprintf(margo_conf,
            "{ \"use_progress_thread\" : true, \"rpc_thread_count\" : %d }",
            num_handlers);
    hii.request_post_init = 1024;
    hii.auto_sm = 0;
    mii.hg_init_info = &hii;
    mii.json_config = margo_conf;
    ABT_init(0, NULL);

#ifdef HAVE_DRC

    server->drc_credential_id = 0;
    if(server->rank == 0) {
        ret =
            drc_acquire(&server->drc_credential_id, DRC_FLAGS_FLEX_CREDENTIAL);
        if(ret != DRC_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): drc_acquire failure %d\n", __func__,
                    ret);
            return ret;
        }
    }
    MPI_Bcast(&server->drc_credential_id, 1, MPI_UINT32_T, 0, comm);

    /* access credential on all ranks and convert to string for use by mercury
     */

    drc_info_handle_t drc_credential_info;
    uint32_t drc_cookie;
    char drc_key_str[256] = {0};

    ret = drc_access(server->drc_credential_id, 0, &drc_credential_info);
    if(ret != DRC_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): drc_access failure %d\n", __func__, ret);
        return ret;
    }

    drc_cookie = drc_get_first_cookie(drc_credential_info);
    sprintf(drc_key_str, "%u", drc_cookie);

    memset(&hii, 0, sizeof(hii));
    hii.na_init_info.auth_key = drc_key_str;

    /* rank 0 grants access to the credential, allowing other jobs to use it */
    if(server->rank == 0) {
        ret = drc_grant(server->drc_credential_id, drc_get_wlm_id(),
                        DRC_FLAGS_TARGET_WLM);
        if(ret != DRC_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): drc_grants failure %d\n", __func__,
                    ret);
            return ret;
        }
    }

    server->mid = margo_init_ext(listen_addr_str, MARGO_SERVER_MODE, &mii);

#else

    server->mid = margo_init_ext(listen_addr_str, MARGO_SERVER_MODE, &mii);
    if(server->f_debug) {
        if(!server->rank) {
            char *margo_json = margo_get_config(server->mid);
            fprintf(stderr, "%s", margo_json);
            free(margo_json);
        }
        margo_set_log_level(server->mid, MARGO_LOG_WARNING);
    }
    MPI_Barrier(comm);

#endif /* HAVE_DRC */
    DEBUG_OUT("did margo init\n");
    if(!server->mid) {
        fprintf(stderr, "ERROR: %s: margo_init() failed.\n", __func__);
        return (dspaces_ERR_MERCURY);
    }
    server->listen_addr_str = strdup(listen_addr_str);

    ABT_mutex_create(&server->odsc_mutex);
    ABT_mutex_create(&server->ls_mutex);
    ABT_mutex_create(&server->dht_mutex);
    ABT_mutex_create(&server->sspace_mutex);
    ABT_mutex_create(&server->kill_mutex);

    hg = margo_get_class(server->mid);

    margo_registered_name(server->mid, "put_rpc", &id, &flag);

    if(flag == HG_TRUE) { /* RPCs already registered */
        DEBUG_OUT("RPC names already registered. Setting handlers...\n");
        margo_registered_name(server->mid, "put_rpc", &server->put_id, &flag);
        DS_HG_REGISTER(hg, server->put_id, bulk_gdim_t, bulk_out_t, put_rpc);
        margo_registered_name(server->mid, "put_local_rpc",
                              &server->put_local_id, &flag);
        DS_HG_REGISTER(hg, server->put_local_id, odsc_gdim_t, bulk_out_t,
                       put_local_rpc);
        margo_registered_name(server->mid, "put_meta_rpc", &server->put_meta_id,
                              &flag);
        DS_HG_REGISTER(hg, server->put_meta_id, put_meta_in_t, bulk_out_t,
                       put_meta_rpc);
        margo_registered_name(server->mid, "get_rpc", &server->get_id, &flag);
        DS_HG_REGISTER(hg, server->get_id, bulk_in_t, bulk_out_t, get_rpc);
        margo_registered_name(server->mid, "get_local_rpc",
                              &server->get_local_id, &flag);
        margo_registered_name(server->mid, "query_rpc", &server->query_id,
                              &flag);
        DS_HG_REGISTER(hg, server->query_id, odsc_gdim_t, odsc_list_t,
                       query_rpc);
        margo_registered_name(server->mid, "peek_meta_rpc",
                              &server->peek_meta_id, &flag);
        DS_HG_REGISTER(hg, server->peek_meta_id, peek_meta_in_t,
                       peek_meta_out_t, peek_meta_rpc);
        margo_registered_name(server->mid, "query_meta_rpc",
                              &server->query_meta_id, &flag);
        DS_HG_REGISTER(hg, server->query_meta_id, query_meta_in_t,
                       query_meta_out_t, query_meta_rpc);
        margo_registered_name(server->mid, "obj_update_rpc",
                              &server->obj_update_id, &flag);
        DS_HG_REGISTER(hg, server->obj_update_id, odsc_gdim_t, void,
                       obj_update_rpc);
        margo_registered_name(server->mid, "odsc_internal_rpc",
                              &server->odsc_internal_id, &flag);
        DS_HG_REGISTER(hg, server->odsc_internal_id, odsc_gdim_t, odsc_list_t,
                       odsc_internal_rpc);
        margo_registered_name(server->mid, "ss_rpc", &server->ss_id, &flag);
        DS_HG_REGISTER(hg, server->ss_id, void, ss_information, ss_rpc);
        margo_registered_name(server->mid, "drain_rpc", &server->drain_id,
                              &flag);
        margo_registered_name(server->mid, "kill_rpc", &server->kill_id, &flag);
        DS_HG_REGISTER(hg, server->kill_id, int32_t, void, kill_rpc);
        margo_registered_name(server->mid, "kill_client_rpc",
                              &server->kill_client_id, &flag);
        margo_registered_name(server->mid, "sub_rpc", &server->sub_id, &flag);
        DS_HG_REGISTER(hg, server->sub_id, odsc_gdim_t, void, sub_rpc);
        margo_registered_name(server->mid, "notify_rpc", &server->notify_id,
                              &flag);
        margo_registered_name(server->mid, "do_ops_rpc", &server->do_ops_id,
                              &flag);
        DS_HG_REGISTER(hg, server->do_ops_id, do_ops_in_t, bulk_out_t,
                       do_ops_rpc);
#ifdef DSPACES_HAVE_PYTHON
        margo_registered_name(server->mid, "pexec_rpc", &server->pexec_id,
                              &flag);
        DS_HG_REGISTER(hg, server->pexec_id, pexec_in_t, pexec_out_t,
                       pexec_rpc);
#endif // DSPACES_HAVE_PYTHON
        margo_registered_name(server->mid, "cond_rpc", &server->cond_id, &flag);
        DS_HG_REGISTER(hg, server->cond_id, cond_in_t, void, cond_rpc);
        margo_registered_name(server->mid, "get_vars_rpc", &server->get_vars_id,
                              &flag);
        DS_HG_REGISTER(hg, server->get_vars_id, int32_t, name_list_t,
                       get_vars_rpc);
        margo_registered_name(server->mid, "get_var_objs_rpc",
                              &server->get_var_objs_id, &flag);
        DS_HG_REGISTER(hg, server->get_var_objs_id, get_var_objs_in_t, odsc_hdr,
                       get_var_objs_rpc);
    } else {
        server->put_id = MARGO_REGISTER(server->mid, "put_rpc", bulk_gdim_t,
                                        bulk_out_t, put_rpc);
        margo_register_data(server->mid, server->put_id, (void *)server, NULL);
        server->put_local_id =
            MARGO_REGISTER(server->mid, "put_local_rpc", odsc_gdim_t,
                           bulk_out_t, put_local_rpc);
        margo_register_data(server->mid, server->put_local_id, (void *)server,
                            NULL);
        server->put_meta_id =
            MARGO_REGISTER(server->mid, "put_meta_rpc", put_meta_in_t,
                           bulk_out_t, put_meta_rpc);
        margo_register_data(server->mid, server->put_meta_id, (void *)server,
                            NULL);
        server->get_id = MARGO_REGISTER(server->mid, "get_rpc", bulk_in_t,
                                        bulk_out_t, get_rpc);
        server->get_local_id = MARGO_REGISTER(server->mid, "get_local_rpc",
                                              bulk_in_t, bulk_out_t, NULL);
        margo_register_data(server->mid, server->get_id, (void *)server, NULL);
        server->query_id = MARGO_REGISTER(server->mid, "query_rpc", odsc_gdim_t,
                                          odsc_list_t, query_rpc);
        margo_register_data(server->mid, server->query_id, (void *)server,
                            NULL);
        server->peek_meta_id =
            MARGO_REGISTER(server->mid, "peek_meta_rpc", peek_meta_in_t,
                           peek_meta_out_t, peek_meta_rpc);
        margo_register_data(server->mid, server->peek_meta_id, (void *)server,
                            NULL);
        server->query_meta_id =
            MARGO_REGISTER(server->mid, "query_meta_rpc", query_meta_in_t,
                           query_meta_out_t, query_meta_rpc);
        margo_register_data(server->mid, server->query_meta_id, (void *)server,
                            NULL);
        server->obj_update_id = MARGO_REGISTER(
            server->mid, "obj_update_rpc", odsc_gdim_t, void, obj_update_rpc);
        margo_register_data(server->mid, server->obj_update_id, (void *)server,
                            NULL);
        margo_registered_disable_response(server->mid, server->obj_update_id,
                                          HG_TRUE);
        server->odsc_internal_id =
            MARGO_REGISTER(server->mid, "odsc_internal_rpc", odsc_gdim_t,
                           odsc_list_t, odsc_internal_rpc);
        margo_register_data(server->mid, server->odsc_internal_id,
                            (void *)server, NULL);
        server->ss_id =
            MARGO_REGISTER(server->mid, "ss_rpc", void, ss_information, ss_rpc);
        margo_register_data(server->mid, server->ss_id, (void *)server, NULL);
        server->drain_id = MARGO_REGISTER(server->mid, "drain_rpc", bulk_in_t,
                                          bulk_out_t, NULL);
        server->kill_id =
            MARGO_REGISTER(server->mid, "kill_rpc", int32_t, void, kill_rpc);
        margo_registered_disable_response(server->mid, server->kill_id,
                                          HG_TRUE);
        margo_register_data(server->mid, server->kill_id, (void *)server, NULL);
        server->kill_client_id =
            MARGO_REGISTER(server->mid, "kill_client_rpc", int32_t, void, NULL);
        margo_registered_disable_response(server->mid, server->kill_client_id,
                                          HG_TRUE);
        server->sub_id =
            MARGO_REGISTER(server->mid, "sub_rpc", odsc_gdim_t, void, sub_rpc);
        margo_register_data(server->mid, server->sub_id, (void *)server, NULL);
        margo_registered_disable_response(server->mid, server->sub_id, HG_TRUE);
        server->notify_id =
            MARGO_REGISTER(server->mid, "notify_rpc", odsc_list_t, void, NULL);
        margo_registered_disable_response(server->mid, server->notify_id,
                                          HG_TRUE);
        server->do_ops_id = MARGO_REGISTER(server->mid, "do_ops_rpc",
                                           do_ops_in_t, bulk_out_t, do_ops_rpc);
        margo_register_data(server->mid, server->do_ops_id, (void *)server,
                            NULL);
#ifdef DSPACES_HAVE_PYTHON
        server->pexec_id = MARGO_REGISTER(server->mid, "pexec_rpc", pexec_in_t,
                                          pexec_out_t, pexec_rpc);
        margo_register_data(server->mid, server->pexec_id, (void *)server,
                            NULL);
#endif // DSPACES_HAVE_PYTHON
        server->cond_id =
            MARGO_REGISTER(server->mid, "cond_rpc", cond_in_t, void, cond_rpc);
        margo_register_data(server->mid, server->cond_id, (void *)server, NULL);
        margo_registered_disable_response(server->mid, server->cond_id,
                                          HG_TRUE);
        server->get_vars_id = MARGO_REGISTER(
            server->mid, "get_vars_rpc", int32_t, name_list_t, get_vars_rpc);
        margo_register_data(server->mid, server->get_vars_id, (void *)server,
                            NULL);
        server->get_var_objs_id =
            MARGO_REGISTER(server->mid, "get_var_objs_rpc", get_var_objs_in_t,
                           odsc_hdr, get_var_objs_rpc);
        margo_register_data(server->mid, server->get_var_objs_id,
                            (void *)server, NULL);
    }
    int err = dsg_alloc(server, conf_file, comm);
    if(err) {
        fprintf(stderr,
                "DATASPACES: ERROR: %s: could not allocate internal "
                "structures. (%d)\n",
                __func__, err);
        return (dspaces_ERR_ALLOCATION);
    }
    for(i = 0; i < server->nremote; i++) {
        DEBUG_OUT("initializing client connection to %s\n",
                  server->remotes[i].name);
        dspaces_init_wan(listen_addr_str, server->remotes[i].addr_str, 0,
                         &server->remotes[i].conn);
    }

    server->f_kill = server->dsg->num_apps;
    if(server->f_kill > 0) {
        DEBUG_OUT("Server will wait for %i kill tokens before halting.\n",
                  server->f_kill);
    } else {
        DEBUG_OUT("Server will run indefinitely.\n");
    }

    if(server->f_drain) {
        // thread to drain the data
        ABT_xstream_create(ABT_SCHED_NULL, &server->drain_xstream);
        ABT_xstream_get_main_pools(server->drain_xstream, 1,
                                   &server->drain_pool);
        ABT_thread_create(server->drain_pool, drain_thread, server,
                          ABT_THREAD_ATTR_NULL, &server->drain_t);
    }

    server->pub_ip = getenv("DSPACES_PUBLIC_IP");
    server->priv_ip = getenv("DSPACES_PRIVATE_IP");

    if(server->pub_ip) {
        DEBUG_OUT("public IP is %s\n", server->pub_ip);
    }

    if(server->priv_ip) {
        DEBUG_OUT("private IP is %s\n", server->priv_ip);
    }

    *sv = server;

    is_initialized = 1;

    return dspaces_SUCCESS;
}

static void kill_client(dspaces_provider_t server, char *client_addr)
{
    hg_addr_t server_addr;
    hg_handle_t h;
    margo_request req;
    int arg = -1;

    margo_addr_lookup(server->mid, client_addr, &server_addr);
    margo_create(server->mid, server_addr, server->kill_client_id, &h);
    margo_iforward(h, &arg, &req);
    margo_addr_free(server->mid, server_addr);
    margo_destroy(h);
}

/*
 * Clients with local data need to know when it's safe to finalize. Send kill
 * rpc to any clients in the drain list.
 */
static void kill_local_clients(dspaces_provider_t server)
{
    struct obj_desc_list *odscl;
    struct list_head client_list;
    struct addr_list_entry *client_addr, *temp;
    int found;

    INIT_LIST_HEAD(&client_list);

    DEBUG_OUT("Killing clients with local storage.\n");

    ABT_mutex_lock(server->odsc_mutex);
    list_for_each_entry(odscl, &(server->dsg->obj_desc_drain_list),
                        struct obj_desc_list, odsc_entry)
    {
        found = 0;
        list_for_each_entry(client_addr, &client_list, struct addr_list_entry,
                            entry)
        {
            if(strcmp(client_addr->addr, odscl->odsc.owner) == 0) {
                found = 1;
                break;
            }
        }
        if(!found) {
            DEBUG_OUT("Adding %s to kill list.\n", odscl->odsc.owner);
            client_addr = malloc(sizeof(*client_addr));
            client_addr->addr = strdup(odscl->odsc.owner);
            list_add(&client_addr->entry, &client_list);
        }
    }
    ABT_mutex_unlock(server->odsc_mutex);

    list_for_each_entry_safe(client_addr, temp, &client_list,
                             struct addr_list_entry, entry)
    {
        DEBUG_OUT("Sending kill signal to %s.\n", client_addr->addr);
        kill_client(server, client_addr->addr);
        list_del(&client_addr->entry);
        free(client_addr->addr);
        free(client_addr);
    }
}

static int server_destroy(dspaces_provider_t server)
{
    int i;
    MPI_Barrier(server->comm);
    DEBUG_OUT("Finishing up, waiting for asynchronous jobs to finish...\n");

    if(server->f_drain) {
        ABT_thread_free(&server->drain_t);
        ABT_xstream_join(server->drain_xstream);
        ABT_xstream_free(&server->drain_xstream);
        DEBUG_OUT("drain thread stopped.\n");
    }

    kill_local_clients(server);

    // Hack to avoid possible argobots race condition. Need to track this down
    // at some point.
    sleep(5);

    for(i = 0; i < server->nremote; i++) {
        dspaces_fini(server->remotes[i].conn);
    }

    free_sspace(server->dsg);
    ls_free(server->dsg->ls);
    free(server->dsg);
    free(server->server_address[0]);
    free(server->server_address);
    free(server->listen_addr_str);

    MPI_Barrier(server->comm);
    MPI_Comm_free(&server->comm);
    DEBUG_OUT("finalizing server.\n");
    margo_finalize(server->mid);
    DEBUG_OUT("finalized server.\n");
    return 0;
}

static void address_translate(dspaces_provider_t server, char *addr_str)
{
    char *addr_loc = strstr(addr_str, server->priv_ip);
    char *addr_tail;
    int publen, privlen;

    if(addr_loc) {
        DEBUG_OUT("translating %s.\n", addr_str);
        publen = strlen(server->pub_ip);
        privlen = strlen(server->priv_ip);
        addr_tail = strdup(addr_loc + privlen);
        strcpy(addr_loc, server->pub_ip);
        strcat(addr_str, addr_tail);
        free(addr_tail);
        DEBUG_OUT("translated address: %s\n", addr_str);
    } else {
        DEBUG_OUT("no translation needed.\n");
    }
}

static void odsc_take_ownership(dspaces_provider_t server, obj_descriptor *odsc)
{
    hg_addr_t owner_addr;
    size_t owner_addr_size = 128;

    margo_addr_self(server->mid, &owner_addr);
    margo_addr_to_string(server->mid, odsc->owner, &owner_addr_size,
                         owner_addr);
    if(server->pub_ip && server->priv_ip) {
        address_translate(server, odsc->owner);
    }
}

static void put_rpc(hg_handle_t handle)
{
    hg_return_t hret;
    bulk_gdim_t in;
    bulk_out_t out;
    hg_bulk_t bulk_handle;
    struct timeval start, stop;

    margo_instance_id mid = margo_hg_handle_get_instance(handle);

    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);

    if(server->f_kill == 0) {
        fprintf(stderr, "WARNING: put rpc received when server is finalizing. "
                        "This will likely cause problems...\n");
    }

    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_get_input() failed with "
                "%d.\n",
                __func__, hret);
        margo_destroy(handle);
        return;
    }

    obj_descriptor in_odsc;
    memcpy(&in_odsc, in.odsc.raw_odsc, sizeof(in_odsc));
    // set the owner to be this server address
    odsc_take_ownership(server, &in_odsc);

    struct obj_data *od;
    od = obj_data_alloc(&in_odsc);
    memcpy(&od->gdim, in.odsc.raw_gdim, sizeof(struct global_dimension));

    if(!od)
        fprintf(stderr, "ERROR: (%s): object allocation failed!\n", __func__);

    // do write lock

    hg_size_t size = (in_odsc.size) * bbox_volume(&(in_odsc.bb));

    DEBUG_OUT("Creating a bulk transfer buffer of size %li\n", size);

    hret = margo_bulk_create(mid, 1, (void **)&(od->data), &size,
                             HG_BULK_WRITE_ONLY, &bulk_handle);

    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_bulk_create failed!\n", __func__);
        out.ret = dspaces_ERR_MERCURY;
        margo_respond(handle, &out);
        margo_free_input(handle, &in);
        margo_destroy(handle);
        return;
    }

    gettimeofday(&start, NULL);

    hret = margo_bulk_transfer(mid, HG_BULK_PULL, info->addr, in.handle, 0,
                               bulk_handle, 0, size);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_bulk_transfer failed!\n", __func__);
        out.ret = dspaces_ERR_MERCURY;
        margo_respond(handle, &out);
        margo_free_input(handle, &in);
        margo_bulk_free(bulk_handle);
        margo_destroy(handle);
        return;
    }

    gettimeofday(&stop, NULL);

    if(server->f_debug) {
        long dsec = stop.tv_sec - start.tv_sec;
        long dusec = stop.tv_usec - start.tv_usec;
        float transfer_time = (float)dsec + (dusec / 1000000.0);
        DEBUG_OUT("got %" PRIu64 " bytes in %f sec\n", size, transfer_time);
    }

    ABT_mutex_lock(server->ls_mutex);
    ls_add_obj(server->dsg->ls, od);
    ABT_mutex_unlock(server->ls_mutex);

    DEBUG_OUT("Received obj %s\n", obj_desc_sprint(&od->obj_desc));

    // now update the dht
    out.ret = dspaces_SUCCESS;
    margo_bulk_free(bulk_handle);
    margo_respond(handle, &out);
    margo_free_input(handle, &in);
    margo_destroy(handle);

    obj_update_dht(server, od, DS_OBJ_NEW);
    DEBUG_OUT("Finished obj_put_update from put_rpc\n");
}
DEFINE_MARGO_RPC_HANDLER(put_rpc)

static void put_local_rpc(hg_handle_t handle)
{
    hg_return_t hret;
    odsc_gdim_t in;
    bulk_out_t out;

    margo_instance_id mid = margo_hg_handle_get_instance(handle);

    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);

    DEBUG_OUT("In the local put rpc\n");

    if(server->f_kill == 0) {
        fprintf(stderr,
                "WARNING: (%s): got put rpc with local storage, but server is "
                "shutting down. This will likely cause problems...\n",
                __func__);
    }

    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_get_input() failed with "
                "%d.\n",
                __func__, hret);
        margo_destroy(handle);
        return;
    }

    obj_descriptor in_odsc;
    memcpy(&in_odsc, in.odsc_gdim.raw_odsc, sizeof(in_odsc));

    struct obj_data *od;
    od = obj_data_alloc_no_data(&in_odsc, NULL);
    memcpy(&od->gdim, in.odsc_gdim.raw_gdim, sizeof(struct global_dimension));

    if(!od)
        fprintf(stderr, "ERROR: (%s): failed to allocate object data!\n",
                __func__);

    DEBUG_OUT("Received obj %s  in put_local_rpc\n",
              obj_desc_sprint(&od->obj_desc));

    // now update the dht
    obj_update_dht(server, od, DS_OBJ_NEW);
    DEBUG_OUT("Finished obj_put_local_update in local_put\n");

    // add to the local list for marking as to be drained data
    struct obj_desc_list *odscl;
    odscl = malloc(sizeof(*odscl));
    memcpy(&odscl->odsc, &od->obj_desc, sizeof(obj_descriptor));

    ABT_mutex_lock(server->odsc_mutex);
    DEBUG_OUT("Adding drain list entry.\n");
    list_add_tail(&odscl->odsc_entry, &server->dsg->obj_desc_drain_list);
    ABT_mutex_unlock(server->odsc_mutex);

    // TODO: wake up thread to initiate draining
    out.ret = dspaces_SUCCESS;
    margo_respond(handle, &out);
    margo_free_input(handle, &in);
    margo_destroy(handle);

    free(od);
}
DEFINE_MARGO_RPC_HANDLER(put_local_rpc)

static void put_meta_rpc(hg_handle_t handle)
{
    margo_instance_id mid = margo_hg_handle_get_instance(handle);
    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);
    hg_return_t hret;
    put_meta_in_t in;
    bulk_out_t out;
    struct meta_data *mdata;
    hg_size_t rdma_size;
    hg_bulk_t bulk_handle;

    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_get_input() failed with "
                "%d.\n",
                __func__, hret);
        margo_destroy(handle);
        return;
    }

    DEBUG_OUT("Received meta data of length %d, name '%s' version %d.\n",
              in.length, in.name, in.version);

    mdata = malloc(sizeof(*mdata));
    mdata->name = strdup(in.name);
    mdata->version = in.version;
    mdata->length = in.length;
    mdata->data = (in.length > 0) ? malloc(in.length) : NULL;

    rdma_size = mdata->length;
    if(rdma_size > 0) {
        hret = margo_bulk_create(mid, 1, (void **)&mdata->data, &rdma_size,
                                 HG_BULK_WRITE_ONLY, &bulk_handle);

        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): margo_bulk_create failed!\n",
                    __func__);
            out.ret = dspaces_ERR_MERCURY;
            margo_respond(handle, &out);
            margo_free_input(handle, &in);
            margo_bulk_free(bulk_handle);
            margo_destroy(handle);
            return;
        }

        hret = margo_bulk_transfer(mid, HG_BULK_PULL, info->addr, in.handle, 0,
                                   bulk_handle, 0, rdma_size);
        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): margo_bulk_transfer failed!\n",
                    __func__);
            out.ret = dspaces_ERR_MERCURY;
            margo_respond(handle, &out);
            margo_free_input(handle, &in);
            margo_bulk_free(bulk_handle);
            margo_destroy(handle);
            return;
        }
    }

    DEBUG_OUT("adding to metaddata store.\n");

    ABT_mutex_lock(server->ls_mutex);
    ls_add_meta(server->dsg->ls, mdata);
    ABT_mutex_unlock(server->ls_mutex);

    DEBUG_OUT("successfully stored.\n");

    out.ret = dspaces_SUCCESS;
    if(rdma_size > 0) {
        margo_bulk_free(bulk_handle);
    }
    margo_respond(handle, &out);
    margo_free_input(handle, &in);
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(put_meta_rpc)

static int query_remotes(dspaces_provider_t server, obj_descriptor *q_odsc,
                         struct global_dimension *q_gdim, int timeout,
                         obj_descriptor **results, int req_id)
{
    uint64_t buf_size;
    void *buffer = NULL;
    int found = 0;
    obj_descriptor *odsc;
    struct obj_data *od;
    hg_addr_t owner_addr;
    size_t owner_addr_size = 128;
    int i;

    buf_size = obj_data_size(q_odsc);
    buffer = malloc(buf_size);

    DEBUG_OUT("%i remotes to query\n", server->nremote);
    for(i = 0; i < server->nremote; i++) {
        DEBUG_OUT("req %i: querying remote %s\n", req_id,
                  server->remotes[i].name);
        dspaces_define_gdim(server->remotes[i].conn, q_odsc->name, q_gdim->ndim,
                            q_gdim->sizes.c);
        if(dspaces_get(server->remotes[i].conn, q_odsc->name, q_odsc->version,
                       q_odsc->size, q_odsc->bb.num_dims, q_odsc->bb.lb.c,
                       q_odsc->bb.ub.c, buffer, 0) == 0) {
            DEBUG_OUT("found data\n");
            found = 1;
            break;
        }
    }

    if(found) {
        odsc = calloc(1, sizeof(*odsc));
        odsc->version = q_odsc->version;
        margo_addr_self(server->mid, &owner_addr);
        margo_addr_to_string(server->mid, odsc->owner, &owner_addr_size,
                             owner_addr);
        margo_addr_free(server->mid, owner_addr);
        // Selectively translate address?
        odsc->st = q_odsc->st;
        odsc->size = q_odsc->size;
        memcpy(&odsc->bb, &q_odsc->bb, sizeof(odsc->bb));
        od = obj_data_alloc_no_data(odsc, buffer);
        *results = odsc;
        if(server->f_debug) {
            DEBUG_OUT("created local object %s\n", obj_desc_sprint(odsc));
        }
        ABT_mutex_lock(server->ls_mutex);
        ls_add_obj(server->dsg->ps, od);
        ABT_mutex_unlock(server->ls_mutex);
        return (sizeof(obj_descriptor));
    } else {
        DEBUG_OUT("req %i: not found on any remotes.\n", req_id);
        *results = NULL;
        return (0);
    }
}

struct dspaces_module *dspaces_find_mod(dspaces_provider_t server,
                                        const char *mod_name)
{
    int i;

    for(i = 0; i < server->nmods; i++) {
        if(strcmp(mod_name, server->mods[i].name) == 0) {
            return (&server->mods[i]);
        }
    }

    return (NULL);
}

#ifdef DSPACES_HAVE_PYTHON
PyObject *py_obj_from_arg(struct dspaces_module_args *arg)
{
    PyObject *pArg;
    int i;

    switch(arg->type) {
    case DSPACES_ARG_REAL:
        if(arg->len == -1) {
            return (PyFloat_FromDouble(arg->rval));
        }
        pArg = PyTuple_New(arg->len);
        for(i = 0; i < arg->len; i++) {
            PyTuple_SetItem(pArg, i, PyFloat_FromDouble(arg->rarray[i]));
        }
        return (pArg);
    case DSPACES_ARG_INT:
        if(arg->len == -1) {
            return (PyLong_FromLong(arg->ival));
        }
        pArg = PyTuple_New(arg->len);
        for(i = 0; i < arg->len; i++) {
            PyTuple_SetItem(pArg, i, PyLong_FromLong(arg->iarray[i]));
        }
        return (pArg);
    case DSPACES_ARG_STR:
        return (PyUnicode_DecodeFSDefault(arg->strval));
    case DSPACES_ARG_NONE:
        return (Py_None);
    default:
        fprintf(stderr, "ERROR: unknown arg type in %s (%d)\n", __func__,
                arg->type);
    }

    return (NULL);
}

static struct dspaces_module_ret *py_res_buf(PyObject *pResult)
{
    PyArrayObject *pArray;
    struct dspaces_module_ret *ret = malloc(sizeof(*ret));
    size_t data_len;
    npy_intp *dims;
    int i;

    pArray = (PyArrayObject *)pResult;
    ret->ndim = PyArray_NDIM(pArray);
    ret->dim = malloc(sizeof(*ret->dim * ret->ndim));
    dims = PyArray_DIMS(pArray);
    ret->len = 1;
    for(i = 0; i < ret->ndim; i++) {
        ret->dim[i] = dims[i];
        ret->len *= dims[i];
    }
    ret->tag = PyArray_TYPE(pArray);
    ret->elem_size = PyArray_ITEMSIZE(pArray);
    data_len = ret->len * ret->elem_size;
    ret->data = malloc(data_len);
    memcpy(ret->data, PyArray_DATA(pArray), data_len);

    return (ret);
}

static struct dspaces_module_ret *py_res_to_ret(PyObject *pResult, int ret_type)
{
    struct dspaces_module_ret *ret;

    switch(ret_type) {
    case DSPACES_MOD_RET_ARRAY:
        return (py_res_buf(pResult));
    default:
        fprintf(stderr, "ERROR: unknown module return type in %s (%d)\n",
                __func__, ret_type);
        return (NULL);
    }
}

static struct dspaces_module_ret *
dspaces_module_py_exec(dspaces_provider_t server, struct dspaces_module *mod,
                       const char *operation, struct dspaces_module_args *args,
                       int nargs, int ret_type)
{
    PyObject *pFunc = PyObject_GetAttrString(mod->pModule, operation);
    PyObject *pKey, *pArg, *pArgs, *pKWArgs;
    PyObject *pResult;
    struct dspaces_module_ret *ret;
    int i;

    DEBUG_OUT("doing python module exec.\n");
    if(!pFunc || !PyCallable_Check(pFunc)) {
        fprintf(
            stderr,
            "ERROR! Could not find executable function '%s' in module '%s'\n",
            operation, mod->name);
        return (NULL);
    }
    pArgs = PyTuple_New(0);
    pKWArgs = PyDict_New();
    for(i = 0; i < nargs; i++) {
        pKey = PyUnicode_DecodeFSDefault(args[i].name);
        pArg = py_obj_from_arg(&args[i]);
        PyDict_SetItem(pKWArgs, pKey, pArg);
        Py_DECREF(pKey);
        Py_DECREF(pArg);
    }
    pResult = PyObject_Call(pFunc, pArgs, pKWArgs);
    if(!pResult) {
        PyErr_Print();
        ret = NULL;
    } else {
        ret = py_res_to_ret(pResult, ret_type);
        Py_DECREF(pResult);
    }

    Py_DECREF(pArgs);
    Py_DECREF(pKWArgs);
    Py_DECREF(pFunc);

    return (ret);
}
#endif // DSPACES_HAVE_PYTHON

static struct dspaces_module_ret *
dspaces_module_exec(dspaces_provider_t server, const char *mod_name,
                    const char *operation, struct dspaces_module_args *args,
                    int nargs, int ret_type)
{
    struct dspaces_module *mod = dspaces_find_mod(server, mod_name);

    DEBUG_OUT("sending '%s' to module '%s'\n", operation, mod->name);
    if(mod->type == DSPACES_MOD_PY) {
#ifdef DSPACES_HAVE_PYTHON
        return (dspaces_module_py_exec(server, mod, operation, args, nargs,
                                       ret_type));
#else
        fprintf(stderr, "WARNNING: tried to execute python module, but not "
                        "python support.\n");
        return (NULL);
#endif // DSPACES_HAVE_PYTHON
    } else {
        fprintf(stderr, "ERROR: unknown module request in %s.\n", __func__);
        return (NULL);
    }
}

static int build_module_args_from_odsc(obj_descriptor *odsc,
                                       struct dspaces_module_args **argsp)
{
    struct dspaces_module_args *args;
    int nargs = 4;
    int ndims;
    int i;

    args = malloc(sizeof(*args) * nargs);

    // odsc->name
    args[0].name = strdup("name");
    args[0].type = DSPACES_ARG_STR;
    args[0].len = strlen(odsc->name) + 1;
    args[0].strval = strdup(odsc->name);

    // odsc->version
    args[1].name = strdup("version");
    args[1].type = DSPACES_ARG_INT;
    args[1].len = -1;
    args[1].ival = odsc->version;

    ndims = odsc->bb.num_dims;
    // odsc->bb.lb
    args[2].name = strdup("lb");
    if(ndims > 0) {
        args[2].name = strdup("lb");
        args[2].type = DSPACES_ARG_INT;
        args[2].len = ndims;
        args[2].iarray = malloc(sizeof(*args[2].iarray) * ndims);
        for(i = 0; i < ndims; i++) {
            args[2].iarray[i] = odsc->bb.lb.c[i];
        }
    } else {
        args[2].type = DSPACES_ARG_NONE;
    }

    // odsc->bb.ub
    args[3].name = strdup("ub");
    if(ndims > 0) {
        args[3].type = DSPACES_ARG_INT;
        args[3].len = ndims;
        args[3].iarray = malloc(sizeof(*args[3].iarray) * ndims);
        for(i = 0; i < ndims; i++) {
            args[3].iarray[i] = odsc->bb.ub.c[i];
        }
    } else {
        args[3].type = DSPACES_ARG_NONE;
    }

    *argsp = args;
    return (nargs);
}

static void free_arg(struct dspaces_module_args *arg)
{
    if(arg) {
        free(arg->name);
        if(arg->len > 0 && arg->type != DSPACES_ARG_NONE) {
            free(arg->strval);
        }
    } else {
        fprintf(stderr, "WARNING: trying to free NULL argument in %s\n",
                __func__);
    }
}

static void free_arg_list(struct dspaces_module_args *args, int len)
{
    int i;

    if(args) {
        for(i = 0; i < len; i++) {
            free_arg(&args[i]);
        }
    } else if(len > 0) {
        fprintf(stderr, "WARNING: trying to free NULL argument list in %s\n",
                __func__);
    }
}

static void route_request(dspaces_provider_t server, obj_descriptor *odsc,
                          struct global_dimension *gdim)
{
    const char *s3nc_nspace = "goes17\\";
    struct dspaces_module_args *args;
    struct dspaces_module_ret *res = NULL;
    struct obj_data *od;
    obj_descriptor *od_odsc;
    int nargs;
    int i;

    DEBUG_OUT("Routing '%s'\n", odsc->name);

    if(strstr(odsc->name, s3nc_nspace) == odsc->name) {
        nargs = build_module_args_from_odsc(odsc, &args);
        res = dspaces_module_exec(server, "goes17", "query", args, nargs,
                                  DSPACES_MOD_RET_ARRAY);
        free_arg_list(args, nargs);
        free(args);
    }

    if(res) {
        if(odsc->size && odsc->size != res->elem_size) {
            fprintf(stderr,
                    "WARNING: user requested data with element size %zi, but "
                    "module routing resulted in element size %i. Not adding "
                    "anything to local storage.\n",
                    odsc->size, res->elem_size);
            free(res->data);
        } else {
            odsc->size = res->elem_size;
            if(odsc->bb.num_dims == 0) {
                odsc->bb.num_dims = res->ndim;
                obj_data_resize(odsc, res->dim);
            } else if((odsc->size * res->len) != obj_data_size(odsc)) {
                DEBUG_OUT("returned data is cropped.\n");
                obj_data_resize(odsc, res->dim);
            }
            od_odsc = malloc(sizeof(*od_odsc));
            memcpy(od_odsc, odsc, sizeof(*od_odsc));
            odsc_take_ownership(server, od_odsc);
            od_odsc->tag = res->tag;
            od = obj_data_alloc_no_data(od_odsc, res->data);
            memcpy(&od->gdim, gdim, sizeof(struct global_dimension));
            DEBUG_OUT("adding object to local storage: %s\n",
                      obj_desc_sprint(od_odsc));
            ABT_mutex_lock(server->ls_mutex);
            ls_add_obj(server->dsg->ls, od);
            ABT_mutex_unlock(server->ls_mutex);

            obj_update_dht(server, od, DS_OBJ_NEW);
        }
        free(res);
    }
}

// should we handle this locally
static int local_responsibility(dspaces_provider_t server, obj_descriptor *odsc)
{
    return (!server->remotes || ls_lookup(server->dsg->ls, odsc->name));
}

static int get_query_odscs(dspaces_provider_t server, odsc_gdim_t *query,
                           int timeout, obj_descriptor **results, int req_id)
{
    struct sspace *ssd;
    struct dht_entry **de_tab;
    int peer_num;
    int self_id_num = -1;
    int total_odscs = 0;
    int *odsc_nums;
    obj_descriptor **odsc_tabs, **podsc = NULL;
    obj_descriptor *odsc_curr;
    margo_request *serv_reqs;
    hg_handle_t *hndls;
    hg_addr_t server_addr;
    odsc_list_t dht_resp;
    obj_descriptor *q_odsc;
    struct global_dimension *q_gdim;
    int dup;
    int i, j, k;

    q_odsc = (obj_descriptor *)query->odsc_gdim.raw_odsc;
    q_gdim = (struct global_dimension *)query->odsc_gdim.raw_gdim;

    if(!local_responsibility(server, q_odsc)) {
        DEBUG_OUT("req %i: no local objects with name %s. Checking remotes.\n",
                  req_id, q_odsc->name);
        return (
            query_remotes(server, (obj_descriptor *)query->odsc_gdim.raw_odsc,
                          (struct global_dimension *)query->odsc_gdim.raw_gdim,
                          timeout, results, req_id));
    }

    DEBUG_OUT("getting sspace lock.\n");
    ABT_mutex_lock(server->sspace_mutex);
    DEBUG_OUT("got lock, looking up shared space for global dimensions.\n");
    ssd = lookup_sspace(server, q_odsc->name, q_gdim);
    ABT_mutex_unlock(server->sspace_mutex);
    DEBUG_OUT("found shared space with %i entries.\n", ssd->dht->num_entries);

    de_tab = malloc(sizeof(*de_tab) * ssd->dht->num_entries);
    peer_num = ssd_hash(ssd, &(q_odsc->bb), de_tab);

    DEBUG_OUT("%d peers to query\n", peer_num);

    odsc_tabs = malloc(sizeof(*odsc_tabs) * peer_num);
    odsc_nums = calloc(sizeof(*odsc_nums), peer_num);
    serv_reqs = malloc(sizeof(*serv_reqs) * peer_num);
    hndls = malloc(sizeof(*hndls) * peer_num);

    for(i = 0; i < peer_num; i++) {
        DEBUG_OUT("dht server id %d\n", de_tab[i]->rank);
        DEBUG_OUT("self id %d\n", server->dsg->rank);

        if(de_tab[i]->rank == server->dsg->rank) {
            self_id_num = i;
            continue;
        }
        // remote servers
        margo_addr_lookup(server->mid, server->server_address[de_tab[i]->rank],
                          &server_addr);
        margo_create(server->mid, server_addr, server->odsc_internal_id,
                     &hndls[i]);
        margo_iforward(hndls[i], query, &serv_reqs[i]);
        margo_addr_free(server->mid, server_addr);
    }

    if(peer_num == 0) {
        DEBUG_OUT("no peers in global space, handling with modules only\n");
        odsc_tabs = malloc(sizeof(*odsc_tabs));
        odsc_nums = calloc(sizeof(*odsc_nums), 1);
        self_id_num = 0;
    }

    if(self_id_num > -1) {
        route_request(server, q_odsc, q_gdim);
        DEBUG_OUT("finding local entries for req_id %i.\n", req_id);
        odsc_nums[self_id_num] =
            dht_find_entry_all(ssd->ent_self, q_odsc, &podsc, timeout);
        DEBUG_OUT("%d odscs found in %d\n", odsc_nums[self_id_num],
                  server->dsg->rank);
        total_odscs += odsc_nums[self_id_num];
        if(odsc_nums[self_id_num]) {
            odsc_tabs[self_id_num] =
                malloc(sizeof(**odsc_tabs) * odsc_nums[self_id_num]);
            for(i = 0; i < odsc_nums[self_id_num]; i++) {
                obj_descriptor *odsc =
                    &odsc_tabs[self_id_num][i]; // readability
                *odsc = *podsc[i];
                odsc->st = q_odsc->st;
                bbox_intersect(&q_odsc->bb, &odsc->bb, &odsc->bb);
                DEBUG_OUT("%s\n", obj_desc_sprint(&odsc_tabs[self_id_num][i]));
            }
        }

        free(podsc);
    }

    for(i = 0; i < peer_num; i++) {
        if(i == self_id_num) {
            continue;
        }
        DEBUG_OUT("req_id %i waiting for %d\n", req_id, i);
        margo_wait(serv_reqs[i]);
        margo_get_output(hndls[i], &dht_resp);
        if(dht_resp.odsc_list.size != 0) {
            odsc_nums[i] = dht_resp.odsc_list.size / sizeof(obj_descriptor);
            DEBUG_OUT("received %d odscs from peer %d for req_id %i\n",
                      odsc_nums[i], i, req_id);
            total_odscs += odsc_nums[i];
            odsc_tabs[i] = malloc(sizeof(**odsc_tabs) * odsc_nums[i]);
            memcpy(odsc_tabs[i], dht_resp.odsc_list.raw_odsc,
                   dht_resp.odsc_list.size);

            for(j = 0; j < odsc_nums[i]; j++) {
                // readability
                obj_descriptor *odsc =
                    (obj_descriptor *)dht_resp.odsc_list.raw_odsc;
                DEBUG_OUT("remote buffer: %s\n", obj_desc_sprint(&odsc[j]));
            }
        }
        margo_free_output(hndls[i], &dht_resp);
        margo_destroy(hndls[i]);
    }

    odsc_curr = *results = malloc(sizeof(**results) * total_odscs);

    if(peer_num == 0)
        peer_num = 1;
    for(i = 0; i < peer_num; i++) {
        if(odsc_nums[i] == 0) {
            continue;
        }
        // dedup
        for(j = 0; j < odsc_nums[i]; j++) {
            dup = 0;
            for(k = 0; k < (odsc_curr - *results); k++) {
                if(obj_desc_equals_no_owner(&(*results)[k], &odsc_tabs[i][j])) {
                    dup = 1;
                    total_odscs--;
                    break;
                }
            }
            if(!dup) {
                *odsc_curr = odsc_tabs[i][j];
                odsc_curr++;
            }
        }
        free(odsc_tabs[i]);
    }

    for(i = 0; i < total_odscs; i++) {
        DEBUG_OUT("odsc %i in response for req_id %i: %s\n", i, req_id,
                  obj_desc_sprint(&(*results)[i]));
    }

    free(de_tab);
    free(hndls);
    free(serv_reqs);
    free(odsc_tabs);
    free(odsc_nums);

    return (sizeof(obj_descriptor) * total_odscs);
}

static void query_rpc(hg_handle_t handle)
{
    margo_instance_id mid;
    const struct hg_info *info;
    dspaces_provider_t server;
    odsc_gdim_t in;
    odsc_list_t out;
    obj_descriptor in_odsc;
    struct global_dimension in_gdim;
    int timeout;
    obj_descriptor *results;
    hg_return_t hret;
    static int uid = 0;
    int req_id;

    req_id = __sync_fetch_and_add(&uid, 1);

    // unwrap context and input from margo
    mid = margo_hg_handle_get_instance(handle);
    info = margo_get_info(handle);
    server = (dspaces_provider_t)margo_registered_data(mid, info->id);
    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_get_input() failed with "
                "%d.\n",
                __func__, hret);
        margo_destroy(handle);
        return;
    }

    DEBUG_OUT("received query\n");

    memcpy(&in_odsc, in.odsc_gdim.raw_odsc, sizeof(in_odsc));
    memcpy(&in_gdim, in.odsc_gdim.raw_gdim, sizeof(struct global_dimension));
    timeout = in.param;
    DEBUG_OUT("Received query for %s with timeout %d\n",
              obj_desc_sprint(&in_odsc), timeout);

    out.odsc_list.size =
        get_query_odscs(server, &in, timeout, &results, req_id);

    out.odsc_list.raw_odsc = (char *)results;
    margo_respond(handle, &out);
    margo_free_input(handle, &in);
    margo_destroy(handle);

    free(results);
}
DEFINE_MARGO_RPC_HANDLER(query_rpc)

static int peek_meta_remotes(dspaces_provider_t server, peek_meta_in_t *in)
{
    margo_request *reqs;
    hg_handle_t *peek_hndls;
    peek_meta_out_t *resps;
    hg_addr_t addr;
    int i;
    int ret = -1;
    size_t index;
    hg_return_t hret;

    reqs = malloc(sizeof(*reqs) * server->nremote);
    resps = malloc(sizeof(*resps) * server->nremote);
    peek_hndls = malloc(sizeof(*peek_hndls) * server->nremote);

    DEBUG_OUT("sending peek request to remotes for metadata '%s'\n", in->name);
    for(i = 0; i < server->nremote; i++) {
        DEBUG_OUT("querying %s at %s\n", server->remotes[i].name,
                  server->remotes[i].addr_str);
        margo_addr_lookup(server->mid, server->remotes[i].addr_str, &addr);
        hret = margo_create(server->mid, addr, server->peek_meta_id,
                            &peek_hndls[i]);
        hret = margo_iforward(peek_hndls[i], in, &reqs[i]);
        margo_addr_free(server->mid, addr);
    }

    for(i = 0; i < server->nremote; i++) {
        hret = margo_wait_any(server->nremote, reqs, &index);
        margo_get_output(peek_hndls[index], &resps[index]);
        DEBUG_OUT("%s replied with %i\n", server->remotes[index].name,
                  resps[index].res);
        if(resps[index].res == 1) {
            ret = i;
        }
        margo_free_output(peek_hndls[index], &resps[index]);
        margo_destroy(peek_hndls[index]);
    }

    free(reqs);
    free(resps);
    free(peek_hndls);

    return (ret);
}

static void peek_meta_rpc(hg_handle_t handle)
{
    margo_instance_id mid = margo_hg_handle_get_instance(handle);
    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);
    peek_meta_in_t in;
    peek_meta_out_t out;
    hg_return_t hret;
    hg_addr_t addr;

    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_get_input() failed with "
                "%d.\n",
                __func__, hret);
        margo_destroy(handle);
        return;
    }

    DEBUG_OUT("received peek request for metadata '%s'\n", in.name);

    out.res = 0;

    if(meta_find_next_entry(server->dsg->ls, in.name, -1, 0)) {
        DEBUG_OUT("found the metadata\n");
        out.res = 1;
    } else if(server->nremote) {
        DEBUG_OUT("no such metadata in local storage.\n");
        if(peek_meta_remotes(server, &in) > -1) {
            out.res = 1;
        }
    }

    margo_respond(handle, &out);
    margo_free_input(handle, &in);
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(peek_meta_rpc)

static void query_meta_rpc(hg_handle_t handle)
{
    margo_instance_id mid;
    const struct hg_info *info;
    dspaces_provider_t server;
    query_meta_in_t in;
    query_meta_out_t rem_out, out;
    peek_meta_in_t rem_in;
    struct meta_data *mdata, *mdlatest;
    int remote, found_remote;
    hg_handle_t rem_hndl;
    hg_addr_t rem_addr;
    hg_return_t hret;

    mid = margo_hg_handle_get_instance(handle);
    info = margo_get_info(handle);
    server = (dspaces_provider_t)margo_registered_data(mid, info->id);
    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_get_input() failed with "
                "%d.\n",
                __func__, hret);
        margo_destroy(handle);
        return;
    }

    DEBUG_OUT("received metadata query for version %d of '%s', mode %d.\n",
              in.version, in.name, in.mode);

    found_remote = 0;
    if(server->nremote) {
        rem_in.name = in.name;
        remote = peek_meta_remotes(server, &rem_in);
        if(remote > -1) {
            DEBUG_OUT("remote %s has %s metadata\n",
                      server->remotes[remote].name, in.name);
            margo_addr_lookup(server->mid, server->remotes[remote].addr_str,
                              &rem_addr);
            hret = margo_create(server->mid, rem_addr, server->query_meta_id,
                                &rem_hndl);
            if(hret != HG_SUCCESS) {
                fprintf(stderr, "ERROR: (%s): margo_create() failed\n",
                        __func__);
                margo_addr_free(server->mid, rem_addr);
            } else {
                margo_forward(rem_hndl, &in);
                hret = margo_get_output(rem_hndl, &out);
                if(hret != HG_SUCCESS) {
                    fprintf(stderr,
                            "ERROR: %s: margo_get_output() failed with %d.\n",
                            __func__, hret);
                } else {
                    DEBUG_OUT("retreived metadata from %s\n",
                              server->remotes[remote].name);
                    found_remote = 1;
                }
            }
        }
    }
    if(!found_remote) {
        switch(in.mode) {
        case META_MODE_SPEC:
            DEBUG_OUT("spec query - searching without waiting...\n");
            mdata = meta_find_entry(server->dsg->ls, in.name, in.version, 0);
            break;
        case META_MODE_NEXT:
            DEBUG_OUT("find next query...\n");
            mdata =
                meta_find_next_entry(server->dsg->ls, in.name, in.version, 1);
            break;
        case META_MODE_LAST:
            DEBUG_OUT("find last query...\n");
            mdata =
                meta_find_next_entry(server->dsg->ls, in.name, in.version, 1);
            mdlatest = mdata;
            do {
                mdata = mdlatest;
                DEBUG_OUT("found version %d. Checking for newer...\n",
                          mdata->version);
                mdlatest = meta_find_next_entry(server->dsg->ls, in.name,
                                                mdlatest->version, 0);
            } while(mdlatest);
            break;
        default:
            fprintf(stderr,
                    "ERROR: unkown mode %d while processing metadata query.\n",
                    in.mode);
        }

        if(mdata) {
            DEBUG_OUT("found version %d, length %d.", mdata->version,
                      mdata->length);
            out.mdata.len = mdata->length;
            out.mdata.buf = malloc(mdata->length);
            memcpy(out.mdata.buf, mdata->data, mdata->length);
            out.version = mdata->version;
        } else {
            out.mdata.len = 0;
            out.version = -1;
        }
    }
    margo_respond(handle, &out);
    margo_free_input(handle, &in);
    if(found_remote) {
        margo_free_output(rem_hndl, &out);
        margo_destroy(rem_hndl);
    }
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(query_meta_rpc)

static void get_rpc(hg_handle_t handle)
{
    hg_return_t hret;
    bulk_in_t in;
    bulk_out_t out;
    hg_bulk_t bulk_handle;
    int csize;
    void *cbuffer;

    margo_instance_id mid = margo_hg_handle_get_instance(handle);

    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);

    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_get_input() failed with "
                "%d.\n",
                __func__, hret);
        margo_destroy(handle);
        return;
    }

    obj_descriptor in_odsc;
    memcpy(&in_odsc, in.odsc.raw_odsc, sizeof(in_odsc));

    DEBUG_OUT("received get request\n");

    struct obj_data *od, *from_obj;

    ABT_mutex_lock(server->ls_mutex);
    if(server->remotes && ls_lookup(server->dsg->ps, in_odsc.name)) {
        from_obj = ls_find(server->dsg->ps, &in_odsc);
    } else {
        from_obj = ls_find(server->dsg->ls, &in_odsc);
    }
    DEBUG_OUT("found source data object\n");
    od = obj_data_alloc(&in_odsc);
    DEBUG_OUT("allocated target object\n");
    ssd_copy(od, from_obj);
    DEBUG_OUT("copied object data\n");
    ABT_mutex_unlock(server->ls_mutex);
    hg_size_t size = (in_odsc.size) * bbox_volume(&(in_odsc.bb));
    void *buffer = (void *)od->data;
    cbuffer = malloc(size);
    hret = margo_bulk_create(mid, 1, (void **)&cbuffer, &size,
                             HG_BULK_READ_ONLY, &bulk_handle);
    DEBUG_OUT("created bulk handle of size %li\n", size);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_bulk_create() failure\n", __func__);
        out.ret = dspaces_ERR_MERCURY;
        margo_respond(handle, &out);
        margo_free_input(handle, &in);
        margo_destroy(handle);
        return;
    }

    csize = LZ4_compress_default(od->data, cbuffer, size, size);

    DEBUG_OUT("compressed result from %li to %i bytes.\n", size, csize);
    if(!csize) {
        DEBUG_OUT("compressed result could not fit in dst buffer - longer than "
                  "original! Sending uncompressed.\n");
        memcpy(cbuffer, od->data, size);
    }

    hret = margo_bulk_transfer(mid, HG_BULK_PUSH, info->addr, in.handle, 0,
                               bulk_handle, 0, (csize ? csize : size));
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_bulk_transfer() failure (%d)\n",
                __func__, hret);
        out.ret = dspaces_ERR_MERCURY;
        margo_respond(handle, &out);
        margo_free_input(handle, &in);
        margo_bulk_free(bulk_handle);
        margo_destroy(handle);
        return;
    }
    DEBUG_OUT("completed bulk transfer.\n");
    margo_bulk_free(bulk_handle);
    out.ret = dspaces_SUCCESS;
    out.len = csize;
    obj_data_free(od);
    margo_respond(handle, &out);
    margo_free_input(handle, &in);
    margo_destroy(handle);
    free(cbuffer);
}
DEFINE_MARGO_RPC_HANDLER(get_rpc)

static void odsc_internal_rpc(hg_handle_t handle)
{
    hg_return_t hret;
    odsc_gdim_t in;
    int timeout;
    odsc_list_t out;
    obj_descriptor **podsc = NULL;
    margo_instance_id mid = margo_hg_handle_get_instance(handle);
    margo_request req;

    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);

    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_get_input() failed with "
                "%d.\n",
                __func__, hret);
        margo_destroy(handle);
        return;
    }

    obj_descriptor in_odsc;
    memcpy(&in_odsc, in.odsc_gdim.raw_odsc, sizeof(in_odsc));
    timeout = in.param;

    struct global_dimension od_gdim;
    memcpy(&od_gdim, in.odsc_gdim.raw_gdim, sizeof(struct global_dimension));

    DEBUG_OUT("Received query for %s with timeout %d\n",
              obj_desc_sprint(&in_odsc), timeout);

    obj_descriptor *odsc_tab;
    DEBUG_OUT("getting sspace lock.\n");
    ABT_mutex_lock(server->sspace_mutex);
    DEBUG_OUT("got sspace lock.\n");
    struct sspace *ssd = lookup_sspace(server, in_odsc.name, &od_gdim);
    ABT_mutex_unlock(server->sspace_mutex);
    route_request(server, &in_odsc, &od_gdim);
    int num_odsc;
    num_odsc = dht_find_entry_all(ssd->ent_self, &in_odsc, &podsc, timeout);
    DEBUG_OUT("found %d DHT entries.\n", num_odsc);
    if(!num_odsc) {
        // need to figure out how to send that number of odscs is null
        out.odsc_list.size = 0;
        out.odsc_list.raw_odsc = NULL;
        margo_respond(handle, &out);
        margo_free_input(handle, &in);
        margo_destroy(handle);

    } else {
        odsc_tab = malloc(sizeof(*odsc_tab) * num_odsc);
        for(int j = 0; j < num_odsc; j++) {
            obj_descriptor odsc;
            odsc = *podsc[j];
            DEBUG_OUT("including %s\n", obj_desc_sprint(&odsc));
            /* Preserve storage type at the destination. */
            odsc.st = in_odsc.st;
            bbox_intersect(&in_odsc.bb, &odsc.bb, &odsc.bb);
            odsc_tab[j] = odsc;
        }
        out.odsc_list.size = num_odsc * sizeof(obj_descriptor);
        out.odsc_list.raw_odsc = (char *)odsc_tab;
        margo_irespond(handle, &out, &req);
        DEBUG_OUT("sent response...waiting on request handle\n");
        margo_free_input(handle, &in);
        margo_wait(req);
        DEBUG_OUT("request handle complete.\n");
        margo_destroy(handle);
    }
    DEBUG_OUT("complete\n");

    free(podsc);
}
DEFINE_MARGO_RPC_HANDLER(odsc_internal_rpc)

/*
  Rpc routine to update (add or insert) an object descriptor in the
  dht table.
*/
static void obj_update_rpc(hg_handle_t handle)
{
    hg_return_t hret;
    odsc_gdim_t in;
    obj_update_t type;
    int err;

    margo_instance_id mid = margo_hg_handle_get_instance(handle);

    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);

    DEBUG_OUT("Received rpc to update obj_dht\n");

    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_get_input() failed with "
                "%d.\n",
                __func__, hret);
        margo_destroy(handle);
        return;
    }

    obj_descriptor in_odsc;
    memcpy(&in_odsc, in.odsc_gdim.raw_odsc, sizeof(in_odsc));
    struct global_dimension gdim;
    memcpy(&gdim, in.odsc_gdim.raw_gdim, sizeof(struct global_dimension));
    type = in.param;

    DEBUG_OUT("received update_rpc %s\n", obj_desc_sprint(&in_odsc));
    ABT_mutex_lock(server->sspace_mutex);
    DEBUG_OUT("got sspace lock.\n");
    struct sspace *ssd = lookup_sspace(server, in_odsc.name, &gdim);
    ABT_mutex_unlock(server->sspace_mutex);
    struct dht_entry *de = ssd->ent_self;

    ABT_mutex_lock(server->dht_mutex);
    switch(type) {
    case DS_OBJ_NEW:
        err = dht_add_entry(de, &in_odsc);
        break;
    case DS_OBJ_OWNER:
        err = dht_update_owner(de, &in_odsc, 1);
        break;
    default:
        fprintf(stderr, "ERROR: (%s): unknown object update type.\n", __func__);
    }
    ABT_mutex_unlock(server->dht_mutex);
    DEBUG_OUT("Updated dht %s in server %d \n", obj_desc_sprint(&in_odsc),
              server->dsg->rank);
    if(err < 0)
        fprintf(stderr, "ERROR (%s): obj_update_rpc Failed with %d\n", __func__,
                err);

    margo_free_input(handle, &in);
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(obj_update_rpc)

static void ss_rpc(hg_handle_t handle)
{
    ss_information out;

    margo_instance_id mid = margo_hg_handle_get_instance(handle);

    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);

    DEBUG_OUT("received ss_rpc\n");

    ss_info_hdr ss_data;
    ss_data.num_dims = ds_conf.ndim;
    ss_data.num_space_srv = server->dsg->size_sp;
    ss_data.max_versions = ds_conf.max_versions;
    ss_data.hash_version = ds_conf.hash_version;
    ss_data.default_gdim.ndim = ds_conf.ndim;

    for(int i = 0; i < ds_conf.ndim; i++) {
        ss_data.ss_domain.lb.c[i] = 0;
        ss_data.ss_domain.ub.c[i] = ds_conf.dims.c[i] - 1;
        ss_data.default_gdim.sizes.c[i] = ds_conf.dims.c[i];
    }

    out.ss_buf.size = sizeof(ss_info_hdr);
    out.ss_buf.raw_odsc = (char *)(&ss_data);
    out.chk_str = strdup("chkstr");
    margo_respond(handle, &out);
    DEBUG_OUT("responded in %s\n", __func__);
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(ss_rpc)

static void send_kill_rpc(dspaces_provider_t server, int target, int *rank)
{
    // TODO: error handling/reporting
    hg_addr_t server_addr;
    hg_handle_t h;
    margo_request req;

    margo_addr_lookup(server->mid, server->server_address[target],
                      &server_addr);
    margo_create(server->mid, server_addr, server->kill_id, &h);
    margo_iforward(h, rank, &req);
    margo_addr_free(server->mid, server_addr);
    margo_destroy(h);
}

static void kill_rpc(hg_handle_t handle)
{
    margo_instance_id mid = margo_hg_handle_get_instance(handle);
    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);
    int32_t src, rank, parent, child1, child2;
    int do_kill = 0;

    margo_get_input(handle, &src);
    DEBUG_OUT("Received kill signal from %d.\n", src);

    rank = server->dsg->rank;
    parent = (rank - 1) / 2;
    child1 = (rank * 2) + 1;
    child2 = child1 + 1;

    ABT_mutex_lock(server->kill_mutex);
    DEBUG_OUT("Kill tokens remaining: %d\n",
              server->f_kill ? (server->f_kill - 1) : 0);
    if(server->f_kill == 0) {
        // already shutting down
        ABT_mutex_unlock(server->kill_mutex);
        margo_free_input(handle, &src);
        margo_destroy(handle);
        return;
    }
    if(--server->f_kill == 0) {
        DEBUG_OUT("Kill count is zero. Initiating shutdown.\n");
        do_kill = 1;
    }

    ABT_mutex_unlock(server->kill_mutex);

    if((src == -1 || src > rank) && rank > 0) {
        send_kill_rpc(server, parent, &rank);
    }
    if((child1 != src && child1 < server->dsg->size_sp)) {
        send_kill_rpc(server, child1, &rank);
    }
    if((child2 != src && child2 < server->dsg->size_sp)) {
        send_kill_rpc(server, child2, &rank);
    }

    margo_free_input(handle, &src);
    margo_destroy(handle);
    if(do_kill) {
        server_destroy(server);
    }
    DEBUG_OUT("finished with kill handling.\n");
}
DEFINE_MARGO_RPC_HANDLER(kill_rpc)

static void sub_rpc(hg_handle_t handle)
{
    margo_instance_id mid = margo_hg_handle_get_instance(handle);
    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);
    odsc_list_t notice;
    odsc_gdim_t in;
    int32_t sub_id;
    obj_descriptor in_odsc;
    obj_descriptor *results;
    struct global_dimension in_gdim;
    hg_addr_t client_addr;
    hg_handle_t notifyh;
    margo_request req;
    static int uid = 0;
    int req_id;

    req_id = __sync_fetch_and_add(&uid, 1L);
    margo_get_input(handle, &in);

    memcpy(&in_odsc, in.odsc_gdim.raw_odsc, sizeof(in_odsc));
    memcpy(&in_gdim, in.odsc_gdim.raw_gdim, sizeof(struct global_dimension));
    sub_id = in.param;

    DEBUG_OUT("received subscription for %s with id %d from %s\n",
              obj_desc_sprint(&in_odsc), sub_id, in_odsc.owner);

    in.param = -1; // this will be interpreted as timeout by any interal queries
    notice.odsc_list.size = get_query_odscs(server, &in, -1, &results, req_id);
    notice.odsc_list.raw_odsc = (char *)results;
    notice.param = sub_id;

    margo_addr_lookup(server->mid, in_odsc.owner, &client_addr);
    margo_create(server->mid, client_addr, server->notify_id, &notifyh);
    margo_iforward(notifyh, &notice, &req);
    DEBUG_OUT("send reply for req_id %i\n", req_id);
    margo_addr_free(server->mid, client_addr);
    margo_destroy(notifyh);

    margo_free_input(handle, &in);
    margo_destroy(handle);

    free(results);
}
DEFINE_MARGO_RPC_HANDLER(sub_rpc)

static void do_ops_rpc(hg_handle_t handle)
{
    margo_instance_id mid = margo_hg_handle_get_instance(handle);
    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);
    do_ops_in_t in;
    bulk_out_t out;
    struct obj_data *od, *stage_od, *res_od;
    struct list_head odl;
    struct ds_data_expr *expr;
    struct global_dimension *gdim;
    obj_descriptor *odsc;
    int res_size;
    int num_odscs;
    obj_descriptor *q_results;
    uint64_t res_buf_size;
    odsc_gdim_t query;
    hg_return_t hret;
    void *buffer, *cbuffer;
    hg_bulk_t bulk_handle;
    hg_size_t size;
    int err;
    int csize;
    long i;

    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: %s: margo_get_input() failed with %d.\n",
                __func__, hret);
        margo_destroy(handle);
        return;
    }
    expr = in.expr;
    DEBUG_OUT("doing expression type %i\n", expr->type);

    INIT_LIST_HEAD(&odl);
    gather_op_ods(expr, &odl);
    list_for_each_entry(od, &odl, struct obj_data, obj_entry)
    {
        odsc = &od->obj_desc;
        DEBUG_OUT("Finding data for '%s'\n", odsc->name);
        res_od = obj_data_alloc(odsc);
        stage_od = ls_find(server->dsg->ls, odsc);
        if(!stage_od) {
            DEBUG_OUT("not stored locally.\n");
            // size is currently not used`
            query.odsc_gdim.size = sizeof(*odsc);
            query.odsc_gdim.raw_odsc = (char *)odsc;
            query.odsc_gdim.gdim_size = sizeof(od->gdim);
            query.odsc_gdim.raw_gdim = (char *)&od->gdim;

            // TODO: assumes data is either local or on a remote
            res_size = get_query_odscs(server, &query, -1, &q_results, -1);
            if(res_size != sizeof(odsc)) {
                fprintf(stderr, "WARNING: %s: multiple odscs for query.\n",
                        __func__);
            }
            stage_od = ls_find(server->dsg->ps, odsc);
            if(!stage_od) {
                fprintf(stderr,
                        "ERROR: %s: nothing in the proxy cache for query.\n",
                        __func__);
            }
        }
        ssd_copy(res_od, stage_od);
        // update any obj in expression to use res_od
        DEBUG_OUT("updating expression data with variable data.\n");
        update_expr_objs(expr, res_od);
    }

    res_buf_size = expr->size;
    buffer = malloc(res_buf_size);
    cbuffer = malloc(res_buf_size);
    if(expr->type == DS_VAL_INT) {
#pragma omp for
        for(i = 0; i < res_buf_size / sizeof(int); i++) {
            ((int *)buffer)[i] = ds_op_calc_ival(expr, i, &err);
        }
    } else if(expr->type == DS_VAL_REAL) {
#pragma omp for
        for(i = 0; i < res_buf_size / sizeof(double); i++) {
            ((double *)buffer)[i] = ds_op_calc_rval(expr, i, &err);
        }
    } else {
        fprintf(stderr, "ERROR: %s: invalid expression data type.\n", __func__);
        goto cleanup;
    }

    size = res_buf_size;
    hret = margo_bulk_create(mid, 1, (void **)&cbuffer, &size,
                             HG_BULK_READ_ONLY, &bulk_handle);

    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_bulk_create() failure\n", __func__);
        out.ret = dspaces_ERR_MERCURY;
        margo_respond(handle, &out);
    }

    csize = LZ4_compress_default(buffer, cbuffer, size, size);

    DEBUG_OUT("compressed result from %li to %i bytes.\n", size, csize);
    if(!csize) {
        DEBUG_OUT("compressed result could not fit in dst buffer - longer than "
                  "original! Sending uncompressed.\n");
        memcpy(cbuffer, buffer, size);
    }

    hret = margo_bulk_transfer(mid, HG_BULK_PUSH, info->addr, in.handle, 0,
                               bulk_handle, 0, (csize ? csize : size));
    if(hret != HG_SUCCESS) {
        fprintf(stderr, "ERROR: (%s): margo_bulk_transfer() failure (%d)\n",
                __func__, hret);
        out.ret = dspaces_ERR_MERCURY;
        margo_respond(handle, &out);
        margo_bulk_free(bulk_handle);
        goto cleanup;
    }
    margo_bulk_free(bulk_handle);
    out.ret = dspaces_SUCCESS;
    out.len = csize;
    margo_respond(handle, &out);
cleanup:
    free(buffer);
    free(cbuffer);
    margo_free_input(handle, &in);
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(do_ops_rpc)

#ifdef DSPACES_HAVE_PYTHON
static PyObject *build_ndarray_from_od(struct obj_data *od)
{
    obj_descriptor *odsc = odsc = &od->obj_desc;
    int tag = odsc->tag;
    PyArray_Descr *descr = PyArray_DescrNewFromType(tag);
    int ndim = odsc->bb.num_dims;
    void *data = od->data;
    PyObject *arr, *fnp;
    npy_intp dims[ndim];
    int i;

    for(i = 0; i < ndim; i++) {
        dims[i] = (odsc->bb.ub.c[i] - odsc->bb.lb.c[i]) + 1;
    }

    arr = PyArray_NewFromDescr(&PyArray_Type, descr, ndim, dims, NULL, data, 0,
                               NULL);

    return (arr);
}

static void pexec_rpc(hg_handle_t handle)
{
    margo_instance_id mid = margo_hg_handle_get_instance(handle);
    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);
    pexec_in_t in;
    pexec_out_t out;
    hg_return_t hret;
    hg_bulk_t bulk_handle;
    obj_descriptor in_odsc;
    hg_size_t rdma_size;
    void *fn = NULL, *res_data;
    struct obj_data *od, *from_obj;
    PyObject *array, *fnp, *arg, *pres, *pres_bytes;
    static PyObject *pklmod = NULL;
    ABT_cond cond;
    ABT_mutex mtx;

    hret = margo_get_input(handle, &in);
    if(hret != HG_SUCCESS) {
        fprintf(stderr,
                "DATASPACES: ERROR handling %s: margo_get_input() failed with "
                "%d.\n",
                __func__, hret);
        margo_destroy(handle);
        return;
    }

    memcpy(&in_odsc, in.odsc.raw_odsc, sizeof(in_odsc));

    DEBUG_OUT("received pexec request\n");
    rdma_size = in.length;
    if(rdma_size > 0) {
        DEBUG_OUT("function included, length %" PRIu32 " bytes\n", in.length);
        fn = malloc(rdma_size);
        hret = margo_bulk_create(mid, 1, (void **)&(fn), &rdma_size,
                                 HG_BULK_WRITE_ONLY, &bulk_handle);

        if(hret != HG_SUCCESS) {
            // TODO: communicate failure
            fprintf(stderr, "ERROR: (%s): margo_bulk_create failed!\n",
                    __func__);
            margo_respond(handle, &out);
            margo_free_input(handle, &in);
            margo_destroy(handle);
            return;
        }

        hret = margo_bulk_transfer(mid, HG_BULK_PULL, info->addr, in.handle, 0,
                                   bulk_handle, 0, rdma_size);
        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): margo_bulk_transfer failed!\n",
                    __func__);
            margo_respond(handle, &out);
            margo_free_input(handle, &in);
            margo_bulk_free(bulk_handle);
            margo_destroy(handle);
            return;
        }
    }
    margo_bulk_free(bulk_handle);
    route_request(server, &in_odsc, &(server->dsg->default_gdim));

    from_obj = ls_find(server->dsg->ls, &in_odsc);
    array = build_ndarray_from_od(from_obj);

    // Race condition? Protect with mutex?
    if((pklmod == NULL) &&
       (pklmod = PyImport_ImportModuleNoBlock("dill")) == NULL) {
        margo_respond(handle, &out);
        margo_free_input(handle, &in);
        margo_destroy(handle);
        return;
    }
    arg = PyBytes_FromStringAndSize(fn, rdma_size);
    fnp = PyObject_CallMethodObjArgs(pklmod, PyUnicode_FromString("loads"), arg,
                                     NULL);
    Py_XDECREF(arg);
    if(fnp && PyCallable_Check(fnp)) {
        pres = PyObject_CallFunctionObjArgs(fnp, array, NULL);
    } else {
        if(!fnp) {
            PyErr_Print();
        }
        fprintf(stderr,
                "ERROR: (%s): provided function could either not be loaded, or "
                "is not callable.\n",
                __func__);
        margo_respond(handle, &out);
        margo_free_input(handle, &in);
        margo_destroy(handle);
        return;
    }

    if(pres && (pres != Py_None)) {
        pres_bytes = PyObject_CallMethodObjArgs(
            pklmod, PyUnicode_FromString("dumps"), pres, NULL);
        Py_XDECREF(pres);
        res_data = PyBytes_AsString(pres_bytes);
        rdma_size = PyBytes_Size(pres_bytes) + 1;
        hret = margo_bulk_create(mid, 1, (void **)&res_data, &rdma_size,
                                 HG_BULK_READ_ONLY, &out.handle);
        if(hret != HG_SUCCESS) {
            fprintf(stderr, "ERROR: (%s): margo_bulk_create failed with %d.\n",
                    __func__, hret);
            out.length = 0;
            margo_respond(handle, &out);
            margo_free_input(handle, &in);
            margo_destroy(handle);
            return;
        }
        out.length = rdma_size;
    } else {
        if(!pres) {
            PyErr_Print();
        }
        out.length = 0;
    }

    if(out.length > 0) {
        ABT_cond_create(&cond);
        ABT_mutex_create(&mtx);
        out.condp = (uint64_t)(&cond);
        out.mtxp = (uint64_t)(&mtx);
        DEBUG_OUT("sending out.condp = %" PRIu64 " and out.mtxp = %" PRIu64
                  "\n",
                  out.condp, out.mtxp);
        ABT_mutex_lock(mtx);
        margo_respond(handle, &out);
        ABT_cond_wait(cond, mtx);
        DEBUG_OUT("signaled on condition\n");
        ABT_mutex_unlock(mtx);
        ABT_mutex_free(&mtx);
        ABT_cond_free(&cond);
    } else {
        out.handle = 0;
        margo_respond(handle, &out);
    }

    margo_free_input(handle, &in);
    margo_destroy(handle);

    DEBUG_OUT("done with pexec handling\n");

    Py_XDECREF(array);
}
DEFINE_MARGO_RPC_HANDLER(pexec_rpc)
#endif // DSPACES_HAVE_PYTHON

static void cond_rpc(hg_handle_t handle)
{
    margo_instance_id mid = margo_hg_handle_get_instance(handle);
    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);
    ABT_mutex *mtx;
    ABT_cond *cond;
    cond_in_t in;

    margo_get_input(handle, &in);
    DEBUG_OUT("condition rpc for mtxp = %" PRIu64 ", condp = %" PRIu64 "\n",
              in.mtxp, in.condp);

    mtx = (ABT_mutex *)in.mtxp;
    cond = (ABT_cond *)in.condp;
    ABT_mutex_lock(*mtx);
    ABT_cond_signal(*cond);
    ABT_mutex_unlock(*mtx);

    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(cond_rpc)

static void send_get_vars_rpc(dspaces_provider_t server, int target, int *rank,
                              hg_addr_t *addr, hg_handle_t *h,
                              margo_request *req)
{
    margo_addr_lookup(server->mid, server->server_address[target], addr);
    margo_create(server->mid, *addr, server->kill_id, h);
    margo_iforward(*h, rank, req);
}

static void get_vars_rpc(hg_handle_t handle)
{
    margo_instance_id mid = margo_hg_handle_get_instance(handle);
    const struct hg_info *info = margo_get_info(handle);
    dspaces_provider_t server =
        (dspaces_provider_t)margo_registered_data(mid, info->id);
    int32_t src, rank, parent, child1, child2;
    int sent_rpc[3] = {0};
    hg_addr_t addr[3];
    hg_handle_t hndl[3];
    margo_request req[3];
    name_list_t out[3];
    name_list_t rout;
    ds_str_hash *results = ds_str_hash_init();
    int num_vars;
    char **names;
    int i, j;

    margo_get_input(handle, &src);
    DEBUG_OUT("Received request for all variable names from %d\n", src);

    rank = server->dsg->rank;
    parent = (rank - 1) / 2;
    child1 = (rank * 2) + 1;
    child2 = child1 + 1;

    if((src == -1 || src > rank) && rank > 0) {
        DEBUG_OUT("querying parent %d\n", parent);
        send_get_vars_rpc(server, parent, &rank, &addr[0], &hndl[0], &req[0]);
        sent_rpc[0] = 1;
    }
    if((child1 != src && child1 < server->dsg->size_sp)) {
        DEBUG_OUT("querying child %d\n", child1);
        send_get_vars_rpc(server, parent, &rank, &addr[1], &hndl[1], &req[1]);
        sent_rpc[1] = 1;
    }
    if((child2 != src && child2 < server->dsg->size_sp)) {
        DEBUG_OUT("querying child %d\n", child2);
        send_get_vars_rpc(server, parent, &rank, &addr[2], &hndl[2], &req[2]);
        sent_rpc[2] = 1;
    }

    ABT_mutex_lock(server->ls_mutex);
    num_vars = ls_get_var_names(server->dsg->ls, &names);
    ABT_mutex_unlock(server->ls_mutex);

    DEBUG_OUT("found %d local variables.\n", num_vars);

    for(i = 0; i < num_vars; i++) {
        ds_str_hash_add(results, names[i]);
        free(names[i]);
    }
    free(names);

    for(i = 0; i < 3; i++) {
        if(sent_rpc[i]) {
            margo_wait(req[i]);
            margo_get_output(hndl[i], &out[i]);
            for(j = 0; j < out[i].count; j++) {
                ds_str_hash_add(results, out[i].names[j]);
            }
            margo_free_output(hndl[i], &out);
            margo_addr_free(server->mid, addr[i]);
            margo_destroy(hndl[i]);
        }
    }

    rout.count = ds_str_hash_get_all(results, &rout.names);
    DEBUG_OUT("returning %zi variable names\n", rout.count);
    margo_respond(handle, &rout);
    for(i = 0; i < rout.count; i++) {
        free(rout.names[i]);
    }
    free(rout.names);
    margo_destroy(handle);
}
DEFINE_MARGO_RPC_HANDLER(get_vars_rpc)

static void get_var_objs_rpc(hg_handle_t handle) {}
DEFINE_MARGO_RPC_HANDLER(get_var_objs_rpc)

void dspaces_server_fini(dspaces_provider_t server)
{
    int err;

    DEBUG_OUT("waiting for finalize to occur\n");
    margo_wait_for_finalize(server->mid);
#ifdef DSPACES_HAVE_PYTHON
    err = Py_FinalizeEx();
#endif // DSPACES_HAVE_PYTHON
    if(err < 0) {
        fprintf(stderr, "ERROR: Python finalize failed with %d\n", err);
    }
    free(server);
}

int dspaces_server_find_objs(dspaces_provider_t server, const char *var_name,
                             int version, struct dspaces_data_obj **objs)
{
    obj_descriptor odsc;
    obj_descriptor **od_tab;
    struct dspaces_data_obj *obj;
    int num_obj = 0;
    int i;
    ;

    strcpy(odsc.name, var_name);
    odsc.version = version;
    ABT_mutex_lock(server->ls_mutex);
    num_obj = ls_find_ods(server->dsg->ls, &odsc, &od_tab);
    ABT_mutex_unlock(server->ls_mutex);

    if(num_obj) {
        *objs = malloc(sizeof(**objs) * num_obj);
        for(i = 0; i < num_obj; i++) {
            obj = &(*objs)[i];
            obj->var_name = var_name;
            obj->version = version;
            obj->ndim = od_tab[i]->bb.num_dims;
            obj->size = od_tab[i]->size;
            obj->lb = malloc(sizeof(*obj->lb) * obj->ndim);
            obj->ub = malloc(sizeof(*obj->ub) * obj->ndim);
            memcpy(obj->lb, od_tab[i]->bb.lb.c,
                   sizeof(*obj->lb) * od_tab[i]->bb.num_dims);
            memcpy(obj->ub, od_tab[i]->bb.ub.c,
                   sizeof(*obj->ub) * od_tab[i]->bb.num_dims);
        }
        free(od_tab);
    }
    return (num_obj);
}

int dspaces_server_get_objdata(dspaces_provider_t server,
                               struct dspaces_data_obj *obj, void *buffer)
{
    obj_descriptor odsc;
    struct obj_data *od;
    int i;

    strcpy(odsc.name, obj->var_name);
    odsc.version = obj->version;
    odsc.bb.num_dims = obj->ndim;
    memcpy(odsc.bb.lb.c, obj->lb, sizeof(*obj->lb) * obj->ndim);
    memcpy(odsc.bb.ub.c, obj->ub, sizeof(*obj->ub) * obj->ndim);

    ABT_mutex_lock(server->ls_mutex);
    od = ls_find(server->dsg->ls, &odsc);
    if(!od) {
        fprintf(stderr, "WARNING: (%s): obj not found in local storage.\n",
                __func__);
        return (-1);
    } else {
        for(i = 0; i < obj->ndim; i++) {
            if(od->obj_desc.bb.lb.c[i] != obj->lb[i] ||
               od->obj_desc.bb.ub.c[i] != obj->ub[i]) {
                fprintf(stderr,
                        "WARNING: (%s): obj found, but not the right size.\n",
                        __func__);
                return (-2);
            }
        }
    }

    memcpy(buffer, od->data, obj->size * bbox_volume(&odsc.bb));
    ABT_mutex_unlock(server->ls_mutex);

    return (0);
}
