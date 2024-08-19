/*
 * Copyright (c) 2020, Rutgers Discovery Informatics Institute, Rutgers
 * University
 *
 * See COPYRIGHT in top-level directory.
 */

#ifndef __DSPACES_COMMON_H
#define __DSPACES_COMMON_H

#include <stdio.h>
#include <stdlib.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define dspaces_SUCCESS 0          /* Success */
#define dspaces_ERR_ALLOCATION -1  /* Error allocating something */
#define dspaces_ERR_INVALID_ARG -2 /* An argument is invalid */
#define dspaces_ERR_MERCURY                                                    \
    -3                     /* An error happened calling a Mercury function */
#define dspaces_ERR_PUT -4 /* Could not put into the server */
#define dspaces_ERR_SIZE                                                       \
    -5 /* Client did not allocate enough for the requested data */
#define dspaces_ERR_ARGOBOTS -6    /* Argobots related error */
#define dspaces_ERR_UNKNOWN_PR -7  /* Could not find server */
#define dspaces_ERR_UNKNOWN_OBJ -8 /* Could not find the object*/
#define dspaces_ERR_END -9         /* End of range for valid error codes */

#define DS_MOD_ENOMOD -1
#define DS_MOD_ENODEF -2
#define DS_MOD_EFAULT -3
#define DS_MOD_ENOSYS -4
#define DS_MOD_ENOSUPPORT -5
#define DS_MOD_ECLIENT -6

#define DS_OBJ_RESIZE 0x02

/**
 * Wrapping your function call with ignore_result makes it more clear to
 * readers, compilers and linters that you are, in fact, ignoring the
 * function's return value on purpose.
 */
static inline void ignore_result(int unused_result) { (void)unused_result; }

#define DSP_FLOAT -1
#define DSP_INT -2
#define DSP_LONG -3
#define DSP_DOUBLE -4
#define DSP_BOOL -5
#define DSP_CHAR -6
#define DSP_UINT -7
#define DSP_ULONG -8
#define DSP_BYTE -9
#define DSP_UINT8 -10
#define DSP_UINT16 -11
#define DSP_UINT32 -12
#define DSP_UINT64 -13
#define DSP_INT8 -14
#define DSP_INT16 -15
#define DSP_INT32 -16
#define DSP_INT64 -17

static size_t type_to_size_map[] = {0,
                                    sizeof(float),
                                    sizeof(int),
                                    sizeof(long),
                                    sizeof(double),
                                    1,
                                    sizeof(char),
                                    sizeof(unsigned),
                                    sizeof(unsigned long),
                                    1,
                                    1,
                                    2,
                                    4,
                                    8,
                                    1,
                                    2,
                                    4,
                                    8};

static int type_to_size(int type_id)
{
    if(type_id >= 0) {
        fprintf(stderr, "WARNING: type ids should be negative.\n");
        return (-1);
    }
    return (type_to_size_map[-type_id]);
}

#if defined(__cplusplus)
}
#endif

#endif
