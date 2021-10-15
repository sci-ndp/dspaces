DataSpaces Examples
===================

The DataSpaces repo includes a set of examples. When built using cmake, 
DataSpaces configures a Makefile and copies these examples in to the `examples/` 
directory in the build environment. This process can easily be manually duplicated
by moving opts.mk.in to opts.mk and filling in a valid value for `CC` (usually `mpicc`).

Each example contains everything needed to build a run a program using dataspaces. 
