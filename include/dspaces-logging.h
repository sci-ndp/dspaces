#ifndef _DSPACES_LOGGING_H
#define _DSPACES_LOGGING_H

#include <abt.h>
#include <inttypes.h>

#define TRACE_OUT                                                              \
    do {                                                                       \
        if(dspaces_trace_enabled()) {                                          \
            ABT_unit_id tid = 0;                                               \
            if(ABT_initialized())                                              \
                ABT_thread_self_id(&tid);                                      \
            fprintf(stderr, "Rank: %i TID: %" PRIu64 " %s:%i (%s): trace\n",   \
                    dspaces_logging_rank(), tid, __FILE__, __LINE__,           \
                    __func__);                                                 \
        }                                                                      \
    } while(0);

#define DEBUG_OUT(dstr, ...)                                                   \
    do {                                                                       \
        if(dspaces_debug_enabled()) {                                          \
            ABT_unit_id tid = 0;                                               \
            if(ABT_initialized())                                              \
                ABT_thread_self_id(&tid);                                      \
            fprintf(stderr, "Rank: %i TID: %" PRIu64 " %s:%i (%s): " dstr,     \
                    dspaces_logging_rank(), tid, __FILE__, __LINE__, __func__, \
                    ##__VA_ARGS__);                                            \
        }                                                                      \
    } while(0);

int dspaecs_trace_enabled();
int dspaces_debug_enabled();
int dspaces_logging_rank();
int dspaces_init_logging(int rank);

#endif // _DSPACES_LOGGING_H
