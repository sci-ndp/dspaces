#ifndef __DSPACES_FILE_STORAGE_H__
#define __DSPACES_FILE_STORAGE_H__

#include "ss_data.h"
#include "file_storage/policy.h"

int file_write_od(struct swap_config* swap_conf, struct obj_data *od);
int file_read_od(struct swap_config* swap_conf, struct obj_data *od);

#endif // __DSPACES_FILE_STORAGE_H__