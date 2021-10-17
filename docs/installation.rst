Installing
==========

The easiest way to install DataSpaces is using `Spack <https://spack.readthedocs.io/en/latest/>`_. Spack is a package manager aimed at HPC and scientific computing.
Using Spack simplifies the installation of DataSpaces and its dependencies.

If you wish to install DataSpaces directly from source, the distribution repo can be found on `GitHub <https://github.com/rdi2dspaces/dspaces>`_.

Installing Spack
----------------

To install Spack, follow the getting started instructions found `here <https://spack.readthedocs.io/en/latest/getting_started.html>`_. 
This will install the package manager, and make a large variety of packages available.

Installing the DataSpaces repository
------------------------------------

The DataSpaces group maintains a repository for the DataSpaces spack package (and any relevant ancillary packages). This can be found `here <https://github.com/rdi2dspaces/dspaces-spack>`_. 
In order to use this package, you will need to first install Spack using the above instructions.
Once you have done this, you can load the DataSpaces package repository by doing the following:

.. code-block:: console

   git clone https://github.com/rdi2dspaces/dspaces-spack.git
   spack repo add dspaces-spack

Installing DataSpaces
---------------------

One the DataSpaces repository has been loaded, the dataspaces package can be installed with:

.. code-block:: console

   spack install dataspaces

This will automatically install allDdataSpaces dependencies and the dataspaces package itself. 
Once the package has been installed the command:

.. code-block:: console

   spack load dataspaces

Configures the environment to use DataSpaces, adding the server binary's directory to ``PATH``, any shared library paths to ``LD_LIBRARY_PATH``, etc. 
This simplifies building and running programs that use DataSpaces.
