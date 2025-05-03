#include "stdio.h"
#include "file_storage/file.h"

#ifdef DSPACES_HAVE_HDF5
#include "file_storage/file_hdf5.h"
#endif

#ifdef DSPACES_HAVE_NetCDF
// TODO: add NetCDF support
#endif

int file_write_od(struct swap_config* swap_conf, struct obj_data *od)
{
    switch (swap_conf->file_backend)
    {
#ifdef DSPACES_HAVE_HDF5
    case DS_FILE_HDF5:
        return hdf5_write_od(swap_conf, od);
#endif

#ifdef DSPACES_HAVE_NetCDF
    case DS_FILE_NetCDF:
        // TODO: add NetCDF support
        break;
#endif
    
    default:
        fprintf(stderr, "This should not happen... ftype must be supported file backends.\n");
        break;
    }
}
int file_read_od(struct swap_config* swap_conf, struct obj_data *od) {
    switch (swap_conf->file_backend)
    {
#ifdef DSPACES_HAVE_HDF5
    case DS_FILE_HDF5:
        return hdf5_read_od(swap_conf, od);
#endif

#ifdef DSPACES_HAVE_NetCDF
    case DS_FILE_NetCDF:
        // TODO: add NetCDF support
        break;
#endif

    default:
        fprintf(stderr, "This should not happen... ftype must be supported file backends.\n");
        break;
    }
}