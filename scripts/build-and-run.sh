#!/bin/bash -x

branch=$1
src_dir=$2
build_dir=$3
install_dir=$4
compiler=$5
fcompiler=$6
ofi_str=$7

cd ${src_dir}
git checkout ${branch}
cd ${build_dir}
cmake ${src_dir} -DCMAKE_INSTALL_PREFIX=${install_dir} -DENABLE_TESTS=True -DCMAKE_C_COMPILER=${compiler} -DCMAKE_Fortran_COMPILER=${fcompiler} -DCMAKE_BUILD_TYPE=Debug
make
make install

fi_info

cd /
if [ -f dspaces.toml ] ; then
    ${install_dir}/bin/dspaces_server ${ofi_str} dspaces.toml &
else
    ${install_dir}/bin/dspaces_server ${ofi_str} &
fi
while [ ! -f conf.ds ] ; do
    sleep 1
done
sleep 2
cat conf.ds

wait
