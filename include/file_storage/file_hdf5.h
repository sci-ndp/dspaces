#ifndef __DSPACES_FILE_STORAGE_HDF5_H__
#define __DSPACES_FILE_STORAGE_HDF5_H__

#include "ss_data.h"
#include "file_storage/policy.h"

int hdf5_write_od(struct swap_config* swap_conf, struct obj_data *od);
int hdf5_read_od(struct swap_config* swap_conf, struct obj_data *od);

#endif // __DSPACES_FILE_STORAGE_HDF5_H__