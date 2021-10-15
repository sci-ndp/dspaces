How to Run
==========

Running the Server
-----------------
The DataSpaces server expects to find the ``dataspaces.conf`` file in its working directory. 
The format of this file is a list of configuration values, one per line:

``<variable> = <value>``, e.g.
``num_apps = 1``

The possible values are as follows:

**num_apps**: this value is the number of ``dspaces_kill()`` calls from clients that are needed to kill the server binary.

**ndim**: number of dimensions for the default global data domain.

**dims**: size of each dimension for the default global data domain.

**max_versions**: maximum number of versions of a data object to be cached in DataSpaces servers.

**hash_version**: the type of distributed hash table used. A value of ``1`` means that a Hilbert SFC is partitioned into continuous segments and distributed across the servers.
A value of ``2`` means the space is partitioned by repeating bisection along the longest domain.

**NOTES** on what values to use:

The global dimensions have implications for performance. Data indexing will be partitioned evently across the global dimensions, 
and so if data is only being writtent to a subset of the global dimensions there is a risk of unabalanced indexing load.
Ideally, the data domain being written to will match the global dimensions as closely as possible. The default value set in
``dataspaces.conf`` is for convenience. The application can set this per variable with ``dspaces_define_gdim()``.

``hash_version = 1`` has better locality in the most general case, and should be preferred unless the dimensions of the data 
domain are not a power of two or the ratio of longest to shortest dimension is greater than two.

``num_apps`` should be set in conjunction with how ``dspaces_kill()`` is used in the application(s) using dataspaces. Generally, one rank
of each application should call ``dspaces_kill()``, and the number of process groups using dataspaces will be the same as ``num_apps``. 
Occasionally, it is not practical to have a client call ``dspaces_kill()``, and the dataspaces repo provides a standalone binary ``terminator`` 
to send a single ``dspaces_kill()`` and then exit.

Bootstrapping communication
---------------------------
The server produces a bootstrap file during its init phase, ``conf.ds``. This file must be read by the clients (or rank zero of the clients 
if ``dspaces_init_mpi()`` is being used. This file provides the clients with enough information to make initial contact with the server and
perform wire-up. In order to find this file, the server and client application must be run in the same working directory, or at last a symlink of ``ds.conf`` should be present.

Environment variables
---------------------
There are a few environment variables that can be used to influence DataSpaces.

``DSPACES_DEBUG`` - enables substantial debug output for both clients and server.

``DSPACES_DEFAULT_NUM_HANDLERS`` - the number of request handling threads launched by the server (in addition to the main thread). Default: 4.
 This value should be changed if it is likely to oversubscribe or underutilize the node the server is running on.

 Running the server
 ------------------

The server binary, ``dspaces_server``, takes a single argument: the listen_address. 
This is a Mercury-specific connection string (see Mercury documentation for details.) 
Common values are: ``sockets`` to use TCP for communication, ``sm`` for shared memory 
(if all clienta and server processes are on the same node) and ``ofi+X`` for RDMA, 
where ``X`` is ``verbs``, ``psm2``, or ``cray`` as is appropriate for the system fabric.
