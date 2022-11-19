#!/bin/bash -x

branch=$1
src_dir=$2
build_dir=$3
install_dir=$4
compiler=$5
fcompiler=$6

git checkout ${branch}
cd ${build_dir}
cmake ${src_dir} -DCMAKE_INSTALL_PREFIX=${install_dir} -DENABLE_TESTS=True -DCMAKE_C_COMPILER=${compiler} ${fcompiler}
make
make install

