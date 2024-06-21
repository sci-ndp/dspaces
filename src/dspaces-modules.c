#include "dspaces-modules.h"

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include <Python.h>
#include <numpy/ndarrayobject.h>
#include <numpy/ndarraytypes.h>

#ifdef DSPACES_HAVE_PYTHON

#define xstr(s) str(s)
#define str(s) #s

static int dspaces_init_py_mod(struct dspaces_module *mod)
{
    PyObject *pName;

    pName = PyUnicode_DecodeFSDefault(mod->file);
    mod->pModule = PyImport_Import(pName);
    if(!mod->pModule) {
        fprintf(
            stderr,
            "WARNING: could not load module '%s' from %s. File missing? Any "
            "%s accesses will fail.\n",
            mod->file, xstr(DSPACES_MOD_DIR), mod->name);
        return (-1);
    }
    Py_DECREF(pName);

    return (0);
}
#endif // DSPACES_HAVE_PYTHON

int dspaces_init_mods(struct list_head *mods)
{
    struct dspaces_module *mod;

    list_for_each_entry(mod, mods, struct dspaces_module, entry)
    {
        switch(mod->type) {
        case DSPACES_MOD_PY:
#ifdef DSPACES_HAVE_PYTHON
            dspaces_init_py_mod(mod);
#else
            fprintf(stderr,
                    "WARNING: Unable to load module '%s', which is a python "
                    "module. DataSpaces was compiled without Python support.\n",
                    mod->name);
#endif // DSPACES_HAVE_PYTHON
            break;
        default:
            fprintf(stderr,
                    "WARNING: unknown type %i for module '%s'. Corruption?\n",
                    mod->type, mod->name);
        }
        // TODO: unlink module on init failure?
    }
}

int build_module_args_from_odsc(obj_descriptor *odsc,
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
            if(odsc->st == row_major) {
                args[2].iarray[i] = odsc->bb.lb.c[i];
            } else {
                args[2].iarray[(ndims - i) - 1] = odsc->bb.lb.c[i];
            }
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
            if(odsc->st == row_major) {
                args[3].iarray[i] = odsc->bb.ub.c[i];
            } else {
                args[3].iarray[(ndims - i) - 1] = odsc->bb.ub.c[i];
            }
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

void free_arg_list(struct dspaces_module_args *args, int len)
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

static struct dspaces_module *find_mod(struct list_head *mods,
                                       const char *mod_name)
{
    struct dspaces_module *mod;
    int i;

    list_for_each_entry(mod, mods, struct dspaces_module, entry)
    {
        if(strcmp(mod_name, mod->name) == 0) {
            return (mod);
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
    int flags;
    npy_intp *dims;
    int i;

    pArray = (PyArrayObject *)pResult;
    flags = PyArray_FLAGS(pArray);
    if(flags & NPY_ARRAY_C_CONTIGUOUS) {
        ret->st = row_major;
    } else if(flags & NPY_ARRAY_F_CONTIGUOUS) {
        ret->st = column_major;
    } else {
        fprintf(stderr, "WARNING: bad array alignment in %s\n", __func__);
        return (NULL);
    }
    ret->ndim = PyArray_NDIM(pArray);
    ret->dim = malloc(sizeof(*ret->dim * ret->ndim));
    dims = PyArray_DIMS(pArray);
    ret->len = 1;
    for(i = 0; i < ret->ndim; i++) {
        ret->dim[ret->st == column_major ? i : (ret->ndim - i) - 1] = dims[i];
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

struct dspaces_module *dspaces_mod_by_od(struct list_head *mods,
                                         obj_descriptor *odsc)
{
    struct dspaces_module *mod;
    int i;

    list_for_each_entry(mod, mods, struct dspaces_module, entry)
    {
        // TODO: query mods for match
        if(strstr(odsc->name, mod->namespace) == odsc->name) {
            return (mod);
        }
    }

    return (NULL);
}

static struct dspaces_module_ret *
dspaces_module_py_exec(struct dspaces_module *mod, const char *operation,
                       struct dspaces_module_args *args, int nargs,
                       int ret_type)
{
    PyObject *pFunc = PyObject_GetAttrString(mod->pModule, operation);
    PyObject *pKey, *pArg, *pArgs, *pKWArgs;
    PyObject *pResult;
    struct dspaces_module_ret *ret;
    int i;

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

struct dspaces_module_ret *dspaces_module_exec(struct dspaces_module *mod,
                                               const char *operation,
                                               struct dspaces_module_args *args,
                                               int nargs, int ret_type)
{
    if(mod->type == DSPACES_MOD_PY) {
#ifdef DSPACES_HAVE_PYTHON
        return (dspaces_module_py_exec(mod, operation, args, nargs, ret_type));
#else
        fprintf(stderr, "WARNNING: tried to execute python module, but no "
                        "python support.\n");
        return (NULL);
#endif // DSPACES_HAVE_PYTHON
    } else {
        fprintf(stderr, "ERROR: unknown module request in %s.\n", __func__);
        return (NULL);
    }
}