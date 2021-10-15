.. DataSpaces documentation master file, created by
   sphinx-quickstart on Tue Sep 28 14:04:59 2021.

Welcome to DataSpaces
=====================

DataSpaces is a communication library aimed at supporting interactions between large-scale scientific simulation, analysis, and visualization programs. 
DataSpaces enables programs to write to and read from shared N-dimensional arrays without centralized query processing or indexing using low-latency RDMA transfers. 
The result is highly-scalable data access between components of an HPC workflow. 
DataSpaces can be used to tranfer data in *in-situ* workflows, such as coupled simulations and in-situ analysis workflows, 
moving data through shared memory and RDMA tranfers, rather than using the file system.
Like a shared file system, DataSpaces allow data readers to be decoupled from writers in both space in time.
In other words, no sychronization of writers and readers is required, and readers may access data written by any process.


Contents
========

.. toctree::
   
   installation
   usage
   running
   API
   examples

Indices and tables
==================

* :ref:`genindex`
* :ref:`modindex`
* :ref:`search`
