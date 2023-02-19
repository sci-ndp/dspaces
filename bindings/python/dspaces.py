from dspaces.dspaces_wrapper import *
import numpy as np

class DSServer:
    def __init__(self, conn = "sockets", comm = None, conf = "dataspaces.conf"):
        from mpi4py import MPI
        if comm == None:
            comm = MPI.COMM_WORLD
        self.server = wrapper_dspaces_server_init(conn.encode('ascii'), comm, conf.encode('ascii'))

    def Run(self):
        wrapper_dspaces_server_fini(self.server)

class DSClient:
    def __init__(self, comm = None, conn = None, rank = None):
        self.nspace = ""
        if conn == None:
            if rank == None or not comm == None:
                from mpi4py import MPI
                if comm == None:
                    comm = MPI.COMM_WORLD
                self.client = wrapper_dspaces_init_mpi(comm)
            else:
                self.client = wrapper_dspaces_init(rank)
        else:
            listen_str = conn.split("://")[0]
            if rank == None or not comm == None:
                from mpi4py import MPI
                if comm == None:
                    comm = MPI.COMM_WORLD
                self.client = wrapper_dspaces_init_wan_mpi(listen_str.encode('ascii'), conn.encode('ascii'), comm)
            else:
                self.client = wrapper_dspaces_init_wan(listen_str.encode('ascii'), conn.encode('ascii'), rank)

    def __del__(self):
        wrapper_dspaces_fini(self.client) 
 
    def KillServer(self, token_count = 1):
        for token in range(token_count):
            wrapper_dspaces_kill(self.client)

    def SetNSpace(self, nspace):
        self.nspace = nspace + "\\"

    def Put(self, data, name, version, offset):
        if len(offset) != len(data.shape):
            raise TypeError("offset should have the same dimensionality as data")
        wrapper_dspaces_put(self.client, data, (self.nspace + name).encode('ascii'), version, offset)

    def Get(self, name, version, lb, ub, dtype, timeout):
        if len(lb) != len(ub):
            raise TypeError("lower-bound and upper-bound must have the same dimensionality")
        return wrapper_dspaces_get(self.client, (self.nspace + name).encode('ascii'), version, lb, ub, np.dtype(dtype), timeout)    

    def DefineGDim(self, name, gdim):
        wrapper_dspaces_define_gdim(self.client, (self.nspace + name).encode('ascii'), gdim)

class DSExpr:
    def __init__(self, client):
        self.client = client
    def __add__(self, other):
        obj = DSExpr(self.client)
        obj.expr = wrapper_dspaces_op_new_add(self.expr, other.expr)
        return(obj)
    def exec(self):
        return(wrapper_dspaces_ops_calc(self.client.client, self.expr))
class DSConst(DSExpr):
    def __init__(self, client, val):
        DSExpr.__init__(self, client)
        if np.dtype(type(val)) == np.int64:
            self.expr = wrapper_dspaces_ops_new_iconst(val)
        elif dtype(type(val)) == np.float64:
            self.expr = wrapper_dspaces_ops_new_rconst(val)
        else:
            raise(TypeError("expression constants must be floats or ints"))
    def __add__(self, other):
        return(DSExpr.__add__(self, other))

class DSData(DSExpr):
    def __init__(self, client, name, version, lb, ub, dtype):
        DSExpr.__init__(self, client)
        if len(lb) != len(ub):
            raise TypeError("lower-bound and upper-bound must have the same dimensionality")
        self.expr = wrapper_dspaces_ops_new_obj(client.client, name.encode('ascii'), version, lb, ub, dtype)
    def __add__(self, other):
        return(DSExpr.__add__(self, other))
