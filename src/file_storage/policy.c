#ifdef DSPACES_HAVE_FILE_STORAGE

#include "stdlib.h"
#include "stdio.h"

#include "util.h"
#include "list.h"
#include "ss_data.h"
#include "file_storage/policy.h"

const char* policy_options[] = {
    "Default",
    "Custom",
    "FIFO",
    "LIFO",
    "LRU"
};

void free_ls_od_list(struct list_head* ls_od_list)
{
    if(!ls_od_list)
        return;
    int cnt = 0;
    struct obj_data *od, *t;
    list_for_each_entry_safe(od, t, ls_od_list, struct obj_data, flat_list_entry.entry)
    {
        list_del(&od->flat_list_entry.entry);
        cnt++;
    }

#ifdef DEBUG
    fprintf(stderr, "%s(): number of object data record is %d\n",
            __func__, cnt);
#endif
}

int policy_str_check(const char* str)
{
    for(int i=0; i<5; i++) {
        if(strcmp(str, policy_options[i]) == 0) return 1;
    }
    return 0;
}

void memory_quota_parser(char* str, struct swap_config* swap)
{
    char *pch;
    int len, itmp;
    float ftmp;
    uint64_t lltmp;

    meminfo_t meminfo;

    pch = strtok(str, "%");
    if(pch !=NULL) {
        /* Check if the input string is a percentage number */
        swap->mem_quota_type = DS_MEM_PERCENT;
        swap->mem_quota.percent = strtof(pch, NULL) / 100.0;
    } else if (sscanf(str, "%f %n", &ftmp, &len) && !str[len]) {
        /* Check if the input string is a float number */
        swap->mem_quota_type = DS_MEM_PERCENT;
        swap->mem_quota.percent = ftmp;
    } else if((pch=strtok(str, "kKmMgGtT")) != NULL) {
        /* Check if the input string is a real value with quantity */
        swap->mem_quota_type = DS_MEM_BYTES;
        ftmp = strtof(pch, &pch);
        // Check if the last char is 'B'(Byte) or 'b'(bit)
        if(*(pch+1) == 'b')
            ftmp = ftmp / 8;
        // Check the quantity.
        if(*pch == 'k' || *pch == 'K') {
            ftmp = ftmp / 2e10;
        } else if (*pch == 'm' || *pch == 'M') {
            ;
        } else if (*pch == 'g' || *pch == 'G') {
            ftmp = ftmp * 2e10;
        } else if (*pch == 't' || *pch == 'T') {
            ftmp = ftmp * 2e20;
        }
        // If the quota is higher than the full memory capacity,
        // Just set the quota to 100%
        meminfo = parse_meminfo();
        if(ftmp > ((float) meminfo.MemTotalMiB)) {
            swap->mem_quota_type = DS_MEM_PERCENT;
            swap->mem_quota.percent = 1.0;
        }
        swap->mem_quota.MB = ftmp;
    }
}

void disk_quota_parser(char* str, struct swap_config* swap)
{
    char *pch;
    float ftmp;

    pch = strtok(str, "kKmMgGtTpP");
    if(pch == NULL) {
        swap->disk_quota_MB = -1.0;
    } else {
        // Round to an approx int
        ftmp = strtof(pch, &pch);
        // Check if the last char is 'B'(Byte) or 'b'(bit)
        if(*(pch+1) == 'b')
            ftmp = ftmp / 8;
        // Check the quantity.
        if(*pch == 'k' || *pch == 'K') {
            ftmp = ftmp / 2e10;
        } else if (*pch == 'm' || *pch == 'M') {
            ;
        } else if (*pch == 'g' || *pch == 'G') {
            ftmp = ftmp * 2e10;
        } else if (*pch == 't' || *pch == 'T') {
            ftmp = ftmp * 2e20;
        } else if (*pch == 'p' || *pch == 'P') {
            ftmp = ftmp * 2e30;
        }
        swap->disk_quota_MB = ftmp;
    }
}

static int default_when_policy(uint64_t size_MB)
{
    meminfo_t meminfo = parse_meminfo();

    if(meminfo.MemAvailableMiB < size_MB)
        return 1;
    else
        return 0;
}

static int percent_when_policy(float threshold, uint64_t size_MB)
{
    meminfo_t meminfo = parse_meminfo();

    if((meminfo.MemAvailablePercent - (size_MB / meminfo.MemTotalMiB)) < (1 - threshold))
        return 1;
    else
        return 0;

}

static int value_when_policy(float threshold, uint64_t size_MB)
{
    meminfo_t meminfo = parse_meminfo();

    if(meminfo.MemAvailableMiB  < (size_MB + meminfo.MemTotalMiB - threshold))
        return 1;
    else
        return 0;

}

static struct obj_data* fifo_which_policy(struct list_head *ls_od_list) {
    struct obj_data *od;

    if(list_empty(ls_od_list)) {
        return NULL;
    } else {
        od = list_entry(ls_od_list->next, struct obj_data, flat_list_entry.entry);
        return od;
    }
}

static struct obj_data* lifo_which_policy(struct list_head *ls_od_list) {
    struct obj_data *od;

    if(list_empty(ls_od_list)) {
        return NULL;
    } else {
        od = list_entry(ls_od_list->prev, struct obj_data, flat_list_entry.entry);
        return od;
    }
}

static struct obj_data* lru_which_policy(struct list_head *ls_od_list) {
    struct obj_data *od, *min;

    if(list_empty(ls_od_list)) {
        return NULL;
    } else {
        min = list_entry(ls_od_list->next, struct obj_data, flat_list_entry.entry);

        list_for_each_entry(od, ls_od_list, struct obj_data, flat_list_entry.entry) {
            if(od->flat_list_entry.usecnt < min->flat_list_entry.usecnt) {
                min = od;
            }
        }
        return min;
    }
}

int need_swap_out(struct swap_config* swap, uint64_t size_MB)
{
    meminfo_t meminfo = parse_meminfo();

    if(swap->mem_quota_type == DS_MEM_BYTES) {
        // Use memory MB
        return value_when_policy(swap->mem_quota.MB, size_MB);
    } else if(swap->mem_quota_type == DS_MEM_PERCENT) {
        // Use memory percent
        if(abs(1.0 - swap->mem_quota.percent) < 1e-6) {
            // Use full node memory
            return default_when_policy(size_MB);
        } else {
            return percent_when_policy(swap->mem_quota.percent, size_MB);
        }
    } else {
        // Use full node memory
        return default_when_policy(size_MB);
    }
}

struct obj_data* which_swap_out(struct swap_config *swap,
                                struct list_head* ls_od_list)
{
    // TODO: support custom policy
    if((strcmp(swap->policy, "Default") == 0) || (strcmp(swap->policy, "FIFO") == 0)) {
        return fifo_which_policy(ls_od_list);
    } else if(strcmp(swap->policy, "LIFO") == 0) {
        return lifo_which_policy(ls_od_list);
    } else if(strcmp(swap->policy, "LRU") == 0) {
        return lru_which_policy(ls_od_list);
    } else {
        return fifo_which_policy(ls_od_list);
    }  
}

#endif // DSPACES_HAVE_FILE_STORAGE



