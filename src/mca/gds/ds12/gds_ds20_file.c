/*
 * Copyright (c) 2018      Mellanox Technologies, Inc.
 *                         All rights reserved.
 *
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "include/pmix_common.h"

#include "src/include/pmix_globals.h"
#include "src/mca/gds/base/base.h"

#include "src/mca/common/dstore/dstore_file.h"
#include "gds_ds12_file.h"

#define ESH_KNAME_PTR_V20(addr)                             \
    ((char *)addr + sizeof(size_t))

#define ESH_KNAME_LEN_V20(kl, key)      \
do {                             \
    size_t _klen = strlen(key) + 1;                     \
    kl = (_klen < ESH_MIN_KEY_LEN) ?            \
    ESH_MIN_KEY_LEN : _klen;                            \
} while(0)

#define ESH_DATA_PTR_V20(dp, addr)                              \
do {                             \
    size_t kl;              \
    ESH_KNAME_LEN_V20(kl, ESH_KNAME_PTR_V20(addr));         \
    dp = addr + sizeof(size_t) + kl;  \
} while(0)

#define ESH_DATA_SIZE_V20(dsz, addr, data_ptr)                   \
do {                             \
    size_t __sz;                                            \
    memcpy(&__sz, addr, sizeof(size_t));                \
    dsz = __sz - (data_ptr - addr);              \
} while(0)

#define ESH_KEY_SIZE_V20(ks, key, size)                         \
do {                \
    size_t kl;                      \
    ESH_KNAME_LEN_V20(kl, (char*)key);              \
    ks = sizeof(size_t) + kl + size;    \
} while(0)

/* in ext slot new offset will be stored in case if
 * new data were added for the same process during
 * next commit
 */
#define EXT_SLOT_SIZE_V20(ssz)                                 \
    ESH_KEY_SIZE_V20(ssz, ESH_REGION_EXTENSION, sizeof(size_t))


#define ESH_PUT_KEY_V20(addr, key, buffer, size)            \
do {                             \
    size_t sz, ksz;                      \
    ESH_KEY_SIZE_V20(sz, key, size);                \
    ESH_KNAME_LEN_V20(ksz, key);                    \
    memcpy(addr, &sz, sizeof(size_t));                      \
    memset(addr + sizeof(size_t), 0, ksz);                            \
    strncpy((char *)addr + sizeof(size_t),                  \
            key, ksz);                   \
    memcpy(addr + sizeof(size_t) + ksz,  \
            buffer, size);                                  \
} while(0)

static size_t pmix_ds20_kv_size(uint8_t *key)
{
    size_t size;

    memcpy(&size, key, sizeof(size_t));
    return size;
}

static char* pmix_ds20_key_name_ptr(uint8_t *addr)
{
    return ESH_KNAME_PTR_V20(addr);
}

static size_t pmix_ds20_key_name_len(char *key)
{
    size_t sz;
    ESH_KNAME_LEN_V20(sz, key);
    return sz;
}

static uint8_t* pmix_ds20_data_ptr(uint8_t *addr)
{
    uint8_t *dptr;
    ESH_DATA_PTR_V20(dptr, addr);
    return dptr;
}

static size_t pmix_ds20_data_size(uint8_t *addr, uint8_t* data_ptr)
{
    size_t dsize;
    ESH_DATA_SIZE_V20(dsize, addr, data_ptr);
    return dsize;
}

static size_t pmix_ds20_key_size(char *addr, size_t data_size)
{
    size_t sz;
    ESH_KEY_SIZE_V20(sz, addr, data_size);
    return sz;
}

static size_t pmix_ds20_ext_slot_size(void)
{
    size_t sz;
    EXT_SLOT_SIZE_V20(sz);
    return sz;
}

static int pmix_ds20_put_key(uint8_t *addr, char *key, void *buf, size_t size)
{
    ESH_PUT_KEY_V20(addr, key, buf, size);
    return PMIX_SUCCESS;
}

static bool pmix_ds20_is_invalid(uint8_t *addr)
{
    size_t klen;
    ESH_KNAME_LEN_V20(klen, ESH_KNAME_PTR_V20(addr));
    bool ret = (0 == strncmp(ESH_REGION_INVALIDATED, ESH_KNAME_PTR_V20(addr),
                            klen));
    return ret;
}

static void pmix_ds20_set_invalid(uint8_t *addr)
{
    size_t klen;
    ESH_KNAME_LEN_V20(klen, ESH_REGION_INVALIDATED);
    strncpy(ESH_KNAME_PTR_V20(addr), ESH_REGION_INVALIDATED,
            klen);
}

static bool pmix_ds20_is_ext_slot(uint8_t *addr)
{
    bool ret;
    size_t klen;
    ESH_KNAME_LEN_V20(klen, ESH_KNAME_PTR_V20(addr));
    ret = (0 == strncmp(ESH_REGION_EXTENSION, ESH_KNAME_PTR_V20(addr),
                        klen));
    return ret;
}

static bool pmix_ds20_kname_match(uint8_t *addr, const char *key, size_t key_hash)
{
    bool ret = 0;
    size_t klen;
    PMIX_HIDE_UNUSED_PARAMS(key_hash);
    ESH_KNAME_LEN_V20(klen, key);
    ret = (0 == strncmp(ESH_KNAME_PTR_V20(addr),
                        key, klen));
    return ret;
}


pmix_common_dstore_file_cbs_t pmix_ds20_file_module = {
    .name = "ds20",
    .kval_size = pmix_ds20_kv_size,
    .kname_ptr = pmix_ds20_key_name_ptr,
    .kname_len = pmix_ds20_key_name_len,
    .data_ptr = pmix_ds20_data_ptr,
    .data_size = pmix_ds20_data_size,
    .key_size = pmix_ds20_key_size,
    .ext_slot_size = pmix_ds20_ext_slot_size,
    .put_key = pmix_ds20_put_key,
    .is_invalid = pmix_ds20_is_invalid,
    .is_extslot = pmix_ds20_is_ext_slot,
    .set_invalid = pmix_ds20_set_invalid,
    .key_hash = NULL,
    .key_match = pmix_ds20_kname_match
};
