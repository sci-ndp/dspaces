How to Use DataSpaces
=====================

DataSpaces consists of two components: a client library and a server library. 
Additionally, the DataSpaces package comes packaged with a standalone, MPI-based server binary.
The typical usage of DataSpaces is to run the server binary along-side the user's application, 
and use the DataSpaces calls provided by the client library to store and access data from the server. 
It is also possible to run the server in a subset of application proccesses, if it is not desired to run 
the server as an independent binary.

DataSpaces provides a full set of bindings for C/C++, and a subset of the API for fortran and python.
This makes it possible to share data between applications written in different programming languages via the common put/get abstraction.

Building a C/C++ program with DataSpaces
----------------------------------------

Flags necessary for compiling a program that uses DataSpaces can be found from the pkg-config file installed by DataSpaces in ``<INSTALL_ROOT>/lib/pkgconfig``.
If installing using spack, the appropriate directory will be added to ``PGK_CONFIG_PATH`` when the dataspaces module is loaded. 
``pkg-config`` can provide useful information that depends on which flag is provided:

    
Provides compilation flags for building a program that uses the dataspaces API:

.. code-block:: console
    
    pkg-config --cflags dspaces

Provides linking flags for building a program that uses the dataspaces API:

.. code-block:: console
    
    pkg-config --libs dspaces

Provides the path to the dspaces_server binary:

.. code-block:: console
    
    pkg-config --variable=exec_prefix dspaces

Alternatively, dataspaces installs a CMake targets file that makes it easy to include dspaces in a CMake project. 
If dataspaces was installed with Spack, ``CMAKE_PREFIX_PATH`` will be updated when the dataspaces package is loaded.
Recent versions of cmake will also be able to find dspaces if ``<INSTALL_ROOT>/bin`` is in the users ``PATH`` environment variable. 

To include dspaces in a CMake project, simply add ``find_package(dspaces)`` to the project's CMakeLists.txt file and include ``dspaces::dspaces`` 
in the target_link_libraries for whatever target is using dspaces.

Building a Fortran program with DataSpaces
------------------------------------------

Flags for Fortran compilation cannot be obtained through pkg-config. However, a CMake project can be configured to automatically configure 
compilation for dataspaces with Fortran. To do this, add ``find_package(dspaces)`` to the project's CMakeLists.txt file and include ``dspaces::fortran``
in the target_link_libraries for whatever target is using dspaces.

Using DataSpaces with Python
----------------------------

In order to use the DataSpaces pythong bindings, ``<INSTALL_ROOT>/lib/<PYTHONVER>/dist-packages`` must be added to ``PYTHONPATH``. 
Spack will do this automatically when the dataspaces package is loaded. To use the Python bindings, import the ``dspaces`` module.`
