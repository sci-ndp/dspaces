#include "dspaces-logging.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_int f_dsp_debug = 0;
static atomic_int f_dsp_trace = 0;
static atomic_int dsp_log_rank = -1;

int dspaces_debug_enabled() { return (f_dsp_debug); }

int dspaces_trace_enabled() { return (f_dsp_trace); }

int dspaces_logging_rank() { return (dsp_log_rank); }

int dspaces_init_logging(int rank)
{
    const char *envdebug = getenv("DSPACES_DEBUG");
    const char *envtrace = getenv("DSPACES_TRACE");
    int err;

    dsp_log_rank = rank;

    if(envdebug) {
        f_dsp_debug = 1;
    } else {
        f_dsp_debug = 0;
    }

    if(envtrace) {
        f_dsp_trace = 1;
    } else {
        f_dsp_trace = 0;
    }

    DEBUG_OUT("initialized logging.\n");

    return (0);
err_out:
    return (err);
}
