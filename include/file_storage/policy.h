#ifndef __DSPACES_FILE_STORAGE_POLICY_H__
#define __DSPACES_FILE_STORAGE_POLICY_H__

#include "stdint.h"


#include "ss_data.h"

/* Server swap space configuration parameters */
typedef enum mem_value_type {DS_MEM_BYTES, DS_MEM_PERCENT} ds_mem_val_t;

typedef enum file_type {
#ifdef DSPACES_HAVE_HDF5
    DS_FILE_HDF5, 
#endif
#ifdef DSPACES_HAVE_NetCDF
    DS_FILE_NetCDF
#endif
} ds_file_t;

struct swap_config
{
    char *file_dir;
    ds_mem_val_t mem_quota_type;
    ds_file_t file_backend;
    char *policy;
    float disk_quota_MB;
    union {
        float MB;
        float percent;
    } mem_quota;
};

void free_ls_od_list(struct list_head* ls_od_list);

void memory_quota_parser(char* str, struct swap_config* swap);
void disk_quota_parser(char* str, struct swap_config* swap);

int policy_str_check(const char*str);

int need_swap_out(struct swap_config *swap, uint64_t size_MB);

struct obj_data* which_swap_out(struct swap_config* swap, struct list_head* ls_od_list);

#endif // __DSPACES_FILE_STORAGE_POLICY_H__