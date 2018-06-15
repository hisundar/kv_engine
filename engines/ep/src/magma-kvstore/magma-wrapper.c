#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "magma-wrapper.h"

void
init_magma(const uint64_t memQuota,
        const bool dio,
        const bool kv,
        const int cleaner,
        const int cleanermax,
        const int delta,
        const int items,
        const int segments,
        const int sync,
        const bool upsert)
{

}

int shutdown_magma(void)
{
    return 0;
}

int
open_magma(const char *dbPath, const int vbid)
{

    return 1;
}

int
close_magma(const int vbid, const int handle_id, uint64_t *ret_seq_num)
{
    return 1;
}

int
insert_kv(const int db,
        const int vbid,
        const int handle_id,
        const void *key,
        const int keylen,
        const void *value,
        const int valuelen,
        const uint64_t seq_num)
{
    return 1;
}

int
delete_kv(const int db,
        const int vbid,
        const int handle_id,
        const void *key,
        const int keylen)
{
    return 1;
}

int
lookup_kv(const int db,
        const int vbid,
        const int handle_id,
        const void *key,
        const int keylen,
        void **value,
        int *valuelen)
{
    return 1;
}

void
get_stats(const int vbid,
    uint64_t *di_memsz,
    uint64_t *di_memszidx,
    uint64_t *di_numpages,
    uint64_t *di_itemscount,
    uint64_t *di_lssfrag,
    uint64_t *di_lssdatasize,
    uint64_t *di_lssusedspace,
    uint64_t *di_reclaimpending,
    uint64_t *st_memsz,
    uint64_t *st_memszidx,
    uint64_t *st_reclaimpending)
{
    return;
}

int
open_backfill_query(const int vbid, const uint64_t seq_num)
{
    return 0;
}

int
close_backfill_query(const int vbid, const int handle_id)
{
    return 0;
}

int
next_backfill_query(
        const int vbid,
        const int handle_id,
        void **retkey,
        int *retkeylen,
        void **retval,
        int *retvallen,
        uint64_t *ret_seq_num)
{
    return 0;
}
