#ifndef __DS_UTIL_H_
#define __DS_UTIL_H_

#include <stdlib.h>
#include <stdint.h>

size_t str_len(const char *str);
char *str_append_const(char *, const char *);
char *str_append(char *, char *);

/*******************************************************
   Processing parameter lists
**********************************************************/
/*
   Process a ;-separated and possibly multi-line text and
   create a list of name=value pairs from each
   item which has a "name=value" pattern. Whitespaces are removed.
   Input is not modified. Space is allocated;
   Also, simple "name" or "name=" patterns are processed and
   returned with value=NULL.
*/
struct name_value_pair {
    char *name;
    char *value;
    struct name_value_pair *next;
};

struct name_value_pair *text_to_nv_pairs(const char *text);
void free_nv_pairs(struct name_value_pair *pairs);
char *alloc_sprintf(const char *fmt_str, ...);

/*******************************************************
   Memory Info
**********************************************************/

#define MEM_PATH_LEN 256
typedef struct {
    // Values from /proc/meminfo, in KiB or converted to MiB.
    uint64_t MemTotalKiB;
    uint64_t MemTotalMiB;
    uint64_t MemAvailableMiB; // -1 means no data available
    uint64_t SwapTotalMiB;
    uint64_t SwapTotalKiB;
    uint64_t SwapFreeMiB;
    // Calculated percentages
    double MemAvailablePercent; // percent of total memory that is available
    double SwapFreePercent; // percent of total swap that is free
} meminfo_t;

typedef struct procinfo {
    int pid;
    int pidfd;
    int uid;
    int badness;
    int oom_score_adj;
    long long VmRSSkiB;
    char name[MEM_PATH_LEN];
} procinfo_t;

meminfo_t parse_meminfo();

/*******************************************************
   Directory Opreations
**********************************************************/
int check_dir_exist(const char* dir_path);
int check_dir_write_permission(const char* dir_path);
void mkdir_all_owner_permission(const char* dir_path);
int remove_dir_rf(const char *dir_path);

#endif
