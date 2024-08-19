#ifndef __DSPACES_MODULES_H__
#define __DSPACES_MODULES_H__

#ifdef DSPACES_HAVE_PYTHON
#include "Python.h"
#endif // DSPACES_HAVE_PYTHON
#include "list.h"
#include "ss_data.h"

typedef enum dspaces_mod_type { DSPACES_MOD_PY } dspaces_mod_type_t;

struct dspaces_module {
    struct list_head entry;
    char *name;
    char *namespace;
    char *file;
    dspaces_mod_type_t type;
    union {
#ifdef DSPACES_HAVE_PYTHON
        PyObject *pModule;
#endif // DSPACES_HAVE_PYTHON
    };
};

typedef enum dspaces_module_arg_type {
    DSPACES_ARG_REAL,
    DSPACES_ARG_INT,
    DSPACES_ARG_STR,
    DSPACES_ARG_NONE
} dspaces_mod_arg_type_t;

struct dspaces_module_args {
    char *name;
    dspaces_mod_arg_type_t type;
    int len;
    union {
        double rval;
        long ival;
        double *rarray;
        long *iarray;
        char *strval;
    };
};

typedef enum dspaces_module_return_type {
    DSPACES_MOD_RET_ARRAY,
    DSPACES_MOD_RET_NONE,
    DSPACES_MOD_RET_ERR
} dspaces_mod_ret_type_t;

struct dspaces_module_ret {
    dspaces_mod_ret_type_t type;
    int len;
    uint64_t *dim;
    union {
        int ndim;
        int err;
    };
    int tag;
    int elem_size;
    enum storage_type st;
    void *data;
};

int dspaces_init_mods(struct list_head *mods);

int build_module_args_from_odsc(obj_descriptor *odsc,
                                struct dspaces_module_args **argsp);

int build_module_args_from_reg(reg_in_t *reg,
                               struct dspaces_module_args **argsp);

void free_arg_list(struct dspaces_module_args *args, int len);

struct dspaces_module *dspaces_mod_by_od(struct list_head *mods,
                                         obj_descriptor *odsc);

struct dspaces_module *dspaces_mod_by_name(struct list_head *mods,
                                           const char *name);

struct dspaces_module_ret *dspaces_module_exec(struct dspaces_module *mod,
                                               const char *operation,
                                               struct dspaces_module_args *args,
                                               int nargs, int ret_type);

#endif // __DSPACES_MODULES_H__
