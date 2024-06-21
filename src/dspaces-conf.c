#include "dspaces-conf.h"
#include "dspaces-modules.h"
#include "dspaces-storage.h"
#include "toml.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

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

static int parse_line(int lineno, char *line, struct ds_conf *conf)
{
    struct {
        const char *opt;
        int *pval;
    } options[] = {{"ndim", &conf->ndim},
                   {"dims", (int *)&conf->dims},
                   {"max_versions", &conf->max_versions},
                   {"hash_version", &conf->hash_version},
                   {"num_apps", &conf->num_apps}};

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

int parse_conf(const char *fname, struct ds_conf *conf)
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
        err = parse_line(lineno++, buff, conf);
        if(err < 0) {
            fclose(fin);
            return err;
        }
    }

    fclose(fin);
    return 0;
}

static int get_toml_int(toml_table_t *t, const char *key, int *i)
{
    toml_datum_t dat;

    if(toml_key_exists(t, key)) {
        dat = toml_int_in(t, key);
        *i = dat.u.i;
        return (1);
    }

    return (0);
}

static void get_toml_double(toml_table_t *t, const char *key, double *d)
{
    toml_datum_t dat;

    if(toml_key_exists(t, key)) {
        dat = toml_double_in(t, key);
        *d = dat.u.d;
    }
}

static int get_toml_str(toml_table_t *t, const char *key, char **str)
{
    toml_datum_t dat;

    if(toml_key_exists(t, key)) {
        dat = toml_string_in(t, key);
        *str = strdup(dat.u.s);
    }
}

static int get_toml_arr_ui64(toml_table_t *t, const char *key, uint64_t *vals)
{
    toml_array_t *arr;
    toml_datum_t dat;
    int i;

    arr = toml_array_in(t, key);
    if(!arr) {
        return (0);
    }
    for(i = 0;; i++) {
        dat = toml_int_at(arr, i);
        if(!dat.ok) {
            break;
        }
        vals[i] = dat.u.i;
    }
    return (i);
}

static void parse_remotes_table(toml_table_t *remotes, struct ds_conf *conf)
{
    toml_table_t *remote;
    char *ip;
    int port;
    int i;

    conf->nremote = toml_table_ntab(remotes);
    *conf->remotes = malloc(sizeof(**conf->remotes) * conf->nremote);
    for(i = 0; i < conf->nremote; i++) {
        (*conf->remotes)[i].name = strdup(toml_key_in(remotes, i));
        remote = toml_table_in(remotes, (*conf->remotes)[i].name);
        get_toml_str(remote, "ip", &ip);
        get_toml_int(remote, "port", &port);
        sprintf((*conf->remotes)[i].addr_str, "%s:%i", ip, port);
        free(ip);
    }
}

static void parse_storage_table(toml_table_t *storage, struct ds_conf *conf)
{
    int ndir, nfile;
    struct dspaces_dir *dir;
    struct dspaces_file *file;
    toml_table_t *conf_dir;
    toml_array_t *arr;
    toml_datum_t dat;
    int i, j;

    ndir = toml_table_ntab(storage);
    for(i = 0; i < ndir; i++) {
        dir = malloc(sizeof(*dir));
        dir->name = strdup(toml_key_in(storage, i));
        conf_dir = toml_table_in(storage, dir->name);
        get_toml_str(conf_dir, "directory", &dir->path);
        if(0 != (arr = toml_array_in(conf_dir, "files"))) {
            INIT_LIST_HEAD(&dir->files);
            nfile = toml_array_nelem(arr);
            for(j = 0; j < nfile; j++) {
                file = malloc(sizeof(*file));
                file->type = DS_FILE_NC;
                dat = toml_string_at(arr, j);
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
    }
}

static void parse_modules_table(toml_table_t *modules, struct ds_conf *conf)
{
    struct dspaces_module *mod;
    toml_table_t *module;
    char *server, *file, *url, *type, *ext;
    int nmod;
    int i;

    nmod = toml_table_ntab(modules);
    for(i = 0; i < nmod; i++) {
        mod = calloc(1, sizeof(*mod));
        mod->name = strdup(toml_key_in(modules, i));
        module = toml_table_in(modules, mod->name);
        get_toml_str(module, "namespace", &mod->namespace);
        if(!mod->namespace) {
            fprintf(stderr,
                    "WARNING: No namespace for '%s'. Query matching currently "
                    "requires a namespace.\n",
                    mod->name);
        }
        get_toml_str(module, "file", &file);
        if(file) {
            ext = strrchr(file, '.');
            if(strcmp(ext, ".py") == 0) {
                mod->type = DSPACES_MOD_PY;
                ext[0] = '\0';
                mod->file = file;
            } else {
                fprintf(stderr,
                        "WARNING: could not determine type of module '%s', "
                        "with extension '%s'. Skipping.\n",
                        mod->name, ext);
                free(file);
                continue;
            }
        }
        list_add(&mod->entry, conf->mods);
    }
}

int parse_conf_toml(const char *fname, struct ds_conf *conf)
{
    FILE *fin;
    toml_table_t *toml_conf, *server, *remotes, *storage, *modules;
    char errbuf[200];
    char *ip;
    int port;
    int ndir;
    struct dspaces_dir *dir;
    struct dspaces_file *file;
    int i;

    fin = fopen(fname, "r");
    if(!fin) {
        fprintf(stderr, "ERROR: could not open configuration file '%s'.\n",
                fname);
        return -errno;
    }

    toml_conf = toml_parse_file(fin, errbuf, sizeof(errbuf));
    fclose(fin);

    if(!conf) {
        fprintf(stderr, "could not parse %s, %s.\n", fname, errbuf);
        return -1;
    }

    server = toml_table_in(toml_conf, "server");
    if(!server) {
        fprintf(stderr, "missing [server] block from %s\n", fname);
        return -1;
    }

    conf->ndim = get_toml_arr_ui64(server, "dims", conf->dims.c);
    get_toml_int(server, "max_versions", &conf->max_versions);
    get_toml_int(server, "hash_version", &conf->hash_version);
    get_toml_int(server, "num_apps", &conf->num_apps);

    remotes = toml_table_in(toml_conf, "remotes");
    if(remotes) {
        parse_remotes_table(remotes, conf);
    }

    storage = toml_table_in(toml_conf, "storage");
    if(storage) {
        parse_storage_table(storage, conf);
    }

    modules = toml_table_in(toml_conf, "modules");
    if(modules) {
        parse_modules_table(modules, conf);
    }

    toml_free(toml_conf);
}

void print_conf(struct ds_conf *conf)
{
    int i;

    printf("DataSpaces server config:\n");
    printf("=========================\n");
    printf(" Default global dimensions: (");
    printf("%" PRIu64, conf->dims.c[0]);
    for(i = 1; i < conf->ndim; i++) {
        printf(", %" PRIu64, conf->dims.c[i]);
    }
    printf(")\n");
    printf(" MAX STORED VERSIONS: %i\n", conf->max_versions);
    printf(" HASH TYPE: %s\n", hash_strings[conf->hash_version]);
    if(conf->num_apps >= 0) {
        printf(" APPS EXPECTED: %i\n", conf->num_apps);
    } else {
        printf(" RUN UNTIL KILLED\n");
    }
    printf("=========================\n");
    fflush(stdout);
}