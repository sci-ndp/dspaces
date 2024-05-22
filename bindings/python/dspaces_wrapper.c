#include <Python.h>
#include <mpi4py/mpi4py.h>
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#define PY_ARRAY_UNIQUE_SYMBOL ds
#include <numpy/ndarrayobject.h>
#include <numpy/ndarraytypes.h>

#include <dspaces-ops.h>
#include <dspaces-server.h>
#include <dspaces.h>

#include <stdio.h>

PyObject *wrapper_dspaces_init(int rank)
{
    dspaces_client_t *clientp;
    char err_str[100];
    int ret;

    import_array();
    import_mpi4py();

    clientp = malloc(sizeof(*clientp));

    // clang-format off
    //Py_BEGIN_ALLOW_THREADS
    ret = dspaces_init(rank, clientp);
    //Py_END_ALLOW_THREADS
    // clang-format on
    if(ret != 0) {
        sprintf(err_str, "dspaces_init() failed with %i", ret);
        PyErr_SetString(PyExc_RuntimeError, err_str);
        return NULL;
    }

    PyObject *client = PyLong_FromVoidPtr((void *)clientp);

    return (client);
}

PyObject *wrapper_dspaces_init_mpi(PyObject *commpy)
{
    MPI_Comm *comm_p = NULL;
    dspaces_client_t *clientp;
    char err_str[100];
    int ret;

    import_array();
    import_mpi4py();

    comm_p = PyMPIComm_Get(commpy);
    if(!comm_p) {
        return (NULL);
    }
    clientp = malloc(sizeof(*clientp));

    // clang-format off
    //Py_BEGIN_ALLOW_THREADS
    ret = dspaces_init_mpi(*comm_p, clientp);
    //Py_END_ALLOW_THREADS
    // clang-format on
    if(ret != 0) {
        sprintf(err_str, "dspaces_init_mpi() failed with %i", ret);
        PyErr_SetString(PyExc_RuntimeError, err_str);
        return NULL;
    }

    PyObject *client = PyLong_FromVoidPtr((void *)clientp);

    return (client);
}

PyObject *wrapper_dspaces_init_wan(const char *listen_str, const char *conn,
                                   int rank)
{
    dspaces_client_t *clientp;
    char err_str[100];
    int ret;

    import_array();
    import_mpi4py();

    clientp = malloc(sizeof(*clientp));

    // clang-format off
    //Py_BEGIN_ALLOW_THREADS
    ret = dspaces_init_wan(listen_str, conn, rank, clientp);
    //Py_END_ALLOW_THREADS
    // clang-format on
    if(ret != 0) {
        sprintf(err_str, "dspaces_init_wan() failed with %i", ret);
        PyErr_SetString(PyExc_RuntimeError, err_str);
        return NULL;
    }

    PyObject *client = PyLong_FromVoidPtr((void *)clientp);

    return (client);
}

PyObject *wrapper_dspaces_init_wan_mpi(const char *listen_str, const char *conn,
                                       PyObject *commpy)
{
    MPI_Comm *comm_p = NULL;
    dspaces_client_t *clientp;
    char err_str[100];
    int ret;

    import_array();
    import_mpi4py();

    comm_p = PyMPIComm_Get(commpy);
    if(!comm_p) {
        return (NULL);
    }
    clientp = malloc(sizeof(*clientp));

    // clang-format off
    //Py_BEGIN_ALLOW_THREADS
    ret = dspaces_init_wan_mpi(listen_str, conn, *comm_p, clientp);
    //Py_END_ALLOW_THREADS
    // clang-format on
    if(ret != 0) {
        sprintf(err_str, "dspaces_init_wan_mpi() failed with %i", ret);
        PyErr_SetString(PyExc_RuntimeError, err_str);
        return NULL;
    }

    PyObject *client = PyLong_FromVoidPtr((void *)clientp);

    return (client);
}

PyObject *wrapper_dspaces_server_init(const char *listen_str, PyObject *commpy,
                                      const char *conf)
{
    MPI_Comm *comm_p = NULL;
    dspaces_provider_t *serverp;
    char err_str[100];
    int ret;

    import_array();
    import_mpi4py();

    comm_p = PyMPIComm_Get(commpy);
    if(!comm_p) {
        return (NULL);
    }
    serverp = malloc(sizeof(*serverp));

    // clang-format off
    //Py_BEGIN_ALLOW_THREADS
    ret = dspaces_server_init(listen_str, *comm_p, conf, serverp);
    //Py_END_ALLOW_THREADS
    // clang-format on
    if(ret != 0) {
        sprintf(err_str, "dspaces_init_mpi() failed with %i", ret);
        PyErr_SetString(PyExc_RuntimeError, err_str);
        return NULL;
    }

    PyObject *server = PyLong_FromVoidPtr((void *)serverp);

    return (server);
}

void wrapper_dspaces_fini(PyObject *clientppy)
{
    dspaces_client_t *clientp = PyLong_AsVoidPtr(clientppy);

    dspaces_fini(*clientp);

    free(clientp);
}

void wrapper_dspaces_server_fini(PyObject *serverppy)
{
    dspaces_provider_t *serverp = PyLong_AsVoidPtr(serverppy);

    dspaces_server_fini(*serverp);

    free(serverp);
}

void wrapper_dspaces_kill(PyObject *clientppy)
{
    dspaces_client_t *clientp = PyLong_AsVoidPtr(clientppy);

    dspaces_kill(*clientp);
}

void wrapper_dspaces_put(PyObject *clientppy, PyObject *obj, const char *name,
                         int version, PyObject *offset)
{
    dspaces_client_t *clientp = PyLong_AsVoidPtr(clientppy);
    PyArrayObject *arr = (PyArrayObject *)obj;
    int size = PyArray_ITEMSIZE(arr);
    int ndim = PyArray_NDIM(arr);
    int tag = PyArray_TYPE(arr);
    void *data = PyArray_DATA(arr);
    uint64_t lb[ndim];
    uint64_t ub[ndim];
    npy_intp *shape = PyArray_DIMS(arr);
    PyObject *item;
    int i;

    //Reverse order of indices
    for(i = 0; i < ndim; i++) {
        item = PyTuple_GetItem(offset, i);
        lb[(ndim-1) - i] = PyLong_AsLong(item);
        ub[(ndim-1) - i] = lb[(ndim-1) - i] + ((long)shape[i] - 1);
    }

    // clang-format off
    //Py_BEGIN_ALLOW_THREADS
    dspaces_put_tag(*clientp, name, version, size, tag, ndim, lb, ub, data);
    //Py_END_ALLOW_THREADS
    // clang-format on

    return;
}

PyObject *wrapper_dspaces_get(PyObject *clientppy, const char *name,
                              int version, PyObject *lbt, PyObject *ubt,
                              PyObject *dtype, int timeout)
{
    dspaces_client_t *clientp = PyLong_AsVoidPtr(clientppy);
    struct dspaces_req in_req = {0}, out_req = {0};
    int ndim = PyTuple_GET_SIZE(lbt);
    uint64_t lb[ndim];
    uint64_t ub[ndim];
    int tag;
    void *data;
    PyObject *item;
    PyObject *arr;
    PyArray_Descr *descr;
    npy_intp dims[ndim];
    int i;

     //Reverse order of indices
    for(i = 0; i < ndim; i++) {
        item = PyTuple_GetItem(lbt, i);
        lb[(ndim-1) - i] = PyLong_AsLong(item);
        item = PyTuple_GetItem(ubt, i);
        ub[(ndim-1) - i] = PyLong_AsLong(item);
    }

    in_req.var_name = strdup(name);
    in_req.ver = version;
    in_req.ndim = ndim;
    in_req.lb = lb;
    in_req.ub = ub; 

    // clang-format off
    //Py_BEGIN_ALLOW_THREADS 
    dspaces_get_req(*clientp, &in_req, &out_req, timeout);
    //Py_END_ALLOW_THREADS
    // clang-format on
    data = out_req.buf;

    free(in_req.var_name);

    if(data == NULL)
    {
        Py_INCREF(Py_None);
        return (Py_None);
    }

    if(dtype == Py_None) {
        descr = PyArray_DescrNewFromType(out_req.tag);
    } else {
        descr = PyArray_DescrNew((PyArray_Descr *)dtype);
    }

    ndim = out_req.ndim;
    for(i = 0; i < ndim; i++) {
        dims[(ndim-1) - i] = ((out_req.ub[i] - out_req.lb[i]) + 1);
    }

    arr = PyArray_NewFromDescr(&PyArray_Type, descr, ndim, dims, NULL, data, 0,
                               NULL);

    return (arr);
}

PyObject *wrapper_dspaces_pexec(PyObject *clientppy, PyObject *req_list,
                                PyObject *fn, const char *fn_name)
{
    dspaces_client_t *clientp = PyLong_AsVoidPtr(clientppy);
    PyObject *item;
    PyObject *req_dict, *lbt, *ubt;
    uint64_t *lb, *ub;
    int ndim;
    void *data;
    int data_size;
    PyObject *result;
    struct dspaces_req *reqs;
    int num_reqs;
    int i, j;

    num_reqs = PyList_Size(req_list);
    reqs = calloc(num_reqs, sizeof(*reqs));

    for(i = 0; i < num_reqs; i++) {
        req_dict = PyList_GetItem(req_list, i);
        char *teststr = PyDict_GetItemString(req_dict, "var_name");
        reqs[i].var_name = strdup(PyBytes_AsString(PyDict_GetItemString(req_dict, "var_name")));
        reqs[i].ver = PyLong_AsLong(PyDict_GetItemString(req_dict, "ver"));
        lbt = PyDict_GetItemString(req_dict, "lb");
        ubt = PyDict_GetItemString(req_dict, "ub");
        
        if(lbt == Py_None || ubt == Py_None) {
            if(lbt != ubt) {
                PyErr_SetString(PyExc_TypeError,
                            "both lb and ub must be set or neither");
                return (NULL);
            }
        } else if(PyTuple_GET_SIZE(lbt) != PyTuple_GET_SIZE(ubt)) {
            PyErr_SetString(PyExc_TypeError, "lb and ub must have the same length");
            return (NULL);
        } else if(lbt == Py_None) {
            reqs[i].ndim = 0;
        } else {
            reqs[i].ndim = PyTuple_GET_SIZE(lbt);
        }
        ndim = reqs[i].ndim;

        lb = malloc(sizeof(*lb) * ndim);
        ub = malloc(sizeof(*ub) * ndim);
        //Reverse order of indices
        for(j = 0; j < ndim; j++) {
            item = PyTuple_GetItem(lbt, j);
            lb[(ndim-1) - j] = PyLong_AsLong(item);
            item = PyTuple_GetItem(ubt, j);
            ub[(ndim-1) - j] = PyLong_AsLong(item);
        }
        reqs[i].lb = lb;
        reqs[i].ub = ub;

        if(!PyBytes_Check(fn)) {
            PyErr_SetString(PyExc_TypeError,
                        "fn must be serialized as a a byte string");
            return (NULL);
        }
    }
    dspaces_mpexec(*clientp, num_reqs, reqs, PyBytes_AsString(fn),
                  PyBytes_Size(fn) + 1, fn_name, &data, &data_size);
        

    if(data_size > 0) {
        result = PyBytes_FromStringAndSize(data, data_size);
    } else {
        Py_INCREF(Py_None);
        result = Py_None;
    }

    for(i = 0; i < num_reqs; i++) {
        free(reqs[i].var_name);
        free(reqs[i].lb);
        free(reqs[i].ub);
    }
    free(reqs);

    return (result);
}

void wrapper_dspaces_define_gdim(PyObject *clientppy, const char *name,
                                 PyObject *gdimt)
{
    dspaces_client_t *clientp = PyLong_AsVoidPtr(clientppy);
    int ndim = PyTuple_GET_SIZE(gdimt);
    uint64_t gdim[ndim];
    PyObject *item;
    int i;

    for(i = 0; i < ndim; i++) {
        item = PyTuple_GetItem(gdimt, i);
        gdim[(ndim-1) - i] = PyLong_AsLong(item);
    }

    dspaces_define_gdim(*clientp, name, ndim, gdim);
}

PyObject *wrapper_dspaces_get_vars(PyObject *clientppy)
{
    dspaces_client_t *clientp = PyLong_AsVoidPtr(clientppy);
    int num_vars;
    char **var_names = NULL;
    PyObject *var_list;
    PyObject *name;
    int i;

    num_vars = dspaces_get_var_names(*clientp, &var_names);
    if(num_vars < 0) {
        return (NULL);
    }

    var_list = PyList_New(0);
    for(i = 0; i < num_vars; i++) {
        name = PyUnicode_DecodeASCII(var_names[i], strlen(var_names[i]), NULL);
        if(name) {
            PyList_Append(var_list, name);
        }
        free(var_names[i]);
    }

    if(num_vars) {
        free(var_names);
    }

    return (var_list);
}

PyObject *wrapper_dspaces_get_var_objs(PyObject *clientppy, const char *name)
{
    dspaces_client_t *clientp = PyLong_AsVoidPtr(clientppy);
    int num_obj;
    dspaces_obj_t *objs, *obj;
    PyObject *obj_list;
    PyObject *pobj, *lbt, *ubt;
    int i, j;

    num_obj = dspaces_get_var_objs(*clientp, name, &objs);
    if(num_obj < 0) {
        return (NULL);
    }

    obj_list = PyList_New(0);
    for(i = 0; i < num_obj; i++) {
        obj = &objs[i];
        pobj = PyDict_New();
        PyDict_SetItemString(
            pobj, "name",
            PyUnicode_DecodeASCII(obj->name, strlen(obj->name), NULL));
        PyDict_SetItemString(pobj, "version", PyLong_FromLong(obj->version));
        lbt = PyTuple_New(obj->ndim);
        ubt = PyTuple_New(obj->ndim);
        for(j = 0; j < obj->ndim; j++) {
            PyTuple_SetItem(lbt, j, PyLong_FromLong(obj->lb[j]));
            PyTuple_SetItem(ubt, j, PyLong_FromLong(obj->ub[j]));
        }
        PyDict_SetItemString(pobj, "lb", lbt);
        PyDict_SetItemString(pobj, "ub", ubt);
        PyList_Append(obj_list, pobj);
        free(obj->name);
        free(obj->lb);
        free(obj->ub);
    }

    return (obj_list);
}

PyObject *wrapper_dspaces_ops_new_iconst(long val)
{
    ds_expr_t *exprp;

    exprp = malloc(sizeof(*exprp));

    *exprp = dspaces_op_new_iconst(val);

    PyObject *expr = PyLong_FromVoidPtr((void *)exprp);

    return (expr);
}

PyObject *wrapper_dspaces_ops_new_rconst(double val)
{
    ds_expr_t *exprp;

    exprp = malloc(sizeof(*exprp));

    *exprp = dspaces_op_new_rconst(val);

    PyObject *expr = PyLong_FromVoidPtr((void *)exprp);

    return (expr);
}

PyObject *wrapper_dspaces_ops_new_obj(PyObject *clientppy, const char *name,
                                      int version, PyObject *lbt, PyObject *ubt,
                                      PyObject *dtype)
{
    dspaces_client_t *clientp = PyLong_AsVoidPtr(clientppy);
    ds_expr_t *exprp;
    int ndim = PyTuple_GET_SIZE(lbt);
    uint64_t lb[ndim];
    uint64_t ub[ndim];
    PyObject *item, *item_utf, *expr;
    char *type_str;
    int val_type;
    int i;

    item = PyObject_GetAttrString(dtype, "__name__");
    item_utf = PyUnicode_EncodeLocale(item, "strict");
    type_str = PyBytes_AsString(item_utf);

    if(strcmp(type_str, "float") == 0) {
        val_type = DS_VAL_REAL;
    } else if(strcmp(type_str, "int") == 0) {
        val_type = DS_VAL_INT;
    } else {
        PyErr_SetString(PyExc_TypeError, "type must be int or float");
        return (NULL);
    }

    for(i = 0; i < ndim; i++) {
        item = PyTuple_GetItem(lbt, i);
        lb[i] = PyLong_AsLong(item);
        item = PyTuple_GetItem(ubt, i);
        ub[i] = PyLong_AsLong(item);
    }

    exprp = malloc(sizeof(*exprp));
    *exprp =
        dspaces_op_new_obj(*clientp, name, version, val_type, ndim, lb, ub);
    expr = PyLong_FromVoidPtr((void *)exprp);

    return (expr);
}

PyObject *wrapper_dspaces_op_new_add(PyObject *exprppy1, PyObject *exprppy2)
{
    ds_expr_t *exprp1, *exprp2, *resp;
    PyObject *res;

    exprp1 = PyLong_AsVoidPtr(exprppy1);
    exprp2 = PyLong_AsVoidPtr(exprppy2);

    resp = malloc(sizeof(*resp));
    *resp = dspaces_op_new_add(*exprp1, *exprp2);
    res = PyLong_FromVoidPtr((void *)resp);

    return (res);
}

PyObject *wrapper_dspaces_op_new_sub(PyObject *exprppy1, PyObject *exprppy2)
{
    ds_expr_t *exprp1, *exprp2, *resp;
    PyObject *res;

    exprp1 = PyLong_AsVoidPtr(exprppy1);
    exprp2 = PyLong_AsVoidPtr(exprppy2);

    resp = malloc(sizeof(*resp));
    *resp = dspaces_op_new_sub(*exprp1, *exprp2);
    res = PyLong_FromVoidPtr((void *)resp);

    return (res);
}

PyObject *wrapper_dspaces_op_new_mult(PyObject *exprppy1, PyObject *exprppy2)
{
    ds_expr_t *exprp1, *exprp2, *resp;
    PyObject *res;

    exprp1 = PyLong_AsVoidPtr(exprppy1);
    exprp2 = PyLong_AsVoidPtr(exprppy2);

    resp = malloc(sizeof(*resp));
    *resp = dspaces_op_new_mult(*exprp1, *exprp2);
    res = PyLong_FromVoidPtr((void *)resp);

    return (res);
}

PyObject *wrapper_dspaces_op_new_div(PyObject *exprppy1, PyObject *exprppy2)
{
    ds_expr_t *exprp1, *exprp2, *resp;
    PyObject *res;

    exprp1 = PyLong_AsVoidPtr(exprppy1);
    exprp2 = PyLong_AsVoidPtr(exprppy2);

    resp = malloc(sizeof(*resp));
    *resp = dspaces_op_new_div(*exprp1, *exprp2);
    res = PyLong_FromVoidPtr((void *)resp);

    return (res);
}

PyObject *wrapper_dspaces_op_new_pow(PyObject *exprppy1, PyObject *exprppy2)
{
    ds_expr_t *exprp1, *exprp2, *resp;
    PyObject *res;

    exprp1 = PyLong_AsVoidPtr(exprppy1);
    exprp2 = PyLong_AsVoidPtr(exprppy2);

    resp = malloc(sizeof(*resp));
    *resp = dspaces_op_new_pow(*exprp1, *exprp2);
    res = PyLong_FromVoidPtr((void *)resp);

    return (res);
}

PyObject *wrapper_dspaces_op_new_arctan(PyObject *exprppy1)
{
    ds_expr_t *exprp1, *resp;
    PyObject *res;

    exprp1 = PyLong_AsVoidPtr(exprppy1);

    resp = malloc(sizeof(*resp));
    *resp = dspaces_op_new_arctan(*exprp1);
    res = PyLong_FromVoidPtr((void *)resp);

    return (res);
}

PyObject *wrapper_dspaces_ops_calc(PyObject *clientppy, PyObject *exprppy)
{
    void *result_buf;
    dspaces_client_t *clientp = PyLong_AsVoidPtr(clientppy);
    ds_expr_t *exprp = PyLong_AsVoidPtr(exprppy);
    int typenum;
    int ndim;
    uint64_t *dims;
    ds_val_t etype;
    npy_intp *array_dims;
    long int_res;
    double real_res;
    PyObject *arr;
    int i;

    dspaces_op_calc(*clientp, *exprp, &result_buf);
    dspaces_op_get_result_size(*exprp, &ndim, &dims);
    etype = dspaces_op_get_result_type(*exprp);
    if(ndim == 0) {
        if(etype == DS_VAL_INT) {
            int_res = *(long *)result_buf;
            free(result_buf);
            return (PyLong_FromLong(int_res));
        } else if(etype == DS_VAL_REAL) {
            real_res = *(double *)result_buf;
            free(result_buf);
            return (PyFloat_FromDouble(real_res));
        } else {
            PyErr_SetString(
                PyExc_TypeError,
                "invalid type assigned to expression (corruption?)");
            return (NULL);
        }
    }
    array_dims = malloc(sizeof(*array_dims) * ndim);
    for(i = 0; i < ndim; i++) {
        array_dims[i] = dims[i];
    }
    free(dims);
    if(etype == DS_VAL_INT) {
        typenum = NPY_INT64;
    } else if(etype == DS_VAL_REAL) {
        typenum = NPY_FLOAT64;
    } else {
        PyErr_SetString(PyExc_TypeError,
                        "invalid type assigned to expression (corruption?)");
        return (NULL);
    }
    arr = PyArray_SimpleNewFromData(ndim, array_dims, typenum, result_buf);
    return (arr);
}
