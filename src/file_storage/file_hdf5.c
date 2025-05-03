#include "stdio.h"
#include "stdlib.h"
#include "stdbool.h"
#include "ctype.h"
#include "string.h"

#include "dspaces-common.h"
#include "util.h"
#include "bbox.h"
#include "file_storage/file_hdf5.h"

#include <hdf5.h>

static hid_t dstype_to_h5type(int type_id)
{
    switch (type_id)
    {
    case -1:  // DSP_FLOAT
        return H5T_NATIVE_FLOAT;
    case -2:  // DSP_INT
        return H5T_NATIVE_INT;
    case -3:  // DSP_LONG
        return H5T_NATIVE_LONG;
    case -4:  // DSP_DOUBLE
        return H5T_NATIVE_DOUBLE;
    case -5:  // DSP_BOOL
        return H5T_NATIVE_HBOOL;
    case -6:  // DSP_CHAR
        return H5T_NATIVE_CHAR;
    case -7:  // DSP_UINT
        return H5T_NATIVE_UINT;
    case -8:  // DSP_ULONG
        return H5T_NATIVE_ULONG;
    case -9:  // DSP_BYTE
        return H5T_NATIVE_B8;
    case -10: // DSP_UINT8
        return H5T_NATIVE_B8;
    case -11: // DSP_UINT16
        return H5T_NATIVE_B16;
    case -12: // DSP_UINT32
        return H5T_NATIVE_B32;
    case -13: // DSP_UINT64
        return H5T_NATIVE_B64;
    case -14: // DSP_INT8
        return H5T_NATIVE_B8;
    case -15: // DSP_INT16
        return H5T_NATIVE_B16;
    case -16: // DSP_INT32
        return H5T_NATIVE_B32;
    case -17: // DSP_INT64
        return H5T_NATIVE_B64;
    default:
        return H5T_NATIVE_OPAQUE;
    }
}

static char *hdf5_bound_sprint(const uint64_t* bound, int num_dims)
{
    char *str;
    int i;
    int size = 2;

    for(i = 0; i < num_dims; i++) {
        size += snprintf(NULL, 0, "%" PRIu64, bound[i]);
        if(i > 0) {
            size += i ? 2 : 0; // account for ", "
        }
    }
    str = malloc(sizeof(*str) * (size + 1)); // add null terminator
    strcpy(str, "{");
    for(i = 0; i < num_dims; i++) {
        char *tmp = alloc_sprintf(i ? ", %" PRIu64 : "%" PRIu64, bound[i]);
        str = str_append(str, tmp);
    }
    str = str_append_const(str, "}");
    return str;
}

static char* hdf5_dataset_name_sprint(const struct bbox* bbox)
{
    char *str = hdf5_bound_sprint(bbox->lb.c, bbox->num_dims);
    str = str_append_const(str, "-");
    str = str_append(str, hdf5_bound_sprint(bbox->ub.c, bbox->num_dims));
    return str;
}

static void hdf5_dataset_name_parser(char* dname, struct bbox* bbox)
{
    char *p = dname;
    int i = 0;
    while (*p) {
        if(isdigit(*p)) {
            if(i < bbox->num_dims)
                bbox->lb.c[i] = (uint64_t) strtoull(p, &p, 10);
            else
                bbox->ub.c[i-bbox->num_dims] = (uint64_t) strtoull(p, &p, 10);
            i++;
        } else {
            p++;
        }
    }
    
}

/* Search the first dataset that can include the queried bbox from a HDF5 file.
 * If found, the dataset name is returned.
 * Otherwise, return NULL.
 * */
static char* hdf5_search_include_dataset(const char* file_name, hid_t file_id, struct bbox* qbbox)
{
    herr_t status;
    H5G_info_t ginfo;
    char* dname;
    struct bbox sbbox;
    ssize_t dname_size, errh;

    status = H5Gget_info(file_id, &ginfo);
    if(status == H5I_INVALID_HID) {
        fprintf(stderr,"HDF5 failed to get root group info in file: %s.\n", file_name);
        return NULL;
    }

    if(ginfo.nlinks == 0) return NULL;

    sbbox.num_dims = qbbox->num_dims;
    memset(sbbox.lb.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    memset(sbbox.ub.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);

    for(int i=0; i<ginfo.nlinks; i++) {
        // First get the dataset name size, +1 is very important!
        dname_size = H5Lget_name_by_idx(file_id, ".", H5_INDEX_NAME,
                                            H5_ITER_INC, i, NULL, 0, H5P_DEFAULT) + 1;
        if(dname_size < 0) {
            fprintf(stderr,"HDF5 failed to get dataset name size in file: %s.\n", file_name);
            return NULL;
        }

        dname = (char*) malloc(dname_size*sizeof(char));
        // Get the dataset name
        errh = H5Lget_name_by_idx(file_id, ".", H5_INDEX_NAME, H5_ITER_INC,
                                            i, dname, dname_size, H5P_DEFAULT);
        if(errh < 0) {
            fprintf(stderr,"HDF5 failed to get dataset name in file: %s.\n", file_name);
            return NULL;
        }

        // Parse the dataset name
        hdf5_dataset_name_parser(dname, &sbbox);

        // Check if the qbbox is included by sbbox
        if(bbox_can_include(qbbox, &sbbox) == 2) return dname;
    }

    return NULL;
}

/* Search the first dataset that can merge with queried bbox from a HDF5 file.
 * If found, the dataset name is returned through rdname.
 * Return value: -3 - Not Found
 *               -2 - Include. queried bbox is bigger
 *               -1 - Include, searched bbox is bigger
 *               Natural Numbers - Union, the union dimension                                                     
 * */
static int hdf5_search_mergeable_dataset(const char* file_name, hid_t file_id,
                                        struct bbox* qbbox, char* rdname)
{
    herr_t status;
    H5G_info_t ginfo;
    char* dname;
    struct bbox sbbox;
    ssize_t dname_size, errh;
    int include;
    int union_dim;

    status = H5Gget_info(file_id, &ginfo);
    if(status == H5I_INVALID_HID) {
        fprintf(stderr,"HDF5 failed to get root group info in file: %s.\n", file_name);
        return -3;
    }

    if(ginfo.nlinks == 0) return -3;

    sbbox.num_dims = qbbox->num_dims;
    memset(sbbox.lb.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    memset(sbbox.ub.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);

    for(int i=0; i<ginfo.nlinks; i++) {
        // First get the dataset name size
        dname_size = H5Lget_name_by_idx(file_id, ".", H5_INDEX_NAME,
                                            H5_ITER_INC, i, NULL, 0, H5P_DEFAULT);
        if(dname_size < 0) {
            fprintf(stderr,"HDF5 failed to get dataset name size in file: %s.\n", file_name);
            return -3;
        }

        dname = (char*) malloc(dname_size*sizeof(char));
        // Get the dataset name
        errh = H5Lget_name_by_idx(file_id, ".", H5_INDEX_NAME, H5_ITER_INC,
                                            i, dname, dname_size, H5P_DEFAULT);
        if(errh < 0) {
            fprintf(stderr,"HDF5 failed to get dataset name in file: %s.\n", file_name);
            return -3;
        }

        // Parse the dataset name
        hdf5_dataset_name_parser(dname, &sbbox);

        // Check if qbbox & sbbox are the same
        if(bbox_equals(qbbox, &sbbox)) continue;

        /* Check if the memory object & the file object can merge.
         * There are 2 cases in which 2 bbox can be merged:
         * 1. bbox0 includes bbox1 or bbox1 includes bbox0
         * 2. bbox0 & bbox1 can union. */
        include = bbox_can_include(qbbox, &sbbox);
        if(include == 1) {
            rdname = dname;
            return -2;
        } else if(include == 2) {
            rdname = dname;
            return -1;
        }

        union_dim = bbox_can_union(qbbox, &sbbox);
        if(union_dim != -1) return union_dim;

    }

    rdname = NULL;
    return -3;
}


/* Write a data object to a HDF5 dataset.
 * The dataset name is the bbox formatted as
 * "{lb[0],lb[1], ...}-{ub[0], ub[1], ...}"
 * */
static int hdf5_write_dataset(const char* file_name, hid_t file_id, struct bbox *bboxw,
                                int type_id, void* data)
{
    hid_t dataspace_id, dataset_id;
    hsize_t *dims;
    htri_t exist;
    herr_t status;
    char* dataset_name;

    /* Prepare meta data in HDF5 write & check dataset existence. */
    dataset_name = hdf5_dataset_name_sprint(bboxw);
    exist = H5Lexists(file_id, dataset_name, H5P_DEFAULT);
    if(exist < 0) {
        fprintf(stderr,"HDF5 failed to check if dataset: %s exists in file: %s.\n",
                dataset_name, file_name);
        free(dataset_name);
        return dspaces_ERR_HDF5;
    } else if(exist > 0) {
        free(dataset_name);
        return dspaces_SUCCESS;
    }

    dims = (hsize_t*) malloc(bboxw->num_dims*sizeof(hsize_t));
    for(int i=0; i<bboxw->num_dims; i++) {
        dims[i] = bboxw->ub.c[i] - bboxw->lb.c[i] + 1;
    }

    /* Prepare HDF5 data space. */
    dataspace_id = H5Screate_simple(bboxw->num_dims, dims, NULL);
    if(dataspace_id == H5I_INVALID_HID) {
        fprintf(stderr,"HDF5 failed to create N-dimensional dataspace in file: %s.\n",
                file_name);
        free(dataset_name);
        free(dims);
        return dspaces_ERR_HDF5;
    }

    /* Prepare HDF5 dataset. */
    dataset_id = H5Dcreate2(file_id, dataset_name, dstype_to_h5type(type_id),
                            dataspace_id, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if(dataset_id == H5I_INVALID_HID) {
        fprintf(stderr,"HDF5 failed to create dataset: %s in file: %s.\n",
                dataset_name, file_name);
        free(dataset_name);
        free(dims);
        return dspaces_ERR_HDF5;
    }

    /* Write data to HDF5 dataset. */
    status = H5Dwrite(dataset_id, dstype_to_h5type(type_id), 
                        H5S_ALL, H5S_ALL, H5P_DEFAULT, data);
    if(status < 0) {
        fprintf(stderr, "HDF5 failed to write data to dataset: %s in file: %s.\n",
                dataset_name, file_name);
        free(dataset_name);
        free(dims);
        return dspaces_ERR_HDF5;
    }

    status = H5Dclose(dataset_id);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to close the dataset: %s in file: %s.\n", 
                        dataset_name, file_name);
        free(dataset_name);
        free(dims);
        return dspaces_ERR_HDF5;
    }

    status = H5Sclose(dataspace_id);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to close the dataspace of dataset: %s in file: %s.\n", 
                        dataset_name, file_name);
        free(dataset_name);
        free(dims);
        return dspaces_ERR_HDF5;
    }

    free(dataset_name);
    free(dims);

    return dspaces_SUCCESS;
}

static int hdf5_read_full_dataset(const char* file_name, hid_t file_id,
                                    char* dataset_name, void* data)
{
    hid_t dataset_id, dataspace_id, datatype_id;
    hssize_t elem_num;
    herr_t status;
    size_t datatype_size;

    dataset_id = H5Dopen2(file_id, dataset_name, H5P_DEFAULT);
    if(dataset_id == H5I_INVALID_HID) {
        fprintf(stderr, "HDF5 failed to open dataset: %s in file: %s.\n", dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    dataspace_id = H5Dget_space(dataset_id);
    if(dataspace_id == H5I_INVALID_HID) {
        fprintf(stderr, "HDF5 failed to get data space of dataset: %s from file: %s.\n",
                            dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    datatype_id = H5Dget_type(dataset_id);
    if(datatype_id == H5I_INVALID_HID) {
        fprintf(stderr, "HDF5 failed to get data type of dataset: %s from file: %s.\n",
                            dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    datatype_size = H5Tget_size(datatype_id);
    if(datatype_size == 0) {
        fprintf(stderr, "HDF5 failed to get data type size of dataset: %s from file: %s.\n",
                            dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    elem_num = H5Sget_simple_extent_npoints(dataspace_id);
    if(elem_num < 0) {
        fprintf(stderr, "HDF5 failed to get elem_num of dataset: %s from file: %s.\n",
                            dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    data = (void*) malloc(elem_num*datatype_size);

    status = H5Dread(dataset_id, datatype_id, H5S_ALL, H5S_ALL, H5P_DEFAULT, data);
    if(status < 0) {
        fprintf(stderr, "HDF5 failed to read dataset: %s from file: %s.\n",
                            dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    status = H5Sclose(dataspace_id);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to close the dataspace of dataset: %s in file: %s.\n", 
                        dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    status = H5Dclose(dataset_id);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to close the dataset: %s in file: %s.\n", 
                        dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    return dspaces_SUCCESS;
}

static int hdf5_read_dataset(const char* file_name, hid_t file_id, char* dataset_name,
                                hsize_t *offset, hsize_t *count, void* data)
{
    hid_t dataset_id, dataspace_id, datatype_id, memspace_id;
    int ndims;
    size_t elem_num = 1, datatype_size;
    hsize_t *stride, *block;
    herr_t status;

    dataset_id = H5Dopen2(file_id, dataset_name, H5P_DEFAULT);
    if(dataset_id == H5I_INVALID_HID) {
        fprintf(stderr, "HDF5 failed to open dataset: %s in file: %s.\n", dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    dataspace_id = H5Dget_space(dataset_id);
    if(dataspace_id == H5I_INVALID_HID) {
        fprintf(stderr, "HDF5 failed to get data space of dataset: %s from file: %s.\n",
                            dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    datatype_id = H5Dget_type(dataset_id);
    if(datatype_id == H5I_INVALID_HID) {
        fprintf(stderr, "HDF5 failed to get data type of dataset: %s from file: %s.\n",
                            dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    datatype_size = H5Tget_size(datatype_id);
    if(datatype_size == 0) {
        fprintf(stderr, "HDF5 failed to get data type size of dataset: %s from file: %s.\n",
                            dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    ndims = H5Sget_simple_extent_ndims(dataspace_id);
    if(ndims < 0) {
        fprintf(stderr, "HDF5 failed to get number of dimensions of dataset: %s from file: %s.\n",
                            dataset_name, file_name);
        return dspaces_ERR_HDF5;
    }

    stride = (hsize_t*) malloc(ndims*sizeof(hsize_t));
    block = (hsize_t*) malloc(ndims*sizeof(hsize_t));

    for(int i=0; i<ndims; i++) {
        stride[i] = 1;
        block[i] = 1;
        elem_num *= count[i];
    }

    status = H5Sselect_hyperslab(dataspace_id, H5S_SELECT_SET, offset, stride, count, block);
    if(status < 0) {
        fprintf(stderr, "HDF5 failed to select hyperslab from dataset %s in file: %s.\n",
                            dataset_name, file_name);
        free(block);
        free(stride);
        return dspaces_ERR_HDF5;
    }

    memspace_id = H5Screate_simple(ndims, count, NULL);
    if(memspace_id == H5I_INVALID_HID) {
        fprintf(stderr, "HDF5 failed to create the memory space for data reading"
                        "from dataset %s in file: %s.\n", dataset_name, file_name);
        free(block);
        free(stride);
        return dspaces_ERR_HDF5;
    }
    
    if(!data) {
        data = (void*) malloc(elem_num*datatype_size);
    }

    status = H5Dread(dataset_id, datatype_id, memspace_id, dataspace_id, H5P_DEFAULT, data);
    if(status < 0) {
        fprintf(stderr, "HDF5 failed to partially read dataset: %s from file: %s.\n",
                            dataset_name, file_name);
        free(data);
        free(block);
        free(stride);                
        return dspaces_ERR_HDF5;
    }

    status = H5Sclose(memspace_id);
    if(status < 0) {
        fprintf(stderr, "HDF5 failed to close the memory space for data reading "
                        "from dataset: %s in file: %s.\n", dataset_name, file_name);
        free(data);
        free(block);
        free(stride); 
        return dspaces_ERR_HDF5;
    }

    status = H5Sclose(dataspace_id);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to close the dataspace of dataset: %s in file: %s.\n", 
                        dataset_name, file_name);
        free(data);
        free(block);
        free(stride); 
        return dspaces_ERR_HDF5;
    }

    status = H5Dclose(dataset_id);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to close the dataset: %s in file: %s.\n", 
                        dataset_name, file_name);
        free(data);
        free(block);
        free(stride); 
        return dspaces_ERR_HDF5;
    }

    free(block);
    free(stride);
    return dspaces_SUCCESS;
}

static int hdf5_write_scalar_attr(const char* file_name, hid_t file_id,
                                     const char *attr_name, hid_t attr_dtype, const void* value)
{
    hid_t dataspace_id, attr_id;
    herr_t status;

    dataspace_id = H5Screate(H5S_SCALAR);
    if(dataspace_id == H5I_INVALID_HID) {
        fprintf(stderr,"HDF5 failed to create a scalar dataspace for attribute: %s in file: %s.\n",
                        attr_name, file_name);
        return dspaces_ERR_HDF5;
    }

    attr_id = H5Acreate2(file_id, attr_name, attr_dtype, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
    if(attr_id < 0) {
        fprintf(stderr,"HDF5 failed to create attribute: %s in file: %s.\n", attr_name, file_name);
        return dspaces_ERR_HDF5;
    }

    status = H5Awrite(attr_id, attr_dtype, value);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to write attribute: %s to file: %s.\n", attr_name, file_name);
        return dspaces_ERR_HDF5;
    }

    status = H5Aclose(attr_id);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to close attribute: %s in file: %s.\n", attr_name, file_name);
        return dspaces_ERR_HDF5;
    }

    status = H5Sclose(dataspace_id);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to close the scalar dataspace for attribute: %s in file: %s.\n",
                        attr_name, file_name);
        return dspaces_ERR_HDF5;
    }

    return dspaces_SUCCESS;
}

/* Write a dspaces obj_data to a HDF5 file.
 * The HDF5 file name is "{odsc_name}.ver%u.h5"
 * The HDF5 dataset name is the bbox formatted as
 * "{lb[0],lb[1], ...}-{ub[0], ub[1], ...}".
 * The written obj_data remains in dspaces memory after this write.
 * */
int hdf5_write_od(struct swap_config* swap_conf, struct obj_data *od)
{
    /* We will write each version of a variable in a separate file 
       in order to avoid the slow down caused by waiting the HDF5
       file lock when writing version i of "foo" and writing 
       version j of "foo" happens together */
    int ret, search;
    char file_name[256], *rdname, *qdname;
    hid_t file_id;
    herr_t status;
    bool mergeable;
    struct bbox *qbbox;
    void *wdata, *fdata;
    obj_descriptor search_odsc, union_odsc;
    struct obj_data *qod, *search_od, *union_od;

    /* Check if we can open the file path, if not, create one at the default path */
    if(!check_dir_exist(swap_conf->file_dir)) {
        fprintf(stderr, "WARNING: Swap directory: %s does not exist. "
                        "Creating the directory.\n", swap_conf->file_dir);
        mkdir_all_owner_permission(swap_conf->file_dir);
    } else if(!check_dir_write_permission(swap_conf->file_dir)) {
        fprintf(stderr, "WARNING: Failed to write files into swap directory: %s "
                        "Use ./dspaces_swap as the default swap directory instead.\n",
                        swap_conf->file_dir);
        free(swap_conf->file_dir);
        swap_conf->file_dir = strdup("./dspaces_swap/");
        mkdir_all_owner_permission(swap_conf->file_dir);
    }

    /* Concatenate file name */
    sprintf(file_name, "%s/%s.ver%u.h5", swap_conf->file_dir, od->obj_desc.name, od->obj_desc.version);

    /* Mute HDF5 error message for potential concurrent file createion & access. */
    // TODO: Unmute it later, but the HDF5 function has a bug in latest release.
    status = H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to mute error message for "
                       "potential concurrent file createion & access.\n");
        return dspaces_ERR_HDF5;
    }

    /* Try to create the file and write ndims as a file attribute.
     * Fail if the file exists. */
    file_id = H5Fcreate(file_name, H5F_ACC_EXCL, H5P_DEFAULT, H5P_DEFAULT);
    if(file_id != H5I_INVALID_HID) {
        ret = hdf5_write_scalar_attr(file_name, file_id, "ndims",
                                        H5T_NATIVE_INT, &(od->obj_desc.bb.num_dims));
        if(ret != dspaces_SUCCESS) return ret;
    } else {
        /* Try to catch the file lock and open the file. */
        do {
            file_id = H5Fopen(file_name, H5F_ACC_RDWR, H5P_DEFAULT);
        } while (file_id == H5I_INVALID_HID);
    }

    /* Check if the new coming obj can be merged.
     * We have an exclusive file lock in HDF5, so this 
     * merge check will be able to find all meragble objects. */
    
    search_odsc = od->obj_desc;
    union_odsc = od->obj_desc;

    mergeable = false;
    qod = od;
    qbbox = &(od->obj_desc.bb);
    wdata = od->data;

    while((search = hdf5_search_mergeable_dataset(file_name, file_id, qbbox, rdname)) != -3)
    {
        if(!mergeable) mergeable = true;

        if(search == -2) {
            /* qbbox includes some bbox in the file. qbbox remains the same one. */
            // write the memory to a new dataset
            ret = hdf5_write_dataset(file_name, file_id, qbbox, qod->obj_desc.type, wdata);
            if(ret != dspaces_SUCCESS) return ret;

            // delete the found dataset
            status = H5Ldelete(file_id, rdname, H5P_DEFAULT);
            if(status < 0) {
                fprintf(stderr, "HDF5 failed to delete dataset: %s in file: %s.\n",
                        rdname, file_name);
                return dspaces_ERR_HDF5;
            }
            free(rdname);
        } else if(search == -1) {
            /* qbbox is included by some bbox in the file. Do nothing. */
            free(rdname);
            break;
        } else {
            /* qbbox and some bbox searched from the file can union */
            hdf5_dataset_name_parser(rdname, &(search_odsc.bb));
            bbox_union_ondim(qbbox, &(search_odsc.bb), search, &(union_odsc.bb));

            // Read the searched dataset from the file, and do ssd_copy()
            ret = hdf5_read_full_dataset(file_name, file_id, rdname, fdata);
            if(ret != dspaces_SUCCESS) return ret;
            
            search_od = obj_data_alloc_no_data(&search_odsc, fdata);
            union_od = obj_data_alloc(&union_odsc);
            ssd_copy(union_od, search_od);
            ssd_copy(union_od, qod);

            // write the union obj_data to a new dataset
            wdata = union_od->data;
            ret = hdf5_write_dataset(file_name, file_id, &(union_odsc.bb), 
                                        union_od->obj_desc.type, wdata);
            if(ret != dspaces_SUCCESS) return ret;

            /* Free the old memory data buffer and file data buffer.
             * Do not free the origin od for safety.
             * Also, delete the searched dataset from the file
             * and the query dataset if it is stored in the file.
             * Iterate the qbbox to the union bbox. */
            status = H5Ldelete(file_id, rdname, H5P_DEFAULT);
            if(status < 0) {
                fprintf(stderr, "HDF5 failed to delete dataset: %s in file: %s.\n",
                        rdname, file_name);
                return dspaces_ERR_HDF5;
            }
            obj_data_free(search_od);
            free(search_od);
            free(rdname);

            if(qod != od) {
                qdname = hdf5_dataset_name_sprint(&qod->obj_desc.bb);
                status = H5Ldelete(file_id, qdname, H5P_DEFAULT);
                if(status < 0) {
                    fprintf(stderr, "HDF5 failed to delete dataset: %s in file: %s.\n",
                            qdname, file_name);
                    return dspaces_ERR_HDF5;
                }
                obj_data_free(qod);
                free(qod);
                free(qdname);
            }

            qod = union_od;
            qbbox = &(union_od->obj_desc.bb);
        }
    }

    // Final clean up
    if(mergeable && qod!=od) {
        obj_data_free(qod);
        free(qod);
    }

    /* Write a new data obj if not mergeable. */
    if(!mergeable) {
        ret = hdf5_write_dataset(file_name, file_id, &(od->obj_desc.bb), od->obj_desc.type, od->data);
        if(ret != dspaces_SUCCESS) return ret;
    }

    /* Close file */
    status = H5Fclose(file_id);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to close the file: %s.\n", file_name);
        return dspaces_ERR_HDF5;
    }

    return dspaces_SUCCESS;
}

/* Read a dspaces obj_data from a HDF5 file.
 * The HDF5 file name is "{odsc_name}.ver%u.h5"
 * The HDF5 dataset name is the bbox formatted as
 * "{lb[0],lb[1], ...}-{ub[0], ub[1], ...}".
 *  Subset reading from a dataset is supported.
 *  The data read from the HDF5 file is stored in the od->data.
 * */
int hdf5_read_od(struct swap_config* swap_conf, struct obj_data *od)
{
    int ret;
    hid_t file_id;
    hsize_t *offset, *count;
    herr_t status;
    char file_name[256], *dataset_name;
    struct bbox sbbox;

    /* Concatenate file name */
    sprintf(file_name, "%s/%s.ver%u.h5", swap_conf->file_dir, od->obj_desc.name, od->obj_desc.version);

    /* Mute HDF5 error message for potential concurrent file createion & access. */
    // TODO: Unmute it later, but the HDF5 function has a bug in latest release.
    status = H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to mute error message for "
                        "potential concurrent file createion & access.\n");
        return dspaces_ERR_HDF5;
    }

    /* Try to catch the file lock and open the file. */
    do {
        file_id = H5Fopen(file_name, H5F_ACC_RDONLY, H5P_DEFAULT);
    } while (file_id == H5I_INVALID_HID);

    /* The queried bbox might be included by the dataset's bbox */
    dataset_name = hdf5_search_include_dataset(file_name, file_id, &od->obj_desc.bb);
    if(!dataset_name) {
        fprintf(stderr,"HDF5 failed to find the requested data object from file: %s."
                        " This should not happen... \n", file_name);
        return dspaces_ERR_HDF5;
    }

    sbbox.num_dims = od->obj_desc.bb.num_dims;
    memset(sbbox.lb.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    memset(sbbox.ub.c, 0, sizeof(uint64_t) * BBOX_MAX_NDIM);
    hdf5_dataset_name_parser(dataset_name, &sbbox);

    offset = (hsize_t*) malloc(od->obj_desc.bb.num_dims*sizeof(hsize_t));
    count = (hsize_t*) malloc(od->obj_desc.bb.num_dims*sizeof(hsize_t));
    for(int i=0; i<od->obj_desc.bb.num_dims; i++) {
        offset[i] = od->obj_desc.bb.lb.c[i] - sbbox.lb.c[i];
        count[i] = od->obj_desc.bb.ub.c[i] - od->obj_desc.bb.lb.c[i] + 1;
    }
    ret = hdf5_read_dataset(file_name, file_id, dataset_name, offset, count, od->data);

    free(count);
    free(offset);
    free(dataset_name);

    /* Close file */
    status = H5Fclose(file_id);
    if(status < 0) {
        fprintf(stderr,"HDF5 failed to close the file: %s.\n", file_name);
        return dspaces_ERR_HDF5;
    }

    return dspaces_SUCCESS;
}