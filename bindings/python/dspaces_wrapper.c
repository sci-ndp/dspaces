#include <Python.h>
#include <mpi4py/mpi4py.h>
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#define PY_ARRAY_UNIQUE_SYMBOL ds
#include <numpy/ndarrayobject.h>
#include <numpy/ndarraytypes.h>

#include <dspaces.h>
#include <dspaces-server.h>

#include <stdio.h>

PyObject *wrapper_dspaces_init(int rank)
{
    dspaces_client_t *clientp;

    import_array();
    import_mpi4py();

    clientp = malloc(sizeof(*clientp));

    dspaces_init(rank, clientp);

    PyObject *client = PyLong_FromVoidPtr((void *)clientp);

    return (client);
}

PyObject *wrapper_dspaces_init_mpi(PyObject *commpy)
{
    MPI_Comm *comm_p = NULL;
    dspaces_client_t *clientp;

    import_array();
    import_mpi4py();

    comm_p = PyMPIComm_Get(commpy);
    if(!comm_p) {
        return(NULL);
    }
    clientp = malloc(sizeof(*clientp));

    dspaces_init_mpi(*comm_p, clientp);

    PyObject *client = PyLong_FromVoidPtr((void *)clientp);

    return (client);
}

PyObject *wrapper_dspaces_server_init(const char *listen_str, PyObject *commpy,
                                const char *conf)
{
    MPI_Comm *comm_p = NULL;
    dspaces_provider_t *serverp;

    import_array();
    import_mpi4py();

    comm_p = PyMPIComm_Get(commpy);
    if(!comm_p) {
        return(NULL);
    }
    serverp = malloc(sizeof(*serverp));

    dspaces_server_init(listen_str, *comm_p, conf, serverp);

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
    void *data = PyArray_DATA(arr);
    uint64_t lb[ndim];
    uint64_t ub[ndim];
    npy_intp *shape = PyArray_DIMS(arr);
    PyObject *item;
    int i;

    for(i = 0; i < ndim; i++) {
        item = PyTuple_GetItem(offset, i);
        lb[i] = PyLong_AsLong(item);
        ub[i] = lb[i] + ((long)shape[i] - 1);
    }
    dspaces_put(*clientp, name, version, size, ndim, lb, ub, data);

    return;
}

PyObject *wrapper_dspaces_get(PyObject *clientppy, const char *name,
                              int version, PyObject *lbt, PyObject *ubt,
                              PyObject *dtype, int timeout)
{
    dspaces_client_t *clientp = PyLong_AsVoidPtr(clientppy);
    int ndim = PyTuple_GET_SIZE(lbt);
    uint64_t lb[ndim];
    uint64_t ub[ndim];
    void *data;
    PyObject *item;
    PyObject *arr;
    PyArray_Descr *descr = PyArray_DescrNew((PyArray_Descr *)dtype);
    npy_intp dims[ndim];
    int i;

    for(i = 0; i < ndim; i++) {
        item = PyTuple_GetItem(lbt, i);
        lb[i] = PyLong_AsLong(item);
        item = PyTuple_GetItem(ubt, i);
        ub[i] = PyLong_AsLong(item);
        dims[i] = (ub[i] - lb[i]) + 1;
    }

    dspaces_aget(*clientp, name, version, ndim, lb, ub, &data, timeout);

    arr = PyArray_NewFromDescr(&PyArray_Type, descr, ndim, dims, NULL, data, 0,
                               NULL);

    return (arr);
}
