PyObject *wrapper_dspaces_init(int rank);

PyObject *wrapper_dspaces_init_mpi(PyObject *commpy);

PyObject *wrapper_dspaces_server_init(const char *listen_str, PyObject *commpy, const char *conf);

void wrapper_dspaces_fini(PyObject *clientppy);

void wrapper_dspaces_server_fini(PyObject *serverppy);

void wrapper_dspaces_kill(PyObject *clientppy);

void wrapper_dspaces_put(PyObject *clientppy, PyObject *obj, const char *name,
                         int version, PyObject *offset);

PyObject *wrapper_dspaces_get(PyObject *clientppy, const char *name,
                              int version, PyObject *lbt, PyObject *ubt,
                              PyObject *dtype, int timeout);
