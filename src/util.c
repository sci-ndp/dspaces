/*
 * Copyright (c) 2009, NSF Cloud and Autonomic Computing Center, Rutgers
 * University All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * - Neither the name of the NSF Cloud and Autonomic Computing Center, Rutgers
 * University, nor the names of its contributors may be used to endorse or
 * promote products derived from this software without specific prior written
 * permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *  Ciprian Docan (2009)  TASSL Rutgers University
 *  docan@cac.rutgers.edu
 *  Tong Jin (2011) TASSL Rutgers University
 *  tjin@cac.rutgers.edu
 */

#define _XOPEN_SOURCE 500
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ftw.h>

#include "dspaces-common.h"
#include "util.h"

size_t str_len(const char *str)
{
    if(str)
        return strlen(str);
    else
        return 0;
}

char *str_append_const(char *str, const char *msg)
{
    int len, fix_str;

    len = str_len(str) + str_len(msg) + 1;
    fix_str = (str == 0);
    str = realloc(str, len);
    if(fix_str)
        *str = '\0';
    if(str)
        strcat(str, msg);

    return str;
}

char *str_append(char *str, char *msg)
{
    str = str_append_const(str, msg);

    free(msg);
    return str;
}

/*
 *  Our own implementation of the asprintf functionality
 */
char *alloc_sprintf(const char *fmt_str, ...)
{
    va_list va_args_tmp, va_args;
    int size;
    char *str;

    va_start(va_args_tmp, fmt_str);
    va_copy(va_args, va_args_tmp);
    size = vsnprintf(NULL, 0, fmt_str, va_args_tmp);
    va_end(va_args_tmp);
    str = malloc(sizeof(*str) * (size + 1));
    vsprintf(str, fmt_str, va_args);
    va_end(va_args);

    return (str);
}

/*******************************************************
   Processing parameter lists
**********************************************************/
static char *remove_whitespace(char *start, char *end)
{
    char *s = start;
    char *e = end;
    // int orig_len = (int) (e-s);
    int final_len;
    char *res;
    // remove front whitespace (but do not go far beyond the end)
    while(s <= e && (*s == ' ' || *s == '\t' || *s == '\n'))
        s++;
    if(s <= e) { // there is some text
        // remove tail whitespace
        while(s <= e && (*e == ' ' || *e == '\t' || *e == '\n'))
            e--;
        // create result
        final_len = e - s + 1; //  length of result
        if(final_len > 0) {
            res = (char *)malloc(final_len + 1); // allocate space s..e and \0
            memcpy(res, s, final_len);
            res[final_len] = 0;
        } else {
            // "   = something" patterns end here
            res = NULL;
        }
    } else {
        // no non-whitespace character found
        res = NULL;
    }
    return res;
}

/* Split a line at = sign into name and value pair
   Remove " ", TAB and Newline from around names and values
   Return NULL for name and value if there is no = sign in line
   Return newly allocated strings otherwise
   Used by: esimmon_internal_text_to_name_value_pairs
 */
static void splitnamevalue(const char *line, int linelen, char **name,
                           char **value)
{
    char *equal; // position of first = sign in line

    equal = strchr(line, '=');
    if(equal && equal != line) {
        /* 1. name */
        // from first char to before =
        *name = remove_whitespace((char *)line, equal - 1);
        /* 2. value */
        // from after = to the last character of line
        *value = remove_whitespace(equal + 1, (char *)line + linelen - 1);

    } else if(equal != line) {
        /* check if it as name without = value statement */
        *name = remove_whitespace((char *)line, (char *)line + linelen - 1);
        *value = NULL;
    } else {
        // funny text starting with =. E.g. "=value"
        *name = NULL;
        *value = NULL;
    }
}

struct name_value_pair *text_to_nv_pairs(const char *text)
{
    /* Process a multi-line and/or ;-separated text and create a list
       of name=value pairs from each line which has a
           name = value
       pattern. Whitespaces are removed.
         "X = 1
          Y = 2"
       is not valid because of missing ';', but
          "X=1; Y=5;
          Z=apple"
       is valid
    */
    char *name, *value;
    char *item, *delim;
    int len;
    char line[256];
    struct name_value_pair *res = NULL, *last = NULL, *pair;

    if(!text)
        return res;

    item = (char *)text;
    while(item) {
        delim = strchr(item, ';');
        if(delim)
            len = (int)(delim - item);
        else
            len = strlen(item);

        strncpy(line, item, len);
        line[len] = '\0';

        splitnamevalue(line, len, &name, &value);
        if(name) {
            pair = (struct name_value_pair *)malloc(
                sizeof(struct name_value_pair));
            pair->name = name;
            pair->value = value;
            pair->next = NULL;
            if(last) {
                last->next = pair;
                last = pair;
            } else {
                res = pair;
                last = pair;
            }
        }
        if(delim && delim + 1 != 0)
            item = delim + 1;
        else
            item = NULL;
    }
    return res;
}

void free_nv_pairs(struct name_value_pair *pairs)
{
    struct name_value_pair *p;
    while(pairs) {
        free(pairs->name);
        free(pairs->value);
        p = pairs;
        pairs = pairs->next;
        free(p);
    }
}

/*******************************************************
   Memory Info
**********************************************************/

/* Parse the contents of /proc/meminfo (in buf), return value of "name"
 * (example: "MemTotal:")
 * Returns -errno if the entry cannot be found. */
static long long get_entry(const char* name, const char* buf)
{
    char* hit = strstr(buf, name);
    if (hit == NULL) {
        return -ENODATA;
    }

    errno = 0;
    long long val = strtoll(hit + strlen(name), NULL, 10);
    if (errno != 0) {
        int strtoll_errno = errno;
        fprintf(stderr, "%s: strtol() failed: %s", __func__, strerror(errno));
        return -strtoll_errno;
    }
    return val;
}

/* Like get_entry(), but exit if the value cannot be found */
static long long get_entry_fatal(const char* name, const char* buf)
{
    long long val = get_entry(name, buf);
    if (val < 0) {
        fprintf(stderr, "%s: fatal error, could not find entry '%s' in /proc/meminfo: %s\n", __func__,
                name, strerror((int)-val));
        return 0;
    }
    return val;
}

/* If the kernel does not provide MemAvailable (introduced in Linux 3.14),
 * approximate it using other data we can get */
static long long available_guesstimate(const char* buf)
{
    long long Cached = get_entry_fatal("Cached:", buf);
    long long MemFree = get_entry_fatal("MemFree:", buf);
    long long Buffers = get_entry_fatal("Buffers:", buf);
    long long Shmem = get_entry_fatal("Shmem:", buf);

    return MemFree + Cached + Buffers - Shmem;
}

/* Parse /proc/meminfo.
 * This function either returns valid data or kills the process
 * with a fatal error.
 */
meminfo_t parse_meminfo()
{
    // Note that we do not need to close static FDs that we ensure to
    // `fopen()` maximally once.
    static FILE* fd;
    static int guesstimate_warned = 0;
    // On Linux 5.3, "wc -c /proc/meminfo" counts 1391 bytes.
    // 8192 should be enough for the foreseeable future.
    char buf[8192] = { 0 };
    meminfo_t m = { 0 };

    if (fd == NULL) {
        char buf[MEM_PATH_LEN] = { 0 };
        snprintf(buf, sizeof(buf), "%s/%s", "/proc", "meminfo");
        fd = fopen(buf, "r");
    }
    if (fd == NULL) {
        fprintf(stderr, "could not open /proc/meminfo: %s\n", strerror(errno));
    }
    rewind(fd);

    size_t len = fread(buf, 1, sizeof(buf) - 1, fd);
    if (ferror(fd)) {
        fprintf(stderr, "could not read /proc/meminfo: %s\n", strerror(errno));
    }
    if (len == 0) {
        fprintf(stderr, "could not read /proc/meminfo: 0 bytes returned\n");
    }

    m.MemTotalKiB = (uint64_t) get_entry_fatal("MemTotal:", buf);
    m.SwapTotalKiB = (uint64_t) get_entry_fatal("SwapTotal:", buf);
    long long SwapFree = (uint64_t) get_entry_fatal("SwapFree:", buf);

    long long MemAvailable = get_entry("MemAvailable:", buf);
    if (MemAvailable < 0) {
        MemAvailable = available_guesstimate(buf);
        if (guesstimate_warned == 0) {
            fprintf(stderr, "Warning: Your kernel does not provide MemAvailable data (needs 3.14+)\n"
                            "         Falling back to guesstimate\n");
            guesstimate_warned = 1;
        }
    }

    // Calculate percentages
    m.MemAvailablePercent = (double)MemAvailable * 100 / (double)m.MemTotalKiB;
    if (m.SwapTotalKiB > 0) {
        m.SwapFreePercent = (double)SwapFree * 100 / (double)m.SwapTotalKiB;
    } else {
        m.SwapFreePercent = 0;
    }

    // Convert kiB to MiB
    m.MemTotalMiB = m.MemTotalKiB >> 10;
    m.MemAvailableMiB = ((uint64_t)(MemAvailable)) >> 10;
    m.SwapTotalMiB = m.SwapTotalKiB >> 10;
    m.SwapFreeMiB = ((uint64_t) SwapFree) >> 10;

    return m;
}

/*******************************************************
   Directory Opreations
**********************************************************/
int check_dir_exist(const char* dir_path)
{
    struct stat s;
    int ret;
    
    if((stat(dir_path, &s) == 0) && S_ISDIR(s.st_mode))
        return 1;
    else
        return 0;
    
}

int check_dir_write_permission(const char* dir_path)
{
    if(access(dir_path, W_OK))
        return 1;
    else
        return 0;
}

void mkdir_all_owner_permission(const char* dir_path)
{
    mkdir(dir_path, 0700);
}

static int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    int rv = remove(fpath);

    if (rv)
        perror(fpath);

    return rv;
}

int remove_dir_rf(const char *dir_path)
{
    return nftw(dir_path, unlink_cb, 256, FTW_DEPTH | FTW_PHYS);
}